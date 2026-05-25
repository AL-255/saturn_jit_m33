/* Adafruit Feather RP2350 — Saturn JIT bench entry.
 *
 * This is the board-equivalent of m33/src/bench_main.c. The bench
 * structure is copied verbatim so the RESULT lines stay byte-for-byte
 * compatible with the QEMU output; the only differences are:
 *
 *   - The JIT code cache is a static buffer placed in either on-chip
 *     SRAM (default) or external PSRAM (compile-time JIT_CACHE_REGION).
 *   - sh_init_runtime() must run first so stdio + DWT are live.
 *   - psram_init() runs first when the cache lives in PSRAM, so the
 *     buffer is actually backed by working external RAM before we
 *     touch it.
 */

#include <string.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "saturn_state.h"
#include "saturn_interp.h"
#include "saturn_jit.h"
#include "jit_dispatch.h"
#include "workload.h"
#include "keyboard.h"
#include "semihost.h"

extern int thumb2_selftest(char *err_out, int errlen);
extern void psram_init(void);
extern void jit_psram_flush_after_emit(void);

/* Overclock is configured fully at compile time via CMake:
 *   SYS_CLOCK_KHZ              — target sys_clk (e.g. 300000, 400000)
 *   PLL_SYS_VCO_FREQ_HZ/PD1/PD2 — accompanying PLL config (from vcocalc.py)
 *   SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST=1 + SYS_CLK_VREG_VOLTAGE_MIN=VREG_VOLTAGE_1_xx
 *                              — SDK bumps VDD core BEFORE PLL_SYS comes up
 * Nothing for clocks needs to happen at runtime; main() can assume
 * the requested freq is already live. */
#ifndef SYS_CLOCK_KHZ
#define SYS_CLOCK_KHZ 0
#endif

/* --- JIT cache buffer ---------------------------------------------- */
#ifndef JIT_CACHE_BYTES
#define JIT_CACHE_BYTES (256u * 1024u)
#endif

#if defined(JIT_CACHE_REGION_PSRAM)
/* SDK 2.2 doesn't ship a `.psram_data` section for this board, so we
 * skip the linker and point directly into the PSRAM XIP window.
 * psram_init() configures QMI CS1 + the XIP mapping before this
 * buffer is touched. The size is whatever JIT_CACHE_BYTES is,
 * capped at 8 MiB by the PSRAM chip.
 *
 *   0x11000000 — cached (default; XIP-cache backed)
 *   0x15000000 — uncached alias (bypass XIP cache, every read/write
 *                round-trips to PSRAM over QSPI). Use this to
 *                measure the unhidden cost of PSRAM. */
#ifdef PSRAM_NOCACHE
static uint8_t *const g_jit_cache_buf = (uint8_t *)0x15000000u;
static const char *g_jit_cache_region_name = "psram-nocache";
#else
static uint8_t *const g_jit_cache_buf = (uint8_t *)0x11000000u;
static const char *g_jit_cache_region_name = "psram";
#endif
static const uint32_t g_jit_cache_size = JIT_CACHE_BYTES;
#else
/* Default: on-chip SRAM. */
__attribute__((aligned(64)))
static uint8_t g_jit_cache_storage[JIT_CACHE_BYTES];
static uint8_t *const g_jit_cache_buf = g_jit_cache_storage;
static const uint32_t g_jit_cache_size = sizeof g_jit_cache_storage;
static const char *g_jit_cache_region_name = "sram";
#endif

static uint8_t g_rom_buf[16 * 1024];
static uint8_t g_ram_buf[64 * 1024];

saturn_t saturn;

/* --- tiny formatting helpers (copied from m33/src/bench_main.c) ---- */
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

