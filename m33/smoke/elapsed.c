/* SYS_ELAPSED + SYS_TICKFREQ smoke. Under qemu -icount, SYS_ELAPSED
 * returns a deterministic 64-bit virtual-tick count, which sidesteps
 * the 24-bit SysTick wrap problem. */

#include <stdint.h>

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

static uint64_t sh_elapsed(void) {
    uint32_t buf[2] = { 0, 0 };
    semihost(0x30, buf);                /* SYS_ELAPSED */
    return ((uint64_t)buf[1] << 32) | buf[0];
}
static int32_t sh_tickfreq(void) {
    return semihost(0x31, (void *)0);   /* SYS_TICKFREQ */
}

__attribute__((noinline))
static uint32_t spin(uint32_t n) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) s += i * 3u + 1u;
    return s;
}

int main(void) {
    char buf[96], *p;
    int32_t freq = sh_tickfreq();

    p = buf;
    const char *l0 = "tickfreq="; while(*l0) *p++=*l0++;
    p = u32_hex(p, (uint32_t)freq); *p++='\n'; *p=0; sh_puts(buf);

    uint64_t e0 = sh_elapsed();
    volatile uint32_t r1 = spin(100);    (void)r1;
    uint64_t e1 = sh_elapsed();
    volatile uint32_t r2 = spin(1000);   (void)r2;
    uint64_t e2 = sh_elapsed();
    volatile uint32_t r3 = spin(10000);  (void)r3;
    uint64_t e3 = sh_elapsed();
    volatile uint32_t r4 = spin(100000); (void)r4;
    uint64_t e4 = sh_elapsed();

    /* Print just the low 32 bits — our deltas fit. */
    p = buf;
    const char *l1 = "spin(100):    "; while(*l1) *p++=*l1++;
    p = u32_hex(p, (uint32_t)(e1 - e0)); *p++='\n'; *p=0; sh_puts(buf);

    p = buf;
    const char *l2 = "spin(1000):   "; while(*l2) *p++=*l2++;
    p = u32_hex(p, (uint32_t)(e2 - e1)); *p++='\n'; *p=0; sh_puts(buf);

    p = buf;
    const char *l3 = "spin(10000):  "; while(*l3) *p++=*l3++;
    p = u32_hex(p, (uint32_t)(e3 - e2)); *p++='\n'; *p=0; sh_puts(buf);

    p = buf;
    const char *l4 = "spin(100000): "; while(*l4) *p++=*l4++;
    p = u32_hex(p, (uint32_t)(e4 - e3)); *p++='\n'; *p=0; sh_puts(buf);

    sh_exit();
    return 0;
}
