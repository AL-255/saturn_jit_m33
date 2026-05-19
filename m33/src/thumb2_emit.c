/* Thumb-2 emitter implementation.
 *
 * Encodings sourced from the Armv8-M Architecture Reference Manual.
 * Cortex-M33 runs Thumb-only, so all branches stay within Thumb mode
 * (set bit 0 of any branch target to indicate Thumb to BX/BLX).
 *
 * Cache maintenance: M33 typically has no separate I-cache for the
 * processor itself (the Cortex-M33 has optional caches but the
 * mps2-an505 model in QEMU does not enable them). We still emit a
 * DSB+ISB in emit_finalize so the emitter is correct on real hardware
 * that does cache instructions. */

#include "thumb2_emit.h"
#include <string.h>

void emit_init(emit_ctx_t *e, void *buf, uint32_t bytes) {
    e->buf = (uint16_t *)buf;
    e->cap_hwords = bytes / 2;
    e->pos = 0;
    e->overflow = false;
}
uint32_t emit_bytes_used(const emit_ctx_t *e) { return e->pos * 2; }

void emit_finalize(emit_ctx_t *e) {
    (void)e;
    __asm__ volatile ("dsb ish; isb" ::: "memory");
}

void emit_hw(emit_ctx_t *e, uint16_t hw) {
    if (e->pos >= e->cap_hwords) { e->overflow = true; return; }
    e->buf[e->pos++] = hw;
}
void emit_w32(emit_ctx_t *e, uint32_t w) {
    emit_hw(e, (uint16_t)(w >> 16));     /* T32 high halfword first */
    emit_hw(e, (uint16_t) w);
}

/* ----------- 16-bit Thumb common forms ----------- */

