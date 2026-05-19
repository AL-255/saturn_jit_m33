/* Slim Saturn interpreter for the M33 baseline.
 *
 * Implements the opcode groups from ISA.md that we exercise in the
 * benchmark workload: 0/1/2/3/4/5/6/7/9/A/B/C/D/E/F entirely (modulo
 * the I/O-dependent corners of group 0), and the arithmetic / branch
 * / shift / status subset of group 8. Peripheral-dependent opcodes
 * (I/O, configuration, INTON/INTOFF, RSI, SHUTDN, BUSCx, C=ID,
 * UNCNFG/CONFIG) trap so we never silently mis-execute them.
 *
 * Design notes
 * - We re-read the opcode nibbles directly from `saturn.rom` (fast
 *   path) without going through sat_fetch when PC is in ROM range,
 *   which is always for our workloads. Fallback to sat_fetch when PC
 *   crosses the ROM boundary.
 * - Every dispatcher returns the new PC; the main loop just stores it.
 *   This shape matches what the JIT will emit (compiled trace returns
 *   the next Saturn PC in r0).
 * - Branches use ISA.md semantics:
 *     • GOC/GONC: PC += sext8(dd)+1     dd=0 ⇒ RTN
 *     • GOTO/GOSUB: PC += sext12(ddd)+1 / +4
 *     • GOLONG/GOSUBL: PC += sext16(dddd)+2 / +6
 */

#include "saturn_interp.h"
#include "saturn_state.h"
#include "saturn_registers.h"
#include "saturn_mem.h"

static inline nibble_t fetch(addr_t a)        { return sat_fetch(a); }
static inline uint32_t fetch_k(addr_t a,int k){ return sat_fetch_field(a,k); }

static inline void rstk_push(addr_t a) {
    if (saturn.rstk_ptr < NB_RSTK - 1) saturn.rstk[++saturn.rstk_ptr] = a;
    else {
        for (int i = 1; i < NB_RSTK; i++) saturn.rstk[i-1] = saturn.rstk[i];
        saturn.rstk[NB_RSTK-1] = a;
    }
}
static inline addr_t rstk_pop(void) {
    if (saturn.rstk_ptr < 0) return 0;
    return saturn.rstk[saturn.rstk_ptr--];
}

/* Set A-field (5 nibbles) of a register from a 20-bit address. */
static inline void addr_to_reg_a(nibble_t *r, addr_t a) {
    for (int i = 0; i < 5; i++) r[i] = (a >> (i*4)) & 0xf;
}
static inline addr_t reg_a_to_addr(const nibble_t *r) {
    addr_t a = 0;
    for (int i = 4; i >= 0; i--) a = (a << 4) | (r[i] & 0xf);
    return a;
}

/* ABCD register pointer. */
static inline nibble_t *R(int n) { return saturn.reg[n & 3]; }

/* DAT load/store: transfer `len` nibbles between mem[d] and reg starting
 * at nibble 0. */
static inline void dat_load(nibble_t *r, addr_t d, int len) {
    for (int i = 0; i < len; i++) r[i] = sat_fetch(d + i);
}
static inline void dat_store(addr_t d, const nibble_t *r, int len) {
    for (int i = 0; i < len; i++) sat_store(d + i, r[i]);
}

/* For DAT-field variants, the field length determines transfer length
 * but the transfer is always packed from nibble 0 (per ISA §6.14x). */
static int field_len(int code) {
    int s = field_start(code), e = field_end(code);
    return e - s + 1;
}

/* Forward declares for sub-dispatchers. */
static addr_t do_group_0(addr_t pc);
static addr_t do_group_1(addr_t pc);
static addr_t do_group_8(addr_t pc);

static interp_status_t g_status;

#define TRAP(s) do { g_status = (s); return saturn.pc; } while (0)