static void run_one(const workload_t *wl, mode_t m, uint64_t budget) {
    if (m == MODE_JIT_WARM) {
        jit_reset(JIT_CACHE_ON);
        reset_for_workload(wl);
        run_mode_core(m, 8000);
        reset_for_workload(wl);
    } else {
        reset_for_workload(wl);
    }
    uint64_t e0 = sh_elapsed();
    interp_status_t st = (m == MODE_JIT_WARM)
        ? run_mode_core(m, budget)
        : run_mode(m, budget);
    uint64_t e1 = sh_elapsed();

    char line[256], *p = line;
    p = strapp(p, "RESULT,");
    p = strapp(p, wl->name);   p = strapp(p, ",");
    p = strapp(p, mode_name(m));
    p = strapp(p, ",ops=");          p = u64_dec(p, (uint64_t)saturn.saturn_ops);
    /* sh_elapsed wraps at 2^32 — the CYCCNT comment in sh_impl.c covers
     * the math, but if e1 < e0 the run overflowed and we report the
     * wrap-corrected delta. */
    uint64_t ticks = (e1 >= e0) ? (e1 - e0) : ((uint64_t)0x100000000ull + e1 - e0);
    p = strapp(p, ",ticks=");        p = u64_dec(p, ticks);
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
    sh_init_runtime();

#if defined(JIT_CACHE_REGION_PSRAM)
    psram_init();
#endif

    char hdr[200], *p = hdr;
    p = strapp(p, "saturn_jit_rp2350 bench\nboard=adafruit_6130\n");
    p = strapp(p, "jit_cache_region="); p = strapp(p, g_jit_cache_region_name);
    p = strapp(p, "\ntickfreq=");
    p = u32_dec(p, (uint32_t)sh_tickfreq());
    *p++ = '\n'; *p = 0;
    sh_puts(hdr);

    {
        char err[64];
        int rc = thumb2_selftest(err, sizeof err);
        if (rc) { sh_puts(err); sh_exit(); }
        sh_puts("thumb2 selftest ok\n");
    }

    jit_init(g_jit_cache_buf, g_jit_cache_size);
    {
        char b[64], *q = b;
        q = strapp(q, "jit cache: "); q = u32_dec(q, (uint32_t)g_jit_cache_size);
        q = strapp(q, " bytes @ "); q = u32_hex(q, (uint32_t)(uintptr_t)g_jit_cache_buf);
        q = strapp(q, "\n");
        *q = 0; sh_puts(b);
    }

    const uint64_t budget = 200000;
    const workload_t *wl;

    /* Crosscheck and jit-off skipped on this board build — see
     * BENCH_SKIP_CROSSCHECK / BENCH_SKIP_JIT_OFF in CMakeLists.txt. */

    wl = workload_build_arith(g_rom_buf, sizeof g_rom_buf, 0);
    run_one(wl, MODE_INTERP,  budget);
    run_one(wl, MODE_JIT_ON,  budget);
    if (jit_get_stats()->cache_evicts == 0) run_one(wl, MODE_JIT_WARM, budget);

    wl = workload_build_memmix(g_rom_buf, sizeof g_rom_buf, 0);
    run_one(wl, MODE_INTERP,  budget);
    run_one(wl, MODE_JIT_ON,  budget);
    if (jit_get_stats()->cache_evicts == 0) run_one(wl, MODE_JIT_WARM, budget);

    wl = workload_build_calltree(g_rom_buf, sizeof g_rom_buf, 0);
    run_one(wl, MODE_INTERP,  budget);
    run_one(wl, MODE_JIT_ON,  budget);
    if (jit_get_stats()->cache_evicts == 0) run_one(wl, MODE_JIT_WARM, budget);

    wl = workload_build_countloop(g_rom_buf, sizeof g_rom_buf, 0);
    run_one(wl, MODE_INTERP,  budget);
    run_one(wl, MODE_JIT_ON,  budget);
    if (jit_get_stats()->cache_evicts == 0) run_one(wl, MODE_JIT_WARM, budget);

    wl = workload_build_nqueens(g_rom_buf, sizeof g_rom_buf, 0);
    run_one(wl, MODE_INTERP,  budget);
    run_one(wl, MODE_JIT_ON,  budget);
    if (jit_get_stats()->cache_evicts == 0) run_one(wl, MODE_JIT_WARM, budget);

    sh_puts("done.\n");
    sh_exit();
    return 0;
}
