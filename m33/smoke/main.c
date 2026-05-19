/* Semihosting smoke test: print a banner, exit cleanly so QEMU
 * returns 0 and we know toolchain+runner are wired up. */

#include <stdint.h>

/* ARM semihosting: SVC #0xAB on A/R, BKPT #0xAB on M-class. */
static inline int semihost(int op, const void *arg) {
    register int      r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    register int      ret __asm__("r0");
    __asm__ volatile ("bkpt #0xAB" : "=r"(ret) : "r"(r0), "r"(r1) : "memory");
    return ret;
}

#define SYS_WRITE0  0x04
#define SYS_EXIT    0x18
#define ADP_Stopped_ApplicationExit 0x20026

static void sh_puts(const char *s) { semihost(SYS_WRITE0, s); }
static void sh_exit(int code) {
    /* Legacy 32-bit form: r1 is the reason code directly. QEMU maps
     * ADP_Stopped_ApplicationExit → host exit 0; anything else → 1. */
    (void)code;
    semihost(SYS_EXIT, (const void *)ADP_Stopped_ApplicationExit);
    for (;;) {}
}

int main(void) {
    sh_puts("saturn_jit_m33 smoke ok\n");
    sh_exit(0);
    return 0;
}
