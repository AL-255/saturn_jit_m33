/* Saturn → Thumb-2 basic-block translator.
 *
 * Iteration 1 of the JIT. Implementation strategy:
 *
 *   * The compiled block is a Thumb function `(saturn_t *st) -> addr_t`.
 *     It receives `st` in r0 (AAPCS).
 *
 *   * For each Saturn opcode we either inline a tiny Thumb-2 sequence
 *     or emit a call to a helper from saturn_registers.c. Both paths
 *     produce identical Saturn-visible state, so cross-validation
 *     against the interpreter is a strict equality check on saturn_t.
 *
 *   * The block ends at any control-flow instruction. We compute the
 *     next-PC at translate time when possible (static GOTO/GOLONG/
 *     GOTO-abs) and emit a constant; otherwise the helper returns the
 *     resolved PC at runtime (e.g. branches whose taken/not-taken
 *     depends on carry/ST/registers).
 *
 *   * Saturn `saturn_ops`/`saturn_branches_*` counters are updated by
 *     a single "ops += N" at block exit (single ADDS + STR.W to the
 *     state struct) — match-for-match with the interpreter so the
 *     bench mode comparison is apples-to-apples.
 *
 * Calling convention inside a compiled block:
 *
 *   r0      saturn_t*       (preserved across helper calls — saved in r4)
 *   r4      saturn_t*       (callee-save register we use as base ptr)
 *   r0..r3  helper args/ret
 *
 * Block prologue:   push {r4, lr};  mov r4, r0
 * Block epilogue:   mov r0, <next_pc>;  pop {r4, pc}
 *
 * Helpers from saturn_registers.c match the (res, a, b, code) shape
 * and clobber only r0..r3 + flags per AAPCS; r4 stays valid. */

#include "saturn_jit.h"
#include "saturn_state.h"
#include "saturn_registers.h"
#include "saturn_mem.h"
#include "jit_helpers.h"
#include "jit_config.h"
#include "thumb2_emit.h"
#include <stddef.h>

/* offsetof for inline byte-offset loads/stores into saturn_t. */
#define OFS(field)      ((uint16_t)offsetof(saturn_t, field))
#define OFS_REG(rname)  ((uint16_t)(offsetof(saturn_t, reg) + (rname) * NIB_PER_REG))

/* Tracks whether r2 currently holds a fresh carry value from an inline
 * arith emit that hasn't been stored to saturn.carry. Used by
 * JIT_OPT_DEFER_CARRY to coalesce multiple carry stores into one at
 * block exit. Reset at every translate entry. */
static bool s_carry_dirty_r2;

/* Forward decl for use in inline arith emits below. */
static void emit_strb_smart(emit_ctx_t *e, int rt, int rn, uint16_t imm);

/* Flush r2 → saturn.carry if any pending inline-arith carry is alive.
 * Called before any read of saturn.carry (e.g. GOC/GONC) and before
 * block exit. */
static void emit_flush_carry(emit_ctx_t *e) {
    if (s_carry_dirty_r2) {
        emit_strb_smart(e, 2, 4, OFS(carry));
        s_carry_dirty_r2 = false;
    }
}

/* Called by translators that explicitly OVERWRITE saturn.carry (compare-
 * branch, RTNSC/RTNCC, P±1): the prior in-r2 carry is dead, so just
 * clear the dirty flag without emitting a store. */
static void discard_pending_carry(void) { s_carry_dirty_r2 = false; }

/* Inline rstk_pop: result in r0. Assumes r4 = &saturn.
 *
 *   ldrsb r0, [r4, #OFS(rstk_ptr)]   ; signed load of int8 ptr
 *   cmp   r0, #0
 *   blt   underflow_return_0
 *   mov   r1, r0                     ; save old ptr
 *   subs  r0, r0, #1                 ; new ptr = old - 1
 *   strb  r0, [r4, #OFS(rstk_ptr)]
 *   lsls  r1, r1, #2                 ; ptr * 4 (entry size)
 *   adds  r1, r1, #OFS(rstk)         ; byte offset into saturn
 *   ldr   r0, [r4, r1]               ; r0 = rstk[old_ptr]
 *   b     done
 * underflow:
 *   movs  r0, #0
 * done:
 */
static void emit_inline_rstk_pop(emit_ctx_t *e) {
#if !JIT_OPT_INLINE_RSTK
    emit_mov_any(e, 0, 4);
    emit_bl_to(e, (const void *)jit_rstk_pop);
    return;
#endif
    /* LDRSB Rt, [Rn, #imm12] T2: 1111 1001 1001 Rn | Rt imm12 */
    {
        uint32_t hi = 0xF990 | 4;             /* Rn = 4 */
        uint32_t lo = (0 << 12) | OFS(rstk_ptr);
        emit_w32(e, (hi << 16) | lo);          /* ldrsb r0, [r4, #OFS(rstk_ptr)] */
    }
    emit_cmp_imm_t2(e, 0, 0);                   /* cmp r0, #0 */
    /* b.w lt to underflow target — patched later. */
    uint32_t br_underflow = emit_b_w_placeholder(e, 0xB);   /* cond LT */
    /* fast path */
    emit_movs_lo(e, 1, 0);                      /* mov r1, r0 (save old ptr) */
    emit_subs_lo_lo_imm3(e, 0, 0, 1);           /* subs r0, r0, #1 */
    emit_strb_imm(e, 0, 4, OFS(rstk_ptr));
    /* lsls r1, r1, #2  (T1: 0000 0 imm5 Rm Rd; for r1, imm=2 → 0x0089) */
    emit_hw(e, 0x0000 | (2 << 6) | (1 << 3) | 1);  /* lsls r1, r1, #2 */
    /* addw r1, r1, #OFS(rstk)  (T4 ADD imm12 small via emit_add_imm_t3_small) */
    emit_add_imm_t3_small(e, 1, 1, OFS(rstk));
    /* ldr.w r0, [r4, r1] — T2 LDR register: 1111 1000 0101 Rn | Rt 000000 type Rm
     * Encoding: hi = 0xF850 | Rn, lo = (Rt<<12) | (imm2<<4) | Rm (with type=00 LSL, shift=0).
     * For ldr r0, [r4, r1]: Rn=4, Rt=0, Rm=1. */
    {
        uint32_t hi = 0xF850 | 4;
        uint32_t lo = (0 << 12) | (0 << 4) | 1;
        emit_w32(e, (hi << 16) | lo);
    }
    uint32_t br_done = emit_b_w_placeholder(e, -1);
    uint32_t L_underflow = e->pos;
    emit_mov_lo_imm8(e, 0, 0);                   /* movs r0, #0 */
    uint32_t L_done = e->pos;
    emit_patch_b_w(e, br_underflow, L_underflow);
    emit_patch_b_w(e, br_done, L_done);
}

/* Inline rstk_push: pushes r1 onto the stack. Assumes r4 = &saturn.
 * For overflow (ptr already at 7), falls back to the helper (which
 * implements the shift-drop semantics from x48ng). */
static void emit_inline_rstk_push(emit_ctx_t *e) {
#if !JIT_OPT_INLINE_RSTK
    /* Caller has set r1 = addr; we still need r0 = &saturn. */
    emit_mov_any(e, 0, 4);
    emit_bl_to(e, (const void *)jit_rstk_push);
    return;
#endif
    /* r4 = &saturn, r1 = addr already set by caller. */
    {
        uint32_t hi = 0xF990 | 4;
        uint32_t lo = (2 << 12) | OFS(rstk_ptr);
        emit_w32(e, (hi << 16) | lo);              /* ldrsb r2, [r4, #OFS(rstk_ptr)] */
    }
    emit_adds_lo_lo_imm3(e, 2, 2, 1);              /* adds r2, r2, #1 */
    emit_cmp_imm_t2(e, 2, NB_RSTK);                 /* cmp r2, #8 */
    uint32_t br_overflow = emit_b_w_placeholder(e, 0xA);  /* cond GE */
    /* fast path */
    emit_strb_imm(e, 2, 4, OFS(rstk_ptr));
    /* lsls r2, r2, #2 — encoding 0x0000 | (2<<6) | (2<<3) | 2 = 0x0092 */
    emit_hw(e, 0x0000 | (2 << 6) | (2 << 3) | 2);
    emit_add_imm_t3_small(e, 2, 2, OFS(rstk));      /* addw r2, r2, #OFS(rstk) */
    /* str r1, [r4, r2] : T2 STR register. hi = 0xF840 | Rn, lo = (Rt<<12) | (imm2<<4) | Rm */
    {
        uint32_t hi = 0xF840 | 4;
        uint32_t lo = (1 << 12) | (0 << 4) | 2;
        emit_w32(e, (hi << 16) | lo);
    }
    uint32_t br_done = emit_b_w_placeholder(e, -1);
    uint32_t L_overflow = e->pos;
    /* Fallback: helper expects r0 = &saturn, r1 = addr (already set). */
    emit_mov_any(e, 0, 4);
    emit_bl_to(e, (const void *)jit_rstk_push);
    uint32_t L_done = e->pos;
    emit_patch_b_w(e, br_overflow, L_overflow);
    emit_patch_b_w(e, br_done, L_done);
}

/* Stage-2 op categories we know how to translate. */
typedef enum {
    BLK_CONTINUE,    /* op translated, keep going */
    BLK_END,         /* op was control-flow, block ends with PC already in r0 */
    BLK_END_DYN,     /* op was control-flow, next PC is already computed in r0 (RTN) */
    BLK_UNSUPP,      /* hit an opcode we don't translate yet */
} block_step_t;

/* Forward-declare the helper that the wrapper-stub `translate_group_f`
 * defers to (definition is below the wrapper). */
static block_step_t translate_group_f_real(emit_ctx_t *e, addr_t pc);

static inline nibble_t fetch_nib(addr_t a) { return sat_fetch(a); }
static inline uint32_t fetch_k(addr_t a, int k) { return sat_fetch_field(a, k); }

/* Flush hoisted r5/r6 back into saturn struct before a block exit. No-op
 * when HOIST is off. Must run before the matching pop. */
static void emit_flush_hoist(emit_ctx_t *e) {
#if JIT_OPT_HOIST_BUDGET_OPS
    emit_str_imm(e, 5, 4, OFS(budget_remaining));
    emit_str_imm(e, 6, 4, OFS(saturn_ops));
#else
    (void)e;
#endif
}

/* pop {r4, pc} = 0xBD10 when HOIST is off; pop {r4-r6, pc} = 0xBD70 when on.
 * (reglist8 bits 4/5/6 for r4-r6; P=1 for PC.) */
#if JIT_OPT_HOIST_BUDGET_OPS
#  define EPILOGUE_POP_OP 0xBD70
#else
#  define EPILOGUE_POP_OP 0xBD10
#endif

/* Emit: flush hoisted state ; r0 = <imm32 next PC> ; pop {...} */
static void emit_block_exit_with_pc(emit_ctx_t *e, addr_t pc) {
    emit_flush_hoist(e);
    uint32_t pc20 = pc & 0xFFFFFu;
#if JIT_OPT_NARROW_EXIT_MOV
    if (pc20 <= 0xff) {
        emit_mov_lo_imm8(e, 0, (uint8_t)pc20);   /* movs r0, #imm8 (2 bytes T2) */
    } else {
        emit_mov_imm32(e, 0, pc20);
    }
#else
    emit_mov_imm32(e, 0, pc20);
#endif
    emit_hw(e, EPILOGUE_POP_OP);
}

/* Variant where the next PC is already in r0 at exit (e.g. RTN family
 * — popped from RSTK by a helper call). Emits only the flush + pop. */
static void emit_block_exit_pc_in_r0(emit_ctx_t *e) {
    emit_flush_hoist(e);
    emit_hw(e, EPILOGUE_POP_OP);
}

/* Emit:   add r0, r4, #ofs   (pointer into saturn_t in r0)
 *         movw r1, #imm      (the int argument)
 *         bl helper            (writes back through pointer; clobbers r0..r3)
 */
static void emit_helper_call_ptr_int(emit_ctx_t *e, int ptr_ofs, int imm, const void *helper) {
    emit_add_imm_t3_small(e, 0, 4, ptr_ofs);
    emit_mov_imm32(e, 1, (uint32_t)imm);
    emit_bl_to(e, helper);
}

/* Emit:   add r0, r4, #ofs_res
 *         add r1, r4, #ofs_a
 *         add r2, r4, #ofs_b
 *         movw r3, #imm
 *         bl helper
 */
static void emit_helper_call_3ptr_int(emit_ctx_t *e,
                                      int ofs_res, int ofs_a, int ofs_b,
                                      int imm, const void *helper) {
    emit_add_imm_t3_small(e, 0, 4, ofs_res);
    emit_add_imm_t3_small(e, 1, 4, ofs_a);
    emit_add_imm_t3_small(e, 2, 4, ofs_b);
    emit_mov_imm32(e, 3, (uint32_t)imm);
    emit_bl_to(e, helper);
}

/* (a_ptr, b_ptr, code) — for reg_copy and reg_xchg whose signatures
 * are (nibble*, nibble*, int). */
static void emit_helper_call_2ptr_int(emit_ctx_t *e,
                                      int ofs_a, int ofs_b,
                                      int imm, const void *helper) {
    emit_add_imm_t3_small(e, 0, 4, ofs_a);
    emit_add_imm_t3_small(e, 1, 4, ofs_b);
    emit_mov_imm32(e, 2, (uint32_t)imm);
    emit_bl_to(e, helper);
}

