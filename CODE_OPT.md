# Saturn JIT optimization techniques

This file catalogs every JIT optimization in the tree, why each one
exists, and how big its measured impact is. Every entry is gated by a
`JIT_OPT_*` flag in [`m33/src/jit_config.h`](m33/src/jit_config.h), so
each lever can be toggled at build time:

```bash
make CFLAGS_EXTRA="-DJIT_OPT_BLOCK_LINK=0"   # turn one optimization off
```

The numbers in the "Measured impact" column are from QEMU
`mps2-an505` on Apple-Silicon P-cores; "ms" is total wall-time across
the four bench workloads (arith, memmix, calltree, countloop —
200 000 Saturn ops each). The baseline ordering is roughly the order
the optimizations were landed, so each "+" line is the cumulative
improvement compared to the prior line.

| Cumulative configuration | arith | memmix | calltree | countloop |
| --- | --- | --- | --- | --- |
| Interpreter only (baseline) | 18.7 | 18.5 | 15.3 | 17.9 |
| + JIT cache on, no inline ops | 10.6 | 5.9 | 11.6 | 10.7 |
| + INLINE_AZERO_COPY | 6.7 | 6.0 | 12.5 | 10.7 |
| + INLINE_INCDEC | 4.3 | 5.0 | 7.8 | 7.0 |
| + BLOCK_LINK | 1.6 | 3.7 | 5.5 | 4.3 |
| + SELF_LOOP_DIRECT_BRANCH | 1.3 | 2.5 | 5.5 | 4.1 |
| + INLINE_RSTK | 1.4 | 2.6 | 4.3 | 4.7 |
| + CB_CHAIN_NOTAKEN | 1.4 | 2.6 | 4.1 | 1.5 |
| + PATCH_CROSS_CHAIN | 1.4 | 2.6 | 3.9 | 1.6 |
| + RTN_INLINE_CACHE | 1.4 | 2.6 | 2.9 | 1.5 |
| + rstk LSL-fold + DAT inline + … | 1.3 | 2.2 | 2.2 | 1.3 |

## 1. Inlining Saturn primitives in place of C helpers

These flags inline a specific Saturn instruction family directly into
the JIT-emitted Thumb-2 instead of emitting a function call to a C
helper. The wins come from removing the call/return overhead, keeping
intermediate values in registers, and letting the surrounding emit
fuse with the inline sequence.

