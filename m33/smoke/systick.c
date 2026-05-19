/* SysTick smoke: use SysTick as a free-running 24-bit counter and
 * sample before/after a known-size loop. On Cortex-M33 SysTick is
 * mandatory; QEMU implements it for mps2-an505. */

#include <stdint.h>

#define SYST_CSR   (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR   (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR   (*(volatile uint32_t *)0xE000E018)
#define SYST_CALIB (*(volatile uint32_t *)0xE000E01C)
#define SYST_CSR_ENABLE     (1u << 0)
#define SYST_CSR_CLKSOURCE  (1u << 2)   /* 1 = processor clock */

static inline int semihost(int op, const void *arg) {
    register int      r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    register int      ret __asm__("r0");
    __asm__ volatile ("bkpt #0xAB" : "=r"(ret) : "r"(r0), "r"(r1) : "memory");
    return ret;
}
static void sh_puts(const char *s) { semihost(0x04, s); }
static void sh_exit(void) { semihost(0x18, (const void *)0x20026); for(;;){} }

static char *u32_hex(char *p, uint32_t v) {
    static const char H[] = "0123456789abcdef";
    *p++ = '0'; *p++ = 'x';
    for (int s = 28; s >= 0; s -= 4) *p++ = H[(v >> s) & 0xf];
    *p = 0; return p;
}

/* SysTick counts DOWN. delta = (prev - now) & 0xFFFFFF (one wrap max). */
static inline uint32_t st_delta(uint32_t prev, uint32_t now) {
    return (prev - now) & 0x00FFFFFFu;
}

__attribute__((noinline))
static uint32_t spin(uint32_t n) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) s += i * 3u + 1u;
    return s;
}

int main(void) {
    /* Configure SysTick: max reload, processor clock, enable. */
    SYST_RVR = 0x00FFFFFFu;
    SYST_CVR = 0;                  /* writing any value clears CURRENT and COUNTFLAG */
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_ENABLE;

    char buf[96], *p;

    uint32_t t0 = SYST_CVR;
    volatile uint32_t r1 = spin(100);
    uint32_t t1 = SYST_CVR;
    volatile uint32_t r2 = spin(1000);
    uint32_t t2 = SYST_CVR;
    volatile uint32_t r3 = spin(10000);
    uint32_t t3 = SYST_CVR;
    (void)r1; (void)r2; (void)r3;

    p = buf;
    const char *l0 = "systick calib="; while(*l0) *p++=*l0++;
    p = u32_hex(p, SYST_CALIB); *p++='\n'; *p=0; sh_puts(buf);

    p = buf;
    const char *l1 = "spin(100):   "; while(*l1) *p++=*l1++;
    p = u32_hex(p, st_delta(t0, t1)); *p++='\n'; *p=0; sh_puts(buf);

    p = buf;
    const char *l2 = "spin(1000):  "; while(*l2) *p++=*l2++;
    p = u32_hex(p, st_delta(t1, t2)); *p++='\n'; *p=0; sh_puts(buf);

    p = buf;
    const char *l3 = "spin(10000): "; while(*l3) *p++=*l3++;
    p = u32_hex(p, st_delta(t2, t3)); *p++='\n'; *p=0; sh_puts(buf);

    sh_exit();
    return 0;
}