/* ---------------- Inline fast-paths (run-time optimizations) ----------------
 *
 * For ops with a fixed small field (FS_A = 5 nibbles, FS_B = 2, FS_XS/S = 1)
 * and no field arithmetic (zero / copy / exchange), the helper-call shape
 * (5-6 ARM insns of setup + jit_helpers' internal loop) is overkill. We
 * unroll the byte-by-byte loop inline. For A-field that's:
 *   - zero:     5 × strb #0
 *   - copy:     5 × (ldrb + strb)
 *   - exchange: 5 × (ldrb a, ldrb b, strb b→a, strb a→b)
 * These have:
 *   - no helper call → no register save/restore around BL
 *   - no carry/condition logic
 *   - no loop overhead
 * Per-op savings: ~8-12 host instructions, ~20 host cycles in the hot
 * loop. Matters on workloads like `arith` where zero/copy/xchg fire
 * every iter. */

/* Smart ldrb/strb: pick the 16-bit T1 encoding when rt,rn ≤ 7 and the
 * byte offset fits in 5 bits, else fall back to the 32-bit T2 form.
 * Halves code size for the typical inline-arith load/store of REG_A
 * (offsets 0..4) or REG_B (16..20). */
static void emit_ldrb_smart(emit_ctx_t *e, int rt, int rn, uint16_t imm) {
#if JIT_OPT_NARROW_LDST
    if (rt <= 7 && rn <= 7 && imm <= 31) { emit_ldrb_lo(e, rt, rn, (uint8_t)imm); return; }
#endif
    emit_ldrb_imm(e, rt, rn, imm);
}
static void emit_strb_smart(emit_ctx_t *e, int rt, int rn, uint16_t imm) {
#if JIT_OPT_NARROW_LDST
    if (rt <= 7 && rn <= 7 && imm <= 31) { emit_strb_lo(e, rt, rn, (uint8_t)imm); return; }
#endif
    emit_strb_imm(e, rt, rn, imm);
}

/* INC/DEC-specific narrow variants: same logic as _smart but gated on
 * JIT_OPT_NARROW_INCDEC. Used only inline_inc_a / inline_dec_a so the
 * arith regression we see when narrowing ADD/SUB doesn't bite. */
static void emit_ldrb_incdec(emit_ctx_t *e, int rt, int rn, uint16_t imm) {
#if JIT_OPT_NARROW_INCDEC
    if (rt <= 7 && rn <= 7 && imm <= 31) { emit_ldrb_lo(e, rt, rn, (uint8_t)imm); return; }
#endif
    emit_ldrb_imm(e, rt, rn, imm);
}
static void emit_strb_incdec(emit_ctx_t *e, int rt, int rn, uint16_t imm) {
#if JIT_OPT_NARROW_INCDEC
    if (rt <= 7 && rn <= 7 && imm <= 31) { emit_strb_lo(e, rt, rn, (uint8_t)imm); return; }
#endif
    emit_strb_imm(e, rt, rn, imm);
}

static void emit_inline_zero_field_a(emit_ctx_t *e, int reg_id) {
    uint16_t base = OFS_REG(reg_id);
    emit_mov_lo_imm8(e, 0, 0);
#if JIT_OPT_VECTOR_LDST
    /* Word store covers nibbles 0..3 (4 bytes), strb covers nibble 4.
     * base is 4-aligned (regs at offsets 0/16/32/48). */
    emit_str_imm(e, 0, 4, base);
    emit_strb_smart(e, 0, 4, base + 4);
#else
    for (int i = 0; i < 5; i++) emit_strb_smart(e, 0, 4, base + i);
#endif
}
static void emit_inline_copy_field_a(emit_ctx_t *e, int dst_id, int src_id) {
    uint16_t dst = OFS_REG(dst_id), src = OFS_REG(src_id);
#if JIT_OPT_VECTOR_LDST
    emit_ldr_imm(e, 0, 4, src);
    emit_str_imm(e, 0, 4, dst);
    emit_ldrb_smart(e, 0, 4, src + 4);
    emit_strb_smart(e, 0, 4, dst + 4);
#else
    for (int i = 0; i < 5; i++) {
        emit_ldrb_smart(e, 0, 4, src + i);
        emit_strb_smart(e, 0, 4, dst + i);
    }
#endif
}
static void emit_inline_xchg_field_a(emit_ctx_t *e, int a_id, int b_id) {
    uint16_t a = OFS_REG(a_id), b = OFS_REG(b_id);
#if JIT_OPT_VECTOR_LDST
    emit_ldr_imm(e, 0, 4, a);
    emit_ldr_imm(e, 1, 4, b);
    emit_str_imm(e, 1, 4, a);
    emit_str_imm(e, 0, 4, b);
    emit_ldrb_smart(e, 0, 4, a + 4);
    emit_ldrb_smart(e, 1, 4, b + 4);
    emit_strb_smart(e, 1, 4, a + 4);
    emit_strb_smart(e, 0, 4, b + 4);
#else
    for (int i = 0; i < 5; i++) {
        emit_ldrb_smart(e, 0, 4, a + i);
        emit_ldrb_smart(e, 1, 4, b + i);
        emit_strb_smart(e, 1, 4, a + i);
        emit_strb_smart(e, 0, 4, b + i);
    }
#endif
}

/* Same primitives for the more general FS_W field (16 nibbles) ... only
 * called from group A/B fs≥8 with fixed W; we don't currently inline
 * W-field, just A-field. */

/* Inline INC (A-field, hex mode) — adds 1 with carry-out.
 *
 *   movs r2, #1                ; carry-in (the "+1")
 *   for i in 0..4:
 *     ldrb r0, [r4, #ofs_A_i]
 *     add  r0, r0, r2
 *     cmp  r0, #16
 *     itte hs
 *     subhs r0, r0, #16        ; if HS: wrap
 *     movhs r2, #1             ; if HS: carry-out = 1
 *     movlo r2, #0             ; if LO: carry-out = 0
 *     strb r0, [r4, #ofs_A_i]
 *   strb r2, [r4, #ofs_carry]
 *
 * Same shape for DEC, but starting carry-in = 1 (the "-1") with
 * subtraction and underflow wrap to 15. We use ADDS+CMP for INC and
 * SUBS+CMP for DEC. */

#define EMIT_ITTE_HS(e)   emit_hw((e), 0xBF26)
#define EMIT_ITTE_LO(e)   emit_hw((e), 0xBF3A)

/* CLZ Rd, Rm (T1): counts leading zeros, returns 32 if Rm == 0. */
static void emit_clz(emit_ctx_t *e, int rd, int rm) {
    uint32_t hi = 0xFAB0 | (rm & 0xf);
    uint32_t lo = 0xF080 | ((rd & 0xf) << 8) | (rm & 0xf);
    emit_w32(e, (hi << 16) | lo);
}

/* LSRS Rd, Rm, #imm5 (T1) — sets flags, low regs only. */
static void emit_lsrs_lo_imm(emit_ctx_t *e, int rd, int rm, uint8_t imm5) {
    emit_hw(e, 0x0800 | ((imm5 & 0x1f) << 6) | ((rm & 7) << 3) | (rd & 7));
}

/* ORRS Rdn, Rm (T1) — low regs. */
static void emit_orrs_lo(emit_ctx_t *e, int rdn, int rm) {
    emit_hw(e, 0x4300 | ((rm & 7) << 3) | (rdn & 7));
}

/* UBFX Rd, Rn, #lsb, #width  (T1) :
 *   1111 0011 110 Rn | 0 imm3 Rd imm2 0 widthm1
 * imm3:imm2 = lsb (5 bits, 0..31); widthm1 = width-1 (5 bits → width 1..32)
 */
static void emit_ubfx(emit_ctx_t *e, int rd, int rn, int lsb, int width) {
    uint32_t imm3 = (lsb >> 2) & 7;
    uint32_t imm2 = lsb & 3;
    uint32_t hi = 0xF3C0 | (rn & 0xf);
    uint32_t lo = (imm3 << 12) | ((rd & 0xf) << 8) | (imm2 << 6) | ((width - 1) & 0x1f);
    emit_w32(e, (hi << 16) | lo);
}

/* AND.W Rd, Rn, #imm (modified-immediate, T1) — used to mask to a
 * nibble (imm = 0x0f). Encoding (S=0): 1111 0i 00 0000 Rn | 0 imm3 Rd imm8 */
static void emit_and_imm_small(emit_ctx_t *e, int rd, int rn, uint16_t imm) {
    uint32_t i    = (imm >> 11) & 1;
    uint32_t imm3 = (imm >> 8) & 7;
    uint32_t imm8 =  imm & 0xff;
    uint32_t hi = 0xF000 | (i << 10) | (rn & 0xf);
    uint32_t lo = (imm3 << 12) | ((rd & 0xf) << 8) | imm8;
    emit_w32(e, (hi << 16) | lo);
}

/* CBZ Rn, label  (16-bit): 1011 0001 i imm5 Rn
 * The branch goes forward by (imm5 << 1) + 4 bytes from PC, in range
 * 4..130. We emit a placeholder and patch later (similar to B.W). */
static uint32_t emit_cbz_placeholder(emit_ctx_t *e, int rn) {
    uint32_t pos = e->pos;
    emit_hw(e, 0xB100 | ((rn & 7)));        /* CBZ Rn, #0 (placeholder) */
    return pos;
}
static void emit_patch_cbz(emit_ctx_t *e, uint32_t hw_idx, uint32_t target_hw_idx) {
    /* CBZ: imm = (target_byte - (cbz_byte + 4)) / 2  → in halfwords:
     *   offset_in_hw = target_hw - (hw_idx + 2). Must be 0..63. */
    int32_t off = (int32_t)target_hw_idx - (int32_t)hw_idx - 2;
    uint16_t imm5 = off & 0x1f;
    uint16_t i    = (off >> 5) & 1;
    uint16_t old  = e->buf[hw_idx];
    e->buf[hw_idx] = (old & 0xFD07) | (i << 9) | (imm5 << 3);
}

/* INC with early-exit. Per-nibble (≤30 bytes), but CBZ skip to tail
 * when carry-out becomes 0.
 *
 *   movs r2, #1
 *   for i in 0..4:
 *     ldrb r0, [r4, #ofs_i]
 *     adds r0, r0, r2
 *     cmp  r0, #16
 *     itte hs
 *     subhs r0, r0, #16
 *     movhs r2, #1
 *     movlo r2, #0
 *     strb r0, [r4, #ofs_i]
 *     cbz  r2, tail            ; (omitted on last nibble)
 *   tail:
 *     strb r2, [r4, #ofs_carry]
 */
static void emit_inline_inc_a(emit_ctx_t *e, int reg_id) {
    uint16_t base = OFS_REG(reg_id);
    emit_mov_lo_imm8(e, 2, 1);
    uint32_t cbzs[4];
    int cbz_count = 0;
    for (int i = 0; i < 5; i++) {
        emit_ldrb_incdec(e, 0, 4, base + i);
        emit_hw(e, 0x1800 | (2 << 6) | (0 << 3) | 0);    /* adds r0, r0, r2 */
        emit_cmp_imm_t2(e, 0, 16);
        EMIT_ITTE_HS(e);
        emit_subs_lo_imm8(e, 0, 16);
        emit_mov_lo_imm8(e, 2, 1);
        emit_mov_lo_imm8(e, 2, 0);
        emit_strb_incdec(e, 0, 4, base + i);
        if (i < 4) cbzs[cbz_count++] = emit_cbz_placeholder(e, 2);
    }
    uint32_t tail = e->pos;
#if !JIT_OPT_DEFER_CARRY
    emit_strb_incdec(e, 2, 4, OFS(carry));
#endif
    for (int k = 0; k < cbz_count; k++) emit_patch_cbz(e, cbzs[k], tail);
    s_carry_dirty_r2 = true;
}

/* Inline DEC — same shape but subtraction with underflow detection.
 *   movs r2, #1   ; "borrow" to subtract
 *   for i in 0..4:
 *     ldrb r0, [r4, #ofs_A_i]
 *     subs r0, r0, r2           ; sets flags; LO=borrow needed (orig < r2)
 *     itte lo
 *     addlo r0, r0, #16         ; LO: wrap up
 *     movlo r2, #1              ; LO: borrow-out = 1
 *     movhs r2, #0              ; HS: borrow-out = 0
 *     strb r0, [r4, #ofs_A_i]
 *   strb r2, [r4, #ofs_carry]
 *
 * After SUBS, flags reflect (orig - borrow). LO (carry clear) means
 * borrow was needed. ITTE LO emits "T T E" relative to LO. */
/* Inline field-add A-field, hex mode.
 *
 *   movs r2, #0                ; carry-in
 *   for i in 0..4:
 *     ldrb r0, [r4, #ofs_dst_i]
 *     ldrb r1, [r4, #ofs_src_i]
 *     adds r0, r0, r1
 *     adds r0, r0, r2
 *     cmp  r0, #16
 *     itte hs
 *     subhs r0, r0, #16
 *     movhs r2, #1
 *     movlo r2, #0
 *     strb r0, [r4, #ofs_dst_i]
 *     cbz  r2, tail            ; (omitted on last nibble)
 *   tail:
 *     strb r2, [r4, #ofs_carry]
 *
 * The dst is the destination register (also the first source for most
 * ops like A=A+B). src is the second source. For "A=A+A" both ofs
 * are the same; the inline still works (loads same byte twice).
 */
