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
 *   2026-05-19 +INLINE_INCDEC_eebs  arith= 4.3 memmix= 5.0 calltree= 7.8 countloop= 7.0
 *   2026-05-19 +BLOCK_LINK          arith= 1.6 memmix= 3.7 calltree= 5.5 countloop= 4.3
 *   2026-05-19 +SELF_LOOP_DIRECT_B  arith= 1.3 memmix= 2.5 calltree= 5.5 countloop= 4.1
 *   2026-05-19 +INLINE_RSTK         arith= 1.4 memmix= 2.6 calltree= 4.3 countloop= 4.7
 *   2026-05-19 +CB_CHAIN_NOTAKEN    arith= 1.4 memmix= 2.6 calltree= 4.1 countloop= 1.5
 *   2026-05-19 +PATCH_CROSS_CHAIN   arith= 1.4 memmix= 2.6 calltree= 3.9 countloop= 1.6
 *   2026-05-19 +RTN_INLINE_CACHE    arith= 1.4 memmix= 2.6 calltree= 2.9 countloop= 1.5
 *   2026-05-19 +rstk_lsl_in_str     arith= 1.3 memmix= 2.5 calltree= 2.1 countloop= 1.4   <- last commit
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

/* Inline P-field arith and zero/copy/exch ops (groups A and B with
 * fs=0). P-field ops are 1-nibble; the generic field-arith helper
 * still runs a loop and re-checks the field code, so calling it costs
 * ~30-40 cycles per Saturn op. Inlined emit is ~10 cycles. Big win on
 * the nqueens workload, which is dominated by P-field arith. Split
 * into a master flag and sub-flags (arith / copy / compare) so a
 * specific sub-shape can be disabled for bisection without losing the
 * others. */
#ifndef JIT_OPT_INLINE_P_FIELD
#define JIT_OPT_INLINE_P_FIELD 1
#endif
#ifndef JIT_OPT_INLINE_P_FIELD_ARITH
#define JIT_OPT_INLINE_P_FIELD_ARITH 1
#endif
#ifndef JIT_OPT_INLINE_P_FIELD_COPY
#define JIT_OPT_INLINE_P_FIELD_COPY 1
#endif

/* Track P statically across a block. When the translator can prove
 * that P holds a known constant value (because the immediately-prior
 * Saturn op was `P=imm`), bake that constant into the LDR/STR offsets
 * of the inline P-field emit, saving the runtime `ldrb r0,[r4,#OFS(p)]`
 * + ADD pair. Cleared on any P-mutating op (P=C n, CPEX n, P=P±1) and
 * at block boundaries. Layered on top of INLINE_P_FIELD. */
#ifndef JIT_OPT_TRACK_P_CONST
#define JIT_OPT_TRACK_P_CONST 1
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
#define JIT_OPT_BLOCK_LINK 1
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

/* Use the T1 `MOVS Rd, #imm8` (2 bytes) form for block-exit PC setup
 * when the next PC fits in 8 bits. Saves 2 bytes per block-exit vs T3
 * MOVW (4 bytes). Helps tight loops where the loop_top PC is small
 * (arith loop_top=0x13, memmix loop_top=0x16, countloop=0xE/0x17). */
#ifndef JIT_OPT_NARROW_EXIT_MOV
#define JIT_OPT_NARROW_EXIT_MOV 1
#endif

/* Inline group-3 LC (load constant into C[P..P+n]) for small n.
 * Currently every LC goes through jit_lc_copy helper. For n ≤ 4 we
 * can emit a small inline sequence using STRB register-offset.
 *
 * Default ON: re-measured after the rest of the JIT settled. countloop
 * picks up ~8% (its restart LC fires every outer iter); arith / memmix
 * / calltree are neutral. Earlier benchmark showing arith -25% was
 * before the chain optimizations stabilised. */
#ifndef JIT_OPT_INLINE_LC
#define JIT_OPT_INLINE_LC 1
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

/* Inline rstk_push / rstk_pop in GOSBVL / GOSUBL / RTN / RTNSC / RTNCC.
 *
 * Default ON: re-measured after block linking and self-loop direct
 * branch landed. calltree drops from 5.65 → 4.34 ms (-23%) — clear win
 * for the call/return-heavy workload. countloop regresses ~12% under
 * QEMU TCG even though its emitted JIT code is bytewise identical
 * (1050 bytes either way), purely because the dispatcher binary grows
 * and shifts TCG cache layout. Net across all four workloads: -5.5%.
 * On real M33 hardware the inline win should be uniformly positive.
 *
 * Fast path (rstk_ptr in [0, 6]):
 *   push: ptr++; rstk[ptr] = addr
 *   pop:  result = rstk[ptr]; ptr--
 * Overflow (rstk_ptr == 7) and underflow (rstk_ptr < 0) fall back to
 * the helper to preserve x48ng's shift-drop / return-0 semantics. */
