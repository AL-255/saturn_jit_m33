/* N-queens benchmark workload.
 *
 * Translation of the HP 48 "8-queens" RPL code (LAHEX 888888888 as the
 * initial row vector) into a self-contained Saturn nibble image. The
 * three ROM dependencies in the original (the leading SAVPTR, the
 * trailing PUSHhxs/SAVPTR/GOVLNG=PUSH#ALOOP to push the solution onto
 * the calculator stack) are dropped — at the "solution found" point we
 * jump back to the program start so the workload runs forever.
 *
 * Algorithm sketch (P-field arithmetic throughout — each queen position
 * is one nibble inside the A/B registers):
 *   L10:  A = 0x888888888 sentinel ; R0 saves it ; B = current row in P
 *   L40:  if A[P] == B[P]   → found a solution → restart at L10
 *   L50:  ++B[P]            (try the next row in this column)
 *   L60:  P = B[P]          ; C[P] = A[P]
 *   L70:  D1++              ; advance the count
 *   L80:  D[P] = B[P]
 *   L90:  D[P]--
 *   L100: if D[P] == 0      → backtrack to L40
 *   L110: call Ptst on C=D[P] and again on C=B[P]
 *   L120: if A == C (diff zero) → L140 ; else if equal magnitudes → L90
 *   L140: P = B[P] ; C[P] -= 1
 *   L150: if C[P] != 0      → L70
 *   L160: --B[P]
 *   L170: if B[P] != 0      → L140 (else fall through)
 *   L180: restart
 *
 *   Ptst(C[0..3])  — permutes C[1..8] based on the value in C[0]
 *
 * The result is *not* equivalent to the original program (we drop the
 * "save solution to stack" step), but every Saturn opcode and the full
 * control-flow shape of the algorithm is preserved — which is what we
 * want for benchmarking.
 */

#include "workload.h"
#include <string.h>

/* --- label tracking + forward-reference patching --- */

#define MAX_LABELS 32
#define MAX_REFS   64

typedef struct {
    const char *name;
    uint32_t off;
} nq_label_t;

typedef struct {
    uint32_t patch_at;     /* offset of the displacement field's low nibble */
    int      width;        /* 3 (group 6/7) or 2 (group 8/9 conditional) */
    int      pc_kind;      /* 0 = pc_at_patch+1, 1 = group-8 +3, 2 = +4 (group 8 absolute), 3 = +6 (GOSUBL) */
    const char *target;
} nq_ref_t;

typedef struct {
    uint8_t   *buf;
    uint32_t   pos;
    nq_label_t labels[MAX_LABELS];
    int        n_labels;
    nq_ref_t   refs[MAX_REFS];
    int        n_refs;
} nq_emitter_t;

static void nq_def_label(nq_emitter_t *e, const char *name) {
    e->labels[e->n_labels].name = name;
    e->labels[e->n_labels].off = e->pos;
    e->n_labels++;
}

static uint32_t nq_find_label(nq_emitter_t *e, const char *name) {
    for (int i = 0; i < e->n_labels; i++) {
        if (e->labels[i].name == name) return e->labels[i].off;
    }
    return 0xffffffffu;
}

static void nq_add_ref(nq_emitter_t *e, uint32_t patch_at, int width,
                       int pc_kind, const char *target) {
    e->refs[e->n_refs].patch_at = patch_at;
    e->refs[e->n_refs].width = width;
    e->refs[e->n_refs].pc_kind = pc_kind;
    e->refs[e->n_refs].target = target;
    e->n_refs++;
}

static void nq_resolve_refs(nq_emitter_t *e) {
    /* The ISA reaches the displacement via `pc + sext(disp) + N` where
     * `pc` is the instruction's opcode address and N depends on the
     * group:
     *   group 6 GOTO  (4 nib): N=1, patch_at = pc + 1
     *   group 7 GOSUB (4 nib): N=4, patch_at = pc + 1
     *   group 8 cond  (5 nib): N=3, patch_at = pc + 3
     *   group 9 cond  (5 nib): N=3, patch_at = pc + 3
     * Solving for disp from `pc + disp + N = target`:
     *   group 6: disp = target - (patch_at - 1) - 1 = target - patch_at
     *   group 7: disp = target - (patch_at - 1) - 4 = target - patch_at - 3
     *   group 8/9: disp = target - (patch_at - 3) - 3 = target - patch_at */
    for (int i = 0; i < e->n_refs; i++) {
        uint32_t tgt = nq_find_label(e, e->refs[i].target);
        int32_t  disp;
        switch (e->refs[i].pc_kind) {
        case 0:  disp = (int32_t)tgt - (int32_t)e->refs[i].patch_at; break;
        case 1:  disp = (int32_t)tgt - (int32_t)e->refs[i].patch_at; break;
        case 3:  disp = (int32_t)tgt - (int32_t)e->refs[i].patch_at - 3; break;
        default: disp = (int32_t)tgt - (int32_t)e->refs[i].patch_at; break;
        }
        uint32_t udisp = (uint32_t)disp;
        int width = e->refs[i].width;
        for (int k = 0; k < width; k++) {
            e->buf[e->refs[i].patch_at + k] = (udisp >> (4 * k)) & 0xf;
        }
    }
}

