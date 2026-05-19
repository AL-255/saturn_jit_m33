/* JIT dispatch loop + simple open-addressing code cache.
 *
 * The cache lives in the .jitcache RAM region (see m33.ld). Entries
 * are { saturn_pc, code_ptr, code_bytes }; the code itself is packed
 * after the entry table. We keep this simple for the first
 * benchmarkable cut:
 *   - Fixed entry-table size (CACHE_SLOTS)
 *   - Linear-probe hash on saturn_pc
 *   - When the code buffer runs out, we flush the cache and start over
 *
 * The dispatch loop:
 *   while (budget > 0):
 *     pc = saturn.pc
 *     compiled = cache_lookup(pc) || cache_install(translate(pc))
 *     if !compiled: status = INTERP_UNIMPL; break
 *     saturn.pc = compiled(&saturn)
 *     budget -= ops_translated
 *     # if JIT couldn't handle some op, fall back to interpreter for one step
 *
 * If translation returned NULL (unsupported start opcode), we run the
 * interpreter for one step then resume the JIT loop. This gives us
 * incremental opcode coverage — we add more JIT-supported opcodes
 * over time and rare ones stay on the interpreter path. */

#include "jit_dispatch.h"
#include "saturn_jit.h"
#include "saturn_interp.h"
#include "saturn_state.h"
#include <stddef.h>
#include <string.h>

/* Allocated by the linker. */
extern uint8_t _jit_cache_start[];
extern uint8_t _jit_cache_end[];

#define CACHE_SLOTS  256        /* power of two for cheap mask */

typedef struct cache_entry_s {
    uint32_t pc;            /* Saturn start PC, or 0xFFFFFFFF if empty */
    uint16_t code_off;      /* offset into code buffer, in halfwords */
    uint16_t code_hw;       /* size in halfwords */
    uint32_t ops;           /* ops translated (informational) */
} cache_entry_t;

#define EMPTY_PC 0xFFFFFFFFu

static cache_entry_t s_table[CACHE_SLOTS];
static uint8_t      *s_code_buf;
static uint32_t      s_code_cap;
static uint32_t      s_code_pos;
static jit_mode_t    s_mode = JIT_CACHE_OFF;
static jit_stats_t   s_stats;

static inline uint32_t hash_pc(uint32_t pc) {
    /* Mix and mask to CACHE_SLOTS. */
    pc ^= pc >> 12; pc *= 0x9E3779B1u; pc ^= pc >> 16;
    return pc & (CACHE_SLOTS - 1);
}

void jit_init(void *cache_buf, uint32_t cache_bytes) {
    s_code_buf = (uint8_t *)cache_buf;
    s_code_cap = cache_bytes;
    s_code_pos = 0;
    for (int i = 0; i < CACHE_SLOTS; i++) s_table[i].pc = EMPTY_PC;
    memset(&s_stats, 0, sizeof s_stats);
}
void jit_reset(jit_mode_t mode) {
    s_mode = mode;
    s_code_pos = 0;
    for (int i = 0; i < CACHE_SLOTS; i++) s_table[i].pc = EMPTY_PC;
    memset(&s_stats, 0, sizeof s_stats);
}
const jit_stats_t *jit_get_stats(void) { return &s_stats; }

/* Returns the matching slot index or -1. */
static int cache_find(uint32_t pc) {
    uint32_t h = hash_pc(pc);
    for (int probe = 0; probe < CACHE_SLOTS; probe++) {
        int idx = (h + probe) & (CACHE_SLOTS - 1);
        if (s_table[idx].pc == EMPTY_PC) return -1;
        if (s_table[idx].pc == pc) return idx;
    }
    return -1;
}

/* Inserts at the first empty slot reachable by probing. Returns slot
 * index, or -1 if the table is full (we just flush in that case). */
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

/* Flush both the entry table and the code buffer. Used when we run out
 * of room — simple, correct, and gives the benchmark a meaningful
 * "after-flush" measurement. */
static void cache_flush(void) {
    for (int i = 0; i < CACHE_SLOTS; i++) s_table[i].pc = EMPTY_PC;
    s_code_pos = 0;
    s_stats.cache_evicts++;
}

interp_status_t jit_run(uint64_t budget) {
    interp_status_t status = INTERP_OK_BUDGET;

    while (budget > 0 && status == INTERP_OK_BUDGET) {
        addr_t pc = saturn.pc;
        jit_block_fn_t fn = NULL;
        uint32_t block_ops = 0;
        uint16_t block_off_hw = 0;
        uint16_t block_size_hw = 0;

        if (s_mode == JIT_CACHE_ON) {
            int slot = cache_find(pc);
            if (slot >= 0) {
                s_stats.cache_hits++;
                fn = (jit_block_fn_t)
                    ((uintptr_t)(s_code_buf + (uint32_t)s_table[slot].code_off * 2) | 1u);
                block_ops = s_table[slot].ops;
                block_off_hw = s_table[slot].code_off;
                block_size_hw = s_table[slot].code_hw;
                (void)block_off_hw; (void)block_size_hw;
            } else {
                s_stats.cache_misses++;
            }
        }

        if (!fn) {
            /* Need to translate. Make sure we have room. */
            if (s_code_pos + 256 > s_code_cap) {
                /* For cache-off we just rewind to 0 (no state to preserve);
                 * for cache-on we flush the table to keep cache consistent. */
                if (s_mode == JIT_CACHE_ON) cache_flush();
                else                        s_code_pos = 0;
            }
            uint32_t used = 0;
            jit_block_meta_t meta = {0};
            uint8_t *here = s_code_buf + s_code_pos;
            fn = saturn_jit_translate(pc, here, s_code_cap - s_code_pos,
                                      &used, &meta);
            if (!fn) {
                /* Couldn't translate at this PC (unsupported start op).
                 * Fall back to interpreter for 1 step. */
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
                int slot = cache_insert(pc, (uint16_t)(s_code_pos / 2),
                                        (uint16_t)(used / 2), block_ops);
                if (slot < 0) cache_flush();
                s_code_pos += used;
            } else {
                /* Cache off: don't advance code_pos — overwrite next time. */
            }
        }

        /* Execute the compiled block. It returns the next Saturn PC. */
        saturn.pc = fn(&saturn);

        /* Block has already bumped saturn.saturn_ops internally. */
        if (block_ops == 0) {
            /* Translator produced a do-nothing block (unsupported at
             * first op). Single-step the interpreter to advance. */
            interp_status_t one = saturn_run_interp(1);
            if (one != INTERP_OK_BUDGET) return one;
            s_stats.interp_fallback_ops++;
            if (budget > 0) budget--;
        } else {
            if (budget >= block_ops) budget -= block_ops;
            else                     budget = 0;
        }
    }
    return status;
}

/* Byte-by-byte comparison of two saturn_t snapshots, ignoring
 * benchmark counters (which advance differently in JIT vs interp by a
 * single end-of-block bump). Returns -1 if identical (excluding
 * counters), or the offset of the first differing byte. */
int compare_state(const saturn_t *a, const saturn_t *b) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    size_t bench_lo = offsetof(saturn_t, saturn_ops);
    size_t bench_hi = offsetof(saturn_t, saturn_branches_skipped) + sizeof(a->saturn_branches_skipped);
    for (size_t i = 0; i < sizeof(saturn_t); i++) {
        if (i >= bench_lo && i < bench_hi) continue;
        if (p[i] != q[i]) return (int)i;
    }
    return -1;
}
