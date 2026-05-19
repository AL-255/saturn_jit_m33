/* Self-test for the Thumb-2 emitter.
 *
 * We emit a tiny function into a RAM buffer marked executable, call
 * it, and check the result. This guards every emit_* primitive that
 * matters for the JIT translator.
 *
 * Each test returns 0 on success or a nonzero numeric error code that
 * gets printed by main, so a failure is recognisable in the QEMU
 * output without bringing in a printf.
 *
 * Tests run in two phases:
 *   1. encode-and-disasm — purely a compile-time / data check that
 *      certain instructions encode to the exact halfwords arm-as would
 *      assemble for the same mnemonic. This catches encoding bugs
 *      regardless of whether the CPU likes the bytes.
 *   2. encode-and-execute — emit a function, call it, check the
 *      returned value. This catches semantic bugs and confirms the
 *      M33 actually executes our bytes.
 */

#include "thumb2_emit.h"
#include <stdint.h>
#include <string.h>

/* 1 KiB of RW-executable scratch in RAM. .jitcache reserves space but
 * for the self-test we use BSS — same effect under QEMU mps2-an505,
 * which doesn't enforce W^X. */
__attribute__((aligned(64)))
static uint8_t st_buf[1024];

typedef int (*fn0_t)(void);
typedef int (*fn1_t)(int);
typedef int (*fn2_t)(int, int);

/* Treat the buffer as a function pointer with Thumb LSB set. */
static fn0_t to_fn0(void *p) { return (fn0_t)((uintptr_t)p | 1u); }
static fn1_t to_fn1(void *p) { return (fn1_t)((uintptr_t)p | 1u); }
static fn2_t to_fn2(void *p) { return (fn2_t)((uintptr_t)p | 1u); }

/* ---- encode-only tests ---- */

static int check_hw(uint16_t got, uint16_t want, int err) {
    return (got == want) ? 0 : err;
}

static int t_encode(void) {
    /* MOVS R0, #42 → 0x202A */
    emit_ctx_t e; emit_init(&e, st_buf, sizeof st_buf);
    emit_mov_lo_imm8(&e, 0, 42);
    int r = check_hw(((uint16_t *)st_buf)[0], 0x202A, 0x01);
    if (r) return r;

    /* ADDS R0, R1, #3 → 0x1CC8 */
    emit_init(&e, st_buf, sizeof st_buf);
    emit_adds_lo_lo_imm3(&e, 0, 1, 3);
    r = check_hw(((uint16_t *)st_buf)[0], 0x1CC8, 0x02);
    if (r) return r;

    /* BX LR → 0x4770 */
    emit_init(&e, st_buf, sizeof st_buf);
    emit_ret(&e);
    r = check_hw(((uint16_t *)st_buf)[0], 0x4770, 0x03);
    if (r) return r;

    /* MOVW R3, #0xBEEF → f64b 63ef (verified via arm-none-eabi-as). */
    emit_init(&e, st_buf, sizeof st_buf);
    emit_movw(&e, 3, 0xBEEF);
    r = check_hw(((uint16_t *)st_buf)[0], 0xF64B, 0x04);
    if (r) return r;
    r = check_hw(((uint16_t *)st_buf)[1], 0x63EF, 0x05);
    if (r) return r;

    /* MOVT R3, #0xDEAD → f6cd 63ad (verified via arm-none-eabi-as). */
    emit_init(&e, st_buf, sizeof st_buf);
    emit_movt(&e, 3, 0xDEAD);
    r = check_hw(((uint16_t *)st_buf)[0], 0xF6CD, 0x06);
    if (r) return r;
    r = check_hw(((uint16_t *)st_buf)[1], 0x63AD, 0x07);
    if (r) return r;

    /* LDR R0, [R1, #4] (T3) → f8d1 0004 */
    emit_init(&e, st_buf, sizeof st_buf);
    emit_ldr_imm(&e, 0, 1, 4);
    r = check_hw(((uint16_t *)st_buf)[0], 0xF8D1, 0x08);
    if (r) return r;
    r = check_hw(((uint16_t *)st_buf)[1], 0x0004, 0x09);
    if (r) return r;

    /* STRB R2, [R3, #0] (T2) → f883 2000 */
    emit_init(&e, st_buf, sizeof st_buf);
    emit_strb_imm(&e, 2, 3, 0);
    r = check_hw(((uint16_t *)st_buf)[0], 0xF883, 0x0A);
    if (r) return r;
    r = check_hw(((uint16_t *)st_buf)[1], 0x2000, 0x0B);
    if (r) return r;

    /* ADDW R0, R1, #1000 → f201 30e8 */
    emit_init(&e, st_buf, sizeof st_buf);
    emit_add_imm_t3_small(&e, 0, 1, 1000);
    r = check_hw(((uint16_t *)st_buf)[0], 0xF201, 0x0C);
    if (r) return r;
    r = check_hw(((uint16_t *)st_buf)[1], 0x30E8, 0x0D);
    if (r) return r;

    /* AND R0, R1, R2 → ea01 0002 */
    emit_init(&e, st_buf, sizeof st_buf);
    emit_and_reg(&e, 0, 1, 2);
    r = check_hw(((uint16_t *)st_buf)[0], 0xEA01, 0x0E);
    if (r) return r;
    r = check_hw(((uint16_t *)st_buf)[1], 0x0002, 0x0F);
    if (r) return r;

    return 0;
}

