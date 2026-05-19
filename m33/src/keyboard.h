/* Random keyboard input generator. Drives the Saturn IN[] register
 * with a reproducible stream of key codes (seeded xorshift32).
 *
 * On the HP 48 the IN register is read via the I/O sub-opcodes
 * (8 0 2 A=IN, 8 0 3 C=IN). For the bench we just refresh IN[] at the
 * start of each measurement window so the keyboard surface is
 * non-trivial and not all zeros. */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void kbd_seed(uint32_t s);
void kbd_step(void);            /* advance one key event into saturn.in[] */
uint32_t kbd_count(void);

#endif
