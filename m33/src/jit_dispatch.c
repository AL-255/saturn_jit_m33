/* JIT dispatch loop + simple open-addressing code cache.
 *
 * Cache layout extended for block linking (JIT_OPT_BLOCK_LINK=1):
 *   Each cache_entry has a `link_target` word that holds the address
 *   of the next block to chain to (or the dispatcher_return_stub when
 *   no chain is available). The JIT-emitted block exit loads this
 *   word and BX's through it. The dispatcher patches link_target
 *   entries when a new block is installed that matches a pending
 *   target_pc.
 *
 * Slot reservation order (linking-enabled mode):
 *   1. cache_reserve(pc) → picks an empty slot, sets pc=that, marks
 *      link_target = stub, body_off = 0; returns slot index.
 *   2. translate(pc, &slot->link_target, &body_off_out) → emits code
 *      with the link target's RAM address baked in as a movw/movt
 *      literal, records the body offset as 'where to enter on chain'.
 *   3. cache_finalize(slot, code_off, ops, next_pc) → fills the rest
 *      of the slot and walks the cache to patch any entries whose
 *      next_pc matches our pc.
 */

#include "jit_dispatch.h"
#include "saturn_jit.h"
#include "saturn_interp.h"
#include "saturn_state.h"
#include "jit_config.h"
#include <stddef.h>
#include <string.h>

/* Offsets used by the inline-asm stub. offsetof would work but the inline
 * asm template needs literal constants for the LDR/STR immediate, so we
 * pin the numeric values here and statically assert they match. */
#define SATURN_OFS_OPS    248
#define SATURN_OFS_BUDGET 272
_Static_assert(offsetof(saturn_t, saturn_ops)       == SATURN_OFS_OPS,
               "stub OFS(saturn_ops) drifted");
_Static_assert(offsetof(saturn_t, budget_remaining) == SATURN_OFS_BUDGET,
               "stub OFS(budget_remaining) drifted");

extern uint8_t _jit_cache_start[];
extern uint8_t _jit_cache_end[];

#define CACHE_SLOTS  256
#define EMPTY_PC     0xFFFFFFFFu
#define DYN_NEXT_PC  0xFFFFFFFEu

/* Hot cache entry: small, walked by cache_find. */
typedef struct cache_entry_s {
    uint32_t pc;
    uint16_t code_off;
    uint16_t code_hw;
    uint32_t ops;
} cache_entry_t;

/* Cold metadata for block linking. Only allocated when JIT_OPT_BLOCK_LINK
 * is on; cache_find never touches this, so cache_find's hot loop keeps
 * walking a compact 16-byte struct. */
#if JIT_OPT_BLOCK_LINK
typedef struct cache_link_meta_s {
    uint32_t  next_pc;          /* statically-known next PC, or DYN_NEXT_PC */
    uint16_t  body_off;         /* halfword offset where body starts (global) */
    uint16_t  chain_insn_off;   /* halfword offset of the patchable cross-chain
                                 * instruction sequence — first halfword of
                                 * `movw r2, ...`. 0 = no cross-chain to
                                 * patch (self-loop direct branch or dyn_end). */
    uintptr_t link_target;      /* patchable: next-block body|1 or stub */
} cache_link_meta_t;
static cache_link_meta_t s_links[CACHE_SLOTS];
#endif

static cache_entry_t s_table[CACHE_SLOTS];
static uint8_t      *s_code_buf;
static uint32_t      s_code_cap;
static uint32_t      s_code_pos;
static jit_mode_t    s_mode = JIT_CACHE_OFF;
static jit_stats_t   s_stats;

#if JIT_OPT_BLOCK_LINK && JIT_OPT_PATCH_CROSS_CHAIN
/* Overwrite a 4-byte indirect-chain prefix with a direct T4 B.W to
 * target_hw. Both arguments are global halfword indices in s_code_buf.
 * Caller must ensure chain_hw != 0 (i.e. a chain insn was actually
 * recorded for the slot). */
static void patch_chain_to_b_w(uint16_t chain_hw, uint16_t target_hw) {
    /* ARM-M PC at the branch insn = chain_addr + 4 bytes; imm32 is the
     * signed byte offset from that PC. (target_hw - chain_hw - 2) * 2. */
    int32_t off_hw = (int32_t)target_hw - (int32_t)chain_hw - 2;
    int32_t i32    = off_hw * 2;
    uint32_t s     = (i32 >> 24) & 1;
    uint32_t imm11 = (i32 >> 1) & 0x7FF;
    uint32_t imm10 = (i32 >> 12) & 0x3FF;
    uint32_t i1    = (i32 >> 23) & 1;
    uint32_t i2    = (i32 >> 22) & 1;
    uint32_t j1    = (~(i1 ^ s)) & 1;
    uint32_t j2    = (~(i2 ^ s)) & 1;
    uint16_t hi = 0xF000 | (s << 10) | imm10;
    uint16_t lo = 0x9000 | (j1 << 13) | (1 << 12) | (j2 << 11) | imm11;
    uint16_t *buf16 = (uint16_t *)s_code_buf;
    buf16[chain_hw]     = hi;
    buf16[chain_hw + 1] = lo;
    /* No DSB/ISB needed under QEMU TCG; for real M33 hardware the dispatch
     * loop already executes a function call (which is a fence) before the
     * patched block can be re-entered, so the prefetch buffer is naturally
     * flushed. */
}
#endif

