#pragma once

#include "types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Z80_TRACE_IO_PLUGIN_ABI_VERSION 1u

typedef enum Z80TracePluginInterruptKind {
    Z80_TRACE_PLUGIN_INTERRUPT_NONE = 0,
    Z80_TRACE_PLUGIN_INTERRUPT_NMI = 1,
    Z80_TRACE_PLUGIN_INTERRUPT_INT = 2,
} Z80TracePluginInterruptKind;

typedef struct Z80TracePluginInterruptRequest {
    Z80TracePluginInterruptKind kind;
    UINT8 vector;
} Z80TracePluginInterruptRequest;

typedef struct Z80TraceIoPluginApi {
    UINT32 abi_version;
    const char *name;
    void *(*create)(const char *config, char *error_out, size_t error_out_len);
    void (*destroy)(void *opaque);
    void (*reset)(void *opaque);
    UINT8 (*read_port)(void *opaque, UINT16 port, UINT8 default_value);
    void (*write_port)(void *opaque, UINT16 port, UINT8 value);
    void (*after_instruction)(void *opaque, UINT64 step, UINT64 total_tstates, Z80TracePluginInterruptRequest *out);
} Z80TraceIoPluginApi;

typedef const Z80TraceIoPluginApi *(*Z80TraceGetIoPluginApiFn)(void);

#ifdef __cplusplus
}
#endif
