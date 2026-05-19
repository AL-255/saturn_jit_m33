/* Field-arithmetic helpers, modeled on x48ng's registers.c. The body
 * is intentionally a near-copy because (a) it's the canonical
 * semantics our JIT must match, and (b) we want both interpreter and
 * JIT-emitted code to fall back to these helpers for the slow paths
 * (variable-length fields, P-dependent fields, etc).
 *
 * All ops operate on the field selected by `code` (FS_*). Functions
 * that update CARRY do so in saturn.carry. */

#ifndef SATURN_REGISTERS_H
#define SATURN_REGISTERS_H

#include "saturn_state.h"

int  field_start(int code);
int  field_end(int code);

void reg_add(nibble_t *res, const nibble_t *a, const nibble_t *b, int code);
void reg_sub(nibble_t *res, const nibble_t *a, const nibble_t *b, int code);
void reg_add_const(nibble_t *r, int code, int val);   /* always hex base */
void reg_sub_const(nibble_t *r, int code, int val);   /* always hex base */
void reg_inc(nibble_t *r, int code);
void reg_dec(nibble_t *r, int code);
void reg_comp1(nibble_t *r, int code);
void reg_comp2(nibble_t *r, int code);
void reg_zero(nibble_t *r, int code);
void reg_or(nibble_t *res, const nibble_t *a, const nibble_t *b, int code);
void reg_and(nibble_t *res, const nibble_t *a, const nibble_t *b, int code);
void reg_copy(nibble_t *to, const nibble_t *from, int code);
void reg_xchg(nibble_t *r1, nibble_t *r2, int code);
void reg_shl(nibble_t *r, int code);
void reg_shr(nibble_t *r, int code);
void reg_shlc(nibble_t *r, int code);
void reg_shrc(nibble_t *r, int code);
void reg_shrb(nibble_t *r, int code);

int  reg_is_zero(const nibble_t *r, int code);
int  reg_eq(const nibble_t *a, const nibble_t *b, int code);
int  reg_lt(const nibble_t *a, const nibble_t *b, int code);
int  reg_le(const nibble_t *a, const nibble_t *b, int code);
int  reg_gt(const nibble_t *a, const nibble_t *b, int code);
int  reg_ge(const nibble_t *a, const nibble_t *b, int code);

/* P-pointer arithmetic; used by the 8 0 9 'C+P+1' instruction.
 * Updates saturn.carry. */
void reg_add_p_plus_one(nibble_t *r);

#endif