/* dispatcher_return_stub: tail-called by chained block exit when
 * budget hasn't expired but the link target is the stub (i.e. no
 * cached chain destination). Pops the original prologue frame
 * (pushed by the C-dispatcher-entered block) and returns to the
 * C dispatcher with r0 holding the next Saturn PC.
 *
 * With JIT_OPT_HOIST_BUDGET_OPS the prologue pushes r4-r6/lr and
 * holds budget in r5, saturn_ops in r6. The stub must flush both to
 * the saturn struct and pop the matching reglist. Without HOIST the
 * prologue is just push {r4, lr}, so a plain pop {r4, pc} suffices.
 *
 * Naked + .thumb_func so GCC emits no prologue and the symbol is
 * tagged as Thumb. The address we put into link_target has the LSB
 * set so BX enters Thumb mode. */
#define STUB_STRINGIFY1(x) #x
#define STUB_STRINGIFY(x)  STUB_STRINGIFY1(x)

__attribute__((naked, used))
void jit_dispatcher_return_stub(void) {
#if JIT_OPT_HOIST_BUDGET_OPS
    __asm__ volatile (
        ".thumb_func\n"
        "str.w r5, [r4, #" STUB_STRINGIFY(SATURN_OFS_BUDGET) "]\n"
        "str.w r6, [r4, #" STUB_STRINGIFY(SATURN_OFS_OPS)    "]\n"
        "pop  {r4, r5, r6, pc}\n");
#else
    __asm__ volatile (".thumb_func\n"
                      "pop {r4, pc}\n");
#endif
}

#if JIT_OPT_BLOCK_LINK
static uintptr_t stub_addr_thumb(void) {
    return ((uintptr_t)&jit_dispatcher_return_stub) | 1u;
}
#endif

static inline uint32_t hash_pc(uint32_t pc) {
    pc ^= pc >> 12; pc *= 0x9E3779B1u; pc ^= pc >> 16;
    return pc & (CACHE_SLOTS - 1);
}

static void reset_slots(void) {
    for (int i = 0; i < CACHE_SLOTS; i++) {
        s_table[i].pc = EMPTY_PC;
#if JIT_OPT_BLOCK_LINK
        s_links[i].link_target = stub_addr_thumb();
        s_links[i].next_pc = DYN_NEXT_PC;
        s_links[i].body_off = 0;
        s_links[i].chain_insn_off = 0;
#endif
    }
}

void jit_init(void *cache_buf, uint32_t cache_bytes) {
    s_code_buf = (uint8_t *)cache_buf;
    s_code_cap = cache_bytes;
    s_code_pos = 0;
    reset_slots();
    memset(&s_stats, 0, sizeof s_stats);
}
void jit_reset(jit_mode_t mode) {
    s_mode = mode;
    s_code_pos = 0;
    reset_slots();
    memset(&s_stats, 0, sizeof s_stats);
}
const jit_stats_t *jit_get_stats(void) { return &s_stats; }

#if !JIT_OPT_BLOCK_LINK
static int cache_insert(uint32_t pc, uint16_t code_off, uint16_t code_hw, uint32_t ops);
#endif

static int cache_find(uint32_t pc) {
    uint32_t h = hash_pc(pc);
    for (int probe = 0; probe < CACHE_SLOTS; probe++) {
        int idx = (h + probe) & (CACHE_SLOTS - 1);
        if (s_table[idx].pc == EMPTY_PC) return -1;
        if (s_table[idx].pc == pc) return idx;
    }
    return -1;
}

#if JIT_OPT_BLOCK_LINK
/* Reserve an empty slot for pc and return its index. Initialises
 * link_target to the stub so that even if translation aborts, any
 * existing chain to this pc lands safely back in the dispatcher. */
static int cache_reserve(uint32_t pc) {
    uint32_t h = hash_pc(pc);
    for (int probe = 0; probe < CACHE_SLOTS; probe++) {
        int idx = (h + probe) & (CACHE_SLOTS - 1);
        if (s_table[idx].pc == EMPTY_PC) {
            s_table[idx].pc = pc;
            s_table[idx].code_off = 0;
            s_table[idx].code_hw = 0;
            s_table[idx].ops = 0;
            s_links[idx].next_pc = DYN_NEXT_PC;
            s_links[idx].body_off = 0;
            s_links[idx].chain_insn_off = 0;
            s_links[idx].link_target = stub_addr_thumb();
            return idx;
        }
    }
    return -1;
}
#endif

