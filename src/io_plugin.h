#pragma once

#include "z80_cpu.h"
#include "z80_io_plugin.h"

#include <string>

class IoPluginHost {
  public:
    IoPluginHost() = default;
    ~IoPluginHost();

    IoPluginHost(const IoPluginHost &) = delete;
    IoPluginHost &operator=(const IoPluginHost &) = delete;

    bool Load(const char *path, const char *config, std::string *error);
    void Reset();
    void Close();

    bool loaded() const { return handle_ != nullptr && api_ != nullptr; }
    const char *name() const;

    void Attach(StateZ80 *state);
    bool AfterInstruction(UINT64 step, UINT64 total_tstates, bool *nmi, UINT8 *vector);

  private:
    static UINT8 ReadThunk(void *user, UINT16 addr, UINT8 default_value);
    static void WriteThunk(void *user, UINT16 addr, UINT8 value);

    UINT8 Read(UINT16 addr, UINT8 default_value);
    void Write(UINT16 addr, UINT8 value);

    void *handle_ = nullptr;
    const Z80TraceIoPluginApi *api_ = nullptr;
    void *opaque_ = nullptr;
};
