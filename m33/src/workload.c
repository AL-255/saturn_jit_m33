/* Synthetic Saturn-bytecode workloads. We emit programs nibble-by-
 * nibble so the encoding is right next to what ISA.md documents.
 *
 * The "arith" workload is a counted loop:
 *   D1 = trip_count            ; 5 nibbles
 *   loop:
 *     A = A + B (W)            ; group A,  3 nibbles
 *     C = C - D (W)            ; group B,  3 nibbles
 *     A = A + 1 (A)            ; group E (E4), 2 nibbles
 *     C = C + 1 (A)            ; group E (E6), 2 nibbles
 *     D1 = D1 - 1              ; using GOSUB-less dec — we open-code via
 *                                  dec on C[A] after D1=C / C=D1 sequence
 *                                  to avoid needing 81C/etc this iteration.
 *     ?A=0 A then GOTO done    ; group 8A, 5 nibbles  (cond on A=0)
 *     GOTO loop                ; group 6, 4 nibbles
 *   done:
 *     GOTO done                ; trivial halt loop; bench stops on budget
 *
 * For this iteration, to keep it simple AND because the JIT doesn't
 * exist yet, we don't need a self-terminating program: we just run the
 * interpreter against a fixed instruction budget and report ops/sec.
 *
 * The "memmix" workload is similar but mixes DAT0=A and A=DAT0
 * 5-nibble traffic into the loop body. */

#include "workload.h"
#include <string.h>

static uint32_t emit(uint8_t *buf, uint32_t off, const uint8_t *nibs, uint32_t n) {
    memcpy(buf + off, nibs, n);
    return off + n;
}

/* Helpers for building a 2/3/4/5-nibble unsigned/signed displacement. */
static uint32_t emit_lc_b(uint8_t *buf, uint32_t off, uint8_t b) {
    /* LC(2) bb: 3, 1, n_low, n_high */
    buf[off++] = 0x3;
    buf[off++] = 0x1;
    buf[off++] = b & 0xf;
    buf[off++] = (b >> 4) & 0xf;
    return off;
}

/* Build the arithmetic-heavy workload. Returns a static workload_t
 * pointing into the caller's buffer. */
static workload_t g_wl_arith;
const workload_t *workload_build_arith(uint8_t *buf, uint32_t cap, uint32_t iters) {
    (void)cap; (void)iters;   /* iters is captured by counter regs at runtime */
    uint32_t o = 0;
    /* Pre-roll: zero out A,B,C,D in A-field. */
    /* A=0 A : D 0 */
    buf[o++] = 0xD; buf[o++] = 0x0;
    /* B=0 A : D 1 */
    buf[o++] = 0xD; buf[o++] = 0x1;
    /* C=0 A : D 2 */
    buf[o++] = 0xD; buf[o++] = 0x2;
    /* D=0 A : D 3 */
    buf[o++] = 0xD; buf[o++] = 0x3;

    /* Load some non-zero values: LC nibbles into C, P stays at 0.
     *   LC(4) 1 2 3 4 → C[0..3] = 1,2,3,4
     */
    buf[o++] = 0x3; buf[o++] = 0x3;       /* group 3, n+1 = 4 */
    buf[o++] = 0x1; buf[o++] = 0x2;
    buf[o++] = 0x3; buf[o++] = 0x4;
    /* Copy C → A (A field) :  D A (A=C A) */
    buf[o++] = 0xD; buf[o++] = 0xA;
    /* Copy C → B (A field) :  D 5  (B=C A) -- using table: D5 = BCEX? Actually D5=BCEX A.
       The copy "B=C A" lives at... let's check: per ISA §16 Group D, n1=5 → BCEX A.
       We want B=C A. Looking at table: n1=5 → BCEX A.  Hmm, B=C A is not directly here.
       Use A FS_A op for the W-field family? Group A, fs=8 (A,fs≥8 = move). Op 5 → B=C fs.
       Need fs encoded; FS_A is code 15 in the nibble. But group A is `A fs op` so fs=F (15)
       with op=5 → B=C A. Encoding: A F 5. */
    buf[o++] = 0xA; buf[o++] = 0xF; buf[o++] = 0x5;     /* B = C, A-field */

    /* Mark loop top. */
    uint32_t loop_top = o;

    /* Body: representative arithmetic + moves. */
    buf[o++] = 0xC; buf[o++] = 0x0;          /* A=A+B A         */
    buf[o++] = 0xE; buf[o++] = 0x6;          /* C=C+1 A         */
    buf[o++] = 0xC; buf[o++] = 0x2;          /* C=C+A A         */
    buf[o++] = 0xD; buf[o++] = 0xC;          /* ABEX A          */
    buf[o++] = 0xC; buf[o++] = 0x4;          /* A=A+A A (shift) */
    buf[o++] = 0xE; buf[o++] = 0x4;          /* A=A+1 A         */
    buf[o++] = 0xD; buf[o++] = 0xE;          /* ACEX A          */
    buf[o++] = 0xC; buf[o++] = 0xC;          /* A=A-1 A         */
    /* P fiddling */
    buf[o++] = 0x2; buf[o++] = 0x0;          /* P=0             */
    /* Branch back unconditionally to loop top: GOTO -displacement.
     * Group 6 ddd: PC = pc + sext12(ddd) + 1. We want target = loop_top.
     * pc here is `o`; displacement = loop_top - o - 1.
     */
    int32_t disp = (int32_t)loop_top - (int32_t)o - 1;
    uint32_t udisp = (uint32_t)(disp & 0xfff);
    buf[o++] = 0x6;
    buf[o++] = udisp & 0xf;
    buf[o++] = (udisp >> 4) & 0xf;
    buf[o++] = (udisp >> 8) & 0xf;

    g_wl_arith.name = "arith";
    g_wl_arith.image = buf;
    g_wl_arith.image_nibs = o;
    g_wl_arith.entry_pc = 0;
    /* 9 body ops + GOTO = 10 saturn ops per iter (P=n counts). */
    g_wl_arith.expected_ops_per_iter = 10;
    g_wl_arith.iters = iters;
    (void)emit; (void)emit_lc_b;
    return &g_wl_arith;
}