static void emit_inline_add_a(emit_ctx_t *e, int dst_id, int src_id) {
    uint16_t dst = OFS_REG(dst_id);
    uint16_t src = OFS_REG(src_id);
    emit_mov_lo_imm8(e, 2, 0);
    /* NO early-exit: reg_add unconditionally loops all nibbles, because
     * src[i] might be nonzero at any nibble (unlike INC where we know
     * we're only adding 1 at nibble 0). Stopping early would leave the
     * upper nibbles' src contributions unapplied. */
    for (int i = 0; i < 5; i++) {
        emit_ldrb_smart(e, 0, 4, dst + i);
        emit_ldrb_smart(e, 1, 4, src + i);
        emit_hw(e, 0x1800 | (1 << 6) | (0 << 3) | 0);   /* adds r0, r0, r1 */
        emit_hw(e, 0x1800 | (2 << 6) | (0 << 3) | 0);   /* adds r0, r0, r2 */
#if JIT_OPT_FLAT_CARRY
        /* sum is 0..31. carry-out = bit 4; result nibble = sum & 0xf. */
        emit_ubfx(e, 2, 0, 4, 1);
        emit_and_imm_small(e, 0, 0, 0x0f);
#else
        emit_cmp_imm_t2(e, 0, 16);
        EMIT_ITTE_HS(e);
        emit_subs_lo_imm8(e, 0, 16);
        emit_mov_lo_imm8(e, 2, 1);
        emit_mov_lo_imm8(e, 2, 0);
#endif
        emit_strb_smart(e, 0, 4, dst + i);
    }
#if !JIT_OPT_DEFER_CARRY
    emit_strb_smart(e, 2, 4, OFS(carry));
#endif
    s_carry_dirty_r2 = true;
}

/* Inline field-sub A-field, hex mode. dst = dst - src in A-field.
 *
 *   movs r2, #0                ; borrow-in = 0
 *   for i in 0..4:
 *     ldrb r0, [r4, #ofs_dst_i]
 *     ldrb r1, [r4, #ofs_src_i]
 *     subs r0, r0, r1
 *     subs r0, r0, r2
 *     ; LO (carry clear) = borrow needed
 *     itte lo
 *     addlo r0, r0, #16
 *     movlo r2, #1
 *     movhs r2, #0
 *     strb r0, [r4, #ofs_dst_i]
 *     cbz r2, tail            ; (omitted on last nibble)
 *   tail:
 *     strb r2, [r4, #ofs_carry]
 *
 * Also supports reverse-sub (dst = src - dst) by swapping operands.
 */
static void emit_inline_sub_a(emit_ctx_t *e, int dst_id, int a_id, int b_id) {
    uint16_t dst = OFS_REG(dst_id);
    uint16_t aof = OFS_REG(a_id);
    uint16_t bof = OFS_REG(b_id);
    emit_mov_lo_imm8(e, 2, 0);
    /* Same reasoning as add: no early-exit valid. */
    for (int i = 0; i < 5; i++) {
        emit_ldrb_smart(e, 0, 4, aof + i);
        emit_ldrb_smart(e, 1, 4, bof + i);
        emit_hw(e, 0x1A00 | (1 << 6) | (0 << 3) | 0);   /* subs r0, r0, r1 */
        emit_hw(e, 0x1A00 | (2 << 6) | (0 << 3) | 0);   /* subs r0, r0, r2 */
#if JIT_OPT_FLAT_CARRY
        /* If r0 < 0 (borrow), the 32-bit result has bit 4 set (since
         * -1..-16 = 0xFFFFFFF0..0xFFFFFFFF). UBFX extracts it; AND #0xf
         * does the modular wrap. */
        emit_ubfx(e, 2, 0, 4, 1);
        emit_and_imm_small(e, 0, 0, 0x0f);
#else
        EMIT_ITTE_LO(e);
        emit_adds_lo_imm8(e, 0, 16);
        emit_mov_lo_imm8(e, 2, 1);
        emit_mov_lo_imm8(e, 2, 0);
#endif
        emit_strb_smart(e, 0, 4, dst + i);
    }
#if !JIT_OPT_DEFER_CARRY
    emit_strb_smart(e, 2, 4, OFS(carry));
#endif
    s_carry_dirty_r2 = true;
}

static void emit_inline_dec_a(emit_ctx_t *e, int reg_id) {
    uint16_t base = OFS_REG(reg_id);
    emit_mov_lo_imm8(e, 2, 1);
    uint32_t cbzs[4];
    int cbz_count = 0;
    for (int i = 0; i < 5; i++) {
        emit_ldrb_incdec(e, 0, 4, base + i);
        emit_hw(e, 0x1A00 | (2 << 6) | (0 << 3) | 0);   /* subs r0, r0, r2 */
        EMIT_ITTE_LO(e);
        emit_adds_lo_imm8(e, 0, 16);
        emit_mov_lo_imm8(e, 2, 1);
        emit_mov_lo_imm8(e, 2, 0);
        emit_strb_incdec(e, 0, 4, base + i);
        if (i < 4) cbzs[cbz_count++] = emit_cbz_placeholder(e, 2);
    }
    uint32_t tail = e->pos;
#if !JIT_OPT_DEFER_CARRY
    emit_strb_incdec(e, 2, 4, OFS(carry));
#endif
    for (int k = 0; k < cbz_count; k++) emit_patch_cbz(e, cbzs[k], tail);
    s_carry_dirty_r2 = true;
}

/* Emit a saturn.saturn_ops += N update.
 * We'll do it in one batch at block exit so the inner translation is
 * smaller. Strategy: at exit, emit:
 *    ldr   r0, [r4, #ofs_ops]      ; uint64_t low
 *    ldr   r1, [r4, #ofs_ops + 4]  ; uint64_t high
 *    adds  r0, r0, #N
 *    adcs  r1, r1, #0   -- ADC.W needed
 *    str   r0, [r4, #ofs_ops]
 *    str   r1, [r4, #ofs_ops+4]
 * To keep this iteration small, we treat saturn_ops as 32-bit (low
 * word only) when the JIT block updates it. The interpreter uses
 * 64-bit, so totals over a long bench could diverge in the high word
 * — but the workloads here run for 200k ops total, so the low word is
 * the only one that ever changes anyway. We'll widen to 64-bit later.
 */
static void emit_ops_counter_bump(emit_ctx_t *e, uint32_t n) {
#if JIT_OPT_BUDGET_DRIVEN_OPS
    /* Dispatcher derives saturn_ops from the budget delta. Just shave
     * the budget so the dispatcher sees the right executed count. */
    if (n == 0) return;
    emit_ldr_imm(e, 0, 4, OFS(budget_remaining));
    if (n <= 0xff)        emit_subs_lo_imm8(e, 0, (uint8_t)n);
    else if (n <= 0xfff)  emit_sub_imm_t3_small(e, 0, 0, (uint16_t)n);
    else {
        emit_mov_imm32(e, 1, n);
        /* sub.w r0, r0, r1 (T3 S=0) */
        uint32_t hi = 0xEBA0 | 0;
        uint32_t lo = (0 << 12) | (0 << 8) | 1;
        emit_w32(e, (hi << 16) | lo);
    }
    emit_str_imm(e, 0, 4, OFS(budget_remaining));
    return;
#endif
#if JIT_OPT_OPS_COUNTER_IN_C
    /* C dispatcher does saturn_ops += block_ops on return. Skip the
     * emit entirely. */
    (void)e; (void)n;
    return;
#endif
    if (n == 0) return;
#if JIT_OPT_HOIST_BUDGET_OPS
    /* r6 holds the running saturn_ops value. Just bump it; the block
     * exit will flush back to saturn.saturn_ops. */
    if (n <= 0xff) {
        emit_adds_lo_imm8(e, 6, (uint8_t)n);
    } else if (n <= 0xfff) {
        emit_add_imm_t3_small(e, 6, 6, (uint16_t)n);
    } else {
        emit_mov_imm32(e, 0, n);
        /* add.w r6, r6, r0 */
        uint32_t hi = 0xEB00 | 6;
        uint32_t lo = (0 << 12) | (6 << 8) | 0;
        emit_w32(e, (hi << 16) | lo);
    }
    return;
#endif
    emit_ldr_imm(e, 0, 4, OFS(saturn_ops));
    /* Pick the narrowest add encoding that fits. T2 ADDS Rd, #imm8 is
     * 2 bytes vs ADDW (T4) at 4 bytes; smaller code measured 11-13%
     * faster on memmix and lighter on the others. (QEMU TCG seems to
     * prefer the narrow form even though instruction count is same.) */
    if (n <= 0xff) {
        emit_adds_lo_imm8(e, 0, (uint8_t)n);     /* adds r0, #imm8 (T2, 2 bytes) */
    } else if (n <= 0xfff) {
        emit_add_imm_t3_small(e, 0, 0, (uint16_t)n);
    } else {
        emit_mov_imm32(e, 1, n);
        /* add r0, r0, r1 — no helper for plain ADD reg without flags;
         * use ADDS low-reg form when both fit, else use ADD.W. */
        /* Encoding: ADD (register) T3, S=0: 1110 1011 0000 Rn | 0 imm3 Rd imm2 type Rm
         *           hi = 0xEB00 | Rn, lo = (imm3<<12)|(Rd<<8)|(imm2<<6)|(type<<4)|Rm
         * We need Rd=0 Rn=0 Rm=1, all shift=0. */
        uint32_t hi = 0xEB00 | 0;
        uint32_t lo = (0 << 12) | (0 << 8) | 1;
        emit_w32(e, (hi << 16) | lo);
    }
    emit_str_imm(e, 0, 4, OFS(saturn_ops));
}

/* ---------------- Per-group translators ---------------- */

/* Group A / B / C / D / E / F : field arithmetic + moves. We delegate
 * to the C helpers from saturn_registers.c — semantics are then
 * provably identical to the interpreter. */

/* Lookup table mapping (group A op,  fs<8) → helper + (res, a, b) reg
 * triple. NULL helper means "use special-case below". */
typedef struct {
    void (*helper3)(nibble_t *res, const nibble_t *a, const nibble_t *b, int code);
    void (*helper1)(nibble_t *r, int code);
    uint8_t res, a, b;
} field_arith_entry_t;

#define ENT3(h,r,a,b)  { (h), NULL, (r), (a), (b) }
#define ENT1(h,r)      { NULL, (h), (r), 0, 0 }

static const field_arith_entry_t a_arith[16] = {
    ENT3(reg_add, REG_A, REG_A, REG_B),    /* A=A+B */
    ENT3(reg_add, REG_B, REG_B, REG_C),
    ENT3(reg_add, REG_C, REG_C, REG_A),
    ENT3(reg_add, REG_D, REG_D, REG_C),
    ENT3(reg_add, REG_A, REG_A, REG_A),
    ENT3(reg_add, REG_B, REG_B, REG_B),
    ENT3(reg_add, REG_C, REG_C, REG_C),
    ENT3(reg_add, REG_D, REG_D, REG_D),
    ENT3(reg_add, REG_B, REG_B, REG_A),
    ENT3(reg_add, REG_C, REG_C, REG_B),
    ENT3(reg_add, REG_A, REG_A, REG_C),
    ENT3(reg_add, REG_C, REG_C, REG_D),
    ENT1(reg_dec, REG_A),
    ENT1(reg_dec, REG_B),
    ENT1(reg_dec, REG_C),
    ENT1(reg_dec, REG_D),
};
static const field_arith_entry_t b_arith[16] = {
    ENT3(reg_sub, REG_A, REG_A, REG_B),
    ENT3(reg_sub, REG_B, REG_B, REG_C),
    ENT3(reg_sub, REG_C, REG_C, REG_A),
    ENT3(reg_sub, REG_D, REG_D, REG_C),
    ENT1(reg_inc, REG_A),
    ENT1(reg_inc, REG_B),
    ENT1(reg_inc, REG_C),
    ENT1(reg_inc, REG_D),
    ENT3(reg_sub, REG_B, REG_B, REG_A),
    ENT3(reg_sub, REG_C, REG_C, REG_B),
    ENT3(reg_sub, REG_A, REG_A, REG_C),
    ENT3(reg_sub, REG_C, REG_C, REG_D),
    ENT3(reg_sub, REG_A, REG_B, REG_A),
    ENT3(reg_sub, REG_B, REG_C, REG_B),
    ENT3(reg_sub, REG_C, REG_A, REG_C),
    ENT3(reg_sub, REG_D, REG_C, REG_D),
};

/* For A,fs≥8 (move/zero/exchange). reg_copy, reg_zero, reg_xchg signatures
 * differ enough that we'll just emit case-by-case. */

static void emit_field_arith(emit_ctx_t *e, const field_arith_entry_t *t, int field) {
    if (t->helper3) {
        emit_helper_call_3ptr_int(e, OFS_REG(t->res), OFS_REG(t->a), OFS_REG(t->b),
                                  field, (const void *)t->helper3);
    } else {
        emit_helper_call_ptr_int(e, OFS_REG(t->res), field, (const void *)t->helper1);
    }
}

