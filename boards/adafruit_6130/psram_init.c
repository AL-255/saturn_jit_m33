/* PSRAM (APS6404L) bring-up for the Adafruit Feather RP2350 P/N 6130.
 *
 * Closely modelled on CircuitPython's setup_psram() in
 * ports/raspberrypi/supervisor/port.c — that is the reference that
 * is known to work for this exact board and chip. Adafruit don't
 * ship a stock Pico SDK helper for the QMI-attached APS6404L on
 * this board, so we drive it ourselves:
 *
 *   1. Mux PSRAM CS (GPIO8 on the 6130) to GPIO_FUNC_XIP_CS1.
 *   2. Enable QMI direct mode at a slow clk (clkdiv=30).
 *   3. Send 0xF5 as quad to exit QPI in case a prior failed boot
 *      left the part in QPI mode (otherwise the JEDEC read below
 *      gets garbage).
 *   4. Read JEDEC ID via 0x9F + 6 dummy bytes; abort if the KGD
 *      byte at index 5 isn't 0x5D (APS6404 magic). Bytes give us
 *      the die size at the EID byte (index 6).
 *   5. Issue RSTEN (0x66), RST (0x99), ENTER_QPI (0x35) — each in
 *      its own CS cycle.
 *   6. Configure QMI window 1 (M[1]) timing + read (0xEB QPI fast
 *      read, 24 dummy cycles) + write (0x38 QPI write) — the values
 *      below are copied verbatim from CircuitPython.
 *   7. Set XIP_CTRL.WRITABLE_M1 so XIP controller doesn't drop
 *      writes to the M1 window.
 *   8. Smoke-test through the no-cache alias at 0x15000000.
 *
 * After this returns successfully, PSRAM is memory-mapped at
 * 0x11000000 (cached) and 0x15000000 (uncached). JIT code reads
 * the cached window; the uncached alias is just for the verify
 * step (avoids stale XIP cache giving false positives).
 *
 * Everything from step 2 onward MUST live in SRAM — direct-mode
 * QMI breaks XIP fetches from flash for the duration. The
 * .time_critical.* section is copied to SRAM by the SDK runtime.
 */

#include <stdint.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/regs/qmi.h"
#include "hardware/regs/xip.h"
#include "hardware/sync.h"

#ifndef PSRAM_CS_PIN
#define PSRAM_CS_PIN 8
#endif

#define PSRAM_BASE_CACHED   ((volatile uint32_t *)0x11000000u)
#define PSRAM_BASE_NOCACHE  ((volatile uint32_t *)0x15000000u)

/* Filled in by psram_init() so the bench can size its JIT cache. */
static uint32_t s_psram_bytes = 0;

uint32_t psram_get_size_bytes(void) { return s_psram_bytes; }

