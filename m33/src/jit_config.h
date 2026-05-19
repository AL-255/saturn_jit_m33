/* JIT optimization compile-time flags.
 *
 * Each optimization is independently togglable so we can A/B benchmark.
 * Set the flag to 0 to disable, 1 to enable. The default flags below
 * reflect the best-known configuration; override via -D on the command
 * line (e.g. `make CFLAGS_EXTRA=-DJIT_OPT_INLINE_INCDEC=0`).
 *
 * To benchmark an optimization in isolation:
 *   make clean && make CFLAGS_EXTRA=-DJIT_OPT_FOO=0 run > before.txt
 *   make clean && make CFLAGS_EXTRA=-DJIT_OPT_FOO=1 run > after.txt
 *   diff before.txt after.txt
 *
 * History (record big wins here so we can reproduce):
 *   2026-05-19 baseline-helpers     arith=18.7 memmix=18.5 calltree=15.3 countloop=17.9 (interp)
 *   2026-05-19 +cache-on            arith=10.6 memmix= 5.9 calltree=11.6 countloop=10.7
 *   2026-05-19 +INLINE_AZERO_COPY   arith= 6.7 memmix= 6.0 calltree=12.5 countloop=10.7
 *   2026-05-19 +INLINE_INCDEC_eebs  arith= 4.3 memmix= 5.0 calltree= 7.8 countloop= 7.0   <- last commit
 */

#ifndef JIT_CONFIG_H
#define JIT_CONFIG_H

/* Inline A-field zero / copy / exchange (group D) instead of helper
 * call. 5–20 unrolled ldrb/strb. */
#ifndef JIT_OPT_INLINE_AZERO_COPY
#define JIT_OPT_INLINE_AZERO_COPY 1
#endif

/* Inline INC / DEC (group E op 4..7, group C op C..F) with CBZ
 * early-exit. Preserves the helper's "stop at no-carry" behavior. */
#ifndef JIT_OPT_INLINE_INCDEC
#define JIT_OPT_INLINE_INCDEC 1
#endif

/* Inline 2-operand field add (group C op 0..B, A op 0..B with fs<8).
 * 5-nibble unrolled add with carry, CBZ early-exit. */
#ifndef JIT_OPT_INLINE_FIELD_ADD
#define JIT_OPT_INLINE_FIELD_ADD 1
#endif

/* Inline 2-operand field sub (group E op 0..B, B op 0..B with fs<8). */
#ifndef JIT_OPT_INLINE_FIELD_SUB
#define JIT_OPT_INLINE_FIELD_SUB 1
#endif

/* Block linking: at translate time, look up the static next-PC in the
 * cache. If hit, emit a direct B/BL to the cached block's entry
 * instead of returning to the C dispatcher. Saves the dispatcher
 * round-trip on every block boundary. */
#ifndef JIT_OPT_BLOCK_LINK
#define JIT_OPT_BLOCK_LINK 0
#endif

/* Use 4-byte LDR/STR for the first 4 nibbles of an A-field op (5
 * nibbles total). Each Saturn reg is at a 4-aligned offset so the
 * word access is safe. Saves ~50% of the load/store bytes in inline
 * zero/copy/xchg ops. */
#ifndef JIT_OPT_VECTOR_LDST
#define JIT_OPT_VECTOR_LDST 1
#endif

/* Move the saturn_ops += block_ops counter bump from JIT-emitted code
 * to the C dispatcher. Default OFF: measured neutral-to-slightly-
 * negative under QEMU (the dispatcher's added += costs as much as the
 * eliminated emit). Toggle to 1 to save ~24 bytes of code per block
 * if code size matters. */
#ifndef JIT_OPT_OPS_COUNTER_IN_C
#define JIT_OPT_OPS_COUNTER_IN_C 0
#endif

/* Use 16-bit Thumb T1 forms of ldrb/strb when offset/regs fit (rt,rn ≤ 7
 * and offset ≤ 31). Halves the size of inline-arith loads/stores when
 * the target register is REG_A or REG_B (offsets 0..20).
 *
 * Default OFF: produces mixed results under QEMU's TCG — arith is 10%
 * faster but calltree is 50% slower (consistent across 3+ runs). On
 * real M33 hardware T1 and T2 ldrb/strb cost the same cycle, so this
 * should be safe to enable on hardware; only QEMU benchmarks see the
 * regression. Toggle to 1 for arith-heavy workloads. */
#ifndef JIT_OPT_NARROW_LDST
#define JIT_OPT_NARROW_LDST 0
#endif

#endif