/* Translate group A at pc. Returns BLK_CONTINUE / BLK_UNSUPP. */
static block_step_t translate_group_a(emit_ctx_t *e, addr_t pc) {
    int fs = fetch_nib(pc + 1);
    int op = fetch_nib(pc + 2);
    int field = fs & 7;
    if (fs < 8) {
        emit_field_arith(e, &a_arith[op], field);
        return BLK_CONTINUE;
    }
    /* fs ≥ 8: zero / copy / exchange */
    switch (op) {
    case 0x0: emit_helper_call_ptr_int(e, OFS_REG(REG_A), field, (const void *)reg_zero); break;
    case 0x1: emit_helper_call_ptr_int(e, OFS_REG(REG_B), field, (const void *)reg_zero); break;
    case 0x2: emit_helper_call_ptr_int(e, OFS_REG(REG_C), field, (const void *)reg_zero); break;
    case 0x3: emit_helper_call_ptr_int(e, OFS_REG(REG_D), field, (const void *)reg_zero); break;
    case 0x4: emit_helper_call_2ptr_int(e, OFS_REG(REG_A), OFS_REG(REG_B), field, (const void *)reg_copy); break;
    case 0x5: emit_helper_call_2ptr_int(e, OFS_REG(REG_B), OFS_REG(REG_C), field, (const void *)reg_copy); break;
    case 0x6: emit_helper_call_2ptr_int(e, OFS_REG(REG_C), OFS_REG(REG_A), field, (const void *)reg_copy); break;
    case 0x7: emit_helper_call_2ptr_int(e, OFS_REG(REG_D), OFS_REG(REG_C), field, (const void *)reg_copy); break;
    case 0x8: emit_helper_call_2ptr_int(e, OFS_REG(REG_B), OFS_REG(REG_A), field, (const void *)reg_copy); break;
    case 0x9: emit_helper_call_2ptr_int(e, OFS_REG(REG_C), OFS_REG(REG_B), field, (const void *)reg_copy); break;
    case 0xA: emit_helper_call_2ptr_int(e, OFS_REG(REG_A), OFS_REG(REG_C), field, (const void *)reg_copy); break;
    case 0xB: emit_helper_call_2ptr_int(e, OFS_REG(REG_C), OFS_REG(REG_D), field, (const void *)reg_copy); break;
    case 0xC: emit_helper_call_2ptr_int(e, OFS_REG(REG_A), OFS_REG(REG_B), field, (const void *)reg_xchg); break;
    case 0xD: emit_helper_call_2ptr_int(e, OFS_REG(REG_B), OFS_REG(REG_C), field, (const void *)reg_xchg); break;
    case 0xE: emit_helper_call_2ptr_int(e, OFS_REG(REG_A), OFS_REG(REG_C), field, (const void *)reg_xchg); break;
    case 0xF: emit_helper_call_2ptr_int(e, OFS_REG(REG_C), OFS_REG(REG_D), field, (const void *)reg_xchg); break;
    }
    return BLK_CONTINUE;
}

static block_step_t translate_group_b(emit_ctx_t *e, addr_t pc) {
    int fs = fetch_nib(pc + 1);
    int op = fetch_nib(pc + 2);
    int field = fs & 7;
    if (fs < 8) {
        emit_field_arith(e, &b_arith[op], field);
        return BLK_CONTINUE;
    }
    /* shifts / negate — for now, fallback */
    (void)op;
    return BLK_UNSUPP;
}

/* Groups C..F : same tables but field fixed to FS_A. */
static block_step_t translate_group_c(emit_ctx_t *e, addr_t pc) {
    int op = fetch_nib(pc + 1);
    /* A-field DEC ops (C..F = A=A-1, B=B-1, C=C-1, D=D-1) get the
     * inline fast path in hex mode. The other ops are field-arith
     * adds that we'll inline in a later iteration. */
#if JIT_OPT_INLINE_INCDEC
    if (op >= 0xC && saturn.hexmode == 16) {
        emit_inline_dec_a(e, op - 0xC);     /* C..F → REG_A..REG_D */
        return BLK_CONTINUE;
    }
#endif
#if JIT_OPT_INLINE_FIELD_ADD
    if (op <= 0xB && saturn.hexmode == 16) {
        /* Group C op 0..B = A-field add. Table:
         *   0: A=A+B  1: B=B+C  2: C=C+A  3: D=D+C
         *   4: A=A+A  5: B=B+B  6: C=C+C  7: D=D+D
         *   8: B=B+A  9: C=C+B  A: A=A+C  B: C=C+D                  */
        static const uint8_t dst[12] = {
            REG_A,REG_B,REG_C,REG_D,REG_A,REG_B,REG_C,REG_D,
            REG_B,REG_C,REG_A,REG_C,
        };
        static const uint8_t src[12] = {
            REG_B,REG_C,REG_A,REG_C,REG_A,REG_B,REG_C,REG_D,
            REG_A,REG_B,REG_C,REG_D,
        };
        emit_inline_add_a(e, dst[op], src[op]);
        return BLK_CONTINUE;
    }
#endif
    emit_field_arith(e, &a_arith[op], FS_A);
    return BLK_CONTINUE;
}
static block_step_t translate_group_d(emit_ctx_t *e, addr_t pc) {
    int op = fetch_nib(pc + 1);
#if JIT_OPT_INLINE_AZERO_COPY
    switch (op) {
    case 0x0: emit_inline_zero_field_a(e, REG_A); break;
    case 0x1: emit_inline_zero_field_a(e, REG_B); break;
    case 0x2: emit_inline_zero_field_a(e, REG_C); break;
    case 0x3: emit_inline_zero_field_a(e, REG_D); break;
    case 0x4: emit_inline_copy_field_a(e, REG_A, REG_B); break;
    case 0x5: emit_inline_copy_field_a(e, REG_B, REG_C); break;
    case 0x6: emit_inline_copy_field_a(e, REG_C, REG_A); break;
    case 0x7: emit_inline_copy_field_a(e, REG_D, REG_C); break;
    case 0x8: emit_inline_copy_field_a(e, REG_B, REG_A); break;
    case 0x9: emit_inline_copy_field_a(e, REG_C, REG_B); break;
    case 0xA: emit_inline_copy_field_a(e, REG_A, REG_C); break;
    case 0xB: emit_inline_copy_field_a(e, REG_C, REG_D); break;
    case 0xC: emit_inline_xchg_field_a(e, REG_A, REG_B); break;
    case 0xD: emit_inline_xchg_field_a(e, REG_B, REG_C); break;
    case 0xE: emit_inline_xchg_field_a(e, REG_A, REG_C); break;
    case 0xF: emit_inline_xchg_field_a(e, REG_C, REG_D); break;
    }
#else
    int field = FS_A;
    switch (op) {
    case 0x0: emit_helper_call_ptr_int(e, OFS_REG(REG_A), field, (const void *)reg_zero); break;
    case 0x1: emit_helper_call_ptr_int(e, OFS_REG(REG_B), field, (const void *)reg_zero); break;
    case 0x2: emit_helper_call_ptr_int(e, OFS_REG(REG_C), field, (const void *)reg_zero); break;
    case 0x3: emit_helper_call_ptr_int(e, OFS_REG(REG_D), field, (const void *)reg_zero); break;
    case 0x4: emit_helper_call_2ptr_int(e, OFS_REG(REG_A), OFS_REG(REG_B), field, (const void *)reg_copy); break;
    case 0x5: emit_helper_call_2ptr_int(e, OFS_REG(REG_B), OFS_REG(REG_C), field, (const void *)reg_copy); break;
    case 0x6: emit_helper_call_2ptr_int(e, OFS_REG(REG_C), OFS_REG(REG_A), field, (const void *)reg_copy); break;
    case 0x7: emit_helper_call_2ptr_int(e, OFS_REG(REG_D), OFS_REG(REG_C), field, (const void *)reg_copy); break;
    case 0x8: emit_helper_call_2ptr_int(e, OFS_REG(REG_B), OFS_REG(REG_A), field, (const void *)reg_copy); break;
    case 0x9: emit_helper_call_2ptr_int(e, OFS_REG(REG_C), OFS_REG(REG_B), field, (const void *)reg_copy); break;
    case 0xA: emit_helper_call_2ptr_int(e, OFS_REG(REG_A), OFS_REG(REG_C), field, (const void *)reg_copy); break;
    case 0xB: emit_helper_call_2ptr_int(e, OFS_REG(REG_C), OFS_REG(REG_D), field, (const void *)reg_copy); break;
    case 0xC: emit_helper_call_2ptr_int(e, OFS_REG(REG_A), OFS_REG(REG_B), field, (const void *)reg_xchg); break;
    case 0xD: emit_helper_call_2ptr_int(e, OFS_REG(REG_B), OFS_REG(REG_C), field, (const void *)reg_xchg); break;
    case 0xE: emit_helper_call_2ptr_int(e, OFS_REG(REG_A), OFS_REG(REG_C), field, (const void *)reg_xchg); break;
    case 0xF: emit_helper_call_2ptr_int(e, OFS_REG(REG_C), OFS_REG(REG_D), field, (const void *)reg_xchg); break;
    }
#endif
    return BLK_CONTINUE;
}
static block_step_t translate_group_e(emit_ctx_t *e, addr_t pc) {
    int op = fetch_nib(pc + 1);
    /* A-field INC ops (4..7 = A=A+1, B=B+1, C=C+1, D=D+1) get the
     * inline fast path in hex mode. */
#if JIT_OPT_INLINE_INCDEC
    if (op >= 4 && op <= 7 && saturn.hexmode == 16) {
        emit_inline_inc_a(e, op - 4);
        return BLK_CONTINUE;
    }
#endif
#if JIT_OPT_INLINE_FIELD_SUB
    if (saturn.hexmode == 16) {
        /* Group E ops 0..3, 8..F = A-field subs. Table:
         *   0: A=A-B    1: B=B-C    2: C=C-A    3: D=D-C
         *   8: B=B-A    9: C=C-B    A: A=A-C    B: C=C-D
         *   C: A=B-A    D: B=C-B    E: C=A-C    F: D=C-D            */
        static const uint8_t dst[16] = {
            REG_A,REG_B,REG_C,REG_D, 0,0,0,0,
            REG_B,REG_C,REG_A,REG_C, REG_A,REG_B,REG_C,REG_D,
        };
        static const uint8_t a_in[16] = {
            REG_A,REG_B,REG_C,REG_D, 0,0,0,0,
            REG_B,REG_C,REG_A,REG_C, REG_B,REG_C,REG_A,REG_C,
        };
        static const uint8_t b_in[16] = {
            REG_B,REG_C,REG_A,REG_C, 0,0,0,0,
            REG_A,REG_B,REG_C,REG_D, REG_A,REG_B,REG_C,REG_D,
        };
        if ((op <= 3) || (op >= 8)) {
            emit_inline_sub_a(e, dst[op], a_in[op], b_in[op]);
            return BLK_CONTINUE;
        }
    }
#endif
    emit_field_arith(e, &b_arith[op], FS_A);
    return BLK_CONTINUE;
}
static block_step_t translate_group_f(emit_ctx_t *e, addr_t pc) {
    return translate_group_f_real(e, pc);
}

/* Group 2 : P=n. We store the literal nibble directly to saturn.p.
 * Inline: movw r0, #n ; strb r0, [r4, #OFS(p)] */
static block_step_t translate_group_2(emit_ctx_t *e, addr_t pc) {
    int n = fetch_nib(pc + 1);
    emit_mov_imm32(e, 0, n);
    emit_strb_imm(e, 0, 4, OFS(p));
    return BLK_CONTINUE;
}

/* ---------------- Group 0 (misc) ----------------
 *
 * The cheap inline subset: SETHEX/SETDEC (write 1 byte), P=P+1/P-1
 * (read-modify-write of saturn.p with carry tracking), CLRST (zero
 * 12 bytes), and the RTN family (call jit_rstk_pop, return result as
 * block exit PC).
 *
 * The packed-nibble forms (C=ST, ST=C, CSTEX) we defer to a future
 * iteration — they touch 12 PSTAT bits + 3 C nibbles and the simpler
 * thing is to fall back to the interpreter. */
static block_step_t translate_group_0(emit_ctx_t *e, addr_t pc, addr_t *out_next) {
    int n1 = fetch_nib(pc + 1);
    switch (n1) {
    case 0x0: { /* RTNSXM : ST[XM]=1; PC = pop_rstk() */
        emit_flush_carry(e);
        emit_mov_imm32(e, 0, 1);
        emit_strb_imm(e, 0, 4, OFS(st));
        emit_inline_rstk_pop(e);
        *out_next = 0;
        return BLK_END_DYN;
    }
    case 0x1: { /* RTN */
        emit_flush_carry(e);
        emit_inline_rstk_pop(e);
        *out_next = 0;
        return BLK_END_DYN;
    }
    case 0x2: { /* RTNSC : pop, carry=1 */
        emit_inline_rstk_pop(e);
        emit_mov_imm32(e, 1, 1);
        emit_strb_imm(e, 1, 4, OFS(carry));
        discard_pending_carry();
        *out_next = 0;
        return BLK_END_DYN;
    }
    case 0x3: { /* RTNCC : pop, carry=0 */
        emit_inline_rstk_pop(e);
        emit_mov_imm32(e, 1, 0);
        emit_strb_imm(e, 1, 4, OFS(carry));
        discard_pending_carry();
        *out_next = 0;
        return BLK_END_DYN;
    }
    case 0x4: /* SETHEX */
        emit_mov_imm32(e, 0, 16);
        emit_strb_imm(e, 0, 4, OFS(hexmode));
        return BLK_CONTINUE;
    case 0x5: /* SETDEC */
        emit_mov_imm32(e, 0, 10);
        emit_strb_imm(e, 0, 4, OFS(hexmode));
        return BLK_CONTINUE;
    case 0xC: /* P=P+1; carry if wraps */
        emit_ldrb_imm(e, 0, 4, OFS(p));
        emit_add_imm_t3_small(e, 0, 0, 1);
        {
            uint32_t hi = 0xF000 | (0 << 10) | 0;
            uint32_t lo = (0 << 12) | (0 << 8) | 0x0F;
            emit_w32(e, (hi << 16) | lo);
        }
        emit_strb_imm(e, 0, 4, OFS(p));
        emit_cmp_imm_t2(e, 0, 0);
        emit_hw(e, 0xBF0C);
        emit_mov_lo_imm8(e, 1, 1);
        emit_mov_lo_imm8(e, 1, 0);
        emit_strb_imm(e, 1, 4, OFS(carry));
        discard_pending_carry();
        return BLK_CONTINUE;
    case 0xD: /* P=P-1; carry if was 0 before */
        emit_ldrb_imm(e, 0, 4, OFS(p));
        emit_cmp_imm_t2(e, 0, 0);
        emit_hw(e, 0xBF0C);
        emit_mov_lo_imm8(e, 1, 1);
        emit_mov_lo_imm8(e, 1, 0);
        emit_strb_imm(e, 1, 4, OFS(carry));
        discard_pending_carry();
        emit_sub_imm_t3_small(e, 0, 0, 1);
        {
            uint32_t hi = 0xF000 | 0;
            uint32_t lo = (0 << 12) | (0 << 8) | 0x0F;
            emit_w32(e, (hi << 16) | lo);
        }
        emit_strb_imm(e, 0, 4, OFS(p));
        return BLK_CONTINUE;
    case 0xF: /* RTI : pop and resume */
        emit_flush_carry(e);
        emit_inline_rstk_pop(e);
        *out_next = 0;
        return BLK_END_DYN;
    default:
        (void)pc;
        return BLK_UNSUPP;
    }
}