static uint32_t __no_inline_not_in_flash_func(psram_init_inner)(uint8_t *kgd_out, uint8_t *eid_out) {
    *kgd_out = 0;
    *eid_out = 0;

    /* (1) Mux CS pin. */
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);

    /* (2) Enable QMI direct mode at a slow clock (sys / 30 ≈ 5 MHz at
     *     150 MHz sys, ~11 MHz at 336 MHz sys). */
    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB)
                       | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) { }

    /* (3) Send 0xF5 (exit QPI) as a quad-width byte so it works
     *     whether the part is currently in QPI or SPI mode. */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS
                      | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB)
                      | 0xf5u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) { }
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    /* (4) JEDEC read: 0x9F prefix + 6 dummy bytes. APS6404L returns
     *     a fixed manufacturer/KGD byte (0x5D) at index 5 and the
     *     EID at index 6, which encodes die size. */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0, eid = 0;
    for (int i = 0; i < 7; i++) {
        qmi_hw->direct_tx = (i == 0) ? 0x9fu : 0xffu;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) { }
        while ( qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) { }
        uint8_t b = (uint8_t)qmi_hw->direct_rx;
        if (i == 5) kgd = b;
        else if (i == 6) eid = b;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    *kgd_out = kgd;
    *eid_out = eid;
    if (kgd != 0x5D) {
        return 0;
    }

    /* (5) RSTEN, RST, ENTER_QPI — each as its own CS cycle. */
    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB)
                       | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) { }

    /* RSTEN, RST, ENTER_QPI in three separate CS cycles.
     * Don't use a const array here — it would live in .rodata (flash),
     * and a flash-XIP fetch with direct mode active would lock up. */
    for (int i = 0; i < 3; i++) {
        uint32_t cmd = (i == 0) ? 0x66u : (i == 1) ? 0x99u : 0x35u;
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        qmi_hw->direct_tx = cmd;
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) { }
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        for (int j = 0; j < 20; j++) __asm__ volatile("nop");
        (void)qmi_hw->direct_rx;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    /* (6) Configure M[1] for QPI XIP. Timing values are
     * CircuitPython's; CLKDIV is computed from clk_sys to keep
     * the PSRAM QSPI clock at ≤ ~110 MHz (APS6404L data brief:
     * 144 MHz QPI fast read, 84 MHz standard). At the stock
     * 150 MHz sys, clkdiv=2 gives 75 MHz — well inside spec.
     * At a 336 MHz OC sys, clkdiv=2 would be 168 MHz which
     * corrupts writes, so step up to clkdiv=4 (84 MHz). */
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t psram_clkdiv = 2;
    while (sys_hz / psram_clkdiv > 110u * 1000u * 1000u) psram_clkdiv++;
    qmi_hw->m[1].timing =
        (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB)
      | (3u  << QMI_M1_TIMING_SELECT_HOLD_LSB)
      | (1u  << QMI_M1_TIMING_COOLDOWN_LSB)
      | (1u  << QMI_M1_TIMING_RXDELAY_LSB)
      | (16u << QMI_M1_TIMING_MAX_SELECT_LSB)
      | (7u  << QMI_M1_TIMING_MIN_DESELECT_LSB)
      | (psram_clkdiv << QMI_M1_TIMING_CLKDIV_LSB);
    qmi_hw->m[1].rfmt =
        (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q  << QMI_M1_RFMT_PREFIX_WIDTH_LSB)
      | (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q    << QMI_M1_RFMT_ADDR_WIDTH_LSB)
      | (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q  << QMI_M1_RFMT_SUFFIX_WIDTH_LSB)
      | (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q   << QMI_M1_RFMT_DUMMY_WIDTH_LSB)
      | (QMI_M1_RFMT_DUMMY_LEN_VALUE_24    << QMI_M1_RFMT_DUMMY_LEN_LSB)
      | (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q    << QMI_M1_RFMT_DATA_WIDTH_LSB)
      | (QMI_M1_RFMT_PREFIX_LEN_VALUE_8    << QMI_M1_RFMT_PREFIX_LEN_LSB)
      | (QMI_M1_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_RFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = 0xebu << QMI_M1_RCMD_PREFIX_LSB;

    qmi_hw->m[1].wfmt =
        (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q  << QMI_M1_WFMT_PREFIX_WIDTH_LSB)
      | (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q    << QMI_M1_WFMT_ADDR_WIDTH_LSB)
      | (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q  << QMI_M1_WFMT_SUFFIX_WIDTH_LSB)
      | (QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q   << QMI_M1_WFMT_DUMMY_WIDTH_LSB)
      | (QMI_M1_WFMT_DUMMY_LEN_VALUE_NONE  << QMI_M1_WFMT_DUMMY_LEN_LSB)
      | (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q    << QMI_M1_WFMT_DATA_WIDTH_LSB)
      | (QMI_M1_WFMT_PREFIX_LEN_VALUE_8    << QMI_M1_WFMT_PREFIX_LEN_LSB)
      | (QMI_M1_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_WFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = 0x38u << QMI_M1_WCMD_PREFIX_LSB;

    /* (7) Allow XIP to forward writes to the M1 window. Default is
     *     read-only (CS0 is flash); without this, our writes get
     *     silently dropped and verify fails reading the prior value. */
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

    /* Derive die size from EID. APS6404 encoding (per Adafruit/AP
     * Memory data brief, matches CircuitPython):
     *   eid == 0x26      → 8 MiB
     *   eid bits 7..5 == 2 → 8 MiB
     *   eid bits 7..5 == 1 → 4 MiB
     *   eid bits 7..5 == 0 → 2 MiB
     * Anything else: assume 1 MiB. */
    uint32_t size = 1u * 1024u * 1024u;
    uint8_t sid = (uint8_t)(eid >> 5);
    if (eid == 0x26 || sid == 2)      size = 8u * 1024u * 1024u;
    else if (sid == 1)                size = 4u * 1024u * 1024u;
    else if (sid == 0)                size = 2u * 1024u * 1024u;

    return size;
}

void psram_init(void) {
    /* Make sure prints are visible before we touch QMI. */
    for (int i = 0; i < 20 && !stdio_usb_connected(); i++) busy_wait_ms(50);

    uint8_t kgd = 0, eid = 0;
    uint32_t irq = save_and_disable_interrupts();
    uint32_t bytes = psram_init_inner(&kgd, &eid);
    restore_interrupts(irq);

    s_psram_bytes = bytes;

    printf("psram: jedec kgd=0x%02x eid=0x%02x", kgd, eid);
    if (!bytes) {
        printf(" — no APS6404 on QMI CS1 (expected kgd 0x5D)\n");
        fflush(stdout);
        return;
    }
    uint32_t psclkdiv = (qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS)
                        >> QMI_M1_TIMING_CLKDIV_LSB;
    printf(", size=%lu KiB, qspi=%lu MHz (sys/%lu)\n",
           (unsigned long)(bytes / 1024u),
           (unsigned long)(clock_get_hz(clk_sys) / psclkdiv / 1000u / 1000u),
           (unsigned long)psclkdiv);
    fflush(stdout);

    /* (8) Smoke test through the no-cache alias so XIP cache hits
     *     can't fake a pass. Walk 256 KiB to catch addressing/data
     *     bugs without spending the entire init budget on RAM check. */
    volatile uint32_t *p = PSRAM_BASE_NOCACHE;
    const uint32_t test_words = 64 * 1024;
    for (uint32_t i = 0; i < test_words; i++) {
        p[i] = i ^ 0xa5a5a5a5u;
    }
    __dmb();
    uint32_t errors = 0;
    for (uint32_t i = 0; i < test_words; i++) {
        uint32_t want = i ^ 0xa5a5a5a5u;
        if (p[i] != want) errors++;
    }
    if (errors == 0) {
        printf("psram: verify ok (%lu KiB walked)\n",
               (unsigned long)(test_words * 4u / 1024u));
    } else {
        printf("psram: verify FAIL — %lu / %lu words mismatched\n",
               (unsigned long)errors, (unsigned long)test_words);
        s_psram_bytes = 0;
    }
    fflush(stdout);
}

void jit_psram_flush_after_emit(void) {
    /* Coherency for self-modifying code on M33: drain any QMI-bound
     * writes, then flush the prefetch buffer before the next fetch. */
    __asm__ volatile("dsb sy\n\tisb sy" ::: "memory");
}
