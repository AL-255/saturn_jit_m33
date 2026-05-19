/* Field-arithmetic helpers. Semantics match x48ng/src/core/registers.c
 * exactly so the JIT can call these for variable-field paths and the
 * interpreter behaves identically.
 *
 * Performance note: the field-start/end tables are tiny constants; the
 * inner loops are byte-wide; on Cortex-M33 each iteration is ~6
 * Thumb-2 instructions and the loops are short (typical fields are
 * 1-5 nibbles), so the function-call overhead dominates for small
 * fields. The JIT path elides these calls for fixed-field opcodes
 * (where start/end are known at translate time) and emits straight-
 * line Thumb-2. */

#include "saturn_registers.h"

#define NB_FIELDS 19

/* Index by FS_* code (0..18). -1 means "use saturn.p". Table values
 * match x48ng exactly. Codes 8..14 mirror 0..6. */
static const int8_t s_start[NB_FIELDS] = {
    -1, 0, 2, 0, 15, 3, 0, 0,
    -1, 0, 2, 0, 15, 3, 0,
    0,  /* FS_A */
    0,  /* FS_IN */
    0,  /* FS_OUT */
    0,  /* FS_OUTS */
};
static const int8_t s_end[NB_FIELDS] = {
    -1, -1, 2, 2, 15, 14, 1, 15,
    -1, -1, 2, 2, 15, 14, 1,
    4,  /* FS_A */
    3,  /* FS_IN */
    2,  /* FS_OUT */
    0,  /* FS_OUTS */
};

int field_start(int code) {
    int s = s_start[code];
    return (s < 0) ? saturn.p : s;
}
int field_end(int code) {
    int e = s_end[code];
    return (e < 0) ? saturn.p : e;
}

void reg_add(nibble_t *res, const nibble_t *a, const nibble_t *b, int code) {
    int s = field_start(code), e = field_end(code);
    int base = saturn.hexmode;       /* 10 or 16 */
    int c = 0;
    for (int i = s; i <= e; i++) {
        int t = a[i] + b[i] + c;
        if (t < base) { res[i] = t & 0xf; c = 0; }
        else          { res[i] = (t - base) & 0xf; c = 1; }
    }
    saturn.carry = c;
}

void reg_sub(nibble_t *res, const nibble_t *a, const nibble_t *b, int code) {
    int s = field_start(code), e = field_end(code);
    int base = saturn.hexmode;
    int c = 0;
    for (int i = s; i <= e; i++) {
        int t = a[i] - b[i] - c;
        if (t >= 0) { res[i] = t & 0xf; c = 0; }
        else        { res[i] = (t + base) & 0xf; c = 1; }
    }
    saturn.carry = c;
}

void reg_add_const(nibble_t *r, int code, int val) {
    int s = field_start(code), e = field_end(code);
    int c = val;
    for (int i = s; i <= e; i++) {
        int t = r[i] + c;
        if (t < 16) { r[i] = t & 0xf; c = 0; break; }
        else        { r[i] = (t - 16) & 0xf; c = 1; }
    }
    saturn.carry = c;
}

void reg_sub_const(nibble_t *r, int code, int val) {
    int s = field_start(code), e = field_end(code);
    int c = val;
    for (int i = s; i <= e; i++) {
        int t = r[i] - c;
        if (t >= 0) { r[i] = t & 0xf; c = 0; break; }
        else        { r[i] = (t + 16) & 0xf; c = 1; }
    }
    saturn.carry = c;
}

void reg_inc(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    int base = saturn.hexmode;
    int c = 1;
    for (int i = s; i <= e; i++) {
        int t = r[i] + c;
        if (t < base) { r[i] = t & 0xf; c = 0; break; }
        else          { r[i] = (t - base) & 0xf; c = 1; }
    }
    saturn.carry = c;
}

void reg_dec(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    int base = saturn.hexmode;
    int c = 1;
    for (int i = s; i <= e; i++) {
        int t = r[i] - c;
        if (t >= 0) { r[i] = t & 0xf; c = 0; break; }
        else        { r[i] = (t + base) & 0xf; c = 1; }
    }
    saturn.carry = c;
}

