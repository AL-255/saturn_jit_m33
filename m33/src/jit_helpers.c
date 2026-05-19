#include "jit_helpers.h"
#include "saturn_mem.h"

void jit_dat_load_w(nibble_t *r, addr_t d, saturn_t *st) {
    (void)st;
    for (int i = 0; i < 5; i++) r[i] = sat_fetch(d + i);
}
void jit_dat_store_w(addr_t d, const nibble_t *r, saturn_t *st) {
    (void)st;
    for (int i = 0; i < 5; i++) sat_store(d + i, r[i]);
}
void jit_dat_load_b(nibble_t *r, addr_t d, saturn_t *st) {
    (void)st;
    r[0] = sat_fetch(d); r[1] = sat_fetch(d + 1);
}
void jit_dat_store_b(addr_t d, const nibble_t *r, saturn_t *st) {
    (void)st;
    sat_store(d, r[0]); sat_store(d + 1, r[1]);
}
void jit_dat_load_n(nibble_t *r, addr_t d, saturn_t *st, int n) {
    (void)st;
    for (int i = 0; i < n; i++) r[i] = sat_fetch(d + i);
}
void jit_dat_store_n(addr_t d, const nibble_t *r, saturn_t *st, int n) {
    (void)st;
    for (int i = 0; i < n; i++) sat_store(d + i, r[i]);
}

addr_t jit_rstk_pop(saturn_t *st) {
    if (st->rstk_ptr < 0) return 0;
    return st->rstk[st->rstk_ptr--];
}
void jit_rstk_push(saturn_t *st, addr_t a) {
    if (st->rstk_ptr < NB_RSTK - 1) {
        st->rstk[++st->rstk_ptr] = a;
    } else {
        for (int i = 1; i < NB_RSTK; i++) st->rstk[i-1] = st->rstk[i];
        st->rstk[NB_RSTK-1] = a;
    }
}

addr_t jit_reg_a_to_addr(const nibble_t *r) {
    addr_t a = 0;
    for (int i = 4; i >= 0; i--) a = (a << 4) | (r[i] & 0xf);
    return a;
}
void jit_addr_to_reg_a(nibble_t *r, addr_t a) {
    for (int i = 0; i < 5; i++) r[i] = (a >> (i * 4)) & 0xf;
}

void jit_lc_copy(saturn_t *st, const uint8_t *src, int count) {
    nibble_t *C = st->reg[REG_C];
    int p = st->p & 0xf;
    for (int i = 0; i < count; i++) {
        C[(p + i) & 0xf] = src[i] & 0xf;
    }
}
