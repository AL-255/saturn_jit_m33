/* PSRAM bring-up for the Adafruit Feather RP2350 (HSTX, 8 MB PSRAM).
 *
 * SDK 2.2.0 does not include a stock PSRAM init for this board (the
 * Adafruit board file `adafruit_feather_rp2350.h` doesn't declare
 * PICO_RP2350_PSRAM_*) so we drive the QMI directly:
 *
 *   1. Mux PSRAM CS pin (GPIO PSRAM_CS_PIN, default 8) to QMI_CS1.
 *   2. Take over QMI in direct mode (DIRECT_CSR.EN = 1).
 *   3. Send the APS6404 reset sequence (0x66 then 0x99) over CS1.
 *   4. Send "Enter QPI mode" (0x35).
 *   5. Configure QMI M1 (CS1 / address window @ 0x11000000) for
 *      QPI Fast-Read (0xEB, 24-bit addr, 6 dummy cycles) and
 *      Quad-Write (0x38).
 *   6. Drop direct mode — PSRAM is now memory-mapped.
 *   7. Verify by writing a pattern through the mapped window and
 *      reading it back.
 *
 * The clock divider is conservative (clkdiv=4 → 37.5 MHz QSPI at the
 * default 150 MHz sys clock; APS6404L supports up to 144 MHz in QPI).
 * Bumping the rate is a "make it faster later" item — first we want
 * correct.
 */

#include <stdint.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/regs/qmi.h"
#include "hardware/sync.h"
#include "pico/runtime_init.h"

/* The QMI direct-mode setup REQUIRES that the CPU not fetch instructions
 * from flash (XIP through QMI CS0) while QMI is being reprogrammed —
 * otherwise the very next fetch stalls and the chip wedges. Force the
 * critical function and its inline helpers to live in SRAM via the
 * standard Pico SDK section. */
#define IN_SRAM __attribute__((section(".time_critical.psram_init")))

#ifndef PSRAM_CS_PIN
#define PSRAM_CS_PIN 8        /* Adafruit Feather RP2350 6130 default */
#endif

#ifndef PSRAM_USE_QPI
#define PSRAM_USE_QPI 0       /* start in SPI for first-contact debugging */
#endif

#define PSRAM_BASE              ((volatile uint32_t *)0x11000000u)
#define PSRAM_TEST_NWORDS       1024

/* APS6404L QPI/SPI command bytes. */
#define APS6404_CMD_RESET_EN    0x66
#define APS6404_CMD_RESET       0x99
#define APS6404_CMD_ENTER_QPI   0x35
#define APS6404_CMD_FAST_READ_Q 0xEB    /* QPI fast read, 6 dummy */
#define APS6404_CMD_QUAD_WRITE  0x38    /* QPI write */

/* --- direct-mode helpers ------------------------------------------ */

/* Returns 0 on success, -1 on timeout — `count` is approximate
 * busy-loop iterations; even at clk_sys=150 MHz a single QMI byte
 * completes in well under 256 iterations, so 10000 is generous. */
IN_SRAM static int qmi_wait_clear(uint32_t mask) {
    for (int i = 0; i < 10000; i++) {
        if (!(qmi_hw->direct_csr & mask)) return 0;
        tight_loop_contents();
    }
    return -1;
}
IN_SRAM static int qmi_wait_set(uint32_t mask) {
    for (int i = 0; i < 10000; i++) {
        if (qmi_hw->direct_csr & mask) return 0;
        tight_loop_contents();
    }
    return -1;
}

/* No printf in here — flash is unreachable while DIRECT_CSR.EN=1.
 * `iwidth` is 0=Single (SPI) or 2=Quad (QPI). */
IN_SRAM static int qmi_direct_send_byte_cs1_w(uint8_t b, int iwidth) {
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx  = (uint32_t)b | (uint32_t)((iwidth & 3) << 16);
    if (qmi_wait_set(QMI_DIRECT_CSR_TXEMPTY_BITS))     return -1;
    if (qmi_wait_clear(QMI_DIRECT_CSR_BUSY_BITS))      return -2;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)) {
        (void)qmi_hw->direct_rx;
    }
    return 0;
}
IN_SRAM static int qmi_direct_send_byte_cs1(uint8_t b) {
    return qmi_direct_send_byte_cs1_w(b, 0);
}

/* --- public API --------------------------------------------------- */