/* Patch every cache entry whose next_pc matches `pc` so that its
 * link_target points at `target` (= newly-installed block's body with
 * Thumb LSB set). When PATCH_CROSS_CHAIN is on, also rewrite each
 * such block's chain insn to a direct B.W (saves the indirect load
 * + BX on every chained iter). */
#if JIT_OPT_BLOCK_LINK
static void cache_patch_links_to(uint32_t pc, uintptr_t target) {
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (s_table[i].pc != EMPTY_PC && s_links[i].next_pc == pc) {
            s_links[i].link_target = target;
#if JIT_OPT_PATCH_CROSS_CHAIN
            if (s_links[i].chain_insn_off != 0) {
                /* target = body_addr | 1 (thumb bit). Strip the bit
                 * to recover the byte address, then convert to global
                 * halfword index. */
                uintptr_t body_byte = target & ~(uintptr_t)1u;
                uint16_t target_hw = (uint16_t)(((uintptr_t)body_byte
                                                 - (uintptr_t)s_code_buf) / 2);
                patch_chain_to_b_w(s_links[i].chain_insn_off, target_hw);
            }
#endif
        }
    }
}

/* body_off is in halfwords RELATIVE to the block's start. Combine with
 * code_off (the block's halfword index in s_code_buf) so body_off in the
 * link_meta is a single global halfword index — that's what the link
 * patch arithmetic needs. */
static void cache_finalize(int slot, uint16_t code_off, uint16_t code_hw,
                           uint16_t body_off_local, uint16_t chain_insn_local,
                           uint32_t ops, uint32_t next_pc) {
    uint16_t body_off_global = (uint16_t)(code_off + body_off_local);
    uint16_t chain_insn_global = (chain_insn_local != 0)
                                 ? (uint16_t)(code_off + chain_insn_local)
                                 : 0;
    s_table[slot].code_off = code_off;
    s_table[slot].code_hw  = code_hw;
    s_table[slot].ops      = ops;
    s_links[slot].body_off = body_off_global;
    s_links[slot].chain_insn_off = chain_insn_global;
    s_links[slot].next_pc  = next_pc;

    if (next_pc != DYN_NEXT_PC) {
        int existing = cache_find(next_pc);
        if (existing >= 0 && s_links[existing].body_off != 0) {
            uint16_t target_hw = s_links[existing].body_off;
            s_links[slot].link_target =
                (uintptr_t)(s_code_buf + (uint32_t)target_hw * 2) | 1u;
#if JIT_OPT_PATCH_CROSS_CHAIN
            if (chain_insn_global != 0) {
                patch_chain_to_b_w(chain_insn_global, target_hw);
            }
#endif
        }
    }
    uintptr_t our_body = (uintptr_t)(s_code_buf + (uint32_t)body_off_global * 2) | 1u;
    cache_patch_links_to(s_table[slot].pc, our_body);
}
#endif

static void cache_flush(void) {
    reset_slots();
    s_code_pos = 0;
    s_stats.cache_evicts++;
}