#ifndef JIT_OPT_INLINE_RSTK
#define JIT_OPT_INLINE_RSTK 1
#endif

/* Inline 5-nibble DAT load/store (group 14x W-field). Skips the
 * jit_dat_load_w / jit_dat_store_w helper call by computing
 * &ram[d - ram_base] inline and transferring 4 + 1 nibbles.
 *
 * Bounds-checked at runtime: compare offset (d - ram_base) against
 * (ram_size - 4), branch to helper fallback when the 5-byte window
 * would straddle the RAM boundary. The -4 is critical: the inline
 * fast path writes 5 bytes starting at offset, so we need
 *     offset + 5 <= ram_size  ⇔  offset <= ram_size - 5
 *     ⇔  offset < ram_size - 4    (BHS branches on offset >= ram_size-4)
 *
 * Earlier versions used the wrong condition (offset < ram_size) which
 * silently corrupted memory past ram_base+ram_size — specifically the
 * adjacent g_rom_buf in BSS, leading to mysterious jit-on translation
 * failures (translator reads zeros from g_rom_buf[0] and emits a tiny
 * RTNSXM block). The off-by-4 took two iterations to find.
 *
 * Helps memmix: 5.04 → 4.56 ms (-9.5%), 3.77× → 4.23× vs interp. */
#ifndef JIT_OPT_INLINE_DAT
#define JIT_OPT_INLINE_DAT 1
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
 * Default OFF: under QEMU TCG the per-workload picture is mixed —
 * arith −22%, memmix +10%, calltree ~same, countloop +15%. The arith
 * regression seems to come from its dense mix of ADD/SUB (which gain
 * the most from T1 but apparently translate slowly under TCG) plus
 * INC/DEC ops. INC/DEC alone is a clean win — see JIT_OPT_NARROW_INCDEC. */
#ifndef JIT_OPT_NARROW_LDST
#define JIT_OPT_NARROW_LDST 0
#endif

/* Narrow LDR/STRB only inside inline INC/DEC (a subset of NARROW_LDST).
 * INC/DEC's loads/stores touch one register's nibbles; for REG_A or
 * REG_B that fits T1 cleanly without the QEMU regression that ADD/SUB
 * shows. Default ON. */
#ifndef JIT_OPT_NARROW_INCDEC
#define JIT_OPT_NARROW_INCDEC 1
#endif

/* When a block's static next_pc is its own start_pc (i.e. the block ends
 * with a GOTO back to its start), emit a direct B.W to body_off instead
 * of loading the patchable link_target word and BX'ing through it. Saves
 * movw + movt + ldr + bx (12 bytes / 4 insns) and one extra memory load
 * per chained iteration in the tightest possible loops.
 *
 * Only activates for true self-loops detected at translate time; other
 * chain patterns (A → B, A → A by way of dispatcher, etc.) keep the
 * patchable indirect path. */
#ifndef JIT_OPT_SELF_LOOP_DIRECT_BRANCH
#define JIT_OPT_SELF_LOOP_DIRECT_BRANCH 1
#endif

/* For static-target compare-branches (?A=0, ?A=B, etc. — group 8A with
 * non-RTN taken side) treat the *not-taken* fall-through as the block's
 * static next_pc instead of returning BLK_END_DYN to the dispatcher on
 * both sides. The taken path becomes an inline exit (bump ops, sub
 * budget, pop {r4, pc}); the not-taken path continues translating past
 * the compare-branch and can chain through the linked tail.
 *
 * Big win for countloop: ?A=0 fires every inner iter; before this the
 * dispatcher round-tripped on every iteration, now 15/16 take the
 * chain path and only 1/16 (when A finally hits zero) exits to the
 * dispatcher. */
#ifndef JIT_OPT_CB_CHAIN_NOTAKEN
#define JIT_OPT_CB_CHAIN_NOTAKEN 1
#endif

/* For cross-block chains (block X → block Y where Y != X), the linked
 * tail normally emits a 4-instr indirect: movw/movt r2, &link_target_word;
 * ldr r2, [r2]; bx r2 (14 bytes). When Y is already finalized at the
 * time X's tail is emitted — or when Y is finalized later — overwrite
 * X's chain insn with a direct B.W to Y's body. The overwritten bytes
 * after the B.W are dead. Single 4-byte B.W instead of 4 instructions
 * per cross-chain execution.
 *
 * Self-loop chains (next_pc == start_pc) already emit a direct B.W at
 * translate time via JIT_OPT_SELF_LOOP_DIRECT_BRANCH; this is the
 * cross-block equivalent for the "block 3 → block 4 → block 2" shape
 * that dominates calltree's iter overhead. */
#ifndef JIT_OPT_PATCH_CROSS_CHAIN
#define JIT_OPT_PATCH_CROSS_CHAIN 1
#endif