/* --- instruction emitters --- */

static void nq_emit_n(nq_emitter_t *e, int n0)              { e->buf[e->pos++] = n0; }
static void nq_emit_2(nq_emitter_t *e, int a, int b)        { e->buf[e->pos++]=a; e->buf[e->pos++]=b; }
static void nq_emit_3(nq_emitter_t *e, int a, int b, int c) { e->buf[e->pos++]=a; e->buf[e->pos++]=b; e->buf[e->pos++]=c; }
static void nq_emit_4(nq_emitter_t *e, int a, int b, int c, int d) {
    e->buf[e->pos++]=a; e->buf[e->pos++]=b; e->buf[e->pos++]=c; e->buf[e->pos++]=d;
}

/* Simple ops. */
static void nq_P_eq(nq_emitter_t *e, int n)         { nq_emit_2(e, 0x2, n & 0xf); }
static void nq_R0_eq_A(nq_emitter_t *e)             { nq_emit_3(e, 0x1, 0x0, 0x0); }
static void nq_A_eq_R0(nq_emitter_t *e)             { nq_emit_3(e, 0x1, 0x1, 0x0); }
static void nq_D1_eq_C(nq_emitter_t *e)             { nq_emit_3(e, 0x1, 0x3, 0x5); }
static void nq_CD1EX(nq_emitter_t *e)               { nq_emit_3(e, 0x1, 0x3, 0x7); }
static void nq_AD1EX(nq_emitter_t *e)               { nq_emit_3(e, 0x1, 0x3, 0x3); }
static void nq_RSTK_eq_C(nq_emitter_t *e)           { nq_emit_2(e, 0x0, 0x6); }
static void nq_C_eq_RSTK(nq_emitter_t *e)           { nq_emit_2(e, 0x0, 0x7); }
static void nq_RTNCC(nq_emitter_t *e)               { nq_emit_2(e, 0x0, 0x3); }

/* Group A: fs<8 ADD; fs>=8 ZERO/COPY/EXCH. fs encodes the field
 * (0=P, 7=W, F=A, mirrors at +8). */
static void nq_A_fs_op(nq_emitter_t *e, int fs, int op) {
    nq_emit_3(e, 0xA, fs & 0xf, op & 0xf);
}
/* Group B: fs<8 SUB+inc/dec; fs>=8 SHL/SHR/NEG. */
static void nq_B_fs_op(nq_emitter_t *e, int fs, int op) {
    nq_emit_3(e, 0xB, fs & 0xf, op & 0xf);
}
/* Group D: A-field zero/copy/exch (2 nib). */
static void nq_D(nq_emitter_t *e, int op)           { nq_emit_2(e, 0xD, op & 0xf); }
/* Group E: A-field SUB/INC (2 nib). */
static void nq_E(nq_emitter_t *e, int op)           { nq_emit_2(e, 0xE, op & 0xf); }

/* LAHEX nibs: 8 0 8 2 nc nibs (nc = count-1). */
static void nq_LAHEX(nq_emitter_t *e, const uint8_t *nibs, int count) {
    nq_emit_4(e, 0x8, 0x0, 0x8, 0x2);
    e->buf[e->pos++] = (count - 1) & 0xf;
    for (int i = 0; i < count; i++) e->buf[e->pos++] = nibs[i] & 0xf;
}

/* Group 8 sub-ops: C=P n / P=C n / CPEX n  (4 nibs: 8 0 op n). */
static void nq_C_eq_P_n(nq_emitter_t *e, int n)     { nq_emit_4(e, 0x8, 0x0, 0xC, n & 0xf); }
static void nq_P_eq_C_n(nq_emitter_t *e, int n)     { nq_emit_4(e, 0x8, 0x0, 0xD, n & 0xf); }
static void nq_CPEX_n(nq_emitter_t *e, int n)       { nq_emit_4(e, 0x8, 0x0, 0xF, n & 0xf); }

