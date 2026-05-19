/* DWT smoke: enable the cycle counter, do a known-size loop, print the
 * delta. Confirms QEMU's M33 model exposes a usable cycle clock for
 * benchmarking. */

#include <stdint.h>

#define DEMCR    (*(volatile uint32_t *)0xE000EDFC)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
#define DEMCR_TRCENA      (1u << 24)
#define DWT_CTRL_CYCCNTENA (1u << 0)

static inline int semihost(int op, const void *arg) {
    register int      r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    register int      ret __asm__("r0");
    __asm__ volatile ("bkpt #0xAB" : "=r"(ret) : "r"(r0), "r"(r1) : "memory");
    return ret;
}
static void sh_puts(const char *s) { semihost(0x04, s); }
static void sh_exit(void) { semihost(0x18, (const void *)0x20026); for(;;){} }

/* No printf — keep code tiny. Render uint32 in hex by hand. */
static char *u32_hex(char *p, uint32_t v) {
    static const char H[] = "0123456789abcdef";
    *p++ = '0'; *p++ = 'x';
    for (int s = 28; s >= 0; s -= 4) *p++ = H[(v >> s) & 0xf];
    *p = 0;
    return p;
}

__attribute__((noinline))
static uint32_t spin(uint32_t n) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) s += i;
    return s;
}

int main(void) {
    DEMCR |= DEMCR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;

    uint32_t c0 = DWT_CYCCNT;
    volatile uint32_t r = spin(10000);
    uint32_t c1 = DWT_CYCCNT;
    (void)r;

    char buf[64], *p = buf;
    const char *lbl = "dwt: ";
    while (*lbl) *p++ = *lbl++;
    p = u32_hex(p, c1 - c0);
    *p++ = ' ';
    *p++ = '('; p = u32_hex(p, DWT_CTRL); *p++ = ')';
    *p++ = '\n'; *p = 0;
    sh_puts(buf);
    sh_exit();
    return 0;
}