void reg_comp1(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    int base = saturn.hexmode;
    for (int i = s; i <= e; i++)
        r[i] = ((base - 1) - r[i]) & 0xf;
    saturn.carry = 0;
}

void reg_comp2(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    int base = saturn.hexmode;
    int c = 1;
    int any = 0;
    for (int i = s; i <= e; i++) {
        int t = (base - 1) - r[i] + c;
        if (t < base) { r[i] = t & 0xf; c = 0; }
        else          { r[i] = (t - base) & 0xf; c = 1; }
        any += r[i];
    }
    saturn.carry = (any != 0);
}

void reg_zero(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = s; i <= e; i++) r[i] = 0;
}

void reg_or(nibble_t *res, const nibble_t *a, const nibble_t *b, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = s; i <= e; i++) res[i] = (a[i] | b[i]) & 0xf;
}

void reg_and(nibble_t *res, const nibble_t *a, const nibble_t *b, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = s; i <= e; i++) res[i] = (a[i] & b[i]) & 0xf;
}

void reg_copy(nibble_t *to, const nibble_t *from, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = s; i <= e; i++) to[i] = from[i];
}

void reg_xchg(nibble_t *r1, nibble_t *r2, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = s; i <= e; i++) { nibble_t t = r1[i]; r1[i] = r2[i]; r2[i] = t; }
}

void reg_shl(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = e; i > s; i--) r[i] = r[i-1];
    r[s] = 0;
}

void reg_shlc(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    nibble_t t = r[e];
    for (int i = e; i > s; i--) r[i] = r[i-1];
    r[s] = t;
}

void reg_shr(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    if (r[s] & 0xf) saturn.st[ST_SB] = 1;
    for (int i = s; i < e; i++) r[i] = r[i+1];
    r[e] = 0;
}

void reg_shrc(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    nibble_t t = r[s];
    for (int i = s; i < e; i++) r[i] = r[i+1];
    r[e] = t;
    if (t) saturn.st[ST_SB] = 1;
}

void reg_shrb(nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    int sb = 0;
    for (int i = e; i >= s; i--) {
        int t = (((r[i] >> 1) & 7) | (sb << 3)) & 0xf;
        sb = r[i] & 1;
        r[i] = t;
    }
    if (sb) saturn.st[ST_SB] = 1;
}

int reg_is_zero(const nibble_t *r, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = s; i <= e; i++) if (r[i] & 0xf) return 0;
    return 1;
}

int reg_eq(const nibble_t *a, const nibble_t *b, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = s; i <= e; i++) if ((a[i] & 0xf) != (b[i] & 0xf)) return 0;
    return 1;
}

int reg_lt(const nibble_t *a, const nibble_t *b, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = e; i >= s; i--) {
        int x = a[i] & 0xf, y = b[i] & 0xf;
        if (x < y) return 1;
        if (x > y) return 0;
    }
    return 0;
}
int reg_gt(const nibble_t *a, const nibble_t *b, int code) {
    int s = field_start(code), e = field_end(code);
    for (int i = e; i >= s; i--) {
        int x = a[i] & 0xf, y = b[i] & 0xf;
        if (x > y) return 1;
        if (x < y) return 0;
    }
    return 0;
}
int reg_le(const nibble_t *a, const nibble_t *b, int code) { return !reg_gt(a,b,code); }
int reg_ge(const nibble_t *a, const nibble_t *b, int code) { return !reg_lt(a,b,code); }

void reg_add_p_plus_one(nibble_t *r) {
    /* Field A (0..4), always hex. carry-in = P+1. */
    int c = saturn.p + 1;
    for (int i = 0; i <= 4; i++) {
        int t = r[i] + c;
        if (t < 16) { r[i] = t & 0xf; c = 0; }
        else        { r[i] = (t - 16) & 0xf; c = 1; }
    }
    saturn.carry = c;
}
