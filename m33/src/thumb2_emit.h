/* Thumb-2 instruction emitter for Cortex-M33 (ARMv8-M Mainline).
 *
 * The emitter writes into a caller-provided code buffer aligned to
 * 4 bytes. Each function appends one instruction. We use the
 * minimum-width encoding when both 16-bit and 32-bit forms are
 * available and our register/imm constraints fit. All instructions
 * are little-endian halfword pairs.
 *
 * Register allocation convention (used by saturn_jit.c):
 *   r4..r7   — Saturn register file pointers (A,B,C,D pinned)
 *   r8       — &saturn (state pointer)
 *   r9       — current Saturn PC (lives in a CPU reg between blocks)
 *   r10/r11  — scratch (caller-save inside a block)
 *   r0..r3   — argument/return regs for helper calls (registers.c)
 *   lr       — return to dispatcher
 *
 * The dispatcher hands every translated block:
 *   r8 = &saturn
 *   lr = dispatcher_return
 * and expects the block to return the next Saturn PC in r0.
 */

#ifndef THUMB2_EMIT_H
#define THUMB2_EMIT_H

#include <stdint.h>
#include <stdbool.h>

typedef struct emit_ctx_s {
    uint16_t *buf;
    uint32_t  cap_hwords;
    uint32_t  pos;        /* halfword index */
    bool      overflow;
} emit_ctx_t;

void emit_init(emit_ctx_t *e, void *buf, uint32_t bytes);
uint32_t emit_bytes_used(const emit_ctx_t *e);
void     emit_finalize(emit_ctx_t *e);  /* flush cache lines */

/* ---- raw halfword/wordword writes (escape hatch) ---- */
void emit_hw(emit_ctx_t *e, uint16_t hw);
void emit_w32(emit_ctx_t *e, uint32_t w);   /* writes [lo, hi] halfword pair */

/* ---- 16-bit Thumb (low-reg, common forms) ---- */
void emit_mov_lo_imm8(emit_ctx_t *e, int rd, uint8_t imm8);             /* movs Rd, #imm8 */
void emit_movs_lo(emit_ctx_t *e, int rd, int rm);                       /* movs Rd, Rm (low regs) */
void emit_mov_any(emit_ctx_t *e, int rd, int rm);                       /* mov Rd, Rm (any reg) */
void emit_adds_lo_lo_imm3(emit_ctx_t *e, int rd, int rn, uint8_t imm3); /* adds Rd, Rn, #imm3 */
void emit_subs_lo_lo_imm3(emit_ctx_t *e, int rd, int rn, uint8_t imm3);
void emit_adds_lo_imm8(emit_ctx_t *e, int rd, uint8_t imm8);
void emit_subs_lo_imm8(emit_ctx_t *e, int rd, uint8_t imm8);
void emit_ldrb_lo(emit_ctx_t *e, int rt, int rn, uint8_t imm5);         /* ldrb Rt,[Rn,#imm5] */
void emit_strb_lo(emit_ctx_t *e, int rt, int rn, uint8_t imm5);
void emit_ldr_lo_pc(emit_ctx_t *e, int rt, uint8_t imm8);               /* ldr Rt,[pc,#imm8*4] */
void emit_bx(emit_ctx_t *e, int rm);                                    /* bx Rm */
void emit_blx(emit_ctx_t *e, int rm);                                   /* blx Rm */
void emit_push(emit_ctx_t *e, uint16_t reglist);                        /* push {regs}  (low + lr) */
void emit_pop(emit_ctx_t *e, uint16_t reglist);                         /* pop {regs}   (low + pc) */
void emit_nop(emit_ctx_t *e);

/* ---- 32-bit Thumb-2 ---- */
/* movw Rd, #imm16 ; movt Rd, #imm16 (any reg 0..14) */
void emit_movw(emit_ctx_t *e, int rd, uint16_t imm16);
void emit_movt(emit_ctx_t *e, int rd, uint16_t imm16);
/* compose a full 32-bit constant into Rd via movw+movt. */
void emit_mov_imm32(emit_ctx_t *e, int rd, uint32_t v);

/* ldr/str (immediate, T3 — 12-bit imm, positive). */
void emit_ldr_imm(emit_ctx_t *e, int rt, int rn, uint16_t imm12);
void emit_str_imm(emit_ctx_t *e, int rt, int rn, uint16_t imm12);
void emit_ldrb_imm(emit_ctx_t *e, int rt, int rn, uint16_t imm12);
void emit_strb_imm(emit_ctx_t *e, int rt, int rn, uint16_t imm12);

/* T3 wide encodings of arithmetic with 12-bit modified-immediate.
 * Limited to values reachable in the simple form (imm < 256 here;
 * caller is expected to materialise larger via movw). */
void emit_add_imm_t3_small(emit_ctx_t *e, int rd, int rn, uint16_t imm12);
void emit_sub_imm_t3_small(emit_ctx_t *e, int rd, int rn, uint16_t imm12);
void emit_cmp_imm_t2(emit_ctx_t *e, int rn, uint16_t imm12_small);

/* and/orr/eor (register), no shift */
void emit_and_reg(emit_ctx_t *e, int rd, int rn, int rm);
void emit_orr_reg(emit_ctx_t *e, int rd, int rn, int rm);
void emit_eor_reg(emit_ctx_t *e, int rd, int rn, int rm);

/* Generic 32-bit branch (B.W). cond=-1 → unconditional. Returns the
 * halfword position of the branch so the caller can patch the
 * displacement later (forward branch). */
uint32_t emit_b_w_placeholder(emit_ctx_t *e, int cond);
void     emit_patch_b_w(emit_ctx_t *e, uint32_t hw_idx, uint32_t target_hw_idx);

/* Function-call helpers. */
void emit_bl_to(emit_ctx_t *e, const void *target);
void emit_ret(emit_ctx_t *e);     /* bx lr */

#endif