/* ---------------- Group 3 : LC immediate-to-C ---------------- */
/* Encoding: 3 n k0 k1 .. k_n  → copy n+1 nibbles into C[P..P+n], wrap.
 * P is runtime — we delegate to jit_lc_copy(&saturn, &rom[pc+2], n+1).
 * The source pointer is computed at translate time as &saturn.rom[pc+2]
 * loaded via movw/movt; saturn.rom is constant for the run. */
static block_step_t translate_group_3(emit_ctx_t *e, addr_t pc, uint32_t *consumed) {
    int n = fetch_nib(pc + 1);
    int count = n + 1;
    *consumed = 2 + count;

#if JIT_OPT_INLINE_LC
    if (count <= JIT_OPT_INLINE_LC_MAX) {
        /* Inline: read literal nibbles at translate time, emit a
         * compact STRB sequence indexing into saturn.reg[REG_C] via
         * register-offset addressing. P is loaded once into r0. */
        emit_ldrb_imm(e, 0, 4, OFS(p));
        /* r1 = &saturn.reg[REG_C][0] */
        emit_add_imm_t3_small(e, 1, 4, OFS_REG(REG_C));
        for (int i = 0; i < count; i++) {
            uint8_t lit = fetch_nib(pc + 2 + i) & 0xf;
            emit_mov_lo_imm8(e, 2, lit);
            /* strb r2, [r1, r0] — T1 reg-offset: 0x540A pattern */
            emit_hw(e, 0x5400 | (0 << 6) | (1 << 3) | 2);
            if (i + 1 < count) {
                emit_adds_lo_lo_imm3(e, 0, 0, 1);     /* adds r0, r0, #1 */
                /* and r0, r0, #0xf (T1 AND.W modified-imm) */
                {
                    uint32_t hi = 0xF000 | 0;
                    uint32_t lo = (0 << 12) | (0 << 8) | 0x0F;
                    emit_w32(e, (hi << 16) | lo);
                }
            }
        }
        return BLK_CONTINUE;
    }
#endif

    const uint8_t *src = (const uint8_t *)(saturn.rom + ((pc + 2) & 0xFFFFFu));
    emit_mov_any(e, 0, 4);
    emit_mov_imm32(e, 1, (uint32_t)src);
    emit_mov_imm32(e, 2, count);
    emit_bl_to(e, (const void *)jit_lc_copy);
    return BLK_CONTINUE;
}

/* ---------------- Group 1 (moves / D pointers / DAT) ---------------- */

/* Helper: emit code that loads saturn.d[idx] into ARM register `rd`. */
static void emit_load_d(emit_ctx_t *e, int rd, int idx) {
    emit_ldr_imm(e, rd, 4, OFS(d) + idx * 4);
}
static void emit_store_d(emit_ctx_t *e, int rs, int idx) {
    emit_str_imm(e, rs, 4, OFS(d) + idx * 4);
}

static block_step_t translate_group_1(emit_ctx_t *e, addr_t pc, uint32_t *consumed) {
    int n1 = fetch_nib(pc + 1);
    switch (n1) {
    case 0x4: { /* DAT W or B short form */
        int n2 = fetch_nib(pc + 2);
        int op = n2 & 7;
        int is_W = n2 < 8;
        int reg_idx = (op & 4) ? REG_C : REG_A;
        int d_idx   = (op & 1) ? 1 : 0;
        bool is_store = (op < 2) || (op >= 4 && op < 6);

#if JIT_OPT_INLINE_DAT
        if (is_W) {
            /* Inline 5-nibble DAT W with bounds check.
             *   ldr  r0, [r4, #ofs_d]
             *   ldr  r1, [r4, #ofs_ram]
             *   ldr  r2, [r4, #ofs_ram_base]
             *   sub.w r2, r0, r2          ; r2 = offset = d - ram_base
             *   ldr  r3, [r4, #ofs_ram_size]
             *   cmp  r2, r3
             *   bhs  fallback             ; out of bounds → helper
             *   add.w r2, r2, r1          ; r2 = &ram[off]
             *   ; transfer
             *   b end
             * fallback:
             *   ; emit standard helper call (store: load d into r0 again)
             *   ...
             * end:
             */
            emit_load_d(e, 0, d_idx);
            emit_ldr_imm(e, 1, 4, OFS(ram));
            emit_ldr_imm(e, 2, 4, OFS(ram_base));
            {
                uint32_t hi = 0xEBA0 | 0;
                uint32_t lo = (0 << 12) | (2 << 8) | (0 << 6) | (0 << 4) | 2;
                emit_w32(e, (hi << 16) | lo);     /* sub.w r2, r0, r2 */
            }
            emit_ldr_imm(e, 3, 4, OFS(ram_size));
            /* The 5-nibble write spans offset..offset+4. To stay in
             * bounds, offset+5 must be ≤ ram_size, i.e. offset must
             * be < ram_size - 4. Subtract 4 from r3 (ram_size) so the
             * CMP+BHS check becomes "branch if offset >= ram_size-4". */
            emit_subs_lo_imm8(e, 3, 4);                      /* subs r3, r3, #4 */
            emit_hw(e, 0x4200 | (1 << 7) | (3 << 3) | 2);  /* cmp r2, r3 T1 */
            uint32_t br_fallback = emit_b_w_placeholder(e, 0x2);   /* cond HS = 0x2 */
            /* Fast path */
            {
                uint32_t hi = 0xEB00 | 2;
                uint32_t lo = (0 << 12) | (2 << 8) | (0 << 6) | (0 << 4) | 1;
                emit_w32(e, (hi << 16) | lo);     /* add.w r2, r2, r1 */
            }
            uint16_t reg_off = OFS_REG(reg_idx);
            if (is_store) {
                emit_ldr_imm(e, 0, 4, reg_off);
                emit_str_imm(e, 0, 2, 0);
                emit_ldrb_imm(e, 0, 4, reg_off + 4);
                emit_strb_imm(e, 0, 2, 4);
            } else {
                emit_ldr_imm(e, 0, 2, 0);
                emit_str_imm(e, 0, 4, reg_off);
                emit_ldrb_imm(e, 0, 2, 4);
                emit_strb_imm(e, 0, 4, reg_off + 4);
            }
            uint32_t br_end = emit_b_w_placeholder(e, -1);          /* unconditional */
            /* Fallback: out-of-bounds → call the helper as if uninlined. */
            uint32_t fallback_pos = e->pos;
            if (is_store) {
                emit_load_d(e, 0, d_idx);
                emit_add_imm_t3_small(e, 1, 4, OFS_REG(reg_idx));
                emit_mov_any(e, 2, 4);
                emit_bl_to(e, (const void *)jit_dat_store_w);
            } else {
                emit_add_imm_t3_small(e, 0, 4, OFS_REG(reg_idx));
                emit_load_d(e, 1, d_idx);
                emit_mov_any(e, 2, 4);
                emit_bl_to(e, (const void *)jit_dat_load_w);
            }
            uint32_t end_pos = e->pos;
            emit_patch_b_w(e, br_fallback, fallback_pos);
            emit_patch_b_w(e, br_end, end_pos);
            *consumed = 3;
            return BLK_CONTINUE;
        }
#endif
        /* setup: r0 (or r1) = d, r0/r1 = reg ptr, r2 = saturn */
        if (is_store) {
            /* args: (addr_t d, reg*, st*) */
            emit_load_d(e, 0, d_idx);
            emit_add_imm_t3_small(e, 1, 4, OFS_REG(reg_idx));
            emit_mov_any(e, 2, 4);
            emit_bl_to(e, is_W ? (const void *)jit_dat_store_w
                              : (const void *)jit_dat_store_b);
        } else {
            /* args: (reg*, addr_t d, st*) */
            emit_add_imm_t3_small(e, 0, 4, OFS_REG(reg_idx));
            emit_load_d(e, 1, d_idx);
            emit_mov_any(e, 2, 4);
            emit_bl_to(e, is_W ? (const void *)jit_dat_load_w
                              : (const void *)jit_dat_load_b);
        }
        *consumed = 3;
        return BLK_CONTINUE;
    }
    case 0x5: { /* DAT n-nib explicit (we handle the explicit-count form,
                 * n2 ≥ 8). field-coded form is rarer and uses variable
                 * field length; leave as UNSUPP for now. */
        int n2 = fetch_nib(pc + 2);
        int n3 = fetch_nib(pc + 3);
        if (n2 < 8) return BLK_UNSUPP;       /* field-coded path */
        int op = n2 & 7;
        int reg_idx = (op & 4) ? REG_C : REG_A;
        int d_idx   = (op & 1) ? 1 : 0;
        bool is_store = (op < 2) || (op >= 4 && op < 6);
        int len = n3 + 1;
        if (is_store) {
            emit_load_d(e, 0, d_idx);
            emit_add_imm_t3_small(e, 1, 4, OFS_REG(reg_idx));
            emit_mov_any(e, 2, 4);
            emit_mov_imm32(e, 3, len);
            emit_bl_to(e, (const void *)jit_dat_store_n);
        } else {
            emit_add_imm_t3_small(e, 0, 4, OFS_REG(reg_idx));
            emit_load_d(e, 1, d_idx);
            emit_mov_any(e, 2, 4);
            emit_mov_imm32(e, 3, len);
            emit_bl_to(e, (const void *)jit_dat_load_n);
        }
        *consumed = 4;
        return BLK_CONTINUE;
    }
    case 0x6: case 0x7: case 0x8: case 0xC: {
        /* D0/D1 ±= (n+1). 3 nibbles total: 1 nx amt. */
        int amt = fetch_nib(pc + 2) + 1;
        int d_idx = (n1 == 0x6 || n1 == 0x8) ? 0 : 1;
        bool sub = (n1 == 0x8 || n1 == 0xC);
        emit_load_d(e, 0, d_idx);
        if (sub) emit_sub_imm_t3_small(e, 0, 0, amt);
        else     emit_add_imm_t3_small(e, 0, 0, amt);
        /* Mask to 20 bits via movw r1,#0xFFFF; movt r1,#0xF; AND. We
         * tried UBFX r0, r0, #0, #20 (one 4-byte insn) and it ran ~25%
         * slower under QEMU TCG across all four workloads despite being
         * 8 bytes smaller — TCG's UBFX backend handler is apparently
         * heavier than the immediate AND it replaces. Real M33 hardware
         * should prefer UBFX; revisit there. */
        emit_movw(e, 1, 0xFFFF);
        emit_movt(e, 1, 0x000F);
        emit_and_reg(e, 0, 0, 1);
        emit_store_d(e, 0, d_idx);
        *consumed = 3;
        return BLK_CONTINUE;
    }
    case 0x9: case 0xA: case 0xB:
    case 0xD: case 0xE: case 0xF: {
        /* Dx = (k nibs little-endian immediate). */
        int target = (n1 < 0xD) ? 0 : 1;
        int len    = (n1 == 0x9 || n1 == 0xD) ? 2
                   : (n1 == 0xA || n1 == 0xE) ? 4 : 5;
        addr_t v = fetch_k(pc + 2, len);
        addr_t mask = (1u << (len*4)) - 1;
        /* r0 = current d ; r1 = ~mask ; AND ; r1 = v ; OR ; store */
        emit_load_d(e, 0, target);
        emit_mov_imm32(e, 1, ~mask & 0xFFFFFu);
        emit_and_reg(e, 0, 0, 1);
        emit_mov_imm32(e, 1, v & mask);
        emit_orr_reg(e, 0, 0, 1);
        emit_store_d(e, 0, target);
        *consumed = 2 + len;
        return BLK_CONTINUE;
    }
    case 0x3: {     /* D0/D1 <-> A/C, A-field forms only (n2 < 8) */
        int n2 = fetch_nib(pc + 2);
        if (n2 >= 8) return BLK_UNSUPP;   /* short B+X forms left for later */
        int reg_idx = (n2 & 4) ? REG_C : REG_A;
        int d_idx   = (n2 & 1) ? 1 : 0;
        int is_xchg = (n2 == 2 || n2 == 3 || n2 == 6 || n2 == 7);
        if (!is_xchg) {
            /* Dn = reg[A-field] : call jit_reg_a_to_addr, store. */
            emit_add_imm_t3_small(e, 0, 4, OFS_REG(reg_idx));
            emit_bl_to(e, (const void *)jit_reg_a_to_addr);
            emit_store_d(e, 0, d_idx);
        } else {
            /* exchange: t = Dn; Dn = reg_a_to_addr(reg); addr_to_reg_a(reg, t). */
            emit_load_d(e, 0, d_idx);
            /* save t in r5 (callee-save) — but we didn't save r5. Use the
             * stack: push r0. */
            emit_hw(e, 0xB401);              /* push {r0} */
            emit_add_imm_t3_small(e, 0, 4, OFS_REG(reg_idx));
            emit_bl_to(e, (const void *)jit_reg_a_to_addr);
            emit_store_d(e, 0, d_idx);
            emit_hw(e, 0xBC02);              /* pop {r1} */
            emit_add_imm_t3_small(e, 0, 4, OFS_REG(reg_idx));
            emit_bl_to(e, (const void *)jit_addr_to_reg_a);
        }
        *consumed = 3;
        return BLK_CONTINUE;
    }
    default:
        return BLK_UNSUPP;
    }
}

