/* Board-specific replacement for m33/src/semihost.h.
 *
 * The CMake build adds boards/adafruit_6130/ to the include path BEFORE
 * m33/src/, so any TU that #include "semihost.h" sees these
 * declarations (RP2350 UART/USB-backed) instead of the inline ARM-
 * semihosting `bkpt #0xAB` calls.
 *
 * sh_init_runtime() is RP2350-specific — call from main() before any
 * other sh_* function so stdio + DWT are live. */

#ifndef SEMIHOST_H
#define SEMIHOST_H
#include <stdint.h>

void     sh_init_runtime(void);
void     sh_puts(const char *s);
void     sh_exit(void);
uint64_t sh_elapsed(void);
uint64_t sh_tickfreq(void);

#endif
