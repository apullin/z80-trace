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

typedef enum Z80BusEventKind {
    Z80_BUS_EVENT_FETCH = 0,
    Z80_BUS_EVENT_READ = 1,
    Z80_BUS_EVENT_WRITE = 2,
    Z80_BUS_EVENT_IO_READ = 3,
    Z80_BUS_EVENT_IO_WRITE = 4,
    Z80_BUS_EVENT_INTERRUPT_ACK = 5,
} Z80BusEventKind;

typedef struct Z80BusEvent {
    Z80BusEventKind kind;
    UINT16 addr;
    UINT8 data;
} Z80BusEvent;

#define Z80_MAX_STEP_EVENTS 32

typedef struct Z80StepEvents {
    Z80BusEvent events[Z80_MAX_STEP_EVENTS];
    size_t count;
} Z80StepEvents;

typedef UINT8 (*Z80IoReadCallback)(void *user, UINT16 addr, UINT8 default_value);
typedef void (*Z80IoWriteCallback)(void *user, UINT16 addr, UINT8 value);

typedef struct StateZ80 {
    UINT8 a;
    UINT8 f;
    UINT8 a_alt;
    UINT8 f_alt;
    UINT8 b;
    UINT8 c;
    UINT8 b_alt;
    UINT8 c_alt;
    UINT8 d;
    UINT8 e;
    UINT8 d_alt;
    UINT8 e_alt;
    UINT8 h;
    UINT8 l;
    UINT8 h_alt;
    UINT8 l_alt;
    UINT16 ix;
    UINT16 iy;
    UINT16 sp;
    UINT16 pc;
    UINT8 i;
    UINT8 r;
    UINT8 im;
    UINT8 iff1;
    UINT8 iff2;
    UINT8 ei_pending;
    UINT8 nmi_pending;
    UINT8 irq_pending;
    UINT8 irq_vector;
    UINT8 halted;
    UINT8 *memory;
    UINT8 *io_space;
    void *io_user;
    Z80IoReadCallback io_read;
    Z80IoWriteCallback io_write;
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

Z80StepResult EmulateZ80Op(StateZ80 *state, ExecutionStatsZ80 *stats, Z80StepEvents *events);
int DisassembleZ80Op(const UINT8 *codebuffer, int pc, char *out, size_t out_len);

UINT8 *GetZ80Memory(StateZ80 *state);
const char *GetZ80LastError(const StateZ80 *state);

#ifdef __cplusplus
}
#endif