/* Memory-mix workload: each iter does a DAT0=A 5-nib store then A=DAT1
 * 5-nib load with D0 and D1 walking through a RAM region. */
static workload_t g_wl_memmix;
const workload_t *workload_build_memmix(uint8_t *buf, uint32_t cap, uint32_t iters) {
    (void)cap; (void)iters;
    uint32_t o = 0;
    /* Zero ABCD A-field. */
    buf[o++] = 0xD; buf[o++] = 0x0;
    buf[o++] = 0xD; buf[o++] = 0x1;
    buf[o++] = 0xD; buf[o++] = 0x2;
    buf[o++] = 0xD; buf[o++] = 0x3;
    /* D0 = 0x80000 (5-nib immediate): 1 B (load 5 nibs) k0 k1 k2 k3 k4 */
    buf[o++] = 0x1; buf[o++] = 0xB;
    buf[o++] = 0x0; buf[o++] = 0x0; buf[o++] = 0x0; buf[o++] = 0x0; buf[o++] = 0x8;
    /* D1 = 0x80100 */
    buf[o++] = 0x1; buf[o++] = 0xF;
    buf[o++] = 0x0; buf[o++] = 0x0; buf[o++] = 0x1; buf[o++] = 0x0; buf[o++] = 0x8;

    uint32_t loop_top = o;
    /* DAT0=A W (5 nib): group 1, 1 4 0 */
    buf[o++] = 0x1; buf[o++] = 0x4; buf[o++] = 0x0;
    /* A=DAT1 W (5 nib): 1 4 3 */
    buf[o++] = 0x1; buf[o++] = 0x4; buf[o++] = 0x3;
    /* D0 = D0 + 5 (n+1=5, n=4) */
    buf[o++] = 0x1; buf[o++] = 0x6; buf[o++] = 0x4;
    /* D1 = D1 + 5 */
    buf[o++] = 0x1; buf[o++] = 0x7; buf[o++] = 0x4;
    /* A=A+1 A */
    buf[o++] = 0xE; buf[o++] = 0x4;
    /* GOTO loop_top */
    int32_t disp = (int32_t)loop_top - (int32_t)o - 1;
    uint32_t udisp = (uint32_t)(disp & 0xfff);
    buf[o++] = 0x6;
    buf[o++] = udisp & 0xf;
    buf[o++] = (udisp >> 4) & 0xf;
    buf[o++] = (udisp >> 8) & 0xf;

    g_wl_memmix.name = "memmix";
    g_wl_memmix.image = buf;
    g_wl_memmix.image_nibs = o;
    g_wl_memmix.entry_pc = 0;
    g_wl_memmix.expected_ops_per_iter = 6;
    g_wl_memmix.iters = iters;
    return &g_wl_memmix;
}

/* Subroutine-heavy workload: outer loop body calls a tiny subroutine
 * each iter. The subroutine does a few arithmetic ops and returns.
 * Exercises GOSBVL (group 8F) + RTN (group 01).
 *
 * Layout:
 *   0x000   main entry
 *           D0..D3 zero
 *           LC(2) 5 0     ; A[0..1] = 0x05, P=0
 *   loop:
 *           GOSBVL 0x100  ; call subroutine at abs 0x100
 *           A+1 A
 *           GOTO loop
 *
 *   0x100   subroutine
 *           C=C+1 A
 *           C=C+A A
 *           RTN
 */