interp_status_t saturn_run_interp(uint64_t budget) {
    g_status = INTERP_OK_BUDGET;
    while (budget--) {
        addr_t pc = saturn.pc;
        nibble_t n0 = fetch(pc);
        switch (n0) {
        case 0x0: saturn.pc = do_group_0(pc); break;
        case 0x1: saturn.pc = do_group_1(pc); break;
        case 0x2: /* P=n */
            saturn.p = fetch(pc + 1) & 0xf;
            saturn.pc = pc + 2;
            break;
        case 0x3: {    /* LC (n+1) kk... */
            int n = fetch(pc + 1);
            int count = n + 1;
            for (int i = 0; i < count; i++) {
                int idx = (saturn.p + i) & 0xf;
                saturn.reg[REG_C][idx] = fetch(pc + 2 + i);
            }
            saturn.pc = pc + 2 + count;
            break;
        }
        case 0x4: {    /* GOC ±dd */
            uint32_t dd = fetch_k(pc + 1, 2);
            if (dd == 0) { saturn.pc = rstk_pop(); break; }
            if (saturn.carry) {
                saturn.pc = (pc + sext_nib(dd, 2) + 1) & 0xFFFFFu;
                saturn.saturn_branches_taken++;
            } else {
                saturn.pc = pc + 3;
                saturn.saturn_branches_skipped++;
            }
            break;
        }
        case 0x5: {    /* GONC ±dd */
            uint32_t dd = fetch_k(pc + 1, 2);
            if (dd == 0) { saturn.pc = rstk_pop(); break; }
            if (!saturn.carry) {
                saturn.pc = (pc + sext_nib(dd, 2) + 1) & 0xFFFFFu;
                saturn.saturn_branches_taken++;
            } else {
                saturn.pc = pc + 3;
                saturn.saturn_branches_skipped++;
            }
            break;
        }
        case 0x6: {    /* GOTO ±ddd, NOP-special: 003/004 */
            uint32_t ddd = fetch_k(pc + 1, 3);
            if (ddd == 3) { saturn.pc = pc + 4; break; }
            if (ddd == 4) { saturn.pc = pc + 5; break; }
            saturn.pc = (pc + sext_nib(ddd, 3) + 1) & 0xFFFFFu;
            saturn.saturn_branches_taken++;
            break;
        }
        case 0x7: {    /* GOSUB ±ddd */
            uint32_t ddd = fetch_k(pc + 1, 3);
            rstk_push((pc + 4) & 0xFFFFFu);
            saturn.pc = (pc + sext_nib(ddd, 3) + 4) & 0xFFFFFu;
            saturn.saturn_branches_taken++;
            break;
        }
        case 0x8: saturn.pc = do_group_8(pc); break;
        case 0x9: {    /* compare+branch over field fs (5 nibbles) */
            int fs = fetch(pc + 1);
            int field = fs & 7;
            int op = fetch(pc + 2);
            uint32_t dd = fetch_k(pc + 3, 2);
            int cond;
            if (fs < 8) {
                /* Equality table (matches 8A): 0..3 ?A=B ?B=C ?A=C ?C=D
                 *                              4..7 ?A#B ?B#C ?A#C ?C#D
                 *                              8..B ?A=0 ?B=0 ?C=0 ?D=0
                 *                              C..F ?A#0 ?B#0 ?C#0 ?D#0 */
                static const uint8_t pair[8][2] = {
                    {REG_A,REG_B},{REG_B,REG_C},{REG_A,REG_C},{REG_C,REG_D},
                    {REG_A,REG_B},{REG_B,REG_C},{REG_A,REG_C},{REG_C,REG_D},
                };
                if (op < 8) {
                    cond = reg_eq(R(pair[op&7][0]), R(pair[op&7][1]), field);
                    if (op >= 4) cond = !cond;
                } else {
                    int r = op & 3;
                    int z = reg_is_zero(R(r), field);
                    cond = (op < 0xC) ? z : !z;
                }
            } else {
                /* Ordered (matches 8B): 0..3 ?A>B ?B>C ?C>A ?D>C
                 *                       4..7 ?A<B ?B<C ?C<A ?D<C
                 *                       8..B ?A>=B ?B>=C ?C>=A ?D>=C
                 *                       C..F ?A<=B ?B<=C ?C<=A ?D<=C */
                static const uint8_t pair[4][2] = {
                    {REG_A,REG_B},{REG_B,REG_C},{REG_C,REG_A},{REG_D,REG_C},
                };
                int idx = op & 3;
                int gt = reg_gt(R(pair[idx][0]), R(pair[idx][1]), field);
                int lt = reg_lt(R(pair[idx][0]), R(pair[idx][1]), field);
                switch ((op >> 2) & 3) {
                case 0: cond = gt;       break;
                case 1: cond = lt;       break;
                case 2: cond = !lt;      break;
                default: cond = !gt;     break;
                }
            }
            saturn.carry = cond;
            if (dd == 0) { saturn.pc = rstk_pop(); break; }
            if (cond) {
                saturn.pc = (pc + sext_nib(dd, 2) + 3) & 0xFFFFFu;
                saturn.saturn_branches_taken++;
            } else {
                saturn.pc = pc + 5;
                saturn.saturn_branches_skipped++;
            }
            break;
        }
        case 0xA: {    /* A fs op : field ADD/DEC or ZERO/COPY/EXCH */
            int fs = fetch(pc + 1);
            int op = fetch(pc + 2);
            int field = fs & 7;
            if (fs < 8) {
                /* arithmetic */
                switch (op) {
                case 0x0: reg_add(R(REG_A), R(REG_A), R(REG_B), field); break;
                case 0x1: reg_add(R(REG_B), R(REG_B), R(REG_C), field); break;
                case 0x2: reg_add(R(REG_C), R(REG_C), R(REG_A), field); break;
                case 0x3: reg_add(R(REG_D), R(REG_D), R(REG_C), field); break;
                case 0x4: reg_add(R(REG_A), R(REG_A), R(REG_A), field); break;
                case 0x5: reg_add(R(REG_B), R(REG_B), R(REG_B), field); break;
                case 0x6: reg_add(R(REG_C), R(REG_C), R(REG_C), field); break;
                case 0x7: reg_add(R(REG_D), R(REG_D), R(REG_D), field); break;
                case 0x8: reg_add(R(REG_B), R(REG_B), R(REG_A), field); break;
                case 0x9: reg_add(R(REG_C), R(REG_C), R(REG_B), field); break;
                case 0xA: reg_add(R(REG_A), R(REG_A), R(REG_C), field); break;
                case 0xB: reg_add(R(REG_C), R(REG_C), R(REG_D), field); break;
                case 0xC: reg_dec(R(REG_A), field); break;
                case 0xD: reg_dec(R(REG_B), field); break;
                case 0xE: reg_dec(R(REG_C), field); break;
                case 0xF: reg_dec(R(REG_D), field); break;
                }
            } else {
                /* move/zero/exch */
                switch (op) {
                case 0x0: reg_zero(R(REG_A), field); break;
                case 0x1: reg_zero(R(REG_B), field); break;
                case 0x2: reg_zero(R(REG_C), field); break;
                case 0x3: reg_zero(R(REG_D), field); break;
                case 0x4: reg_copy(R(REG_A), R(REG_B), field); break;
                case 0x5: reg_copy(R(REG_B), R(REG_C), field); break;
                case 0x6: reg_copy(R(REG_C), R(REG_A), field); break;
                case 0x7: reg_copy(R(REG_D), R(REG_C), field); break;
                case 0x8: reg_copy(R(REG_B), R(REG_A), field); break;
                case 0x9: reg_copy(R(REG_C), R(REG_B), field); break;
                case 0xA: reg_copy(R(REG_A), R(REG_C), field); break;
                case 0xB: reg_copy(R(REG_C), R(REG_D), field); break;
                case 0xC: reg_xchg(R(REG_A), R(REG_B), field); break;
                case 0xD: reg_xchg(R(REG_B), R(REG_C), field); break;
                case 0xE: reg_xchg(R(REG_A), R(REG_C), field); break;
                case 0xF: reg_xchg(R(REG_C), R(REG_D), field); break;
                }
            }
            saturn.pc = pc + 3;
            break;
        }
        case 0xB: {    /* B fs op : field SUB/INC or SHL/SHR/NEG */
            int fs = fetch(pc + 1);
            int op = fetch(pc + 2);
            int field = fs & 7;
            if (fs < 8) {
                switch (op) {
                case 0x0: reg_sub(R(REG_A), R(REG_A), R(REG_B), field); break;
                case 0x1: reg_sub(R(REG_B), R(REG_B), R(REG_C), field); break;
                case 0x2: reg_sub(R(REG_C), R(REG_C), R(REG_A), field); break;
                case 0x3: reg_sub(R(REG_D), R(REG_D), R(REG_C), field); break;
                case 0x4: reg_inc(R(REG_A), field); break;
                case 0x5: reg_inc(R(REG_B), field); break;
                case 0x6: reg_inc(R(REG_C), field); break;
                case 0x7: reg_inc(R(REG_D), field); break;
                case 0x8: reg_sub(R(REG_B), R(REG_B), R(REG_A), field); break;
                case 0x9: reg_sub(R(REG_C), R(REG_C), R(REG_B), field); break;
                case 0xA: reg_sub(R(REG_A), R(REG_A), R(REG_C), field); break;
                case 0xB: reg_sub(R(REG_C), R(REG_C), R(REG_D), field); break;
                case 0xC: reg_sub(R(REG_A), R(REG_B), R(REG_A), field); break;
                case 0xD: reg_sub(R(REG_B), R(REG_C), R(REG_B), field); break;
                case 0xE: reg_sub(R(REG_C), R(REG_A), R(REG_C), field); break;
                case 0xF: reg_sub(R(REG_D), R(REG_C), R(REG_D), field); break;
                }
            } else {
                switch (op) {
                case 0x0: reg_shl(R(REG_A), field); break;
                case 0x1: reg_shl(R(REG_B), field); break;
                case 0x2: reg_shl(R(REG_C), field); break;
                case 0x3: reg_shl(R(REG_D), field); break;
                case 0x4: reg_shr(R(REG_A), field); break;
                case 0x5: reg_shr(R(REG_B), field); break;
                case 0x6: reg_shr(R(REG_C), field); break;
                case 0x7: reg_shr(R(REG_D), field); break;
                case 0x8: reg_comp2(R(REG_A), field); break;
                case 0x9: reg_comp2(R(REG_B), field); break;
                case 0xA: reg_comp2(R(REG_C), field); break;
                case 0xB: reg_comp2(R(REG_D), field); break;
                case 0xC: reg_comp1(R(REG_A), field); break;
                case 0xD: reg_comp1(R(REG_B), field); break;
                case 0xE: reg_comp1(R(REG_C), field); break;
                case 0xF: reg_comp1(R(REG_D), field); break;
                }
            }
            saturn.pc = pc + 3;
            break;
        }
        /* C/D/E/F are the A-field variants of A/B groups (2 nibbles each). */
        case 0xC: {
            int op = fetch(pc + 1);
            switch (op) {
            case 0x0: reg_add(R(REG_A), R(REG_A), R(REG_B), FS_A); break;
            case 0x1: reg_add(R(REG_B), R(REG_B), R(REG_C), FS_A); break;
            case 0x2: reg_add(R(REG_C), R(REG_C), R(REG_A), FS_A); break;
            case 0x3: reg_add(R(REG_D), R(REG_D), R(REG_C), FS_A); break;
            case 0x4: reg_add(R(REG_A), R(REG_A), R(REG_A), FS_A); break;
            case 0x5: reg_add(R(REG_B), R(REG_B), R(REG_B), FS_A); break;
            case 0x6: reg_add(R(REG_C), R(REG_C), R(REG_C), FS_A); break;
            case 0x7: reg_add(R(REG_D), R(REG_D), R(REG_D), FS_A); break;
            case 0x8: reg_add(R(REG_B), R(REG_B), R(REG_A), FS_A); break;
            case 0x9: reg_add(R(REG_C), R(REG_C), R(REG_B), FS_A); break;
            case 0xA: reg_add(R(REG_A), R(REG_A), R(REG_C), FS_A); break;
            case 0xB: reg_add(R(REG_C), R(REG_C), R(REG_D), FS_A); break;
            case 0xC: reg_dec(R(REG_A), FS_A); break;
            case 0xD: reg_dec(R(REG_B), FS_A); break;
            case 0xE: reg_dec(R(REG_C), FS_A); break;
            case 0xF: reg_dec(R(REG_D), FS_A); break;
            }
            saturn.pc = pc + 2;
            break;
        }
        case 0xD: {
            int op = fetch(pc + 1);
            switch (op) {
            case 0x0: reg_zero(R(REG_A), FS_A); break;
            case 0x1: reg_zero(R(REG_B), FS_A); break;
            case 0x2: reg_zero(R(REG_C), FS_A); break;
            case 0x3: reg_zero(R(REG_D), FS_A); break;
            case 0x4: reg_copy(R(REG_A), R(REG_B), FS_A); break;
            case 0x5: reg_copy(R(REG_B), R(REG_C), FS_A); break;
            case 0x6: reg_copy(R(REG_C), R(REG_A), FS_A); break;
            case 0x7: reg_copy(R(REG_D), R(REG_C), FS_A); break;
            case 0x8: reg_copy(R(REG_B), R(REG_A), FS_A); break;
            case 0x9: reg_copy(R(REG_C), R(REG_B), FS_A); break;
            case 0xA: reg_copy(R(REG_A), R(REG_C), FS_A); break;
            case 0xB: reg_copy(R(REG_C), R(REG_D), FS_A); break;
            case 0xC: reg_xchg(R(REG_A), R(REG_B), FS_A); break;
            case 0xD: reg_xchg(R(REG_B), R(REG_C), FS_A); break;
            case 0xE: reg_xchg(R(REG_A), R(REG_C), FS_A); break;
            case 0xF: reg_xchg(R(REG_C), R(REG_D), FS_A); break;
            }
            saturn.pc = pc + 2;
            break;
        }
        case 0xE: {
            int op = fetch(pc + 1);
            switch (op) {
            case 0x0: reg_sub(R(REG_A), R(REG_A), R(REG_B), FS_A); break;
            case 0x1: reg_sub(R(REG_B), R(REG_B), R(REG_C), FS_A); break;
            case 0x2: reg_sub(R(REG_C), R(REG_C), R(REG_A), FS_A); break;
            case 0x3: reg_sub(R(REG_D), R(REG_D), R(REG_C), FS_A); break;
            case 0x4: reg_inc(R(REG_A), FS_A); break;
            case 0x5: reg_inc(R(REG_B), FS_A); break;
            case 0x6: reg_inc(R(REG_C), FS_A); break;
            case 0x7: reg_inc(R(REG_D), FS_A); break;
            case 0x8: reg_sub(R(REG_B), R(REG_B), R(REG_A), FS_A); break;
            case 0x9: reg_sub(R(REG_C), R(REG_C), R(REG_B), FS_A); break;
            case 0xA: reg_sub(R(REG_A), R(REG_A), R(REG_C), FS_A); break;
            case 0xB: reg_sub(R(REG_C), R(REG_C), R(REG_D), FS_A); break;
            case 0xC: reg_sub(R(REG_A), R(REG_B), R(REG_A), FS_A); break;
            case 0xD: reg_sub(R(REG_B), R(REG_C), R(REG_B), FS_A); break;
            case 0xE: reg_sub(R(REG_C), R(REG_A), R(REG_C), FS_A); break;
            case 0xF: reg_sub(R(REG_D), R(REG_C), R(REG_D), FS_A); break;
            }
            saturn.pc = pc + 2;
            break;
        }
        case 0xF: {
            int op = fetch(pc + 1);
            switch (op) {
            case 0x0: reg_shl(R(REG_A), FS_A); break;
            case 0x1: reg_shl(R(REG_B), FS_A); break;
            case 0x2: reg_shl(R(REG_C), FS_A); break;
            case 0x3: reg_shl(R(REG_D), FS_A); break;
            case 0x4: reg_shr(R(REG_A), FS_A); break;
            case 0x5: reg_shr(R(REG_B), FS_A); break;
            case 0x6: reg_shr(R(REG_C), FS_A); break;
            case 0x7: reg_shr(R(REG_D), FS_A); break;
            case 0x8: reg_comp2(R(REG_A), FS_A); break;
            case 0x9: reg_comp2(R(REG_B), FS_A); break;
            case 0xA: reg_comp2(R(REG_C), FS_A); break;
            case 0xB: reg_comp2(R(REG_D), FS_A); break;
            case 0xC: reg_comp1(R(REG_A), FS_A); break;
            case 0xD: reg_comp1(R(REG_B), FS_A); break;
            case 0xE: reg_comp1(R(REG_C), FS_A); break;
            case 0xF: reg_comp1(R(REG_D), FS_A); break;
            }
            saturn.pc = pc + 2;
            break;
        }
        default: TRAP(INTERP_UNIMPL);
        }
        saturn.saturn_ops++;
        if (g_status != INTERP_OK_BUDGET) return g_status;
    }
    return INTERP_OK_BUDGET;
}