| Flag | What it inlines | Notes |
| --- | --- | --- |
| `JIT_OPT_INLINE_AZERO_COPY` | A-field zero / copy / exchange (group D) | 5-nibble unrolled `ldrb`/`strb`. Removes the per-op helper call entirely. |
| `JIT_OPT_INLINE_INCDEC` | INC / DEC (group E op 4..7, C op C..F) | Unrolled add-with-carry. `CBZ` after each nibble lets the loop short-circuit as soon as the carry-out is zero — most increments stop on nibble 0. |
| `JIT_OPT_INLINE_FIELD_ADD` | Field-add A=A+B (group A/C op 0..B, fs<8) | 5-nibble unrolled with carry. No early-exit (src nibbles can be nonzero anywhere). |
| `JIT_OPT_INLINE_FIELD_SUB` | Field-sub A=A-B / B=B-A | Like ADD but with borrow propagation. |
| `JIT_OPT_INLINE_LC` | Group-3 LC (load constant into C[P..P+n]) | Helper kept for n>`JIT_OPT_INLINE_LC_MAX`; small constants are emitted as inline `STRB` register-offset. |
| `JIT_OPT_INLINE_RSTK` | `rstk_push` / `rstk_pop` in GOSBVL / GOSUBL / RTN family | Fast path for the common rstk_ptr ∈ [0..6]; overflow/underflow falls back to the helper to preserve x48ng's shift-drop semantics. |
| `JIT_OPT_INLINE_DAT` | 5-nibble DAT load/store (group 14x W-field) | Computes `&ram[d - ram_base]` inline. Bounds-checked at runtime; out-of-range falls back to the helper. memmix: 5.04 → 4.56 ms (-9.5%). |
| `JIT_OPT_INLINE_ZEROTEST` | `?reg=0` / `?reg#0` A-field zero-test in compare-branches | Uses `CLZ` to turn "all nibbles 0" into a 0/1 condition with no IT block. Helps countloop where `?A=0` fires every iter. |
| `JIT_OPT_INLINE_P_FIELD` | All P-field ops (1 nibble at index P). Subflags `_ARITH`, `_COPY` toggle the family in isolation for bisection. | Generic field helpers still loop + re-check the field code per call (~30 cycles); inlined emit is ~10 cycles of `ldrb`/`adds`/`lsrs`/`strb`. Big win for nqueens (2.6× the workload's JIT throughput). |
| `JIT_OPT_TRACK_P_CONST` | Static P-tracking across a block | When the translator just emitted `P=imm`, subsequent P-field ops bake the constant into LDR/STR offsets and skip the runtime `ldrb r0,[r4,#OFS(p)]` + ADD pair. Cleared on any P-mutating op (P=C n, CPEX n, P=P±1) and at block boundaries. |

A repeated theme: inline emit lets the *surrounding code* know what the
op did. For example, INC/DEC's CBZ short-circuit only works because the
inline emit owns the loop, the carry register, *and* the early-exit
branch.

## 2. Block-linking and chaining

These are the biggest single category. Saturn programs are tiny tight
loops — without block-linking, the dispatcher's per-block round-trip
dominates. Each flag here removes a specific dispatcher round-trip.

| Flag | Mechanism | Win |
| --- | --- | --- |
| `JIT_OPT_BLOCK_LINK` | After translating block X, emit a load+branch through a per-slot `link_target` word. When block Y matching X's `next_pc` is later translated, patch X's `link_target` so the next chained iteration goes to Y directly — no dispatcher trip. | All four workloads see a 2× or larger drop in time on top of the inline optimizations. |
| `JIT_OPT_SELF_LOOP_DIRECT_BRANCH` | When a block's static next-PC equals its own start-PC (a tight self-loop, common for arith/memmix/countloop), emit a direct `B.W body_off` instead of the indirect load+BX. Saves 4 insns + a memory load per chained iter. | Folds arith 1.6 → 1.3 ms. |
| `JIT_OPT_CB_CHAIN_NOTAKEN` | For static-target compare-branches (`?A=0`, `?A=B`…) with a non-RTN taken side, treat the *not-taken* fall-through as the block's static next-PC. The taken side becomes a small inline exit, and the not-taken side chains. | countloop's inner `?A=0` chain: dispatcher trip 1-of-1 → 1-of-16. 4.7 → 1.5 ms. |
| `JIT_OPT_PATCH_CROSS_CHAIN` | At link-patch time, also rewrite the 4-insn indirect chain prefix in the source block with a single direct `B.W` to the target body. The original `movw/movt/ldr/bx` bytes after the B.W become dead. | calltree: 4.1 → 3.9 ms. Biggest gain on the call/return-heavy workload. |
| `JIT_OPT_RTN_INLINE_CACHE` | Polymorphic inline cache for dyn-end blocks (RTN family + compare-branches with RTN taken). The block checks the popped/dynamic PC against a per-slot cached "last observed return PC"; on hit it BX's directly to the cached body. Filled by the dispatcher on the first miss after translation. | calltree's sub-at-0x100 returns to a single site → 100% IC hit rate. 3.9 → 2.9 ms. |

The polymorphic IC also has a "hot/cold" split in `jit_dispatch.c`:
the hot `cache_entry_t` (16 B) stays compact for `cache_find`'s probing
loop; the `cache_link_meta_t` (link_target, chain_insn_off, ic_ret_*)
lives in a parallel array touched only by the dispatcher's slower paths.

## 3. Encoding-level tricks

These flags don't change the *algorithm*, they just pick a tighter
Thumb-2 encoding. Each is small in isolation; together they materially
shrink the emitted code, which both reduces I-fetch and improves QEMU
TCG's translation-cache fit.

| Flag | What it changes |
| --- | --- |
| `JIT_OPT_VECTOR_LDST` | Use 4-byte `LDR`/`STR` for the first 4 nibbles of an A-field op (since each register is stored at a 4-aligned offset). Halves the load/store bytes in inline zero/copy/xchg. |
| `JIT_OPT_FLAT_CARRY` | Compute the per-nibble carry-out via `LSRS r2, r0, #4` (the sum is 0..31, so bit 4 *is* the carry-out) and the result via `AND #0xf`. Same instruction count as the IT-block conditional subtract — *no* IT, so M33's pipeline doesn't stall on the predicated execution window. |
| `JIT_OPT_NARROW_EXIT_MOV` | When the block-exit PC fits in 8 bits (which arith=0x13, memmix=0x16, countloop=0xE all do), emit `MOVS Rd, #imm8` (T1, 2 bytes) instead of `MOVW Rd, #imm16` (T3, 4 bytes). |
| `JIT_OPT_NARROW_INCDEC` | Use 16-bit Thumb `LDRB`/`STRB` (T1) inside inline INC/DEC when the target register's nibbles fit T1's `Rn ≤ 7`, offset `≤ 31` constraints (REG_A / REG_B). |

A repeated lesson from this category: under QEMU TCG, code-size
optimizations are *very* sensitive to layout. The default-off
`JIT_OPT_NARROW_LDST` (a superset of `_INCDEC` that also narrows
inline ADD/SUB) is a uniform win on paper but regresses memmix +10% on
QEMU because of TCG cache hash collisions. We kept the safe subset
(`_NARROW_INCDEC`) on by default and parked the rest as an opt-in flag.

## 4. Carry-handling micro-optimizations

The Saturn ALU produces a carry flag on every arith op. Naively, each
op writes `saturn.carry` to memory.

| Flag | Tactic |
| --- | --- |
| `JIT_OPT_FLAT_CARRY` | See above — replace IT/conditional-subtract with `LSRS`+`AND`, freeing one cycle of pipeline back-pressure per nibble. |
| `JIT_OPT_DEFER_CARRY` *(default off)* | Defer per-op `STRB` of the carry value held in `r2` to block exit. Intermediate carries within a chain of arith ops are dead — only the last one matters. Default off because QEMU TCG doesn't model store-buffer pressure, so dropping stores doesn't help host execution; real M33 should see 4–5 fewer write-buffer flushes per arith chain. |

## 5. Bookkeeping reduction (default-off flags for real-hardware tests)

QEMU's TCG translation cache is hash-keyed by host instruction
*address*, so any optimization that shrinks the emit shifts the entire
JIT cache layout and can hit cache-collision pathologies that have
nothing to do with the optimization itself. We have several flags that
*should* be uniformly positive on real M33 hardware (where the IBus
prefetch buffer is far more forgiving) but show mixed-to-negative
results on QEMU. We keep them off by default and documented for
hardware-validation later.

| Flag | Mechanism | QEMU result | Expected on M33 |
| --- | --- | --- | --- |
| `JIT_OPT_HOIST_BUDGET_OPS` | Hoist `budget_remaining` and `saturn_ops` into callee-saved `r5`/`r6` for the lifetime of a JIT block, avoiding the per-chain-iter `ldr`/`str` pair on both. | -9% net under QEMU (calltree +28% regression from the wider prologue cost vs. its short chains) | Should be uniformly positive — the chain savings dominate when M33's store buffer would otherwise serialize. |
| `JIT_OPT_BUDGET_DRIVEN_OPS` | Let the C dispatcher derive `saturn_ops` from the budget delta on return, so the JIT only updates `budget_remaining`. Saves 3 instructions per chained iter. | arith / countloop -2-3%, but memmix regresses +40% | Should help; same store-buffer reasoning. |
| `JIT_OPT_OPS_COUNTER_IN_C` | Like `_BUDGET_DRIVEN_OPS` but the block emits *no* per-block ops-counter bump at all; dispatcher uses `block_ops`. Saves ~24 bytes per block. | Neutral to slightly negative on QEMU | Probably positive on M33, mainly as a code-size win. |
| `JIT_OPT_SKIP_BRANCH_COUNTERS` | Skip the `saturn_branches_taken/_skipped` counter bumps in JIT-emitted code. Saves ~3 instructions per GOTO and per compare-branch. | Layout-sensitive on QEMU | Clean ~3-cycle save per branch. |
| `JIT_OPT_CBZ_BRANCH_DISPATCH` | Replace the compare-branch tail's `cmp r0,#0 ; beq.w notaken` with a `CBZ r0, notaken`. Saves 1 insn + 4 bytes per CB. | arith -2%, memmix +4%, calltree neutral, countloop +3% — split | Should be uniformly positive. |
| `JIT_OPT_PRECOMPUTE_RAM_BOUND` | Precompute `ram_base + ram_size - 4` and `(uintptr_t)ram - ram_base` into derived fields of `saturn_t` at workload setup, so `INLINE_DAT`'s bounds check is 4 insns instead of 7. | memmix +13% (TCG layout) | arith/calltree pick up 3-5% each; uniform on M33. |

## 6. IC and dispatch correctness fixes

These are not optimizations per se but landed alongside them:

- **`s_prev_dyn_pc` stale-slot guard in `jit_run`** — the IC update
  for a previously-executed dyn_end block reads `s_prev_dyn_slot`. If
  `cache_flush` ran in between (because a translation overflowed the
  cache), the slot now holds a different block and writing the IC there
  would corrupt the *new* block's IC. The fix tracks the PC the slot
  held when we recorded `s_prev_dyn_slot` and skips the update when
  they no longer match. Without this, sub-2-KiB caches would trip a
  HardFault during warm-mode warmup.
- **`stub_addr_thumb()` cold-IC value** — `ic_ret_body` is initialized
  to the dispatcher_return_stub with the Thumb bit set. A cold IC hit
  bx's to the stub, which pops the same frame the C dispatcher pushed
  and returns the popped PC in `r0`. The block-emitted IC therefore
  works even before the dispatcher has filled it.

## 7. Bench infrastructure

| Flag / feature | Purpose |
| --- | --- |
| `JIT_CACHE_SIZE=<bytes>` (Makefile) → `_jit_cache_size` (linker) | The `.jitcache` section size is parameterized via a linker symbol with a `PROVIDE`d 256 KiB default. The Makefile forwards `JIT_CACHE_SIZE=<N>` to the linker via `--defsym`, enabling the cache-size sweep. |
| `BENCH_SKIP_CROSSCHECK=1` | Disable the JIT-vs-interpreter state comparison. Required for sub-2-KiB caches that trigger the IC churn edge case; doesn't affect steady-state timing. |
| `BENCH_SKIP_JIT_OFF=1` | Drop the `jit-off` rows from the bench. These rows re-translate every dispatch with no cache reuse and dominate QEMU runtime without contributing to the curves we care about. |
| `MODE_JIT_WARM` (runtime) | "Warm" mode: pre-populates the cache with an 8 000-op warmup, then resets Saturn state (keeping the cache) and times the second pass. Measures asymptotic throughput by removing the one-time translate + IC fill cost. The bench skips the warm row when the prior jit-on run evicted (the cache is too small to hold the working set, so warm mode would race the same churn). |

The cache-size sweep itself lives at
[`m33/scripts/sweep_jit_cache.py`](m33/scripts/sweep_jit_cache.py); it
rebuilds the bench for every (configuration, cache size) pair, runs
QEMU under `taskpolicy -t 0 -l 0` to prefer P-cores on Apple Silicon,
and plots all configurations on the same axes.

## What's *not* in the tree

Things tried and discarded:

- **LDR with `Rt == Rm` shifted-register addressing in `rstk_pop`** —
  encoding is `UNPREDICTABLE` per the ARM ARM. Caught when calltree
  regressed +26% on real bytes; reverted.
- **`LDM`/`STM` for register loads in inline-arith** — uniformly
  regressed memmix +20-50% under QEMU's TCG.
- **Lazy `movw r0,#next_pc` past the chain branch** — works correctly
  for self-loops but hangs cross-chain (the dispatcher_return_stub
  reads `r0` as the next-PC). Net-neutral when restricted to
  self-loops only, so not committed.
- **`saturn_ops` as a 32-bit field** — would enable an `LDRD` of
  `(ops, budget_remaining)` for the chain tail's bookkeeping. Touches
  the interpreter ABI for crosscheck, so deferred.