void emit_mov_lo_imm8(emit_ctx_t *e, int rd, uint8_t imm8) {
    /* MOVS Rd, #imm8  : 0010 0 Rd[2:0] imm8 */
    emit_hw(e, 0x2000 | ((rd & 7) << 8) | imm8);
}
void emit_movs_lo(emit_ctx_t *e, int rd, int rm) {
    /* MOVS Rd, Rm (low regs) : 0000 0000 00 Rm Rd  (LSL imm5=0) */
    emit_hw(e, 0x0000 | ((rm & 7) << 3) | (rd & 7));
}
void emit_mov_any(emit_ctx_t *e, int rd, int rm) {
    /* MOV Rd, Rm (T1) : 0100 0110 D Rm[3:0] Rd[2:0]  where D=Rd[3] */
    uint16_t op = 0x4600;
    op |= ((rd & 8) << 4);       /* D */
    op |= ((rm & 0xf) << 3);
    op |= (rd & 7);
    emit_hw(e, op);
}
void emit_adds_lo_lo_imm3(emit_ctx_t *e, int rd, int rn, uint8_t imm3) {
    /* 0001 110 imm3 Rn Rd */
    emit_hw(e, 0x1C00 | ((imm3 & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
}
void emit_subs_lo_lo_imm3(emit_ctx_t *e, int rd, int rn, uint8_t imm3) {
    emit_hw(e, 0x1E00 | ((imm3 & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
}
void emit_adds_lo_imm8(emit_ctx_t *e, int rd, uint8_t imm8) {
    /* 0011 0 Rd imm8 */
    emit_hw(e, 0x3000 | ((rd & 7) << 8) | imm8);
}
void emit_subs_lo_imm8(emit_ctx_t *e, int rd, uint8_t imm8) {
    /* 0011 1 Rd imm8 */
    emit_hw(e, 0x3800 | ((rd & 7) << 8) | imm8);
}
void emit_ldrb_lo(emit_ctx_t *e, int rt, int rn, uint8_t imm5) {
    /* 0111 1 imm5 Rn Rt */
    emit_hw(e, 0x7800 | ((imm5 & 0x1f) << 6) | ((rn & 7) << 3) | (rt & 7));
}
void emit_strb_lo(emit_ctx_t *e, int rt, int rn, uint8_t imm5) {
    /* 0111 0 imm5 Rn Rt */
    emit_hw(e, 0x7000 | ((imm5 & 0x1f) << 6) | ((rn & 7) << 3) | (rt & 7));
}
void emit_ldr_lo_pc(emit_ctx_t *e, int rt, uint8_t imm8) {
    /* 0100 1 Rt imm8  (offset = imm8 * 4 from (PC+4) & ~3) */
    emit_hw(e, 0x4800 | ((rt & 7) << 8) | imm8);
}
void emit_bx(emit_ctx_t *e, int rm) {
    /* 0100 0111 0 Rm[3:0] 000 */
    emit_hw(e, 0x4700 | ((rm & 0xf) << 3));
}
void emit_blx(emit_ctx_t *e, int rm) {
    /* 0100 0111 1 Rm[3:0] 000 */
    emit_hw(e, 0x4780 | ((rm & 0xf) << 3));
}
void emit_push(emit_ctx_t *e, uint16_t reglist) {
    /* 1011 010 M reglist[7:0]   (M=1 → include LR) */
    uint16_t M = (reglist >> 14) & 1;
    emit_hw(e, 0xB400 | (M << 8) | (reglist & 0xff));
}
void emit_pop(emit_ctx_t *e, uint16_t reglist) {
    /* 1011 110 P reglist[7:0]   (P=1 → include PC) */
    uint16_t P = (reglist >> 15) & 1;
    emit_hw(e, 0xBC00 | (P << 8) | (reglist & 0xff));
}
void emit_nop(emit_ctx_t *e) { emit_hw(e, 0xBF00); }

/* ----------- 32-bit Thumb-2 forms ----------- */

/* MOVW Rd, #imm16 T3 :
 *   1111 0 i 10 0100  imm4 | 0 imm3 Rd imm8
 * imm16 = imm4:i:imm3:imm8
 */
void emit_movw(emit_ctx_t *e, int rd, uint16_t imm16) {
    uint32_t i    = (imm16 >> 11) & 1;
    uint32_t imm4 = (imm16 >> 12) & 0xf;
    uint32_t imm3 = (imm16 >> 8)  & 7;
    uint32_t imm8 =  imm16        & 0xff;
    uint32_t hi = 0xF240 | (i << 10) | imm4;
    uint32_t lo = (imm3 << 12) | ((rd & 0xf) << 8) | imm8;
    emit_w32(e, (hi << 16) | lo);
}
void emit_movt(emit_ctx_t *e, int rd, uint16_t imm16) {
    uint32_t i    = (imm16 >> 11) & 1;
    uint32_t imm4 = (imm16 >> 12) & 0xf;
    uint32_t imm3 = (imm16 >> 8)  & 7;
    uint32_t imm8 =  imm16        & 0xff;
    uint32_t hi = 0xF2C0 | (i << 10) | imm4;
    uint32_t lo = (imm3 << 12) | ((rd & 0xf) << 8) | imm8;
    emit_w32(e, (hi << 16) | lo);
}
void emit_mov_imm32(emit_ctx_t *e, int rd, uint32_t v) {
    emit_movw(e, rd, v & 0xffff);
    if ((v >> 16) != 0) emit_movt(e, rd, (v >> 16) & 0xffff);
}

/* LDR (immediate, T3) : 1111 1000 1101 Rn | Rt imm12  (offset, no wb) */
void emit_ldr_imm(emit_ctx_t *e, int rt, int rn, uint16_t imm12) {
    uint32_t hi = 0xF8D0 | (rn & 0xf);
    uint32_t lo = ((rt & 0xf) << 12) | (imm12 & 0xfff);
    emit_w32(e, (hi << 16) | lo);
}
/* STR (immediate, T3) : 1111 1000 1100 Rn | Rt imm12 */
void emit_str_imm(emit_ctx_t *e, int rt, int rn, uint16_t imm12) {
    uint32_t hi = 0xF8C0 | (rn & 0xf);
    uint32_t lo = ((rt & 0xf) << 12) | (imm12 & 0xfff);
    emit_w32(e, (hi << 16) | lo);
}
/* LDRB (immediate, T2) : 1111 1000 1001 Rn | Rt imm12 */
void emit_ldrb_imm(emit_ctx_t *e, int rt, int rn, uint16_t imm12) {
    uint32_t hi = 0xF890 | (rn & 0xf);
    uint32_t lo = ((rt & 0xf) << 12) | (imm12 & 0xfff);
    emit_w32(e, (hi << 16) | lo);
}
/* STRB (immediate, T2) : 1111 1000 1000 Rn | Rt imm12 */
void emit_strb_imm(emit_ctx_t *e, int rt, int rn, uint16_t imm12) {
    uint32_t hi = 0xF880 | (rn & 0xf);
    uint32_t lo = ((rt & 0xf) << 12) | (imm12 & 0xfff);
    emit_w32(e, (hi << 16) | lo);
}

/* ADD (immediate, T4) "ADDW Rd,Rn,#imm12" :
 *   1111 0 i 10 0000 Rn | 0 imm3 Rd imm8
 * imm12 = i:imm3:imm8, plain 12-bit unsigned (0..4095). */
void emit_add_imm_t3_small(emit_ctx_t *e, int rd, int rn, uint16_t imm12) {
    uint32_t i    = (imm12 >> 11) & 1;
    uint32_t imm3 = (imm12 >> 8)  & 7;
    uint32_t imm8 =  imm12        & 0xff;
    uint32_t hi = 0xF200 | (i << 10) | (rn & 0xf);
    uint32_t lo = (imm3 << 12) | ((rd & 0xf) << 8) | imm8;
    emit_w32(e, (hi << 16) | lo);
}
/* SUB (immediate, T4) "SUBW Rd,Rn,#imm12" :
 *   1111 0 i 10 1010 Rn | 0 imm3 Rd imm8 */
void emit_sub_imm_t3_small(emit_ctx_t *e, int rd, int rn, uint16_t imm12) {
    uint32_t i    = (imm12 >> 11) & 1;
    uint32_t imm3 = (imm12 >> 8)  & 7;
    uint32_t imm8 =  imm12        & 0xff;
    uint32_t hi = 0xF2A0 | (i << 10) | (rn & 0xf);
    uint32_t lo = (imm3 << 12) | ((rd & 0xf) << 8) | imm8;
    emit_w32(e, (hi << 16) | lo);
}
/* CMP (immediate) T2 : 0010 1 Rn imm8 (low regs, 8-bit immediate). */
void emit_cmp_imm_t2(emit_ctx_t *e, int rn, uint16_t imm12_small) {
    /* Caller guarantees imm12_small fits in 8 bits and rn is low. */
    emit_hw(e, 0x2800 | ((rn & 7) << 8) | (imm12_small & 0xff));
}

/* AND/ORR/EOR (register, T2) "no shift", S=0 :
 *   AND : 1110 1010 0000 Rn | 0000 Rd 0000 Rm
 *   ORR : 1110 1010 0100 Rn | 0000 Rd 0000 Rm
 *   EOR : 1110 1010 1000 Rn | 0000 Rd 0000 Rm
 */
static void emit_data_reg(emit_ctx_t *e, uint16_t hi_base, int rd, int rn, int rm) {
    uint32_t hi = (uint32_t)hi_base | (rn & 0xf);
    uint32_t lo = ((rd & 0xf) << 8) | (rm & 0xf);
    emit_w32(e, (hi << 16) | lo);
}
void emit_and_reg(emit_ctx_t *e, int rd, int rn, int rm) { emit_data_reg(e, 0xEA00, rd, rn, rm); }
void emit_orr_reg(emit_ctx_t *e, int rd, int rn, int rm) { emit_data_reg(e, 0xEA40, rd, rn, rm); }
void emit_eor_reg(emit_ctx_t *e, int rd, int rn, int rm) { emit_data_reg(e, 0xEA80, rd, rn, rm); }

/* B.W (T4 unconditional) :
 *   1111 0 S imm10 | 10 J1 1 J2 imm11
 *   I1 = !(J1 XOR S); I2 = !(J2 XOR S); imm32 = SignExtend(S:I1:I2:imm10:imm11:0)
 * We initially encode displacement = 0 and let emit_patch_b_w fill it in.
 *
 * B<cond>.W (T3) :
 *   1111 0 S cond imm6 | 10 J1 0 J2 imm11
 *   imm32 = SignExtend(S:J2:J1:imm6:imm11:0)
 * We support a handful of common conds; cond=-1 selects T4. */
uint32_t emit_b_w_placeholder(emit_ctx_t *e, int cond) {
    uint32_t hw_pos = e->pos;
    if (cond < 0) {
        /* T4: 11110 S imm10 / 10 J1 1 J2 imm11   J1=J2=1, S=0 */
        emit_hw(e, 0xF000);
        emit_hw(e, 0xB800);
    } else {
        /* T3 conditional. Encode cond in [25:22] of hi halfword. */
        emit_hw(e, 0xF000 | ((cond & 0xf) << 6));
        emit_hw(e, 0x8000);
    }
    return hw_pos;
}

void emit_patch_b_w(emit_ctx_t *e, uint32_t hw_idx, uint32_t target_hw_idx) {
    /* Branch displacement (in halfwords) is target - (hw_idx + 2). The
     * ARM spec measures the branch offset from PC at the branch, which
     * is (branch+4) for a 32-bit instr, i.e. (branch+2 halfwords). */
    int32_t off_hw = (int32_t)target_hw_idx - (int32_t)hw_idx - 2;
    int32_t off    = off_hw * 2;           /* bytes */
    uint16_t hi = e->buf[hw_idx];
    uint16_t lo = e->buf[hw_idx + 1];
    /* Detect T4 vs T3 from the lo halfword: T4 sets bit 12, T3 clears it.
     * Our placeholders are lo=0xB800 (T4) and lo=0x8000 (T3). */
    if (lo & 0x1000) {
        /* T4 unconditional. */
        int32_t i32 = off;
        uint32_t s   = (i32 >> 24) & 1;
        uint32_t imm11 = (i32 >> 1) & 0x7FF;
        uint32_t imm10 = (i32 >> 12) & 0x3FF;
        uint32_t i1   = (i32 >> 23) & 1;
        uint32_t i2   = (i32 >> 22) & 1;
        uint32_t j1   = (~(i1 ^ s)) & 1;
        uint32_t j2   = (~(i2 ^ s)) & 1;
        hi = 0xF000 | (s << 10) | imm10;
        lo = 0x9000 | (j1 << 13) | (1 << 12) | (j2 << 11) | imm11;
    } else {
        /* T3 conditional. cond was preserved in the original hi. */
        uint32_t cond = (hi >> 6) & 0xf;
        int32_t i32 = off;
        uint32_t s   = (i32 >> 20) & 1;
        uint32_t imm11 = (i32 >> 1) & 0x7FF;
        uint32_t imm6  = (i32 >> 12) & 0x3F;
        uint32_t j1   = (i32 >> 19) & 1;
        uint32_t j2   = (i32 >> 18) & 1;
        hi = 0xF000 | (s << 10) | (cond << 6) | imm6;
        lo = 0x8000 | (j1 << 13) | (j2 << 11) | imm11;
    }
    e->buf[hw_idx]     = hi;
    e->buf[hw_idx + 1] = lo;
}

/* BL to absolute target — we materialise the address in r12 (IP) and BLX it.
 * Two reasons over BL.W: (1) on M33 BL has ±16MB range which is fine but
 * we'd need to compute the displacement from final code address, which
 * we don't know during emission; (2) doing it via movw/movt+blx is
 * trivially relocatable. Cost: 3 instructions (movw, movt, blx) = 10 bytes. */
void emit_bl_to(emit_ctx_t *e, const void *target) {
    uintptr_t a = (uintptr_t)target | 1u;     /* Thumb LSB */
    emit_movw(e, 12, a & 0xffff);
    emit_movt(e, 12, (a >> 16) & 0xffff);
    emit_blx(e, 12);
}
void emit_ret(emit_ctx_t *e) { emit_bx(e, 14); }   /* bx lr */
