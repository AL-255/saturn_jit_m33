/* JIT dispatch loop.
 *
 * Three modes:
 *   - JIT_CACHE_OFF: translate the block at saturn.pc fresh every
 *     time, execute, discard. Measures the cost of translation +
 *     emitted code, no amortization.
 *   - JIT_CACHE_ON:  hash-table cache keyed on Saturn start-PC. First
 *     visit pays translation; subsequent visits jump straight to the
 *     compiled trace.
 *   - (interpreter is a separate code path in bench_main)
 *
 * The dispatcher executes Saturn ops until `budget` is reached, an
 * unsupported opcode forces fallback to the interpreter, or PC walks
 * outside any mapped region.
 *
 * `compare_state` helps the cross-validator: it returns the offset of
 * the first byte where two saturn_t snapshots differ (or -1 if
 * identical), so when interp/JIT diverge we know what field broke. */

#ifndef JIT_DISPATCH_H
#define JIT_DISPATCH_H

#include <stdint.h>
#include "saturn_state.h"
#include "saturn_interp.h"

typedef enum { JIT_CACHE_OFF, JIT_CACHE_ON } jit_mode_t;

typedef struct jit_stats_s {
    uint32_t blocks_translated;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t cache_evicts;
    uint32_t bytes_emitted;
    uint32_t interp_fallback_ops;
} jit_stats_t;

void   jit_init(void *cache_buf, uint32_t cache_bytes);
void   jit_reset(jit_mode_t mode);
const jit_stats_t *jit_get_stats(void);

interp_status_t jit_run(uint64_t budget);

int compare_state(const saturn_t *a, const saturn_t *b);

#endif