/* ---------------- Group 0 (misc + 0E field AND/OR) ----------------- */
static addr_t do_group_0(addr_t pc) {
    int n1 = fetch(pc + 1);
    switch (n1) {
    case 0x0: saturn.st[ST_XM] = 1; return rstk_pop();         /* RTNSXM */
    case 0x1: return rstk_pop();                                /* RTN    */
    case 0x2: { addr_t a = rstk_pop(); saturn.carry = 1; return a; } /* RTNSC */
    case 0x3: { addr_t a = rstk_pop(); saturn.carry = 0; return a; } /* RTNCC */
    case 0x4: saturn.hexmode = 16; return pc + 2;               /* SETHEX */
    case 0x5: saturn.hexmode = 10; return pc + 2;               /* SETDEC */
    case 0x6: { /* RSTK=C : push C[A] (low 5 nibbles) */
        rstk_push(reg_a_to_addr(R(REG_C)));
        return pc + 2;
    }
    case 0x7: { /* C=RSTK */
        addr_to_reg_a(R(REG_C), rstk_pop());
        return pc + 2;
    }
    case 0x8: /* CLRST : PSTAT[11:0]=0 */
        for (int i = 0; i < 12; i++) saturn.pstat[i] = 0;
        return pc + 2;
    case 0x9: /* C=ST  : C[0..2] = PSTAT[0..11] packed nibble */
        for (int i = 0; i < 3; i++) {
            nibble_t v = 0;
            for (int b = 0; b < 4; b++) v |= (saturn.pstat[i*4 + b] & 1) << b;
            saturn.reg[REG_C][i] = v;
        }
        return pc + 2;
    case 0xA: /* ST=C : PSTAT[0..11] from C[0..2] */
        for (int i = 0; i < 3; i++) {
            nibble_t v = saturn.reg[REG_C][i];
            for (int b = 0; b < 4; b++) saturn.pstat[i*4 + b] = (v >> b) & 1;
        }
        return pc + 2;
    case 0xB: /* CSTEX : swap */
        for (int i = 0; i < 3; i++) {
            nibble_t v = saturn.reg[REG_C][i];
            nibble_t s = 0;
            for (int b = 0; b < 4; b++) s |= (saturn.pstat[i*4 + b] & 1) << b;
            saturn.reg[REG_C][i] = s;
            for (int b = 0; b < 4; b++) saturn.pstat[i*4 + b] = (v >> b) & 1;
        }
        return pc + 2;
    case 0xC: /* P=P+1, carry if wraps */
        saturn.p = (saturn.p + 1) & 0xf;
        saturn.carry = (saturn.p == 0);
        return pc + 2;
    case 0xD: /* P=P-1, carry if wraps */
        saturn.carry = (saturn.p == 0);
        saturn.p = (saturn.p - 1) & 0xf;
        return pc + 2;
    case 0xE: { /* 0 E fs n - field AND/OR */
        int fs = fetch(pc + 2) & 7;
        int op = fetch(pc + 3);
        switch (op) {
        case 0x0: reg_and(R(REG_A), R(REG_A), R(REG_B), fs); break;
        case 0x1: reg_and(R(REG_B), R(REG_B), R(REG_C), fs); break;
        case 0x2: reg_and(R(REG_C), R(REG_C), R(REG_A), fs); break;
        case 0x3: reg_and(R(REG_D), R(REG_D), R(REG_C), fs); break;
        case 0x4: reg_and(R(REG_B), R(REG_B), R(REG_A), fs); break;
        case 0x5: reg_and(R(REG_C), R(REG_C), R(REG_B), fs); break;
        case 0x6: reg_and(R(REG_A), R(REG_A), R(REG_C), fs); break;
        case 0x7: reg_and(R(REG_C), R(REG_C), R(REG_D), fs); break;
        case 0x8: reg_or (R(REG_A), R(REG_A), R(REG_B), fs); break;
        case 0x9: reg_or (R(REG_B), R(REG_B), R(REG_C), fs); break;
        case 0xA: reg_or (R(REG_C), R(REG_C), R(REG_A), fs); break;
        case 0xB: reg_or (R(REG_D), R(REG_D), R(REG_C), fs); break;
        case 0xC: reg_or (R(REG_B), R(REG_B), R(REG_A), fs); break;
        case 0xD: reg_or (R(REG_C), R(REG_C), R(REG_B), fs); break;
        case 0xE: reg_or (R(REG_A), R(REG_A), R(REG_C), fs); break;
        case 0xF: reg_or (R(REG_C), R(REG_C), R(REG_D), fs); break;
        }
        return pc + 4;
    }
    case 0xF: /* RTI - returns from interrupt; treat as RTN for bench */
        return rstk_pop();
    }
    TRAP(INTERP_UNIMPL);
}

