/* PSRAM bring-up for the Adafruit Feather RP2350 (HSTX, 8 MB PSRAM).
 *
 * Pico SDK 2.x ships with `runtime_init_setup_psram()` that runs at
 * startup for boards whose board-config file sets PICO_RP2350_PSRAM_*.
 * The board file `adafruit_feather_rp2350_hstx.h` in the upstream SDK
 * does that, so PSRAM at 0x11000000+ is live before main() is reached.
 * `psram_init()` here is therefore a verification stub: it pokes a
 * test pattern through the mapped PSRAM and checks it reads back the
 * same. If that fails the board hangs in `for(;;)` — easier to spot in
 * the serial output than mysterious crashes during JIT execution.
 *
 * `jit_psram_flush_after_emit()` is provided for the bench harness;
 * on the RP2350 the unified XIP cache for the PSRAM region is the same
 * cache used for fetches, so writes through QMI are immediately
 * visible to subsequent fetches. We still emit a DSB+ISB pair to
 * flush the M33's prefetch buffer before the JIT-emitted code runs.
 */

#include <stdint.h>
#include <stdio.h>
#include "pico/stdlib.h"

#define PSRAM_BASE      ((volatile uint32_t *)0x11000000u)
#define PSRAM_TEST_NWORDS 1024

void psram_init(void) {
    /* PSRAM is brought up by the Pico SDK runtime_init machinery using
     * the board-config values. We just verify it's actually responding. */
    volatile uint32_t *p = PSRAM_BASE;
    for (uint32_t i = 0; i < PSRAM_TEST_NWORDS; i++) {
        p[i] = 0xA5A5A500u | (i & 0xff);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint32_t i = 0; i < PSRAM_TEST_NWORDS; i++) {
        uint32_t expect = 0xA5A5A500u | (i & 0xff);
        if (p[i] != expect) {
            printf("PSRAM verify FAIL at 0x%08lx: got 0x%08lx, want 0x%08lx\n",
                   (unsigned long)(uintptr_t)&p[i],
                   (unsigned long)p[i], (unsigned long)expect);
            fflush(stdout);
            for (;;) tight_loop_contents();
        }
    }
    printf("psram verify ok (%u words @ 0x11000000)\n", PSRAM_TEST_NWORDS);
    fflush(stdout);
}

/* Hook the bench can call after writing JIT code to PSRAM (or SRAM).
 * On M33, the only coherency requirement for self-modifying code in a
 * single core is to drain the prefetch / write buffers before fetching
 * the new instructions. DSB ensures any QMI-bound writes have committed;
 * ISB flushes the prefetch so the next fetch goes back through the XIP
 * cache to PSRAM. */
void jit_psram_flush_after_emit(void) {
    __asm__ volatile("dsb sy\n\t"
                     "isb sy"
                     ::: "memory");
}