static workload_t g_wl_calltree;
const workload_t *workload_build_calltree(uint8_t *buf, uint32_t cap, uint32_t iters) {
    (void)cap; (void)iters;
    /* Zero the buffer up to 0x110 so the subroutine area is well defined. */
    for (uint32_t i = 0; i < 0x110; i++) buf[i] = 0;

    uint32_t o = 0;
    buf[o++] = 0xD; buf[o++] = 0x0;     /* A=0 A */
    buf[o++] = 0xD; buf[o++] = 0x1;     /* B=0 A */
    buf[o++] = 0xD; buf[o++] = 0x2;     /* C=0 A */
    buf[o++] = 0xD; buf[o++] = 0x3;     /* D=0 A */

    uint32_t loop_top = o;
    /* GOSBVL 0x100 : 8 F a4 a3 a2 a1 a0   (5-nib abs addr, little-end) */
    buf[o++] = 0x8; buf[o++] = 0xF;
    buf[o++] = 0x0; buf[o++] = 0x0; buf[o++] = 0x1; buf[o++] = 0x0; buf[o++] = 0x0;
    /* A=A+1 A */
    buf[o++] = 0xE; buf[o++] = 0x4;
    /* GOTO loop_top : 6 ddd  (3-nib signed displacement) */
    int32_t disp = (int32_t)loop_top - (int32_t)o - 1;
    uint32_t udisp = (uint32_t)(disp & 0xfff);
    buf[o++] = 0x6;
    buf[o++] = udisp & 0xf;
    buf[o++] = (udisp >> 4) & 0xf;
    buf[o++] = (udisp >> 8) & 0xf;

    /* Subroutine at 0x100. */
    uint32_t sub = 0x100;
    buf[sub + 0] = 0xE; buf[sub + 1] = 0x6;  /* C=C+1 A */
    buf[sub + 2] = 0xC; buf[sub + 3] = 0x2;  /* C=C+A A */
    buf[sub + 4] = 0x0; buf[sub + 5] = 0x1;  /* RTN */

    g_wl_calltree.name = "calltree";
    g_wl_calltree.image = buf;
    g_wl_calltree.image_nibs = 0x108;
    g_wl_calltree.entry_pc = 0;
    /* per outer iter: GOSBVL + 2 sub ops + RTN + A+1 + GOTO = 6 ops */
    g_wl_calltree.expected_ops_per_iter = 6;
    g_wl_calltree.iters = iters;
    return &g_wl_calltree;
}

/* Counted-loop workload. Reloads A to 0x10 every cycle and decrements
 * until A=0 (16 iters per cycle). Each body iter is C+1, A-1, ?A=0
 * branch-back-to-restart, GOTO loop. The ?A=0 path tests both
 * "not taken" (15 times/cycle) and "taken" (1 time/cycle), exercising
 * both halves of emit_compare_branch_tail. */
static workload_t g_wl_countloop;
const workload_t *workload_build_countloop(uint8_t *buf, uint32_t cap, uint32_t iters) {
    (void)cap; (void)iters;
    uint32_t o = 0;
    /* Preroll: zero ABCD A-field. */
    buf[o++] = 0xD; buf[o++] = 0x0;     /* A=0 A */
    buf[o++] = 0xD; buf[o++] = 0x1;     /* B=0 A */
    buf[o++] = 0xD; buf[o++] = 0x2;     /* C=0 A */
    buf[o++] = 0xD; buf[o++] = 0x3;     /* D=0 A */

    uint32_t restart = o;                /* re-enter here when A hits 0 */
    /* LC(2) value 0x10 → C[0..1] = (0,1) little-end nibble form. */
    buf[o++] = 0x3; buf[o++] = 0x1; buf[o++] = 0x0; buf[o++] = 0x1;
    /* A = C in A-field. */
    buf[o++] = 0xD; buf[o++] = 0xA;

    uint32_t loop_body = o;
    buf[o++] = 0xE; buf[o++] = 0x6;     /* C = C+1 A */
    buf[o++] = 0xC; buf[o++] = 0xC;     /* A = A-1 A */
    /* ?A=0 A ±dd → branch to `restart`. dd = sext8(restart - pc - 3). */
    uint32_t at_test = o;
    int32_t dd_to_restart = (int32_t)restart - (int32_t)at_test - 3;
    uint32_t dd = (uint32_t)(dd_to_restart & 0xff);
    buf[o++] = 0x8; buf[o++] = 0xA; buf[o++] = 0x8;
    buf[o++] = dd & 0xf;
    buf[o++] = (dd >> 4) & 0xf;
    /* GOTO loop_body. disp = sext12(target - pc - 1). */
    uint32_t at_goto = o;
    int32_t disp = (int32_t)loop_body - (int32_t)at_goto - 1;
    uint32_t udisp = (uint32_t)(disp & 0xfff);
    buf[o++] = 0x6;
    buf[o++] = udisp & 0xf;
    buf[o++] = (udisp >> 4) & 0xf;
    buf[o++] = (udisp >> 8) & 0xf;

    g_wl_countloop.name = "countloop";
    g_wl_countloop.image = buf;
    g_wl_countloop.image_nibs = o;
    g_wl_countloop.entry_pc = 0;
    /* Mixed: typical loop iter is 4 ops (C+1, A-1, ?A=0 fall-through, GOTO),
     * with a 3-op "?A=0 taken" iter every 16th time. */
    g_wl_countloop.expected_ops_per_iter = 4;
    g_wl_countloop.iters = iters;
    return &g_wl_countloop;
}
