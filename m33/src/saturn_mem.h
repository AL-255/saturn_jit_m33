/* Flat nibble-addressable memory model.
 *
 *   Saturn addr 0x00000..rom_size-1  → ROM nibble in flash
 *   Saturn addr ram_base..+ram_size  → RAM nibble in SRAM
 *   anywhere else                    → reads return 0, writes drop
 *
 * For benchmarking we put a tight Saturn-bytecode workload at the
 * start of ROM (program origin 0). RAM begins at 0x80000 to give
 * realistic 20-bit addresses for DAT0/DAT1 traffic. */

#ifndef SATURN_MEM_H
#define SATURN_MEM_H

#include "saturn_state.h"

static inline nibble_t sat_fetch(addr_t a) {
    a &= 0xFFFFFu;
    if (a < saturn.rom_size) return saturn.rom[a] & 0xf;
    uint32_t off = a - saturn.ram_base;
    if (off < saturn.ram_size) return saturn.ram[off] & 0xf;
    return 0;
}

static inline void sat_store(addr_t a, nibble_t v) {
    a &= 0xFFFFFu;
    uint32_t off = a - saturn.ram_base;
    if (off < saturn.ram_size) saturn.ram[off] = v & 0xf;
    /* ROM writes silently dropped, matching real HP 48 ROM behavior. */
}

/* Fetch an unsigned k-nibble little-endian field starting at addr. */
static inline uint32_t sat_fetch_field(addr_t a, int k) {
    uint32_t v = 0;
    for (int i = 0; i < k; i++) v |= (uint32_t)sat_fetch(a + i) << (i * 4);
    return v;
}

/* Sign-extend a k-nibble (k*4-bit) signed value. */
static inline int32_t sext_nib(uint32_t v, int k) {
    int bits = k * 4;
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((v ^ sign) - sign);
}

#endif
