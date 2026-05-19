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

/* Compute the per-nibble carry-out via UBFX of bit 4 and wrap with
 * AND #0xf instead of IT-block conditional subtraction. Same number
 * of instructions per nibble as the IT approach but no IT, so M33's
 * pipeline doesn't stall on the predicated execution window. */
#ifndef JIT_OPT_FLAT_CARRY
#define JIT_OPT_FLAT_CARRY 1
#endif

/* Inline group-3 LC (load constant into C[P..P+n]) for small n.
 * Currently every LC goes through jit_lc_copy helper. For n ≤ 4 we
 * can emit a small inline sequence using STRB register-offset.
 *
 * Default OFF: measured 25-28% SLOWER on arith and calltree under
 * QEMU, ~same on countloop (which was supposed to benefit). QEMU
 * TCG likely models inline integer code less efficiently per-insn
 * than my model assumes. Cross-check still passes; the inline emit
 * is semantically correct. Re-evaluate on real hardware. */
#ifndef JIT_OPT_INLINE_LC
#define JIT_OPT_INLINE_LC 0
#endif

/* Maximum LC count we inline; larger LCs fall back to the helper. */
#ifndef JIT_OPT_INLINE_LC_MAX
#define JIT_OPT_INLINE_LC_MAX 4
#endif

/* Defer per-op carry-store to block exit. Each inline ADD/SUB/INC/DEC
 * currently emits `strb r2, [r4, #OFS(carry)]` (4 bytes, 1 cycle) at
 * the end of its emit. In a chain of arith ops, only the LAST carry
 * value matters before block exit — intermediate stores are dead.
 *
 * Default OFF under QEMU: measured ~7-20% slower across workloads
 * because QEMU's TCG host-side translation does NOT model store-buffer
 * stalls, so eliminating stores doesn't help host execution. On real
 * M33 hardware (where 4-5 fewer write-buffer flushes per loop iter
 * meaningfully reduces stall cycles) this is expected to be a win.
 *
 * Implementation maintains s_carry_dirty_r2 in the translator and
 * inserts emit_flush_carry() calls before any read of saturn.carry
 * (GOC/GONC) or before any helper BL that would clobber r2 (RTN
 * family). discard_pending_carry() is called when an op explicitly
 * overwrites saturn.carry (compare-branch, RTNSC/RTNCC, P±1). */
#ifndef JIT_OPT_DEFER_CARRY
#define JIT_OPT_DEFER_CARRY 0
#endif

/* Inline ?reg=0 / ?reg#0 A-field zero-test inside the compare-branch
 * emitter, instead of calling reg_is_zero. Uses CLZ to turn "all
 * nibbles 0" into a 0/1 condition without IT or branches.
 *   ldr  r0, [r4, #base]
 *   ldrb r1, [r4, #base+4]
 *   orrs r0, r0, r1
 *   clz  r0, r0          ; 32 iff r0 was 0, else < 32
 *   lsrs r0, r0, #5      ; 1 iff was 0, else 0
 * Helps countloop, which fires ?A=0 once per iter. */
#ifndef JIT_OPT_INLINE_ZEROTEST
#define JIT_OPT_INLINE_ZEROTEST 1
#endif

/* Inline 5-nibble DAT load/store (group 14x W-field) with bounds check.
 *
 * **STILL DEFAULT OFF** — even with the bounds-check fallback added,
 * the JIT-on path produces a stuck 38-byte block at PC=0 that loops
 * 200k times, while jit-off translates correctly (33k blocks of
 * ~336 bytes each, full br_taken count). The standalone translate
 * call from bench_main produces 432 bytes (correct) — only the
 * dispatcher's translation path differs. Root cause not yet found.
 *
 * Investigation:
 *   - Encoding verified vs arm-as (cmp r2,r3 = 0x429A, b.w hs cond=2 ok).
 *   - Cross-check at budget=200 passes (too few iters to hit OOB).
 *   - jit-off path: same translation, runs correctly.
 *   - jit-on output: blocks=1, bytes=38, hits=199999, br_taken=0.
 *   - Hypothesis (unverified): something about cache_insert + subsequent
 *     translation interactions. Maybe ops/code_hw wraps or struct
 *     alignment changes when memmix block grows past some threshold. */
#ifndef JIT_OPT_INLINE_DAT
#define JIT_OPT_INLINE_DAT 0
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