/* Polymorphic inline cache for dyn_end blocks (RTN family + compare-
 * branches with RTN taken). After the helper / inline_rstk_pop sets
 * r0 to the dynamic return PC, the JIT-emitted block checks r0 against
 * a per-slot cached "last observed return PC". On a hit it branches
 * directly into the cached body. On a miss it falls through to the
 * normal dispatcher exit.
 *
 * The dispatcher updates the IC after each dyn_end fn() return: it
 * already does a cache_find on the new saturn.pc, so the cost is one
 * extra store pair per dyn_end iteration. For monomorphic call sites
 * (most real-world RTNs) this is ~100% hit rate.
 *
 * Big win for calltree where the sub at 0x100 is called from a single
 * place and always returns to pc=0x0f. */
#ifndef JIT_OPT_RTN_INLINE_CACHE
#define JIT_OPT_RTN_INLINE_CACHE 1
#endif

/* Update saturn.saturn_ops in the C dispatcher (from the budget delta
 * between entry and return) rather than emitting an ldr/add/str triplet
 * in every block tail. The JIT only updates saturn.budget_remaining.
 *
 * Default OFF: shrank the linked tail by 3 instructions per chained
 * iter as expected, and helped arith/countloop ~2-3% each, but memmix
 * regressed +40% under QEMU TCG (consistent across re-runs). The other
 * three workloads' chained inner loop tightened cleanly; memmix's
 * INLINE_DAT path apparently changes branch / TCG behavior when the
 * surrounding chain shrinks. Re-evaluate on real hardware. */
#ifndef JIT_OPT_BUDGET_DRIVEN_OPS
#define JIT_OPT_BUDGET_DRIVEN_OPS 0
#endif

/* Hoist saturn.budget_remaining and saturn.saturn_ops into callee-saved
 * registers (r5/r6) for the lifetime of a JIT block. Without this, the
 * linked-tail loop loads/stores both fields on every chained iteration.
 *
 * Default OFF: helps the chain-heavy workloads (arith -2%, memmix -1%,
 * countloop -4%) but regresses calltree +28% under QEMU because every
 * RTN dyn-ends back to the dispatcher, paying the bigger prologue cost
 * without amortizing it across a chain. The net is -9% overall.
 *
 * Likely a QEMU-TCG artifact: real M33 hardware should benefit more from
 * the chain savings (eliminating store-buffer round-trips) than it pays
 * for the prologue's extra ldr/str. Revisit on hardware. */
#ifndef JIT_OPT_HOIST_BUDGET_OPS
#define JIT_OPT_HOIST_BUDGET_OPS 0
#endif

/* Skip the JIT-emitted saturn_branches_taken / saturn_branches_skipped
 * counter bumps. Saves ~3 instructions per GOTO and per compare-branch
 * (taken or skipped), which fires every chained loop iteration. The
 * counters are only used by the benchmark's RESULT line; disabling
 * them changes the printed br_taken to 0 but doesn't affect any
 * functional or cross-check behaviour. */
#ifndef JIT_OPT_SKIP_BRANCH_COUNTERS
#define JIT_OPT_SKIP_BRANCH_COUNTERS 0
#endif

/* Replace `strb r0,[carry] ; cmp r0,#0 ; beq.w notaken` with
 * `strb r0,[carry] ; cbz r0, notaken` in compare-branch tails.
 * CBZ is T1 (2 bytes) and combines compare-with-zero + forward
 * branch into one instruction; saves 1 instruction + 4 bytes per
 * CB execution. Range ±126 bytes forward is plenty for the taken-
 * exit emit (~40 bytes typical).
 *
 * Default OFF: arith −2%, countloop +3%, memmix +4%, calltree
 * unchanged under QEMU TCG — same emit-layout sensitivity story.
 * Expected uniformly positive on real M33 hardware. */
#ifndef JIT_OPT_CBZ_BRANCH_DISPATCH
#define JIT_OPT_CBZ_BRANCH_DISPATCH 0
#endif

/* Precompute saturn.ram_dat_bound (= ram_base + ram_size - 4) and
 * saturn.ram_minus_ram_base (= (uintptr_t)ram - ram_base) at workload
 * setup. INLINE_DAT then needs only 2 ldrs + cmp + add (4 instrs)
 * for the bounds check vs the original 4 ldrs + subs + cmp + add (7).
 *
 * Default OFF: under QEMU TCG memmix still regresses +13% even with
 * the new fields appended to saturn_t (no offset shift). The emit
 * shrink alone (782 → 742 bytes for memmix) is enough to trigger
 * memmix's TCG-translation-cache layout sensitivity. arith / calltree
 * each pick up ~3-5% from this flag; on real M33 the win should be
 * uniform. */
#ifndef JIT_OPT_PRECOMPUTE_RAM_BOUND
#define JIT_OPT_PRECOMPUTE_RAM_BOUND 0
#endif

#endif
