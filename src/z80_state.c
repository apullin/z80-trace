#include "z80_cpu.h"

#include <stdlib.h>
#include <string.h>

StateZ80 *InitZ80(void) {
    StateZ80 *state = (StateZ80 *)calloc(1, sizeof(StateZ80));
    if (!state) {
        return NULL;
    }

    state->memory = (UINT8 *)calloc(0x10000u, sizeof(UINT8));
    if (!state->memory) {
        free(state);
        return NULL;
    }

    return state;
}

void FreeZ80(StateZ80 *state) {
    if (!state) {
        return;
    }

    free(state->memory);
    free(state);
}

void ResetZ80(StateZ80 *state, UINT16 pc, UINT16 sp) {
    if (!state) {
        return;
    }

    state->a = 0;
    state->f = 0;
    state->b = 0;
    state->c = 0;
    state->d = 0;
    state->e = 0;
    state->h = 0;
    state->l = 0;
    state->ix = 0;
    state->iy = 0;
    state->sp = sp;
    state->pc = pc;
    state->i = 0;
    state->r = 0;
    state->iff1 = 0;
    state->iff2 = 0;
    state->halted = 0;
    memset(state->last_error, 0, sizeof(state->last_error));
}

UINT8 *GetZ80Memory(StateZ80 *state) {
    return state ? state->memory : NULL;
}

const char *GetZ80LastError(const StateZ80 *state) {
    if (!state) {
        return "no state";
    }
    return state->last_error;
}
