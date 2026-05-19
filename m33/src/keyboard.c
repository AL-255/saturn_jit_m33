#include "keyboard.h"
#include "saturn_state.h"

static uint32_t s_state = 0xC0FFEE42u;
static uint32_t s_count = 0;

static inline uint32_t xs32(void) {
    uint32_t x = s_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s_state = x;
}

void kbd_seed(uint32_t s) { s_state = s ? s : 1; s_count = 0; }

void kbd_step(void) {
    uint32_t r = xs32();
    /* The HP 48 has a 16-bit-keyboard-scan-style IN register. Treat
     * the 16 nibbles of IN[0..3] as 16 bits worth of "pressed keys"
     * — at most one or two bits set per refresh. */
    for (int i = 0; i < 4; i++) saturn.in[i] = 0;
    int idx1 = r & 0xf;        r >>= 4;
    int idx2 = (r & 0xf);      r >>= 4;
    int prob = r & 0xf;
    saturn.in[idx1 / 4] |= (1 << (idx1 & 3));
    if (prob > 12)
        saturn.in[idx2 / 4] |= (1 << (idx2 & 3));
    saturn.kbd_pending = (saturn.in[0] | saturn.in[1] | saturn.in[2] | saturn.in[3]) != 0;
    s_count++;
}

uint32_t kbd_count(void) { return s_count; }