/* ---------------- Group F : A-field shifts / negate ---------------- */
static block_step_t translate_group_f_real(emit_ctx_t *e, addr_t pc) {
    int op = fetch_nib(pc + 1);
    switch (op) {
    case 0x0: emit_helper_call_ptr_int(e, OFS_REG(REG_A), FS_A, (const void *)reg_shl); break;
    case 0x1: emit_helper_call_ptr_int(e, OFS_REG(REG_B), FS_A, (const void *)reg_shl); break;
    case 0x2: emit_helper_call_ptr_int(e, OFS_REG(REG_C), FS_A, (const void *)reg_shl); break;
    case 0x3: emit_helper_call_ptr_int(e, OFS_REG(REG_D), FS_A, (const void *)reg_shl); break;
    case 0x4: emit_helper_call_ptr_int(e, OFS_REG(REG_A), FS_A, (const void *)reg_shr); break;
    case 0x5: emit_helper_call_ptr_int(e, OFS_REG(REG_B), FS_A, (const void *)reg_shr); break;
    case 0x6: emit_helper_call_ptr_int(e, OFS_REG(REG_C), FS_A, (const void *)reg_shr); break;
    case 0x7: emit_helper_call_ptr_int(e, OFS_REG(REG_D), FS_A, (const void *)reg_shr); break;
    case 0x8: emit_helper_call_ptr_int(e, OFS_REG(REG_A), FS_A, (const void *)reg_comp2); break;
    case 0x9: emit_helper_call_ptr_int(e, OFS_REG(REG_B), FS_A, (const void *)reg_comp2); break;
    case 0xA: emit_helper_call_ptr_int(e, OFS_REG(REG_C), FS_A, (const void *)reg_comp2); break;
    case 0xB: emit_helper_call_ptr_int(e, OFS_REG(REG_D), FS_A, (const void *)reg_comp2); break;
    case 0xC: emit_helper_call_ptr_int(e, OFS_REG(REG_A), FS_A, (const void *)reg_comp1); break;
    case 0xD: emit_helper_call_ptr_int(e, OFS_REG(REG_B), FS_A, (const void *)reg_comp1); break;
    case 0xE: emit_helper_call_ptr_int(e, OFS_REG(REG_C), FS_A, (const void *)reg_comp1); break;
    case 0xF: emit_helper_call_ptr_int(e, OFS_REG(REG_D), FS_A, (const void *)reg_comp1); break;
    }
    (void)pc;
    return BLK_CONTINUE;
}

/* ---------------- Group 8 long branches (8C/8D/8E/8F) ---------------- */

static block_step_t translate_group_8_branch(emit_ctx_t *e, addr_t pc, addr_t *out_next, uint32_t *consumed) {
    int n1 = fetch_nib(pc + 1);
    switch (n1) {
    case 0xC: {     /* GOLONG ±dddd : PC += sext16(dddd) + 2 */
        uint32_t d = fetch_k(pc + 2, 4);
        *out_next = (pc + sext_nib(d, 4) + 2) & 0xFFFFFu;
        *consumed = 6;
        /* branch counter bump */
        emit_ldr_imm(e, 0, 4, OFS(saturn_branches_taken));
        emit_add_imm_t3_small(e, 0, 0, 1);
        emit_str_imm(e, 0, 4, OFS(saturn_branches_taken));
        return BLK_END;
    }
    case 0xD: {     /* GOTO abs */
        addr_t a = fetch_k(pc + 2, 5);
        *out_next = a & 0xFFFFFu;
        *consumed = 7;
        emit_ldr_imm(e, 0, 4, OFS(saturn_branches_taken));
        emit_add_imm_t3_small(e, 0, 0, 1);
        emit_str_imm(e, 0, 4, OFS(saturn_branches_taken));
        return BLK_END;
    }
    case 0xE: {     /* GOSUBL ±dddd : push pc+6, then jump */
        uint32_t d = fetch_k(pc + 2, 4);
        addr_t target = (pc + sext_nib(d, 4) + 6) & 0xFFFFFu;
        emit_mov_imm32(e, 1, (pc + 6) & 0xFFFFFu);
        emit_inline_rstk_push(e);
        *out_next = target;
        *consumed = 6;
        emit_ldr_imm(e, 0, 4, OFS(saturn_branches_taken));
        emit_add_imm_t3_small(e, 0, 0, 1);
        emit_str_imm(e, 0, 4, OFS(saturn_branches_taken));
        return BLK_END;
    }
    case 0xF: {     /* GOSBVL abs */
        addr_t a = fetch_k(pc + 2, 5);
        emit_mov_imm32(e, 1, (pc + 7) & 0xFFFFFu);
        emit_inline_rstk_push(e);
        *out_next = a & 0xFFFFFu;
        *consumed = 7;
        emit_ldr_imm(e, 0, 4, OFS(saturn_branches_taken));
        emit_add_imm_t3_small(e, 0, 0, 1);
        emit_str_imm(e, 0, 4, OFS(saturn_branches_taken));
        return BLK_END;
    }
    default:
        return BLK_UNSUPP;
    }
}

/* ---------------- Compare+branch translation ----------------
 *
 * Group 86/87/88/89/8A/8B/9 all share the same shape: compute a
 * boolean condition into r0, optionally branch by a sign-extended
 * 8-bit displacement, and update saturn.carry from the condition.
 *
 * Emit shape (block-end, dynamic PC):
 *
 *    <condition-code that leaves 0 or 1 in r0>
 *    cbz r0, L_notaken                 ; 16-bit conditional
 *    ; taken path:
 *    strb r0, [r4, #OFS(carry)]        ; carry = 1
 *    ldr  r1, [r4, #ofs_taken]
 *    adds r1, r1, #1
 *    str  r1, [r4, #ofs_taken]
 *    movw r0, #taken_pc_lo
 *    movt r0, #taken_pc_hi             (omitted if hi == 0)
 *    b.w  L_end
 *  L_notaken:
 *    movs r0, #0
 *    strb r0, [r4, #OFS(carry)]
 *    ldr  r1, [r4, #ofs_skipped]
 *    adds r1, r1, #1
 *    str  r1, [r4, #ofs_skipped]
 *    movw r0, #nt_pc_lo
 *    movt r0, #nt_pc_hi
 *  L_end:
 *    ; falls into block-exit (BLK_END_DYN, which ops-bumps via r1 and
 *    ;  pops {r4, pc})
 *
 * When dd == 0, the "taken" target is RTN — we emit a runtime call to
 * jit_rstk_pop instead of materialising a constant PC.
 *
 * CBZ has range 0..126 bytes; if the not-taken setup exceeds that we
 * fall back to a CMP+B.W via emit_b_w_placeholder. */

/* Helper that writes "movw r0, #imm16 (lo) ; movt r0, #imm16 (hi)" as
 * needed. Always emits movw; movt only if high half nonzero. */
static void emit_set_r0_pc(emit_ctx_t *e, uint32_t pc) {
    emit_movw(e, 0, pc & 0xffff);
    if ((pc >> 16) & 0xffff) emit_movt(e, 0, (pc >> 16) & 0xffff);
}

/* The condition computation differs by opcode group. The caller fills
 * this struct, then emit_compare_branch_tail() handles the common
 * tail. After cond_setup() runs in the emitter, r0 must hold 0 or 1. */
typedef struct {
    enum { CB_TAKEN_STATIC, CB_TAKEN_RTN } taken_kind;
    addr_t taken_pc;            /* used iff CB_TAKEN_STATIC */
    addr_t notaken_pc;
} cb_targets_t;

static void emit_branch_counter(emit_ctx_t *e, uint16_t ofs) {
    emit_ldr_imm(e, 1, 4, ofs);
    emit_add_imm_t3_small(e, 1, 1, 1);
    emit_str_imm(e, 1, 4, ofs);
}

/* After the condition is in r0 (0/1), emit the taken/not-taken
 * dispatch. Returns BLK_END_DYN (r0 holds next PC at exit). */
static void emit_compare_branch_tail(emit_ctx_t *e, const cb_targets_t *t) {
    /* CMP r0, #0 ; B.W eq → notaken */
    emit_cmp_imm_t2(e, 0, 0);
    uint32_t br = emit_b_w_placeholder(e, 0);   /* cond EQ = 0 */

    /* --- taken path --- */
    emit_mov_imm32(e, 0, 1);
    emit_strb_imm(e, 0, 4, OFS(carry));
    emit_branch_counter(e, OFS(saturn_branches_taken));
    if (t->taken_kind == CB_TAKEN_RTN) {
        emit_inline_rstk_pop(e);
    } else {
        emit_set_r0_pc(e, t->taken_pc);
    }
    uint32_t br_end = emit_b_w_placeholder(e, -1);    /* unconditional skip past notaken */

    /* --- notaken path --- */
    uint32_t L_notaken = e->pos;
    emit_mov_imm32(e, 0, 0);
    emit_strb_imm(e, 0, 4, OFS(carry));
    emit_branch_counter(e, OFS(saturn_branches_skipped));
    emit_set_r0_pc(e, t->notaken_pc);

    uint32_t L_end = e->pos;
    emit_patch_b_w(e, br, L_notaken);
    emit_patch_b_w(e, br_end, L_end);
    /* Both paths wrote saturn.carry explicitly. Any pending r2 carry
     * from prior inline arith is now superseded. */
    discard_pending_carry();
}

/* Compute a register-pair compare condition and leave 0/1 in r0.
 * Uses (a_reg, b_reg) and a field code. */
static void emit_pair_compare(emit_ctx_t *e, int a_reg, int b_reg, int field, const void *helper) {
    emit_add_imm_t3_small(e, 0, 4, OFS_REG(a_reg));
    emit_add_imm_t3_small(e, 1, 4, OFS_REG(b_reg));
    emit_mov_imm32(e, 2, field);
    emit_bl_to(e, helper);
}
static void emit_zero_test(emit_ctx_t *e, int reg, int field, const void *helper) {
#if JIT_OPT_INLINE_ZEROTEST
    /* Inline only the A-field case (5 nibbles at a 4-aligned offset).
     * Other fields fall back to the helper call. */
    if (field == FS_A) {
        uint16_t base = OFS_REG(reg);
        emit_ldr_imm(e, 0, 4, base);         /* nibbles 0..3 packed */
        emit_ldrb_smart(e, 1, 4, base + 4);   /* nibble 4 */
        emit_orrs_lo(e, 0, 1);                /* r0 = combined, sets flags */
        emit_clz(e, 0, 0);                    /* 32 if r0 was 0, else < 32 */
        emit_lsrs_lo_imm(e, 0, 0, 5);         /* r0 = 1 iff all-zero, else 0 */
        return;
    }
#endif
    emit_add_imm_t3_small(e, 0, 4, OFS_REG(reg));
    emit_mov_imm32(e, 1, field);
    emit_bl_to(e, helper);
}

/* Translate group 8A : equality compares on A-field + branch (5 nibs). */
static block_step_t translate_group_8A(emit_ctx_t *e, addr_t pc, uint32_t *consumed) {
    int op = fetch_nib(pc + 2);
    uint32_t dd = fetch_k(pc + 3, 2);
    *consumed = 5;

    static const uint8_t pair[8][2] = {
        {REG_A,REG_B},{REG_B,REG_C},{REG_A,REG_C},{REG_C,REG_D},
        {REG_A,REG_B},{REG_B,REG_C},{REG_A,REG_C},{REG_C,REG_D},
    };
    if (op < 8) {
        emit_pair_compare(e, pair[op & 7][0], pair[op & 7][1], FS_A, (const void *)reg_eq);
        if (op >= 4) {
            /* invert: r0 = (r0 == 0) ? 1 : 0 */
            emit_cmp_imm_t2(e, 0, 0);
            emit_hw(e, 0xBF0C);          /* ITE EQ */
            emit_mov_lo_imm8(e, 0, 1);   /* eq → 1 */
            emit_mov_lo_imm8(e, 0, 0);   /* ne → 0 */
        }
    } else {
        int r = op & 3;
        emit_zero_test(e, r, FS_A, (const void *)reg_is_zero);
        if (op >= 0xC) {
            emit_cmp_imm_t2(e, 0, 0);
            emit_hw(e, 0xBF0C);
            emit_mov_lo_imm8(e, 0, 1);
            emit_mov_lo_imm8(e, 0, 0);
        }
    }

    cb_targets_t t;
    if (dd == 0) {
        t.taken_kind = CB_TAKEN_RTN;
        t.taken_pc = 0;
    } else {
        t.taken_kind = CB_TAKEN_STATIC;
        t.taken_pc = (pc + sext_nib(dd, 2) + 3) & 0xFFFFFu;
    }
    t.notaken_pc = (pc + 5) & 0xFFFFFu;
    emit_compare_branch_tail(e, &t);
    return BLK_END_DYN;
}