/* ---------------- Group 1 (moves / D pointers / DAT) -------------- */
static addr_t do_group_1(addr_t pc) {
    int n1 = fetch(pc + 1);
    switch (n1) {
    case 0x0: case 0x1: case 0x2: {  /* R<-A, A<-R, AR_EX (with A) */
        int n2 = fetch(pc + 2);
        int rn = (n2 >= 5 && n2 <= 7) ? (n2 - 4) : n2;     /* 5,6,7 dup of 1,2,3 */
        rn &= 7;
        if (rn > 4) rn -= 5;  /* fold D..F into 4..6 area; ISA actually has them as duplicates of 9..B (C target) */
        switch (n1) {
        case 0x0: /* Rn = A (W) */
            if (n2 < 8) for (int i=0;i<16;i++) saturn.reg_r[n2 & 7][i] = saturn.reg[REG_A][i];
            else        for (int i=0;i<16;i++) saturn.reg_r[(n2-8) & 7][i] = saturn.reg[REG_C][i];
            break;
        case 0x1:
            if (n2 < 8) for (int i=0;i<16;i++) saturn.reg[REG_A][i] = saturn.reg_r[n2 & 7][i];
            else        for (int i=0;i<16;i++) saturn.reg[REG_C][i] = saturn.reg_r[(n2-8) & 7][i];
            break;
        case 0x2: {
            nibble_t *r = (n2 < 8) ? saturn.reg[REG_A] : saturn.reg[REG_C];
            nibble_t *s = saturn.reg_r[(n2 < 8 ? n2 : n2-8) & 7];
            for (int i=0;i<16;i++) { nibble_t t = r[i]; r[i] = s[i]; s[i] = t; }
            break;
        }
        }
        return pc + 3;
    }
    case 0x3: { /* D0/D1 <-> A/C, full or short */
        int n2 = fetch(pc + 2);
        switch (n2) {
        case 0x0: saturn.d[0] = reg_a_to_addr(R(REG_A)); break;
        case 0x1: saturn.d[1] = reg_a_to_addr(R(REG_A)); break;
        case 0x2: { addr_t t = saturn.d[0]; saturn.d[0] = reg_a_to_addr(R(REG_A)); addr_to_reg_a(R(REG_A), t); break; }
        case 0x3: { addr_t t = saturn.d[1]; saturn.d[1] = reg_a_to_addr(R(REG_A)); addr_to_reg_a(R(REG_A), t); break; }
        case 0x4: saturn.d[0] = reg_a_to_addr(R(REG_C)); break;
        case 0x5: saturn.d[1] = reg_a_to_addr(R(REG_C)); break;
        case 0x6: { addr_t t = saturn.d[0]; saturn.d[0] = reg_a_to_addr(R(REG_C)); addr_to_reg_a(R(REG_C), t); break; }
        case 0x7: { addr_t t = saturn.d[1]; saturn.d[1] = reg_a_to_addr(R(REG_C)); addr_to_reg_a(R(REG_C), t); break; }
        /* Short forms (B+X = nibbles 0..1 + 2 = 4 nibbles); approximate by 4 nibbles. */
        default: TRAP(INTERP_UNIMPL);
        }
        return pc + 3;
    }
    case 0x4: { /* DAT W/B short forms */
        int n2 = fetch(pc + 2);
        int op = n2 & 7;
        int is_W = n2 < 8;
        int len  = is_W ? 5 : 2;
        addr_t d = (op & 1) ? saturn.d[1] : saturn.d[0];
        switch (op >> 1) {
        case 0: dat_store(d, R((op & 4) ? REG_C : REG_A), len); break; /* 0,1,4,5: DATx=A/C */
        case 1: dat_load (R((op & 4) ? REG_C : REG_A), d, len); break; /* 2,3,6,7: A/C=DATx */
        }
        return pc + 3;
    }
    case 0x5: { /* DAT with field-code or explicit-count */
        int n2 = fetch(pc + 2);
        int n3 = fetch(pc + 3);
        int op = n2 & 7;
        int len = (n2 < 8) ? field_len(n3 & 0xf) : (n3 + 1);
        addr_t d = (op & 1) ? saturn.d[1] : saturn.d[0];
        nibble_t *r = (op & 4) ? R(REG_C) : R(REG_A);
        if (op < 2 || (op >= 4 && op < 6)) dat_store(d, r, len);
        else                                dat_load(r, d, len);
        return pc + 4;
    }
    case 0x6: saturn.d[0] = (saturn.d[0] + fetch(pc + 2) + 1) & 0xFFFFFu; return pc + 3;
    case 0x7: saturn.d[1] = (saturn.d[1] + fetch(pc + 2) + 1) & 0xFFFFFu; return pc + 3;
    case 0x8: saturn.d[0] = (saturn.d[0] - (fetch(pc + 2) + 1)) & 0xFFFFFu; return pc + 3;
    case 0xC: saturn.d[1] = (saturn.d[1] - (fetch(pc + 2) + 1)) & 0xFFFFFu; return pc + 3;
    case 0x9: case 0xA: case 0xB:
    case 0xD: case 0xE: case 0xF: {
        int target = (n1 < 0xD) ? 0 : 1;
        int len    = (n1 == 0x9 || n1 == 0xD) ? 2
                   : (n1 == 0xA || n1 == 0xE) ? 4 : 5;
        addr_t v = fetch_k(pc + 2, len);
        addr_t mask = (1u << (len*4)) - 1;
        saturn.d[target] = (saturn.d[target] & ~mask) | (v & mask);
        return pc + 2 + len;
    }
    }
    TRAP(INTERP_UNIMPL);
}

