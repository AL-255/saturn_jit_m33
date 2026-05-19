#ifndef SEMIHOST_H
#define SEMIHOST_H
#include <stdint.h>

static inline int semihost(int op, const void *arg) {
    register int      r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    register int      ret __asm__("r0");
    __asm__ volatile ("bkpt #0xAB" : "=r"(ret) : "r"(r0), "r"(r1) : "memory");
    return ret;
}

#define SH_WRITE0  0x04
#define SH_EXIT    0x18
#define SH_ELAPSED 0x30
#define SH_TICKFRQ 0x31

static inline void sh_puts(const char *s) { semihost(SH_WRITE0, s); }
static inline void sh_exit(void) { semihost(SH_EXIT, (const void *)0x20026); for(;;){} }

static inline uint64_t sh_elapsed(void) {
    uint32_t b[2] = {0,0};
    semihost(SH_ELAPSED, b);
    return ((uint64_t)b[1] << 32) | b[0];
}
static inline int32_t sh_tickfreq(void) {
    return semihost(SH_TICKFRQ, (void *)0);
}

#endif
