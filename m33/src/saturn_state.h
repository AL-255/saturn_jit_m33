/* Slimmed Saturn state for the bare-metal M33 JIT/interpreter.
 *
 * Compared to x48ng's saturn_t, we drop everything tied to the HP 48's
 * peripherals (display, audio, serial, timers, card, IR, bus config,
 * keyboard scan controller). The bench harness drives keyboard input
 * directly via `in[]`, and we keep ST/PSTAT/RSTK/CARRY/HEXMODE exactly
 * as the ISA defines them so JIT-emitted code and the interpreter
 * share invariants.
 *
 * Field codes match the ISA spec (see ISA.md §2). Register A,B,C,D
 * each hold 16 nibbles laid out little-end-first: reg[r][0] = nibble 0
 * = least significant. */

#ifndef SATURN_STATE_H
#define SATURN_STATE_H

#include <stdint.h>
#include <stdbool.h>

typedef uint8_t  nibble_t;
typedef uint32_t addr_t;     /* 20-bit Saturn address, stored in low 20 bits of 32 */

#define NIB_PER_REG  16

enum saturn_reg  { REG_A = 0, REG_B = 1, REG_C = 2, REG_D = 3 };
enum saturn_st   { ST_XM = 0, ST_SB = 1, ST_SR = 2, ST_MP = 3 };
enum field_code  {
    FS_P   = 0,
    FS_WP  = 1,
    FS_XS  = 2,
    FS_X   = 3,
    FS_S   = 4,
    FS_M   = 5,
    FS_B   = 6,
    FS_W   = 7,
    FS_A   = 15,
    FS_IN  = 16,
    FS_OUT = 17,
    FS_OUTS= 18,
};

#define NB_RSTK   8
#define NB_PSTAT 16

typedef struct saturn_state_s {
    /* Working registers: 4 × 16 nibbles. */
    nibble_t reg[4][NIB_PER_REG];
    /* Scratch R0..R4. */
    nibble_t reg_r[5][NIB_PER_REG];

    /* Data pointers (20-bit). */
    addr_t   d[2];

    /* Field pointer (0..15). */
    nibble_t p;

    /* Program counter (20-bit). */
    addr_t   pc;

    /* IO registers. */
    nibble_t in[4];
    nibble_t out[3];

    /* Hardware status flags {XM,SB,SR,MP}. */
    nibble_t st[4];

    /* Program status (16 application flags). */
    uint8_t  pstat[NB_PSTAT];

    /* Return stack. Push past 7 shift-drops the oldest. */
    addr_t   rstk[NB_RSTK];
    int8_t   rstk_ptr;        /* -1 when empty */

    uint8_t  carry;
    uint8_t  hexmode;         /* 10 = BCD, 16 = HEX */

    uint8_t  kbd_pending;     /* set by benchmark keyboard generator */

    /* Memory base pointers. ROM is the program; RAM is read-write. */
    const uint8_t *rom;       /* nibble per byte, low nibble used */
    uint32_t       rom_size;  /* in nibbles */
    uint8_t       *ram;
    uint32_t       ram_base;  /* Saturn address of ram[0] */
    uint32_t       ram_size;  /* in nibbles */
    /* Derived fields for the INLINE_DAT W bounds check fast path.
     * Caller (jit_run / reset_for_workload) must keep these consistent
     * with ram/ram_base/ram_size. */
    uint32_t       ram_dat_bound;        /* ram_base + ram_size - 4 */
    uintptr_t      ram_minus_ram_base;   /* (uintptr_t)ram - ram_base */

    /* Bench counters - filled by interpreter & JIT, read by main. */
    uint64_t       saturn_ops;
    uint64_t       saturn_branches_taken;
    uint64_t       saturn_branches_skipped;

    /* Remaining op budget for the current jit_run() invocation. When
     * block linking is enabled, JIT-emitted blocks subtract their own
     * block_ops from this field in their epilogue and exit to the C
     * dispatcher when it goes non-positive. Initialised by jit_run. */
    int32_t        budget_remaining;
} saturn_t;

extern saturn_t saturn;

#endif