interp_status_t jit_run(uint64_t budget) {
    interp_status_t status = INTERP_OK_BUDGET;
#if JIT_OPT_BLOCK_LINK
    saturn.budget_remaining = (int32_t)(budget > 0x7fffffff ? 0x7fffffff : budget);
#endif

    while (budget > 0 && status == INTERP_OK_BUDGET) {
        addr_t pc = saturn.pc;
        jit_block_fn_t fn = NULL;
        uint32_t block_ops = 0;

        if (s_mode == JIT_CACHE_ON) {
            int slot = cache_find(pc);
            if (slot >= 0 && s_table[slot].code_hw != 0) {
                s_stats.cache_hits++;
                fn = (jit_block_fn_t)
                    ((uintptr_t)(s_code_buf + (uint32_t)s_table[slot].code_off * 2) | 1u);
                block_ops = s_table[slot].ops;
            } else {
                s_stats.cache_misses++;
            }
        }

        if (!fn) {
            if (s_code_pos + 256 > s_code_cap) {
                if (s_mode == JIT_CACHE_ON) cache_flush();
                else                        s_code_pos = 0;
            }
            uint32_t used = 0;
            jit_block_meta_t meta = {0};
            uint8_t *here = s_code_buf + s_code_pos;

#if JIT_OPT_BLOCK_LINK
            int slot = -1;
            uintptr_t *link_ptr = NULL;
            if (s_mode == JIT_CACHE_ON) {
                slot = cache_reserve(pc);
                if (slot < 0) { cache_flush(); slot = cache_reserve(pc); }
                if (slot >= 0) link_ptr = &s_links[slot].link_target;
            }
            fn = saturn_jit_translate_linked(pc, here, s_code_cap - s_code_pos,
                                             &used, &meta, link_ptr);
#else
            fn = saturn_jit_translate(pc, here, s_code_cap - s_code_pos,
                                      &used, &meta);
#endif
            if (!fn) {
                interp_status_t one = saturn_run_interp(1);
                if (one != INTERP_OK_BUDGET) return one;
                s_stats.interp_fallback_ops++;
                if (budget > 0) budget--;
                continue;
            }
            s_stats.blocks_translated++;
            s_stats.bytes_emitted += used;
            block_ops = meta.saturn_ops;
            if (s_mode == JIT_CACHE_ON) {
#if JIT_OPT_BLOCK_LINK
                if (slot >= 0) {
                    cache_finalize(slot, (uint16_t)(s_code_pos / 2),
                                   (uint16_t)(used / 2),
                                   (uint16_t)(meta.body_off_hw),
                                   (uint16_t)(meta.chain_insn_hw),
                                   block_ops, meta.static_next_pc);
                }
#else
                int slot2 = cache_insert(pc, (uint16_t)(s_code_pos / 2),
                                         (uint16_t)(used / 2), block_ops);
                if (slot2 < 0) cache_flush();
#endif
                s_code_pos += used;
            }
        }

#if JIT_OPT_BLOCK_LINK
        /* In linked mode the block may chain across many compiled
         * blocks before returning. Reset budget tracker each call. */
        int32_t initial_budget = (int32_t)(budget > 0x7fffffff ? 0x7fffffff : budget);
        saturn.budget_remaining = initial_budget;
#if JIT_OPT_BUDGET_DRIVEN_OPS
        saturn.pc = fn(&saturn);
        uint32_t executed = (uint32_t)(initial_budget - saturn.budget_remaining);
        saturn.saturn_ops += executed;
#else
        addr_t before_ops_lo = (addr_t)saturn.saturn_ops;
        saturn.pc = fn(&saturn);
        uint32_t executed = (uint32_t)((uint32_t)saturn.saturn_ops - (uint32_t)before_ops_lo);
        if (executed == 0) executed = block_ops;
#endif
        if (budget >= executed) budget -= executed; else budget = 0;
#else
#if JIT_OPT_BUDGET_DRIVEN_OPS
        /* Track budget delta for saturn_ops attribution. */
        int32_t initial_budget = (int32_t)(budget > 0x7fffffff ? 0x7fffffff : budget);
        saturn.budget_remaining = initial_budget;
        saturn.pc = fn(&saturn);
        uint32_t executed = (uint32_t)(initial_budget - saturn.budget_remaining);
        if (executed == 0 && block_ops != 0) executed = block_ops;
        saturn.saturn_ops += executed;
        if (block_ops == 0 && executed == 0) {
            interp_status_t one = saturn_run_interp(1);
            if (one != INTERP_OK_BUDGET) return one;
            s_stats.interp_fallback_ops++;
            if (budget > 0) budget--;
        } else {
            if (budget >= executed) budget -= executed; else budget = 0;
        }
#else
        saturn.pc = fn(&saturn);
        if (block_ops == 0) {
            interp_status_t one = saturn_run_interp(1);
            if (one != INTERP_OK_BUDGET) return one;
            s_stats.interp_fallback_ops++;
            if (budget > 0) budget--;
        } else {
            if (budget >= block_ops) budget -= block_ops; else budget = 0;
        }
#endif
#endif
    }
    return status;
}

#if !JIT_OPT_BLOCK_LINK
/* Compat helper for the non-linking path. */
static int cache_insert(uint32_t pc, uint16_t code_off, uint16_t code_hw, uint32_t ops) {
    uint32_t h = hash_pc(pc);
    for (int probe = 0; probe < CACHE_SLOTS; probe++) {
        int idx = (h + probe) & (CACHE_SLOTS - 1);
        if (s_table[idx].pc == EMPTY_PC) {
            s_table[idx].pc       = pc;
            s_table[idx].code_off = code_off;
            s_table[idx].code_hw  = code_hw;
            s_table[idx].ops      = ops;
            return idx;
        }
    }
    return -1;
}
#endif

int compare_state(const saturn_t *a, const saturn_t *b) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    size_t bench_lo = offsetof(saturn_t, saturn_ops);
    size_t bench_hi = offsetof(saturn_t, budget_remaining) + sizeof(a->budget_remaining);
    for (size_t i = 0; i < sizeof(saturn_t); i++) {
        if (i >= bench_lo && i < bench_hi) continue;
        if (p[i] != q[i]) return (int)i;
    }
    return -1;
}