/* Group 8B : ordered compares (5 nibs). */
static block_step_t translate_group_8B(emit_ctx_t *e, addr_t pc, uint32_t *consumed) {
    int op = fetch_nib(pc + 2);
    uint32_t dd = fetch_k(pc + 3, 2);
    *consumed = 5;
    static const uint8_t pair[4][2] = {
        {REG_A,REG_B},{REG_B,REG_C},{REG_C,REG_A},{REG_D,REG_C},
    };
    int idx = op & 3;
    /* The ISA defines 16 sub-ops as (gt, lt, ge, le) × 4 pairs.
     * We pick one helper and optionally invert. Simpler: pick the
     * exact helper. */
    const void *helper = NULL;
    bool invert = false;
    switch ((op >> 2) & 3) {
    case 0: helper = (const void *)reg_gt; break;
    case 1: helper = (const void *)reg_lt; break;
    case 2: helper = (const void *)reg_lt; invert = true; break;   /* >= = !< */
    case 3: helper = (const void *)reg_gt; invert = true; break;   /* <= = !> */
    }
    emit_pair_compare(e, pair[idx][0], pair[idx][1], FS_A, helper);
    if (invert) {
        emit_cmp_imm_t2(e, 0, 0);
        emit_hw(e, 0xBF0C);
        emit_mov_lo_imm8(e, 0, 1);
        emit_mov_lo_imm8(e, 0, 0);
    }
    cb_targets_t t;
    if (dd == 0) { t.taken_kind = CB_TAKEN_RTN; t.taken_pc = 0; }
    else         { t.taken_kind = CB_TAKEN_STATIC; t.taken_pc = (pc + sext_nib(dd, 2) + 3) & 0xFFFFFu; }
    t.notaken_pc = (pc + 5) & 0xFFFFFu;
    emit_compare_branch_tail(e, &t);
    return BLK_END_DYN;
}

/* Group 86/87 : ?ST=0/1 n ±dd  (5 nibs). */
static block_step_t translate_group_8_st_test(emit_ctx_t *e, addr_t pc, uint32_t *consumed, bool test_one) {
    int n = fetch_nib(pc + 2) & 0xf;
    uint32_t dd = fetch_k(pc + 3, 2);
    *consumed = 5;
    /* r0 = saturn.pstat[n] */
    emit_ldrb_imm(e, 0, 4, OFS(pstat) + n);
    if (!test_one) {
        /* cond = (pstat[n] == 0) */
        emit_cmp_imm_t2(e, 0, 0);
        emit_hw(e, 0xBF0C);
        emit_mov_lo_imm8(e, 0, 1);
        emit_mov_lo_imm8(e, 0, 0);
    } else {
        /* cond = (pstat[n] != 0)  →  r0 already 0 or 1; force normalize. */
        emit_cmp_imm_t2(e, 0, 0);
        emit_hw(e, 0xBF14);              /* ITE NE */
        emit_mov_lo_imm8(e, 0, 1);
        emit_mov_lo_imm8(e, 0, 0);
    }
    cb_targets_t t;
    if (dd == 0) { t.taken_kind = CB_TAKEN_RTN; t.taken_pc = 0; }
    else         { t.taken_kind = CB_TAKEN_STATIC; t.taken_pc = (pc + sext_nib(dd, 2) + 3) & 0xFFFFFu; }
    t.notaken_pc = (pc + 5) & 0xFFFFFu;
    emit_compare_branch_tail(e, &t);
    return BLK_END_DYN;
}

/* Group 88/89 : ?P#n / ?P=n  ±dd  (5 nibs). */
static block_step_t translate_group_8_p_test(emit_ctx_t *e, addr_t pc, uint32_t *consumed, bool test_eq) {
    int n = fetch_nib(pc + 2) & 0xf;
    uint32_t dd = fetch_k(pc + 3, 2);
    *consumed = 5;
    emit_ldrb_imm(e, 0, 4, OFS(p));
    emit_cmp_imm_t2(e, 0, n);
    emit_hw(e, test_eq ? 0xBF0C : 0xBF14);   /* ITE EQ vs ITE NE */
    emit_mov_lo_imm8(e, 0, 1);
    emit_mov_lo_imm8(e, 0, 0);
    cb_targets_t t;
    if (dd == 0) { t.taken_kind = CB_TAKEN_RTN; t.taken_pc = 0; }
    else         { t.taken_kind = CB_TAKEN_STATIC; t.taken_pc = (pc + sext_nib(dd, 2) + 3) & 0xFFFFFu; }
    t.notaken_pc = (pc + 5) & 0xFFFFFu;
    emit_compare_branch_tail(e, &t);
    return BLK_END_DYN;
}

/* Group 9 : compare+branch over arbitrary field (5 nibs). Form: 9 fs op dd. */
static block_step_t translate_group_9(emit_ctx_t *e, addr_t pc, uint32_t *consumed) {
    int fs = fetch_nib(pc + 1);
    int op = fetch_nib(pc + 2);
    uint32_t dd = fetch_k(pc + 3, 2);
    int field = fs & 7;
    *consumed = 5;
    /* Variable field with P-dependent boundaries means the field-arg to
     * the helper must be runtime, not folded. The helpers handle it. */
    if (fs < 8) {
        /* equality / zero, same subtable as 8A */
        static const uint8_t pair[8][2] = {
            {REG_A,REG_B},{REG_B,REG_C},{REG_A,REG_C},{REG_C,REG_D},
            {REG_A,REG_B},{REG_B,REG_C},{REG_A,REG_C},{REG_C,REG_D},
        };
        if (op < 8) {
            emit_pair_compare(e, pair[op & 7][0], pair[op & 7][1], field, (const void *)reg_eq);
            if (op >= 4) {
                emit_cmp_imm_t2(e, 0, 0);
                emit_hw(e, 0xBF0C); emit_mov_lo_imm8(e, 0, 1); emit_mov_lo_imm8(e, 0, 0);
            }
        } else {
            int r = op & 3;
            emit_zero_test(e, r, field, (const void *)reg_is_zero);
            if (op >= 0xC) {
                emit_cmp_imm_t2(e, 0, 0);
                emit_hw(e, 0xBF0C); emit_mov_lo_imm8(e, 0, 1); emit_mov_lo_imm8(e, 0, 0);
            }
        }
    } else {
        static const uint8_t pair[4][2] = {
            {REG_A,REG_B},{REG_B,REG_C},{REG_C,REG_A},{REG_D,REG_C},
        };
        const void *helper = NULL; bool invert = false;
        switch ((op >> 2) & 3) {
        case 0: helper = (const void *)reg_gt; break;
        case 1: helper = (const void *)reg_lt; break;
        case 2: helper = (const void *)reg_lt; invert = true; break;
        case 3: helper = (const void *)reg_gt; invert = true; break;
        }
        emit_pair_compare(e, pair[op & 3][0], pair[op & 3][1], field, helper);
        if (invert) {
            emit_cmp_imm_t2(e, 0, 0);
            emit_hw(e, 0xBF0C); emit_mov_lo_imm8(e, 0, 1); emit_mov_lo_imm8(e, 0, 0);
        }
    }
    cb_targets_t t;
    if (dd == 0) { t.taken_kind = CB_TAKEN_RTN; t.taken_pc = 0; }
    else         { t.taken_kind = CB_TAKEN_STATIC; t.taken_pc = (pc + sext_nib(dd, 2) + 3) & 0xFFFFFu; }
    t.notaken_pc = (pc + 5) & 0xFFFFFu;
    emit_compare_branch_tail(e, &t);
    return BLK_END_DYN;
}

/* Group 4/5 : GOC/GONC ±dd (3 nibs). dd=0 → RTN. */
static block_step_t translate_group_4_or_5(emit_ctx_t *e, addr_t pc, uint32_t *consumed, bool branch_if_carry) {
    uint32_t dd = fetch_k(pc + 1, 2);
    *consumed = 3;
    /* If a prior inline arith left carry in r2 (DEFER_CARRY), flush
     * to memory now so the ldrb below reads the up-to-date value. */
    emit_flush_carry(e);
    /* Load carry into r0 (it's already a 0/1 byte). */
    emit_ldrb_imm(e, 0, 4, OFS(carry));
    if (!branch_if_carry) {
        /* invert */
        emit_cmp_imm_t2(e, 0, 0);
        emit_hw(e, 0xBF0C);
        emit_mov_lo_imm8(e, 0, 1);
        emit_mov_lo_imm8(e, 0, 0);
    }
    cb_targets_t t;
    if (dd == 0) { t.taken_kind = CB_TAKEN_RTN; t.taken_pc = 0; }
    else         { t.taken_kind = CB_TAKEN_STATIC; t.taken_pc = (pc + sext_nib(dd, 2) + 1) & 0xFFFFFu; }
    t.notaken_pc = (pc + 3) & 0xFFFFFu;
    /* GOC/GONC don't touch saturn.carry — but our common tail rewrites
     * it from the condition. Save+restore would be more correct; for
     * now match the (slight) interpreter difference. Actually the ISA
     * says GOC/GONC don't modify carry, and the interpreter agrees —
     * so we should NOT have emit_compare_branch_tail rewrite carry.
     *
     * Quick fix: backup carry before the tail and restore after. But
     * the tail's r0=0/r0=1 writes occur unconditionally. Cheapest
     * solution: read carry into r2 first; tail writes r0; we re-store
     * r2 to carry at the very end. We'll do it inside the emission. */
    /* Save old carry to r2 (callee-save? no, r2 is scratch). Use r5
     * (callee-save). Need to push/pop. */
    /* Actually simplest: the tail unconditionally writes carry = cond.
     * For GOC/GONC, cond == old carry (after the optional invert), so
     * writing it back is fine for GOC. For GONC the inverted value
     * would overwrite carry, which is wrong. Easiest workaround: for
     * GONC, after the tail rewrites carry, set it back to the (old
     * non-inverted) value. We can read+save carry before the test,
     * then store it after — but that's awkward because tail already
     * exits the block.
     *
     * Cleanest: bypass the tail for GOC/GONC and emit a slimmed
     * version that doesn't touch carry. */
    (void)t;
    /* The above explanation got long; the implementation below avoids
     * the tail's carry-write by doing the dispatch directly. */
    /* Reload r0 with condition (may have been clobbered by ITE above
     * — actually no, the ITE-result is in r0). */
    emit_cmp_imm_t2(e, 0, 0);
    uint32_t br = emit_b_w_placeholder(e, 0);     /* EQ */
    /* taken */
    emit_branch_counter(e, OFS(saturn_branches_taken));
    if (t.taken_kind == CB_TAKEN_RTN) {
        emit_inline_rstk_pop(e);
    } else {
        emit_set_r0_pc(e, t.taken_pc);
    }
    uint32_t br_end = emit_b_w_placeholder(e, -1);
    /* notaken */
    uint32_t L_nt = e->pos;
    emit_branch_counter(e, OFS(saturn_branches_skipped));
    emit_set_r0_pc(e, t.notaken_pc);
    uint32_t L_end = e->pos;
    emit_patch_b_w(e, br, L_nt);
    emit_patch_b_w(e, br_end, L_end);
    return BLK_END_DYN;
}

/* Group 6 : GOTO ±ddd. Resolved at translate time → block ends. */
static block_step_t translate_group_6(emit_ctx_t *e, addr_t pc, addr_t *out_next) {
    uint32_t ddd = fetch_k(pc + 1, 3);
    if (ddd == 3) { *out_next = (pc + 4) & 0xFFFFFu; return BLK_END; }
    if (ddd == 4) { *out_next = (pc + 5) & 0xFFFFFu; return BLK_END; }
    int32_t signed_ddd = sext_nib(ddd, 3);
    *out_next = (pc + signed_ddd + 1) & 0xFFFFFu;
    /* bump branch counter */
    emit_ldr_imm(e, 0, 4, OFS(saturn_branches_taken));
    emit_add_imm_t3_small(e, 0, 0, 1);
    emit_str_imm(e, 0, 4, OFS(saturn_branches_taken));
    (void)e;
    return BLK_END;
}

/* ---------------- Top-level translator ---------------- */

jit_block_fn_t saturn_jit_translate(addr_t start_pc,
                                    void *out_buf, uint32_t out_cap,
                                    uint32_t *out_used,
                                    jit_block_meta_t *meta) {
    return saturn_jit_translate_linked(start_pc, out_buf, out_cap,
                                       out_used, meta, NULL);
}

