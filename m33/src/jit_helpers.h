/* Out-of-line helpers callable from JIT-emitted code.
 *
 * Three categories:
 *  - mem ops: DAT load/store with a known size (2 or 5 nibbles
 *    typically; we also have a generic variable-length form for the
 *    field-coded 15x family)
 *  - RSTK push/pop (return-stack management with x48ng's shift-drop
 *    semantics when full)
 *  - misc state mutators that are awkward to inline (P=P+1 with carry,
 *    CLRST mask, packed C=ST/ST=C/CSTEX nibble↔bit conversions). */

#ifndef JIT_HELPERS_H
#define JIT_HELPERS_H

#include "saturn_state.h"

/* DAT memory traffic. `st` is &saturn (so the helpers see saturn.rom /
 * saturn.ram). reg points at the register's nibble array; d is the
 * 20-bit pointer value. */
void jit_dat_load_w(nibble_t *reg, addr_t d, saturn_t *st);   /* 5 nibs (A-field width) */
void jit_dat_store_w(addr_t d, const nibble_t *reg, saturn_t *st);
void jit_dat_load_b(nibble_t *reg, addr_t d, saturn_t *st);   /* 2 nibs (B-field) */
void jit_dat_store_b(addr_t d, const nibble_t *reg, saturn_t *st);
void jit_dat_load_n (nibble_t *reg, addr_t d, saturn_t *st, int n);
void jit_dat_store_n(addr_t d, const nibble_t *reg, saturn_t *st, int n);

/* Return stack. */
addr_t jit_rstk_pop(saturn_t *st);
void   jit_rstk_push(saturn_t *st, addr_t a);

/* 5-nibble addr unpack/pack used by RSTK=C / C=RSTK / D0=C etc. */
addr_t jit_reg_a_to_addr(const nibble_t *r);
void   jit_addr_to_reg_a(nibble_t *r, addr_t a);

/* Group 3 LC: copy `count` nibbles from `src` into saturn.reg[C] starting
 * at C[P], wrapping at 16. The source is a pointer into ROM (or any
 * memory) holding one nibble per byte. */
void   jit_lc_copy(saturn_t *st, const uint8_t *src, int count);

#endif
