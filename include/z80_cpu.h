#pragma once

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum Z80StepResult {
    Z80_STEP_OK = 0,
    Z80_STEP_HALT = 1,
    Z80_STEP_UNIMPLEMENTED = 2,
} Z80StepResult;

typedef struct StateZ80 {
    UINT8 a;
    UINT8 f;
    UINT8 b;
    UINT8 c;
    UINT8 d;
    UINT8 e;
    UINT8 h;
    UINT8 l;
    UINT16 ix;
    UINT16 iy;
    UINT16 sp;
    UINT16 pc;
    UINT8 i;
    UINT8 r;
    UINT8 iff1;
    UINT8 iff2;
    UINT8 halted;
    UINT8 *memory;
    char last_error[128];
} StateZ80;

typedef struct ExecutionStatsZ80 {
    UINT64 total_tstates;
    UINT16 min_sp;
    bool min_sp_set;
} ExecutionStatsZ80;

StateZ80 *InitZ80(void);
void FreeZ80(StateZ80 *state);
void ResetZ80(StateZ80 *state, UINT16 pc, UINT16 sp);

Z80StepResult EmulateZ80Op(StateZ80 *state, ExecutionStatsZ80 *stats);
int DisassembleZ80Op(const UINT8 *codebuffer, int pc, char *out, size_t out_len);

UINT8 *GetZ80Memory(StateZ80 *state);
const char *GetZ80LastError(const StateZ80 *state);

#ifdef __cplusplus
}
#endif
