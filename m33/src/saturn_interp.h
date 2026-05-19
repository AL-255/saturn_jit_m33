/* Interpreter entry point. Returns the number of Saturn ops actually
 * executed before either hitting `budget`, hitting an unimplemented
 * opcode (and trapping), or PC leaving any mapped region. */

#ifndef SATURN_INTERP_H
#define SATURN_INTERP_H

#include "saturn_state.h"
#include <stdint.h>

typedef enum {
    INTERP_OK_BUDGET = 0,    /* ran `budget` ops and stopped */
    INTERP_UNIMPL    = 1,    /* hit an opcode we don't model */
    INTERP_HALT      = 2,    /* SHUTDN or similar */
    INTERP_PC_OOB    = 3,    /* PC walked into unmapped memory */
} interp_status_t;

interp_status_t saturn_run_interp(uint64_t budget);

#endif