/* GOTO ±ddd : group 6 (4 nibs, displacement = pc + sext + 1). */
static void nq_GOTO(nq_emitter_t *e, const char *target) {
    e->buf[e->pos++] = 0x6;
    uint32_t at = e->pos;
    e->buf[e->pos++] = 0; e->buf[e->pos++] = 0; e->buf[e->pos++] = 0;
    nq_add_ref(e, at, 3, 0, target);
}
/* GOSUB ±ddd : group 7 (4 nibs, displacement = pc + sext + 4). */
static void nq_GOSUB(nq_emitter_t *e, const char *target) {
    e->buf[e->pos++] = 0x7;
    uint32_t at = e->pos;
    e->buf[e->pos++] = 0; e->buf[e->pos++] = 0; e->buf[e->pos++] = 0;
    /* For group 7: pc + sext + 4 = target ⇒ sext = target - pc - 4.
     * patch_at = pc + 1, so sext = target - (patch_at-1) - 4 = target - patch_at - 3.
     * Pre-bias the patch_at by -3 so the generic resolver writes
     * (target - patch_at) and we silently get the right value. We do
     * this with a custom pc_kind handled in nq_resolve_refs. */
    nq_add_ref(e, at, 3, 3, target);
}

/* Group-9 compare+branch over field fs. Encoding: 9 fs op dd_lo dd_hi.
 * 5 nibs total; PC base = pc + sext + 3 so disp = target - patch_at. */
static void nq_cmp9(nq_emitter_t *e, int fs, int op, const char *target) {
    e->buf[e->pos++] = 0x9;
    e->buf[e->pos++] = fs & 0xf;
    e->buf[e->pos++] = op & 0xf;
    uint32_t at = e->pos;
    e->buf[e->pos++] = 0; e->buf[e->pos++] = 0;
    nq_add_ref(e, at, 2, 1, target);
}

/* Group-8 P-equality compare + branch: 8 9 n dd dd (5 nibs). op=9 is
 * ?P=n; op=8 is ?P#n. */
static void nq_cmp8P(nq_emitter_t *e, int op, int n, const char *target) {
    e->buf[e->pos++] = 0x8;
    e->buf[e->pos++] = op & 0xf;
    e->buf[e->pos++] = n & 0xf;
    uint32_t at = e->pos;
    e->buf[e->pos++] = 0; e->buf[e->pos++] = 0;
    nq_add_ref(e, at, 2, 1, target);
}

/* --- the N-queens program itself --- */

static workload_t g_wl_nqueens;
static uint8_t    g_nq_image[1024];