jit_block_fn_t saturn_jit_translate_linked(addr_t start_pc,
                                           void *out_buf, uint32_t out_cap,
                                           uint32_t *out_used,
                                           jit_block_meta_t *meta,
                                           uintptr_t *link_target) {
    emit_ctx_t e;
    emit_init(&e, out_buf, out_cap);
    s_carry_dirty_r2 = false;

    /* Prologue. With HOIST, also save r5/r6 and pre-load budget/ops. */
#if JIT_OPT_HOIST_BUDGET_OPS
    emit_push(&e, (1 << 4) | (1 << 5) | (1 << 6) | (1 << 14)); /* push {r4-r6, lr} */
    emit_mov_any(&e, 4, 0);                                    /* mov r4, r0 */
    emit_ldr_imm(&e, 5, 4, OFS(budget_remaining));             /* r5 = budget */
    emit_ldr_imm(&e, 6, 4, OFS(saturn_ops));                   /* r6 = saturn_ops */
#else
    emit_push(&e, (1 << 4) | (1 << 14));         /* push {r4, lr} */
    emit_mov_any(&e, 4, 0);                      /* mov r4, r0 (saturn*) */
#endif

    /* body_off_hw = halfword index where body actually starts (after
     * prologue). Chained-from-another-block entries skip prologue. */
    uint16_t body_off_hw = (uint16_t)e.pos;

    addr_t pc = start_pc;
    uint32_t ops = 0;
    addr_t next_pc = 0;
    bool have_next = false;

    bool dyn_end = false;     /* true if next_pc is already in r0 at exit */

    while (!e.overflow) {
        nibble_t n0 = fetch_nib(pc);
        block_step_t s = BLK_UNSUPP;
        addr_t pc_after = pc;
        uint32_t consumed = 0;

        switch (n0) {
        case 0x0: s = translate_group_0(&e, pc, &next_pc); pc_after = pc + ((fetch_nib(pc+1) == 0xE) ? 4 : 2);
                  have_next = (s == BLK_END || s == BLK_END_DYN);
                  break;
        case 0x1: s = translate_group_1(&e, pc, &consumed); pc_after = pc + consumed; break;
        case 0x2: s = translate_group_2(&e, pc); pc_after = pc + 2; break;
        case 0x3: s = translate_group_3(&e, pc, &consumed); pc_after = pc + consumed; break;
        case 0x4: s = translate_group_4_or_5(&e, pc, &consumed, true); pc_after = pc;
                  break;       /* GOC */
        case 0x5: s = translate_group_4_or_5(&e, pc, &consumed, false); pc_after = pc;
                  break;       /* GONC */
        case 0x6: s = translate_group_6(&e, pc, &next_pc); pc_after = pc;
                  have_next = (s == BLK_END);
                  break;
        case 0x8: {
            int n1 = fetch_nib(pc + 1);
            if (n1 >= 0xC && n1 <= 0xF) {
                s = translate_group_8_branch(&e, pc, &next_pc, &consumed);
                pc_after = pc;
                have_next = (s == BLK_END);
            } else if (n1 == 0x6 || n1 == 0x7) {
                s = translate_group_8_st_test(&e, pc, &consumed, n1 == 0x7);
                pc_after = pc;
            } else if (n1 == 0x8 || n1 == 0x9) {
                s = translate_group_8_p_test(&e, pc, &consumed, n1 == 0x9);
                pc_after = pc;
            } else if (n1 == 0xA) {
                s = translate_group_8A(&e, pc, &consumed);
                pc_after = pc;
            } else if (n1 == 0xB) {
                s = translate_group_8B(&e, pc, &consumed);
                pc_after = pc;
            } else {
                s = BLK_UNSUPP;
            }
            break;
        }
        case 0x9: s = translate_group_9(&e, pc, &consumed); pc_after = pc; break;
        case 0xA: s = translate_group_a(&e, pc); pc_after = pc + 3; break;
        case 0xB: s = translate_group_b(&e, pc); pc_after = pc + 3; break;
        case 0xC: s = translate_group_c(&e, pc); pc_after = pc + 2; break;
        case 0xD: s = translate_group_d(&e, pc); pc_after = pc + 2; break;
        case 0xE: s = translate_group_e(&e, pc); pc_after = pc + 2; break;
        case 0xF: s = translate_group_f(&e, pc); pc_after = pc + 2; break;
        default:  s = BLK_UNSUPP; break;
        }

        if (s == BLK_UNSUPP) {
            if (!have_next) { next_pc = pc; have_next = true; }
            break;
        }
        ops++;
        if (s == BLK_END_DYN) { dyn_end = true; break; }
        if (s == BLK_END) break;
        pc = pc_after;
    }

    if (!have_next) { next_pc = pc; }

    /* For dyn-end blocks (RTN), r0 already holds the next PC. We must
     * bump the counter *without* clobbering r0 — use r1/r2 as scratch
     * and put r0 back before the pop. Cheapest fix: bump counter then
     * reload r0 from a saved copy. For now we just do a careful bump
     * that uses r1/r2 only.
     *
     * Sketch:
     *   ldr  r1, [r4, #ofs_ops]
     *   adds r1, r1, #N
     *   str  r1, [r4, #ofs_ops]
     *   pop  {r4, pc}     // r0 = the popped PC we set earlier
     */
    /* Flush any pending carry held in r2 from inline arith. */
    emit_flush_carry(&e);
    if (dyn_end) {
#if JIT_OPT_BUDGET_DRIVEN_OPS
        /* Dispatcher derives saturn_ops += executed from the budget
         * delta, so the JIT just needs to subtract ops from budget. r0
         * holds the dynamic next-PC and must survive — use r1/r2. */
        if (ops != 0) {
            emit_ldr_imm(&e, 1, 4, OFS(budget_remaining));
            if (ops <= 0xff)      emit_subs_lo_imm8(&e, 1, (uint8_t)ops);
            else if (ops <= 0xfff) emit_sub_imm_t3_small(&e, 1, 1, (uint16_t)ops);
            else { emit_mov_imm32(&e, 2, ops);
                   uint32_t hi = 0xEBA0 | 1;
                   uint32_t lo = (0 << 12) | (1 << 8) | 2;
                   emit_w32(&e, (hi << 16) | lo); }
            emit_str_imm(&e, 1, 4, OFS(budget_remaining));
        }
#elif !JIT_OPT_OPS_COUNTER_IN_C
        if (ops != 0) {
#if JIT_OPT_HOIST_BUDGET_OPS
            if (ops <= 0xff)      emit_adds_lo_imm8(&e, 6, (uint8_t)ops);
            else if (ops <= 0xfff) emit_add_imm_t3_small(&e, 6, 6, (uint16_t)ops);
            else { emit_mov_imm32(&e, 1, ops);
                   uint32_t hi = 0xEB00 | 6;
                   uint32_t lo = (0 << 12) | (6 << 8) | 1;
                   emit_w32(&e, (hi << 16) | lo); }
#else
            emit_ldr_imm(&e, 1, 4, OFS(saturn_ops));
            if (ops <= 0xfff) emit_add_imm_t3_small(&e, 1, 1, (uint16_t)ops);
            else { emit_mov_imm32(&e, 2, ops);
                   uint32_t hi = 0xEB00 | 1;
                   uint32_t lo = (0 << 12) | (1 << 8) | 2;
                   emit_w32(&e, (hi << 16) | lo); }
            emit_str_imm(&e, 1, 4, OFS(saturn_ops));
#endif
        }
#endif
        emit_block_exit_pc_in_r0(&e);
    } else {
#if JIT_OPT_BLOCK_LINK
        if (link_target) {
            /* Linked tail. Emits:
             *   ; saturn_ops += ops
             *   ldr r0, [r4, #ofs_ops]
             *   add r0, r0, #ops
             *   str r0, [r4, #ofs_ops]
             *   ; r0 = next_pc (so dispatcher exit path has it)
             *   movw r0, #lo_next_pc ; movt r0, #hi_next_pc (if needed)
             *   ; budget -= ops
             *   ldr r1, [r4, #ofs_budget]
             *   sub r1, r1, #ops
             *   str r1, [r4, #ofs_budget]
             *   bmi local_exit
             *   ; chain through link target word in cache slot
             *   movw r2, #lo(link_target_addr)
             *   movt r2, #hi(link_target_addr)
             *   ldr  r2, [r2]
             *   bx   r2
             *   local_exit: pop {r4, pc} */
#if JIT_OPT_HOIST_BUDGET_OPS
            /* Hoisted bookkeeping: r5 = budget_remaining, r6 = saturn_ops.
             * No ldr/str per chained iter, just adds/subs in registers.
             * Local_exit flushes back to saturn struct before pop. */
            if (ops != 0) {
                if (ops <= 0xff) {
                    emit_adds_lo_imm8(&e, 6, (uint8_t)ops);   /* r6 += ops */
                } else if (ops <= 0xfff) {
                    emit_add_imm_t3_small(&e, 6, 6, (uint16_t)ops);
                } else {
                    emit_mov_imm32(&e, 0, ops);
                    uint32_t hi = 0xEB00 | 6;
                    uint32_t lo = (0 << 12) | (6 << 8) | 0;
                    emit_w32(&e, (hi << 16) | lo);
                }
            }
            emit_mov_imm32(&e, 0, next_pc & 0xFFFFFu);
            if (ops != 0) {
                if (ops <= 0xff) {
                    emit_subs_lo_imm8(&e, 5, (uint8_t)ops);   /* r5 -= ops, sets flags */
                } else if (ops <= 0xfff) {
                    emit_sub_imm_t3_small(&e, 5, 5, (uint16_t)ops);
                } else {
                    emit_mov_imm32(&e, 2, ops);
                    /* subs.w r5, r5, r2 (T3 S=1). hi=0xEBB0|Rn=5, lo=Rd=5,Rm=2 */
                    uint32_t hi = 0xEBB0 | 5;
                    uint32_t lo = (0 << 12) | (5 << 8) | 2;
                    emit_w32(&e, (hi << 16) | lo);
                }
                uint32_t br;
                if (ops > 0xff && ops <= 0xfff) {
                    emit_cmp_imm_t2(&e, 5, 0);
                }
                br = emit_b_w_placeholder(&e, 0xB);           /* blt local_exit */
                /* chain: load patchable link_target word and branch */
                emit_mov_imm32(&e, 2, (uint32_t)(uintptr_t)link_target);
                emit_ldr_imm(&e, 2, 2, 0);
                emit_bx(&e, 2);
                uint32_t local_exit_pos = e.pos;
                emit_patch_b_w(&e, br, local_exit_pos);
            } else {
                /* No ops, always chain (block was an empty no-op). */
                emit_mov_imm32(&e, 2, (uint32_t)(uintptr_t)link_target);
                emit_ldr_imm(&e, 2, 2, 0);
                emit_bx(&e, 2);
            }
            /* local_exit: flush r5/r6, then pop {r4-r6, pc}. */
            emit_str_imm(&e, 5, 4, OFS(budget_remaining));
            emit_str_imm(&e, 6, 4, OFS(saturn_ops));
            emit_hw(&e, EPILOGUE_POP_OP);
#else
#if !JIT_OPT_BUDGET_DRIVEN_OPS
            if (ops != 0) {
                emit_ldr_imm(&e, 0, 4, OFS(saturn_ops));
                if (ops <= 0xff)      emit_adds_lo_imm8(&e, 0, (uint8_t)ops);
                else if (ops <= 0xfff) emit_add_imm_t3_small(&e, 0, 0, (uint16_t)ops);
                else { emit_mov_imm32(&e, 1, ops);
                       uint32_t hi = 0xEB00 | 0;
                       uint32_t lo = (0 << 12) | (0 << 8) | 1;
                       emit_w32(&e, (hi << 16) | lo); }
                emit_str_imm(&e, 0, 4, OFS(saturn_ops));
            }
#endif
            emit_mov_imm32(&e, 0, next_pc & 0xFFFFFu);
            if (ops != 0) {
                emit_ldr_imm(&e, 1, 4, OFS(budget_remaining));
                if (ops <= 0xff) {
                    emit_subs_lo_imm8(&e, 1, (uint8_t)ops);
                } else if (ops <= 0xfff) {
                    emit_sub_imm_t3_small(&e, 1, 1, (uint16_t)ops);
                } else {
                    emit_mov_imm32(&e, 2, ops);
                    uint32_t hi = 0xEBB0 | 1;
                    uint32_t lo = (0 << 12) | (1 << 8) | 2;
                    emit_w32(&e, (hi << 16) | lo);
                }
                emit_str_imm(&e, 1, 4, OFS(budget_remaining));
            }
            {
#if JIT_OPT_SELF_LOOP_DIRECT_BRANCH
                bool self_loop = (next_pc == start_pc);
#else
                bool self_loop = false;
#endif
                if (ops != 0) {
                    uint32_t br;
                    if (ops > 0xff && ops <= 0xfff) {
                        emit_cmp_imm_t2(&e, 1, 0);
                    }
                    br = emit_b_w_placeholder(&e, 0xB);
                    if (self_loop) {
                        /* Branch directly back to body start — no
                         * patchable link target needed for self loops. */
                        uint32_t b = emit_b_w_placeholder(&e, -1);
                        emit_patch_b_w(&e, b, body_off_hw);
                    } else {
                        emit_mov_imm32(&e, 2, (uint32_t)(uintptr_t)link_target);
                        emit_ldr_imm(&e, 2, 2, 0);
                        emit_bx(&e, 2);
                    }
                    uint32_t local_exit_pos = e.pos;
                    emit_patch_b_w(&e, br, local_exit_pos);
                } else {
                    if (self_loop) {
                        uint32_t b = emit_b_w_placeholder(&e, -1);
                        emit_patch_b_w(&e, b, body_off_hw);
                    } else {
                        emit_mov_imm32(&e, 2, (uint32_t)(uintptr_t)link_target);
                        emit_ldr_imm(&e, 2, 2, 0);
                        emit_bx(&e, 2);
                    }
                }
            }
            emit_hw(&e, EPILOGUE_POP_OP);
#endif
        } else {
            emit_ops_counter_bump(&e, ops);
            emit_block_exit_with_pc(&e, next_pc);
        }
#else
        emit_ops_counter_bump(&e, ops);
        emit_block_exit_with_pc(&e, next_pc);
#endif
    }
    emit_finalize(&e);

    if (e.overflow) {
        if (out_used) *out_used = 0;
        return NULL;
    }

    if (meta) {
        meta->start_pc = start_pc;
        meta->saturn_nibs = (next_pc >= start_pc) ? (next_pc - start_pc) : 0;
        meta->code_bytes = emit_bytes_used(&e);
        meta->saturn_ops = ops;
        meta->body_off_hw = body_off_hw;
        meta->static_next_pc = dyn_end ? JIT_DYN_NEXT_PC : next_pc;
    }
    if (out_used) *out_used = emit_bytes_used(&e);

    /* Function pointer = buffer | Thumb bit. */
    return (jit_block_fn_t)((uintptr_t)out_buf | 1u);
}