/* ---- execute-and-check tests ---- */

/* Build:   movs r0, #imm ; bx lr     → returns imm */
static int t_exec_mov_imm8(void) {
    emit_ctx_t e; emit_init(&e, st_buf, sizeof st_buf);
    emit_mov_lo_imm8(&e, 0, 99);
    emit_ret(&e);
    emit_finalize(&e);
    int got = to_fn0(st_buf)();
    return (got == 99) ? 0 : 0x10;
}

/* Build:   adds r0, r0, r1 ; bx lr   actually need: movs r0, r0; adds r0, r0, #5; bx lr → x+5  */
static int t_exec_add5(void) {
    emit_ctx_t e; emit_init(&e, st_buf, sizeof st_buf);
    emit_adds_lo_imm8(&e, 0, 5);
    emit_ret(&e);
    emit_finalize(&e);
    int got = to_fn1(st_buf)(37);
    return (got == 42) ? 0 : 0x11;
}

/* Build:   movw r0, #0x1234 ; movt r0, #0x5678 ; bx lr → 0x56781234 */
static int t_exec_mov_imm32(void) {
    emit_ctx_t e; emit_init(&e, st_buf, sizeof st_buf);
    emit_mov_imm32(&e, 0, 0x56781234u);
    emit_ret(&e);
    emit_finalize(&e);
    int got = to_fn0(st_buf)();
    return ((uint32_t)got == 0x56781234u) ? 0 : 0x12;
}

/* Build:   and r0, r0, r1 ; bx lr → x & y. (Note: this 32-bit form
 * targets any reg, so we use AND.W to set rd=r0.) */
static int t_exec_and(void) {
    emit_ctx_t e; emit_init(&e, st_buf, sizeof st_buf);
    emit_and_reg(&e, 0, 0, 1);
    emit_ret(&e);
    emit_finalize(&e);
    int got = to_fn2(st_buf)(0xF0F0F0F0, 0x12345678);
    return (got == (int)(0xF0F0F0F0u & 0x12345678u)) ? 0 : 0x13;
}

/* Build a forward conditional branch:
 *     cmp r0, #0
 *     b.eq skip                (T3 conditional)
 *     movs r0, #1
 *     bx lr
 *   skip:
 *     movs r0, #2
 *     bx lr
 * Returns 1 if arg was nonzero, 2 if zero. */
static int t_exec_branch(void) {
    emit_ctx_t e; emit_init(&e, st_buf, sizeof st_buf);
    emit_cmp_imm_t2(&e, 0, 0);
    uint32_t br = emit_b_w_placeholder(&e, 0);          /* cond 0 = EQ */
    emit_mov_lo_imm8(&e, 0, 1);
    emit_ret(&e);
    uint32_t skip_target = e.pos;
    emit_mov_lo_imm8(&e, 0, 2);
    emit_ret(&e);
    emit_patch_b_w(&e, br, skip_target);
    emit_finalize(&e);

    fn1_t f = to_fn1(st_buf);
    if (f(7) != 1) return 0x14;
    if (f(0) != 2) return 0x15;
    return 0;
}

int thumb2_selftest(char *err_out, int errlen) {
    int rc;
    rc = t_encode();        if (rc) goto fail;
    rc = t_exec_mov_imm8(); if (rc) goto fail;
    rc = t_exec_add5();     if (rc) goto fail;
    rc = t_exec_mov_imm32();if (rc) goto fail;
    rc = t_exec_and();      if (rc) goto fail;
    rc = t_exec_branch();   if (rc) goto fail;
    return 0;
fail:
    /* Best-effort error string. */
    if (err_out && errlen >= 32) {
        const char *p = "selftest fail code=0x";
        int n = 0;
        while (*p && n + 1 < errlen) err_out[n++] = *p++;
        static const char H[] = "0123456789abcdef";
        err_out[n++] = H[(rc >> 4) & 0xf];
        err_out[n++] = H[rc & 0xf];
        err_out[n++] = '\n';
        err_out[n] = 0;
    }
    return rc;
}
