/* Synthetic Saturn workloads for benchmarking.
 *
 * Each workload emits a self-contained nibble image: a body that loops
 * over a known number of Saturn ops, then GOSUBs (or just falls
 * through) to a sentinel that causes the interpreter/JIT to stop.
 *
 * We aim for a representative mix of the opcodes a real HP 48 hot
 * loop would touch: field ADD/SUB/INC/DEC, A-field moves/exchanges,
 * D0/D1 manipulation, DAT0/DAT1 5-nibble I/O, conditional branches
 * forming an outer counted loop. */

#ifndef WORKLOAD_H
#define WORKLOAD_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint8_t    *image;     /* nibble per byte, low nibble used */
    uint32_t    image_nibs;
    uint32_t    entry_pc;
    uint32_t    expected_ops_per_iter;
    uint32_t    iters;     /* the inner counted-loop trip count */
} workload_t;

/* Build the workloads into a provided RAM buffer (we generate them at
 * boot because they're tiny and that lets us parameterize the loop
 * trip count from main). */
const workload_t *workload_build_arith(uint8_t *buf, uint32_t cap, uint32_t iters);
const workload_t *workload_build_memmix(uint8_t *buf, uint32_t cap, uint32_t iters);
/* "calltree": tight subroutine — outer counted loop with GOSBVL/RTN on each iter. */
const workload_t *workload_build_calltree(uint8_t *buf, uint32_t cap, uint32_t iters);
/* "countloop": down-counter with ?A=0 A then restart. Exercises group 8A compare+branch. */
const workload_t *workload_build_countloop(uint8_t *buf, uint32_t cap, uint32_t iters);
/* "nqueens": tight 8-queens backtracking solver in Saturn assembly,
 * adapted from the classic HP-48 RPL routine. Exercises P-field
 * arithmetic, GOSUB/RTNCC, and many compare-branches. */
const workload_t *workload_build_nqueens(uint8_t *buf, uint32_t cap, uint32_t iters);

#endif