/* Bail out cleanly without ever stalling on the bus. If anything looks
 * off — direct-mode hung from a previous boot, GPIO function still wrong,
 * etc. — we abort with a printf so the bench can keep running with the
 * SRAM cache (the PSRAM build degrades gracefully into "JIT cache lives
 * at a never-used address" but at least USB stays alive). */
IN_SRAM void psram_init(void) {
    /* `sh_init_runtime` already ran stdio_init_all + DWT setup. Make
     * sure prints are visible before we touch QMI. */
    for (int i = 0; i < 20 && !stdio_usb_connected(); i++) busy_wait_ms(50);
    printf("psram init: CS pin GPIO%u, base 0x11000000\n", PSRAM_CS_PIN);
    fflush(stdout);
    busy_wait_ms(20);  /* let the line drain */

    /* 1. Mux the PSRAM CS pin to QMI's CS1 function. */
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
    printf("psram: GPIO%u set to XIP_CS1 (func 9)\n", PSRAM_CS_PIN);
    fflush(stdout);

    /* 2. Take over QMI in direct mode at a conservative ~37 MHz
     *    (clkdiv=4 against the 150 MHz sys clock). IRQs off + no
     *    printf while DIRECT_CSR.EN=1 because flash XIP through CS0
     *    is broken in that window — the printf code lives in flash. */
    uint32_t saved_csr = qmi_hw->direct_csr;
    printf("psram: pre-init direct_csr=0x%08lx\n", (unsigned long)saved_csr);
    fflush(stdout);

    uint32_t irq_state = save_and_disable_interrupts();
    int rc_busy_pre = 0, rc_resetA = 0, rc_resetB = 0, rc_qpi = 0;

    qmi_hw->direct_csr =
        QMI_DIRECT_CSR_EN_BITS |
        (30u << QMI_DIRECT_CSR_CLKDIV_LSB);
    if (qmi_wait_clear(QMI_DIRECT_CSR_BUSY_BITS)) {
        rc_busy_pre = -1;
        goto release_direct_mode;
    }

    /* 3. The PSRAM may be left in QPI mode by a previous failed boot
     *    where we couldn't power-cycle. Send the reset sequence FIRST
     *    in QPI (4-bit) and THEN in SPI (1-bit) so we hit it
     *    regardless of which mode it's currently in. The APS6404
     *    accepts reset commands in either mode and returns to SPI. */
    (void)qmi_direct_send_byte_cs1_w(APS6404_CMD_RESET_EN, 2);
    (void)qmi_direct_send_byte_cs1_w(APS6404_CMD_RESET,    2);
    for (volatile int i = 0; i < 4000; i++) { __asm__ volatile("nop"); }
    rc_resetA = qmi_direct_send_byte_cs1(APS6404_CMD_RESET_EN);
    rc_resetB = qmi_direct_send_byte_cs1(APS6404_CMD_RESET);
    for (volatile int i = 0; i < 10000; i++) { __asm__ volatile("nop"); }
    if (rc_resetA || rc_resetB) goto release_direct_mode;

#if PSRAM_USE_QPI
    /* 4. Enter QPI mode. */
    rc_qpi = qmi_direct_send_byte_cs1(APS6404_CMD_ENTER_QPI);
    if (rc_qpi) goto release_direct_mode;
#else
    rc_qpi = 0;  /* staying in SPI mode for first contact */
#endif

    /* 5. Configure QMI window 1 for QPI XIP read/write. The
     *    DUMMY_LEN value is in units of 4 bits; APS6404L wants 6
     *    dummy cycles for 0xEB so the field is 6/2 = 3 (per RP2350
     *    DUMMY_LEN encoding: 0=0, 1=4, ... — see datasheet table). */
    qmi_hw->m[1].timing =
        (1u << QMI_M1_TIMING_COOLDOWN_LSB) |  /* 1-cycle CS cooldown */
        (2u << QMI_M1_TIMING_RXDELAY_LSB)  |  /* 2 half-cycles sample delay */
        (10u << QMI_M1_TIMING_CLKDIV_LSB);    /* 15 MHz — conservative for first contact */

#if PSRAM_USE_QPI
    qmi_hw->m[1].rfmt =
        (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q  << QMI_M1_RFMT_PREFIX_WIDTH_LSB)  |
        (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q    << QMI_M1_RFMT_ADDR_WIDTH_LSB)    |
        (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q  << QMI_M1_RFMT_SUFFIX_WIDTH_LSB)  |
        (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q   << QMI_M1_RFMT_DUMMY_WIDTH_LSB)   |
        (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q    << QMI_M1_RFMT_DATA_WIDTH_LSB)    |
        (QMI_M1_RFMT_PREFIX_LEN_VALUE_8    << QMI_M1_RFMT_PREFIX_LEN_LSB)    |
        (6u                                 << QMI_M1_RFMT_DUMMY_LEN_LSB);
    qmi_hw->m[1].rcmd = (uint32_t)APS6404_CMD_FAST_READ_Q;

    qmi_hw->m[1].wfmt =
        (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q  << QMI_M1_WFMT_PREFIX_WIDTH_LSB)  |
        (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q    << QMI_M1_WFMT_ADDR_WIDTH_LSB)    |
        (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q  << QMI_M1_WFMT_SUFFIX_WIDTH_LSB)  |
        (QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q   << QMI_M1_WFMT_DUMMY_WIDTH_LSB)   |
        (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q    << QMI_M1_WFMT_DATA_WIDTH_LSB)    |
        (QMI_M1_WFMT_PREFIX_LEN_VALUE_8    << QMI_M1_WFMT_PREFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = (uint32_t)APS6404_CMD_QUAD_WRITE;
#else
    /* SPI mode: Fast Read 0x0B with 8-bit (= 1 byte) dummy; ordinary
     * write command 0x02. All widths Single. */
    qmi_hw->m[1].rfmt =
        (QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB) |
        (2u                              << QMI_M1_RFMT_DUMMY_LEN_LSB);
    qmi_hw->m[1].rcmd = 0x0B;  /* SPI Fast Read */
    qmi_hw->m[1].wfmt =
        (QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = 0x02;  /* SPI Write */
#endif

release_direct_mode:
    /* 6. Drop direct mode — XIP through M1 (and M0 for flash) is now live. */
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
    (void)saved_csr;
    __dmb();
    restore_interrupts(irq_state);

    if (rc_busy_pre) {
        printf("psram: direct-mode enable BUSY timeout — QMI was stuck\n");
        fflush(stdout);
        return;
    }
    if (rc_resetA || rc_resetB) {
        printf("psram: reset sequence timed out (resetA=%d resetB=%d) — CS pin GPIO%u likely wrong\n",
               rc_resetA, rc_resetB, PSRAM_CS_PIN);
        fflush(stdout);
        return;
    }
    if (rc_qpi) {
        printf("psram: ENTER_QPI timed out (rc=%d)\n", rc_qpi);
        fflush(stdout);
        return;
    }
    printf("psram: direct-mode init done, M1 configured for QPI XIP\n");
    fflush(stdout);

    /* 7. Smoke test: try reading a single word from PSRAM first (a
     *    passive op that won't lock the bus if the chip isn't there),
     *    then write+read a small pattern. On mismatch we print and
     *    keep going — the bench still runs and the symptoms (HardFault
     *    on JIT exec, garbage in output) tell us PSRAM isn't live. */
    volatile uint32_t *p = PSRAM_BASE;
    uint32_t first_read = p[0];
    printf("psram first read: 0x%08lx\n", (unsigned long)first_read);
    fflush(stdout);

    int mismatches = 0;
    for (uint32_t i = 0; i < PSRAM_TEST_NWORDS; i++) {
        p[i] = 0xA5A5A500u | (i & 0xff);
    }
    __dmb();
    for (uint32_t i = 0; i < PSRAM_TEST_NWORDS; i++) {
        uint32_t expect = 0xA5A5A500u | (i & 0xff);
        if (p[i] != expect) {
            if (mismatches < 4) {
                printf("PSRAM mismatch at 0x%08lx: got 0x%08lx, want 0x%08lx\n",
                       (unsigned long)(uintptr_t)&p[i],
                       (unsigned long)p[i], (unsigned long)expect);
            }
            mismatches++;
        }
    }
    if (mismatches == 0) {
        printf("psram verify ok (%u words @ 0x11000000)\n", PSRAM_TEST_NWORDS);
    } else {
        printf("psram verify FAIL: %d/%u mismatches\n", mismatches, PSRAM_TEST_NWORDS);
    }
    fflush(stdout);
}

void jit_psram_flush_after_emit(void) {
    /* Coherency for self-modifying code on M33: drain any QMI-bound
     * writes, then flush the prefetch buffer before the next fetch. */
    __asm__ volatile("dsb sy\n\tisb sy" ::: "memory");
}
