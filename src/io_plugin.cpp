#include "io_plugin.h"

#include <dlfcn.h>

#include <cstring>

IoPluginHost::~IoPluginHost() {
    Close();
}

bool IoPluginHost::Load(const char *path, const char *config, std::string *error) {
    Close();

    handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        if (error) {
            *error = dlerror() ? dlerror() : "dlopen failed";
        }
        return false;
    }

    dlerror();
    auto get_api = reinterpret_cast<Z80TraceGetIoPluginApiFn>(dlsym(handle_, "z80_trace_get_io_plugin_api"));
    const char *sym_error = dlerror();
    if (sym_error || !get_api) {
        if (error) {
            *error = sym_error ? sym_error : "missing z80_trace_get_io_plugin_api";
        }
        Close();
        return false;
    }

    api_ = get_api();
    if (!api_) {
        if (error) {
            *error = "plugin returned null API";
        }
        Close();
        return false;
    }
    if (api_->abi_version != Z80_TRACE_IO_PLUGIN_ABI_VERSION) {
        if (error) {
            *error = "plugin ABI version mismatch";
        }
        Close();
        return false;
    }

    if (api_->create) {
        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
        opaque_ = api_->create(config, buffer, sizeof(buffer));
        if (!opaque_ && buffer[0] != '\0') {
            if (error) {
                *error = buffer;
            }
            Close();
            return false;
        }
    }

    return true;
}

void IoPluginHost::Reset() {
    if (api_ && api_->reset) {
        api_->reset(opaque_);
    }
}

void IoPluginHost::Close() {
    if (api_ && api_->destroy) {
        api_->destroy(opaque_);
    }
    opaque_ = nullptr;
    api_ = nullptr;
    if (handle_) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

const char *IoPluginHost::name() const {
    if (!api_ || !api_->name || api_->name[0] == '\0') {
        return "plugin";
    }
    return api_->name;
}

void IoPluginHost::Attach(StateZ80 *state) {
    if (!state) {
        return;
    }
    if (!loaded()) {
        state->io_user = nullptr;
        state->io_read = nullptr;
        state->io_write = nullptr;
        return;
    }
    state->io_user = this;
    state->io_read = &IoPluginHost::ReadThunk;
    state->io_write = &IoPluginHost::WriteThunk;
}

bool IoPluginHost::AfterInstruction(UINT64 step, UINT64 total_tstates, bool *nmi, UINT8 *vector) {
    if (nmi) {
        *nmi = false;
    }
    if (vector) {
        *vector = 0xFF;
    }
    if (!api_ || !api_->after_instruction) {
        return false;
    }

    Z80TracePluginInterruptRequest request = {};
    api_->after_instruction(opaque_, step, total_tstates, &request);
    if (request.kind == Z80_TRACE_PLUGIN_INTERRUPT_NMI) {
        if (nmi) {
            *nmi = true;
        }
        return true;
    }
    if (request.kind == Z80_TRACE_PLUGIN_INTERRUPT_INT) {
        if (vector) {
            *vector = request.vector;
        }
        return true;
    }
    return false;
}

UINT8 IoPluginHost::ReadThunk(void *user, UINT16 addr, UINT8 default_value) {
    auto *self = static_cast<IoPluginHost *>(user);
    return self ? self->Read(addr, default_value) : default_value;
}

void IoPluginHost::WriteThunk(void *user, UINT16 addr, UINT8 value) {
    auto *self = static_cast<IoPluginHost *>(user);
    if (self) {
        self->Write(addr, value);
    }
}

UINT8 IoPluginHost::Read(UINT16 addr, UINT8 default_value) {
    if (!api_ || !api_->read_port) {
        return default_value;
    }
    return api_->read_port(opaque_, addr, default_value);
}

void IoPluginHost::Write(UINT16 addr, UINT8 value) {
    if (api_ && api_->write_port) {
        api_->write_port(opaque_, addr, value);
    }
}
