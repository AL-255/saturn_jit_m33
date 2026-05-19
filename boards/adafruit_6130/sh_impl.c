/* RP2350 implementations of the sh_* helpers that the bench harness
 * expects. Under QEMU these were ARM-semihosting calls; here we wire
 * sh_puts → stdio (USB CDC), sh_elapsed → DWT CYCCNT, and
 * sh_tickfreq → clock_get_hz(clk_sys). */

#include "semihost.h"

#include <stdint.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/structs/scb.h"
#include "hardware/clocks.h"

/* DWT (Data Watchpoint and Trace) — enable CYCCNT once at boot. */
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
#define DEMCR      (*(volatile uint32_t *)0xE000EDFC)

void sh_init_runtime(void) {
    /* Bring up the standard stdio (USB CDC, configured in CMakeLists)
     * and enable the DWT cycle counter. */
    stdio_init_all();

    /* The host normally enables TRCENA + CYCCNTENA over SWD when
     * debugging; without that, we have to do it ourselves. */
    DEMCR    |= 0x01000000u;     /* TRCENA */
    DWT_CYCCNT = 0;
    DWT_CTRL  |= 0x00000001u;    /* CYCCNTENA */
}

void sh_puts(const char *s) {
    fputs(s, stdout);
    /* USB CDC is buffered; flush so output lands promptly even if the
     * bench is not yet "done." */
    fflush(stdout);
}

uint64_t sh_elapsed(void) {
    /* CYCCNT is 32-bit and ticks at clk_sys (default 150 MHz on
     * RP2350). At 150 MHz it wraps every ~28.6 seconds — the bench's
     * longest individual run is well under that. The host post-processor
     * divides by sh_tickfreq() to get seconds. */
    return (uint64_t)DWT_CYCCNT;
}

uint64_t sh_tickfreq(void) {
    return (uint64_t)clock_get_hz(clk_sys);
}

void sh_exit(void) {
    /* No semihosting "exit" on bare metal — just spin so the printed
     * output stays on the wire. The user can hit reset. */
    fflush(stdout);
    for (;;) tight_loop_contents();
}