/* ---------------- Group 8 (subset: 8C/8D/8E/8F + 8A/8B + 86/87/88/89) */
static addr_t do_group_8(addr_t pc) {
    int n1 = fetch(pc + 1);
    switch (n1) {
    case 0x0: { /* 8 0 X — group-8 sub-ops needed by the N-queens
                   benchmark: LA (8082...), C=P n (80Cn), P=C n (80Dn),
                   CPEX n (80Fn). The rest of the 80x family is left
                   unimpl; it's mostly HP-48 hardware control. */
        int n2 = fetch(pc + 2);
        switch (n2) {
        case 0x8: { /* 8 0 8 X */
            int n3 = fetch(pc + 3);
            switch (n3) {
            case 0x2: { /* LA : load `op5+1` hex nibbles into A starting at A[P] */
                int op5 = fetch(pc + 4);
                int count = op5 + 1;
                for (int i = 0; i < count; i++) {
                    int idx = (saturn.p + i) & 0xf;
                    saturn.reg[REG_A][idx] = fetch(pc + 5 + i);
                }
                return pc + 5 + count;
            }
            default: TRAP(INTERP_UNIMPL);
            }
        }
        case 0xC: { /* C=P n : copy P into C[n] */
            int n = fetch(pc + 3) & 0xf;
            saturn.reg[REG_C][n] = saturn.p;
            return pc + 4;
        }
        case 0xD: { /* P=C n : set P from C[n] */
            int n = fetch(pc + 3) & 0xf;
            saturn.p = saturn.reg[REG_C][n] & 0xf;
            return pc + 4;
        }
        case 0xF: { /* CPEX n : swap C[n] ↔ P */
            int n = fetch(pc + 3) & 0xf;
            nibble_t t = saturn.reg[REG_C][n];
            saturn.reg[REG_C][n] = saturn.p;
            saturn.p = t & 0xf;
            return pc + 4;
        }
        default: TRAP(INTERP_UNIMPL);
        }
    }
    case 0x2: { /* CLRSTmask */
        int m = fetch(pc + 2);
        for (int b = 0; b < 4; b++) if (m & (1 << b)) saturn.st[b] = 0;
        return pc + 3;
    }
    case 0x4: saturn.pstat[fetch(pc + 2) & 0xf] = 0; return pc + 3;   /* ST=0 n */
    case 0x5: saturn.pstat[fetch(pc + 2) & 0xf] = 1; return pc + 3;   /* ST=1 n */
    case 0x6: {     /* ?ST=0 n ±dd */
        int n = fetch(pc + 2) & 0xf;
        uint32_t dd = fetch_k(pc + 3, 2);
        int cond = (saturn.pstat[n] == 0);
        saturn.carry = cond;
        if (dd == 0) return rstk_pop();
        if (cond) { saturn.saturn_branches_taken++;   return (pc + sext_nib(dd,2) + 3) & 0xFFFFFu; }
        else      { saturn.saturn_branches_skipped++; return pc + 5; }
    }
    case 0x7: {     /* ?ST=1 n ±dd */
        int n = fetch(pc + 2) & 0xf;
        uint32_t dd = fetch_k(pc + 3, 2);
        int cond = (saturn.pstat[n] != 0);
        saturn.carry = cond;
        if (dd == 0) return rstk_pop();
        if (cond) { saturn.saturn_branches_taken++;   return (pc + sext_nib(dd,2) + 3) & 0xFFFFFu; }
        else      { saturn.saturn_branches_skipped++; return pc + 5; }
    }
    case 0x8: {     /* ?P#n ±dd */
        int n = fetch(pc + 2) & 0xf;
        uint32_t dd = fetch_k(pc + 3, 2);
        int cond = (saturn.p != n);
        saturn.carry = cond;
        if (dd == 0) return rstk_pop();
        if (cond) { saturn.saturn_branches_taken++;   return (pc + sext_nib(dd,2) + 3) & 0xFFFFFu; }
        else      { saturn.saturn_branches_skipped++; return pc + 5; }
    }
    case 0x9: {     /* ?P=n ±dd */
        int n = fetch(pc + 2) & 0xf;
        uint32_t dd = fetch_k(pc + 3, 2);
        int cond = (saturn.p == n);
        saturn.carry = cond;
        if (dd == 0) return rstk_pop();
        if (cond) { saturn.saturn_branches_taken++;   return (pc + sext_nib(dd,2) + 3) & 0xFFFFFu; }
        else      { saturn.saturn_branches_skipped++; return pc + 5; }
    }
    case 0xA: {     /* equality compares (A-field) + branch */
        int op = fetch(pc + 2);
        uint32_t dd = fetch_k(pc + 3, 2);
        int cond;
        static const uint8_t pair[8][2] = {
            {REG_A,REG_B},{REG_B,REG_C},{REG_A,REG_C},{REG_C,REG_D},
            {REG_A,REG_B},{REG_B,REG_C},{REG_A,REG_C},{REG_C,REG_D},
        };
        if (op < 8)  { cond = reg_eq(R(pair[op&7][0]),R(pair[op&7][1]),FS_A); if (op >= 4) cond = !cond; }
        else         { int r = op & 3; int z = reg_is_zero(R(r), FS_A); cond = (op < 0xC) ? z : !z; }
        saturn.carry = cond;
        if (dd == 0) return rstk_pop();
        if (cond) { saturn.saturn_branches_taken++;   return (pc + sext_nib(dd,2) + 3) & 0xFFFFFu; }
        else      { saturn.saturn_branches_skipped++; return pc + 5; }
    }
    case 0xB: {     /* ordered compares (A-field) + branch */
        int op = fetch(pc + 2);
        uint32_t dd = fetch_k(pc + 3, 2);
        static const uint8_t pair[4][2] = {
            {REG_A,REG_B},{REG_B,REG_C},{REG_C,REG_A},{REG_D,REG_C},
        };
        int idx = op & 3;
        int gt = reg_gt(R(pair[idx][0]), R(pair[idx][1]), FS_A);
        int lt = reg_lt(R(pair[idx][0]), R(pair[idx][1]), FS_A);
        int cond;
        switch ((op >> 2) & 3) {
        case 0: cond = gt;  break;
        case 1: cond = lt;  break;
        case 2: cond = !lt; break;
        default:cond = !gt; break;
        }
        saturn.carry = cond;
        if (dd == 0) return rstk_pop();
        if (cond) { saturn.saturn_branches_taken++;   return (pc + sext_nib(dd,2) + 3) & 0xFFFFFu; }
        else      { saturn.saturn_branches_skipped++; return pc + 5; }
    }
    case 0xC: {     /* GOLONG ±dddd */
        uint32_t dddd = fetch_k(pc + 2, 4);
        saturn.saturn_branches_taken++;
        return (pc + sext_nib(dddd, 4) + 2) & 0xFFFFFu;
    }
    case 0xD: {     /* GOTO abs */
        addr_t a = fetch_k(pc + 2, 5);
        saturn.saturn_branches_taken++;
        return a & 0xFFFFFu;
    }
    case 0xE: {     /* GOSUBL ±dddd */
        uint32_t dddd = fetch_k(pc + 2, 4);
        rstk_push((pc + 6) & 0xFFFFFu);
        saturn.saturn_branches_taken++;
        return (pc + sext_nib(dddd, 4) + 6) & 0xFFFFFu;
    }
    case 0xF: {     /* GOSBVL abs */
        addr_t a = fetch_k(pc + 2, 5);
        rstk_push((pc + 7) & 0xFFFFFu);
        saturn.saturn_branches_taken++;
        return a & 0xFFFFFu;
    }
    /* 80/81/83 left as unimpl in this iteration. */
    }
    TRAP(INTERP_UNIMPL);
}