const workload_t *workload_build_nqueens(uint8_t *buf, uint32_t cap,
                                         uint32_t iters_unused) {
    (void)iters_unused;
    (void)cap;
    nq_emitter_t E = { .buf = g_nq_image, .pos = 0 };
    nq_emitter_t *e = &E;
    memset(g_nq_image, 0, sizeof g_nq_image);

    /* --- main routine --- */
    static const uint8_t sentinel_nibs[9] = {8,8,8,8,8,8,8,8,8};

    nq_def_label(e, "L10");
        nq_LAHEX(e, sentinel_nibs, 9);              /* A[W] = 0x888888888 */
        nq_R0_eq_A(e);                              /* R0 = A             */
        nq_A_fs_op(e, 0xF, 0x2);                    /* C=0 W  (fs=F → W) */
        nq_D1_eq_C(e);                              /* D1 = 0             */
        nq_D(e, 0x1);                               /* B=0 A              */
    nq_def_label(e, "L40");
        nq_P_eq(e, 0);
        nq_A_eq_R0(e);
        nq_cmp9(e, 0x0, 0x0, "L180");               /* ?A=B P → L180      */
    nq_def_label(e, "L50");
        nq_B_fs_op(e, 0x0, 0x5);                    /* B=B+1 P            */
    nq_def_label(e, "L60");
        nq_P_eq(e, 0);
        nq_A_fs_op(e, 0x8, 0x9);                    /* C=B P              */
        nq_P_eq_C_n(e, 0);                          /* P = C[0]           */
        nq_A_fs_op(e, 0x8, 0x6);                    /* C=A P              */
    nq_def_label(e, "L70");
        nq_CD1EX(e);                                /* C ↔ D1             */
        nq_E(e, 0x6);                               /* C=C+1 A            */
        nq_CD1EX(e);
    nq_def_label(e, "L80");
        nq_P_eq(e, 0);
        nq_A_fs_op(e, 0x8, 0x9);                    /* C=B P              */
        nq_A_fs_op(e, 0x8, 0x7);                    /* D=C P              */
    nq_def_label(e, "L90");
        nq_B_fs_op(e, 0x0, 0xF);                    /* D=D-1 P            */
    nq_def_label(e, "L100");
        nq_cmp9(e, 0x0, 0xB, "L40");                /* ?D=0 P → L40       */
    nq_def_label(e, "L110");
        nq_P_eq(e, 0);
        nq_A_fs_op(e, 0x8, 0xB);                    /* C=D P              */
        nq_GOSUB(e, "Ptst");
        nq_P_eq(e, 0);
        nq_A_fs_op(e, 0x8, 0xA);                    /* A=C P              */
        nq_A_fs_op(e, 0x8, 0x9);                    /* C=B P              */
        nq_GOSUB(e, "Ptst");
        nq_P_eq(e, 0);
        nq_cmp9(e, 0x8, 0xE, "NoSwp");              /* ?A>=C P (= ?C<=A P) */
        nq_A_fs_op(e, 0x8, 0xE);                    /* ACEX P             */
    nq_def_label(e, "NoSwp");
        nq_B_fs_op(e, 0x0, 0xA);                    /* A=A-C P            */
    nq_def_label(e, "L120");
        nq_cmp9(e, 0x0, 0x8, "L140");               /* ?A=0 P → L140      */
    nq_def_label(e, "L130");
        nq_A_fs_op(e, 0x8, 0x9);                    /* C=B P              */
        nq_B_fs_op(e, 0x0, 0xB);                    /* C=C-D P            */
        nq_cmp9(e, 0x0, 0x6, "L90");                /* ?A#C P → L90       */
    nq_def_label(e, "L140");
        nq_P_eq(e, 0);
        nq_A_fs_op(e, 0x8, 0x9);                    /* C=B P              */
        nq_P_eq_C_n(e, 0);                          /* P = C[0]           */
        nq_B_fs_op(e, 0x0, 0xE);                    /* C=C-1 P            */
    nq_def_label(e, "L150");
        nq_cmp9(e, 0x0, 0xE, "L70");                /* ?C#0 P → L70       */
    nq_def_label(e, "L160");
        nq_P_eq(e, 0);
        nq_B_fs_op(e, 0x0, 0xD);                    /* B=B-1 P            */
    nq_def_label(e, "L170");
        nq_cmp9(e, 0x0, 0xD, "L140");               /* ?B#0 P → L140      */
    nq_def_label(e, "L180");
        /* The original would push a solution onto the calculator stack
         * via three ROM calls and a GOVLNG. We skip that and just
         * restart at L10 — the workload then loops forever finding the
         * same solution over and over, which is what we want for a
         * benchmark. */
        nq_GOTO(e, "L10");

    /* --- subroutine Ptst --- */
    nq_def_label(e, "Ptst");
        nq_C_eq_P_n(e, 0);                          /* P=C 0              */
        nq_cmp8P(e, 0x8, 1, "tP2");                 /* ?P#1 → tP2         */
        nq_CPEX_n(e, 1);
        nq_C_eq_P_n(e, 0);
        nq_CPEX_n(e, 1);
        nq_RTNCC(e);
    nq_def_label(e, "tP2");
        nq_cmp8P(e, 0x8, 2, "tP3");
        nq_CPEX_n(e, 2); nq_C_eq_P_n(e, 0); nq_CPEX_n(e, 2); nq_RTNCC(e);
    nq_def_label(e, "tP3");
        nq_cmp8P(e, 0x8, 3, "tP4");
        nq_CPEX_n(e, 3); nq_C_eq_P_n(e, 0); nq_CPEX_n(e, 3); nq_RTNCC(e);
    nq_def_label(e, "tP4");
        nq_cmp8P(e, 0x8, 4, "tP5");
        nq_CPEX_n(e, 4); nq_C_eq_P_n(e, 0); nq_CPEX_n(e, 4); nq_RTNCC(e);
    nq_def_label(e, "tP5");
        nq_cmp8P(e, 0x8, 5, "tP6");
        nq_CPEX_n(e, 5); nq_C_eq_P_n(e, 0); nq_CPEX_n(e, 5); nq_RTNCC(e);
    nq_def_label(e, "tP6");
        nq_cmp8P(e, 0x8, 6, "tP7");
        nq_CPEX_n(e, 6); nq_C_eq_P_n(e, 0); nq_CPEX_n(e, 6); nq_RTNCC(e);
    nq_def_label(e, "tP7");
        nq_cmp8P(e, 0x8, 7, "tP8");
        nq_CPEX_n(e, 7); nq_C_eq_P_n(e, 0); nq_CPEX_n(e, 7); nq_RTNCC(e);
    nq_def_label(e, "tP8");
        nq_CPEX_n(e, 8); nq_C_eq_P_n(e, 0); nq_CPEX_n(e, 8); nq_RTNCC(e);

    nq_resolve_refs(e);

    /* Copy into the caller-provided buffer. */
    memcpy(buf, g_nq_image, e->pos);

    g_wl_nqueens.name     = "nqueens";
    g_wl_nqueens.image    = buf;
    g_wl_nqueens.image_nibs = e->pos;
    g_wl_nqueens.entry_pc = 0;
    g_wl_nqueens.expected_ops_per_iter = 0;
    g_wl_nqueens.iters    = 0;
    return &g_wl_nqueens;
}
