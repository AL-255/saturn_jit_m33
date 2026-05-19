/* Saturn → Thumb-2 basic-block JIT.
 *
 * A *block* is the longest sequence of Saturn nibbles starting at
 * `start_pc` that ends at either:
 *   - a control-flow instruction (GOTO/GOSUB/GOC/GONC/GOLONG/GOSUBL/
 *     GOTO-abs/GOSBVL/RTN-family/9x/8A/8B/86/87/88/89)
 *   - an unimplemented opcode (forces fallback to interpreter)
 *   - a configurable maximum nibble count (`max_nibs`)
 *
 * `saturn_jit_translate` writes compiled Thumb-2 into `out_buf` and
 * returns a function pointer that the dispatcher can call with:
 *     r0 = &saturn
 *     return-value = next Saturn PC to execute
 * On translation failure (out of buffer space, unsupported opcode at
 * start_pc) it returns NULL and *out_used is 0. */

#ifndef SATURN_JIT_H
#define SATURN_JIT_H

#include <stdint.h>
#include "saturn_state.h"

typedef addr_t (*jit_block_fn_t)(saturn_t *st);

typedef struct jit_block_meta_s {
    addr_t   start_pc;
    uint32_t saturn_nibs;     /* Saturn nibbles consumed */
    uint32_t code_bytes;      /* Thumb-2 bytes emitted */
    uint32_t saturn_ops;      /* Saturn ops translated */
    uint16_t body_off_hw;     /* halfword index where body starts (after prologue) */
    uint16_t chain_insn_hw;   /* halfword offset (block-relative) of the
                               * indirect-chain insn that can be patched to
                               * a direct B.W. 0 if no patchable chain
                               * (self-loop, dyn_end, no link). */
    uint32_t static_next_pc;  /* statically-known next PC, or 0xFFFFFFFE for dynamic */
} jit_block_meta_t;

jit_block_fn_t saturn_jit_translate(addr_t start_pc,
                                    void *out_buf, uint32_t out_cap,
                                    uint32_t *out_used,
                                    jit_block_meta_t *meta);

/* Linked variant: link_target points at the slot's link_target word
 * in cache RAM; the emitter bakes its address in via movw/movt. Pass
 * NULL to disable linking for this block (e.g. cache-off mode). */
jit_block_fn_t saturn_jit_translate_linked(addr_t start_pc,
                                           void *out_buf, uint32_t out_cap,
                                           uint32_t *out_used,
                                           jit_block_meta_t *meta,
                                           uintptr_t *link_target);

#define JIT_DYN_NEXT_PC 0xFFFFFFFEu

#endif
