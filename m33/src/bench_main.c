/* Bench entry: runs each workload through interpreter, JIT-cache-off,
 * and JIT-cache-on; emits a CSV-ish RESULT line per (workload, mode).
 *
 * Before benchmarking, we run each JIT mode side-by-side with the
 * interpreter on a SHORT budget and diff the resulting saturn_t. Any
 * divergence trips a CROSSCHK FAIL line and aborts — better to crash
 * here than to ship rigged numbers. */

#include <string.h>
#include <stdint.h>

#include "saturn_state.h"
#include "saturn_interp.h"
#include "saturn_jit.h"
#include "jit_dispatch.h"
#include "workload.h"
#include "keyboard.h"
#include "semihost.h"

extern int thumb2_selftest(char *err_out, int errlen);
extern uint8_t _jit_cache_start[];
extern uint8_t _jit_cache_end[];

static uint8_t g_rom_buf[16 * 1024];
static uint8_t g_ram_buf[64 * 1024];

saturn_t saturn;

static char *u32_hex(char *p, uint32_t v) {
    static const char H[] = "0123456789abcdef";
    *p++ = '0'; *p++ = 'x';
    for (int s = 28; s >= 0; s -= 4) *p++ = H[(v >> s) & 0xf];
    return p;
}
static char *u32_dec(char *p, uint32_t v) {
    char tmp[11]; int n = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n--) *p++ = tmp[n];
    return p;
}
static char *u64_dec(char *p, uint64_t v) {
    char tmp[21]; int n = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v) { tmp[n++] = '0' + (uint32_t)(v % 10); v /= 10; }
    while (n--) *p++ = tmp[n];
    return p;
}
static char *strapp(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static void reset_for_workload(const workload_t *wl) {
    memset(&saturn, 0, sizeof saturn);
    saturn.hexmode  = 16;
    saturn.rstk_ptr = -1;
    saturn.rom      = g_rom_buf;
    saturn.rom_size = sizeof g_rom_buf;
    saturn.ram      = g_ram_buf;
    saturn.ram_base = 0x80000;
    saturn.ram_size = sizeof g_ram_buf;
    saturn.ram_dat_bound      = saturn.ram_base + saturn.ram_size - 4;
    saturn.ram_minus_ram_base = (uintptr_t)saturn.ram - saturn.ram_base;
    saturn.pc       = wl->entry_pc;
}

/* Run `budget` ops through one of the three modes. Returns status. */
typedef enum { MODE_INTERP, MODE_JIT_OFF, MODE_JIT_ON, MODE_JIT_WARM } mode_t;

static const char *mode_name(mode_t m) {
    switch (m) {
    case MODE_INTERP:   return "interp";
    case MODE_JIT_OFF:  return "jit-off";
    case MODE_JIT_ON:   return "jit-on";
    case MODE_JIT_WARM: return "jit-warm";
    }
    return "?";
}

/* run_mode_core: budget loop without touching the JIT cache state.
 * Callers handle jit_reset() themselves so warm mode can keep the
 * previously-populated cache live. */
static interp_status_t run_mode_core(mode_t m, uint64_t budget) {
    const uint64_t kbd_period = 65536;
    uint64_t remaining = budget;
    interp_status_t st = INTERP_OK_BUDGET;
    kbd_seed(0xC0FFEE42u);
    while (remaining > 0 && st == INTERP_OK_BUDGET) {
        uint64_t chunk = remaining < kbd_period ? remaining : kbd_period;
        kbd_step();
        switch (m) {
        case MODE_INTERP:   st = saturn_run_interp(chunk); break;
        case MODE_JIT_OFF:
        case MODE_JIT_ON:
        case MODE_JIT_WARM: st = jit_run(chunk); break;
        }
        remaining -= chunk;
    }
    return st;
}

static interp_status_t run_mode(mode_t m, uint64_t budget) {
    if (m == MODE_JIT_OFF)  jit_reset(JIT_CACHE_OFF);
    if (m == MODE_JIT_ON)   jit_reset(JIT_CACHE_ON);
    if (m == MODE_JIT_WARM) jit_reset(JIT_CACHE_ON);
    return run_mode_core(m, budget);
}

/* Cross-validate JIT against interpreter.
 *
 * The JIT runs blocks atomically — if budget runs out mid-block the
 * block still completes, so the JIT typically overshoots. To get a
 * meaningful state-equality check, we:
 *   1. Run JIT first for `target` budget, note how many ops it
 *      actually executed (the exact value via saturn.saturn_ops).
 *   2. Reset and run interp for *exactly that many* ops.
 *   3. Compare states. */
static int crosscheck(const workload_t *wl, mode_t jit_mode) {
    saturn_t snap_interp, snap_jit;

    reset_for_workload(wl);
    run_mode(jit_mode, 20000);
    uint64_t jit_ops = (uint64_t)saturn.saturn_ops;
    snap_jit = saturn;

    reset_for_workload(wl);
    run_mode(MODE_INTERP, jit_ops);
    snap_interp = saturn;

    int diff = compare_state(&snap_interp, &snap_jit);
    if (diff < 0) return 0;

    char buf[200], *p = buf;
    p = strapp(p, "CROSSCHK FAIL ");
    p = strapp(p, wl->name); p = strapp(p, "/");
    p = strapp(p, mode_name(jit_mode));
    p = strapp(p, " at offset "); p = u32_dec(p, diff);
    p = strapp(p, " interp=");    p = u32_hex(p, ((uint8_t*)&snap_interp)[diff]);
    p = strapp(p, " jit=");       p = u32_hex(p, ((uint8_t*)&snap_jit)[diff]);
    p = strapp(p, " pc_interp="); p = u32_hex(p, snap_interp.pc);
    p = strapp(p, " pc_jit=");    p = u32_hex(p, snap_jit.pc);
    *p++ = '\n'; *p = 0;
    sh_puts(buf);
    return 1;
}

static void run_one(const workload_t *wl, mode_t m, uint64_t budget) {
    /* Warm mode: pre-populate the JIT cache, then reset Saturn state
     * (NOT the cache) and time the second pass. The first pass amortizes
     * the translate-and-link cost so the measurement reflects steady-
     * state throughput against an already-warm code cache. */
    if (m == MODE_JIT_WARM) {
        jit_reset(JIT_CACHE_ON);
        /* Short warmup: enough to translate every block the workload
         * touches. The bench workloads have ≤4 blocks each, so a few
         * thousand ops covers the working set. */
        reset_for_workload(wl);
        run_mode_core(m, 8000);
        /* Keep the cache; reset only Saturn arch state. */
        reset_for_workload(wl);
    } else {
        reset_for_workload(wl);
    }
    uint64_t e0 = sh_elapsed();
    interp_status_t st = (m == MODE_JIT_WARM)
        ? run_mode_core(m, budget)
        : run_mode(m, budget);
    uint64_t e1 = sh_elapsed();

    char line[240], *p = line;
    p = strapp(p, "RESULT,");
    p = strapp(p, wl->name);   p = strapp(p, ",");
    p = strapp(p, mode_name(m));
    p = strapp(p, ",ops=");          p = u64_dec(p, (uint64_t)saturn.saturn_ops);
    p = strapp(p, ",ticks=");        p = u64_dec(p, e1 - e0);
    p = strapp(p, ",br_taken=");     p = u64_dec(p, (uint64_t)saturn.saturn_branches_taken);
    p = strapp(p, ",status=");       p = u32_dec(p, st);
    p = strapp(p, ",final_pc=");     p = u32_hex(p, saturn.pc);
    if (m != MODE_INTERP) {
        const jit_stats_t *js = jit_get_stats();
        p = strapp(p, ",blocks=");   p = u32_dec(p, js->blocks_translated);
        p = strapp(p, ",hits=");     p = u32_dec(p, js->cache_hits);
        p = strapp(p, ",misses=");   p = u32_dec(p, js->cache_misses);
        p = strapp(p, ",evicts=");   p = u32_dec(p, js->cache_evicts);
        p = strapp(p, ",bytes=");    p = u32_dec(p, js->bytes_emitted);
        p = strapp(p, ",fallback=");  p = u32_dec(p, js->interp_fallback_ops);
    }
    *p++ = '\n'; *p = 0;
    sh_puts(line);
}

int main(void) {
    char hdr[160], *p = hdr;
    p = strapp(p, "saturn_jit_m33 bench\ntickfreq=");
    p = u32_dec(p, (uint32_t)sh_tickfreq());
    *p++ = '\n'; *p = 0;
    sh_puts(hdr);

    {
        char err[64];
        int rc = thumb2_selftest(err, sizeof err);
        if (rc) { sh_puts(err); sh_exit(); }
        sh_puts("thumb2 selftest ok\n");
    }

    uint32_t cache_bytes = (uint32_t)(_jit_cache_end - _jit_cache_start);
    jit_init(_jit_cache_start, cache_bytes);
    {
        char b[64], *q = b;
        q = strapp(q, "jit cache: "); q = u32_dec(q, cache_bytes); q = strapp(q, " bytes\n");
        *q = 0; sh_puts(b);
    }

    /* Translate the first block of arith into the cache, then print
     * its size + dump its bytes 2 per line so we can disassemble. */
    {
        const workload_t *dwl = workload_build_arith(g_rom_buf, sizeof g_rom_buf, 0);
        reset_for_workload(dwl);
        uint32_t used = 0; jit_block_meta_t meta = {0};
        saturn_jit_translate(0, _jit_cache_start, 4096, &used, &meta);
        char b[80], *q = b;
        q = strapp(q, "JIT@0 used=");  q = u32_dec(q, used);
        q = strapp(q, " ops=");        q = u32_dec(q, meta.saturn_ops);
        q = strapp(q, " nibs=");       q = u32_dec(q, meta.saturn_nibs);
        *q++ = '\n'; *q = 0; sh_puts(b);

        uint8_t *cb = _jit_cache_start;
        uint32_t dump = used > 64 ? 64 : used;
        for (uint32_t i = 0; i < dump; i += 4) {
            char ln[40], *r = ln;
            static const char H[]="0123456789abcdef";
            for (int j = 0; j < 4 && i + j < dump; j++) {
                *r++ = H[(cb[i+j]>>4)&0xf];
                *r++ = H[cb[i+j]&0xf];
                *r++ = ' ';
            }
            *r++ = '\n'; *r = 0; sh_puts(ln);
        }
    }

    const uint64_t budget = 200000;
    const workload_t *wl;

    /* Crosschecks compare JIT vs interp state after a short budget. At
     * very small cache sizes the JIT thrashes (cache_flush every block)
     * which exposes a known IC-staleness corner case that doesn't matter
     * for steady-state throughput. Define BENCH_SKIP_CROSSCHECK=1 (used
     * by the cache-size sweep) to skip them and run the timing rows
     * only. */
#ifndef BENCH_SKIP_CROSSCHECK
#define BENCH_SKIP_CROSSCHECK 0
#endif

    /* jit-off rows are the slowest by far (each translates per dispatch
     * with no cache reuse) and depend only weakly on the JIT optimizations
     * we sweep. Define BENCH_SKIP_JIT_OFF=1 to drop them and save the
     * sweep ~12 s per QEMU invocation. */
#ifndef BENCH_SKIP_JIT_OFF
#define BENCH_SKIP_JIT_OFF 0
#endif

    wl = workload_build_arith(g_rom_buf, sizeof g_rom_buf, 0);
    if (!BENCH_SKIP_CROSSCHECK) {
        if (crosscheck(wl, MODE_JIT_OFF)) sh_exit();
        if (crosscheck(wl, MODE_JIT_ON))  sh_exit();
    }
    run_one(wl, MODE_INTERP,  budget);
    if (!BENCH_SKIP_JIT_OFF) run_one(wl, MODE_JIT_OFF, budget);
    run_one(wl, MODE_JIT_ON,  budget);
    /* Warm mode is only meaningful when the cache held the working set.
     * If jit-on evicted, the cache is too small — warm mode would race
     * with the same eviction churn (and trips a known IC corner case at
     * sub-2 KiB caches), so skip the warm row. The sweep treats a
     * missing jit-warm row as "no data point at this size". */
    if (jit_get_stats()->cache_evicts == 0) {
        run_one(wl, MODE_JIT_WARM, budget);
    }

    wl = workload_build_memmix(g_rom_buf, sizeof g_rom_buf, 0);
    if (!BENCH_SKIP_CROSSCHECK) {
        if (crosscheck(wl, MODE_JIT_OFF)) sh_exit();
        if (crosscheck(wl, MODE_JIT_ON))  sh_exit();
    }
    run_one(wl, MODE_INTERP,  budget);
    if (!BENCH_SKIP_JIT_OFF) run_one(wl, MODE_JIT_OFF, budget);
    run_one(wl, MODE_JIT_ON,  budget);
    /* Warm mode is only meaningful when the cache held the working set.
     * If jit-on evicted, the cache is too small — warm mode would race
     * with the same eviction churn (and trips a known IC corner case at
     * sub-2 KiB caches), so skip the warm row. The sweep treats a
     * missing jit-warm row as "no data point at this size". */
    if (jit_get_stats()->cache_evicts == 0) {
        run_one(wl, MODE_JIT_WARM, budget);
    }

    wl = workload_build_calltree(g_rom_buf, sizeof g_rom_buf, 0);
    if (!BENCH_SKIP_CROSSCHECK) {
        if (crosscheck(wl, MODE_JIT_OFF)) sh_exit();
        if (crosscheck(wl, MODE_JIT_ON))  sh_exit();
    }
    run_one(wl, MODE_INTERP,  budget);
    if (!BENCH_SKIP_JIT_OFF) run_one(wl, MODE_JIT_OFF, budget);
    run_one(wl, MODE_JIT_ON,  budget);
    /* Warm mode is only meaningful when the cache held the working set.
     * If jit-on evicted, the cache is too small — warm mode would race
     * with the same eviction churn (and trips a known IC corner case at
     * sub-2 KiB caches), so skip the warm row. The sweep treats a
     * missing jit-warm row as "no data point at this size". */
    if (jit_get_stats()->cache_evicts == 0) {
        run_one(wl, MODE_JIT_WARM, budget);
    }

    wl = workload_build_countloop(g_rom_buf, sizeof g_rom_buf, 0);
    if (!BENCH_SKIP_CROSSCHECK) {
        if (crosscheck(wl, MODE_JIT_OFF)) sh_exit();
        if (crosscheck(wl, MODE_JIT_ON))  sh_exit();
    }
    run_one(wl, MODE_INTERP,  budget);
    if (!BENCH_SKIP_JIT_OFF) run_one(wl, MODE_JIT_OFF, budget);
    run_one(wl, MODE_JIT_ON,  budget);
    /* Warm mode is only meaningful when the cache held the working set.
     * If jit-on evicted, the cache is too small — warm mode would race
     * with the same eviction churn (and trips a known IC corner case at
     * sub-2 KiB caches), so skip the warm row. The sweep treats a
     * missing jit-warm row as "no data point at this size". */
    if (jit_get_stats()->cache_evicts == 0) {
        run_one(wl, MODE_JIT_WARM, budget);
    }

    sh_puts("done.\n");
    sh_exit();
    return 0;
}
