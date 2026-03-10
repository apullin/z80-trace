#include "io_plugin.h"
#include "z80_cpu.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <netinet/in.h>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

struct MemoryDump {
    UINT16 start;
    UINT16 length;
};

struct Tracepoint {
    UINT16 pc;
    UINT32 hits = 0;
};

struct InterruptSpec {
    bool nmi = false;
    UINT64 step = 0;
    UINT8 vector = 0xFF;
    bool delivered = false;
};

struct TimerSpec {
    bool nmi = false;
    UINT64 period = 0;
    UINT64 nextFire = 0;
    UINT8 vector = 0xFF;
};

struct Config {
    const char *inputFile = nullptr;
    const char *outputFile = nullptr;
    const char *coverageFile = nullptr;
    const char *ioPluginPath = nullptr;
    const char *ioPluginConfig = nullptr;
    UINT16 loadAddr = 0x0000;
    UINT16 entryAddr = 0x0000;
    UINT16 spAddr = 0xFFFF;
    UINT64 maxSteps = 1000000;
    int gdbPort = -1;
    bool loopDetect = true;
    bool quiet = false;
    bool summary = false;
    bool busTrace = false;
    bool ioTrace = false;
    bool entrySet = false;
    std::vector<UINT16> stopAddrs;
    std::vector<MemoryDump> dumps;
    std::vector<Tracepoint> tracepoints;
    std::vector<InterruptSpec> interrupts;
    std::vector<TimerSpec> timers;
    UINT64 tracepointMax = 0;
    bool tracepointStop = false;
};

struct TraceState {
    UINT64 step;
    UINT16 pc;
    UINT16 sp;
    UINT8 flags;
    UINT64 clocks;
    const char *mnemonic;
    const char *disasm;
};

struct LoopState {
    UINT16 pc = 0;
    UINT16 sp = 0;
    UINT8 a = 0;
    UINT8 f = 0;
    UINT8 a_alt = 0;
    UINT8 f_alt = 0;
    UINT8 b = 0;
    UINT8 c = 0;
    UINT8 b_alt = 0;
    UINT8 c_alt = 0;
    UINT8 d = 0;
    UINT8 e = 0;
    UINT8 d_alt = 0;
    UINT8 e_alt = 0;
    UINT8 h = 0;
    UINT8 l = 0;
    UINT8 h_alt = 0;
    UINT8 l_alt = 0;
    UINT16 ix = 0;
    UINT16 iy = 0;
    UINT8 i = 0;
    UINT8 r = 0;
    UINT8 im = 0;
    UINT8 iff1 = 0;
    UINT8 iff2 = 0;
    UINT8 halted = 0;
};

static void PrintUsage(const char *prog) {
    fprintf(stderr, "Z80 Trace - Standalone Z80 CPU Simulator\n\n");
    fprintf(stderr, "Usage: %s [options] <binary.bin>\n\n", prog);
    fprintf(stderr, "Memory Options:\n");
    fprintf(stderr, "  -l, --load=ADDR       Load address (hex, default: 0x0000)\n");
    fprintf(stderr, "  -e, --entry=ADDR      Entry point (hex, default: same as load)\n");
    fprintf(stderr, "  -p, --sp=ADDR         Initial stack pointer (hex, default: 0xFFFF)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Execution Options:\n");
    fprintf(stderr, "  -n, --max-steps=N     Max instructions (default: 1000000)\n");
    fprintf(stderr, "  -s, --stop-at=ADDR    Stop at address (hex, can repeat)\n");
    fprintf(stderr, "  --no-loop-detect      Disable infinite loop detection\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Output Options:\n");
    fprintf(stderr, "  -o, --output=FILE     Output file (default: stdout)\n");
    fprintf(stderr, "  -q, --quiet           Only output trace or summary JSON\n");
    fprintf(stderr, "  -S, --summary         Output only final state as JSON, unless tracepoints are set\n");
    fprintf(stderr, "  --bus-trace           Emit harness-style BUS/WRITE/HALT lines instead of NDJSON trace records\n");
    fprintf(stderr, "  -d, --dump=START:LEN  Dump memory range at exit (hex, can repeat)\n");
    fprintf(stderr, "  --cov=FILE            Write coverage JSON (pc/opcode hit counts)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Tracepoint Options (require -S):\n");
    fprintf(stderr, "  -t, --tracepoint=ADDR       Trace only this address (hex, can repeat)\n");
    fprintf(stderr, "  -T, --tracepoint-file=FILE  Load tracepoint addresses from file\n");
    fprintf(stderr, "  --tracepoint-max=N          Stop after N total tracepoint hits\n");
    fprintf(stderr, "  --tracepoint-stop           Stop when all tracepoints hit at least once\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Runtime Options:\n");
    fprintf(stderr, "  --irq=SPEC            Schedule interrupt injection with SPEC as nmi:STEP or int:STEP[:0xVV]\n");
    fprintf(stderr, "  --timer=SPEC          Periodic timer as nmi:PERIOD[:START] or int:PERIOD[:START[:0xVV]] in T-states\n");
    fprintf(stderr, "  --io-plugin=PATH      Load a shared-library I/O plugin\n");
    fprintf(stderr, "  --io-plugin-config=S  Pass opaque config string S to the loaded I/O plugin\n");
    fprintf(stderr, "  --io-trace            Emit I/O activity JSON records on stderr\n");
    fprintf(stderr, "  --gdb=PORT            Serve a GDB Remote Serial Protocol session on 127.0.0.1:PORT (0 picks any free port)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Other:\n");
    fprintf(stderr, "  -h, --help            Show this help\n");
}

static bool ParseHex(const char *str, UINT16 *value) {
    if (!str || str[0] != '0' || (str[1] != 'x' && str[1] != 'X')) {
        return false;
    }

    char *end = nullptr;
    unsigned long parsed = strtoul(str, &end, 16);
    if (*end != '\0' || parsed > 0xFFFF) {
        return false;
    }

    *value = (UINT16)parsed;
    return true;
}

static bool ParseByteValue(const char *str, UINT8 *value) {
    char *end = nullptr;
    unsigned long parsed = 0;
    if (str && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        parsed = strtoul(str, &end, 16);
    } else {
        parsed = strtoul(str, &end, 10);
    }
    if (!str || *str == '\0' || *end != '\0' || parsed > 0xFF) {
        return false;
    }
    *value = (UINT8)parsed;
    return true;
}

static bool ParseU64Value(const char *str, UINT64 *value) {
    char *end = nullptr;
    unsigned long long parsed = 0;
    if (!str || *str == '\0') {
        return false;
    }
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        parsed = strtoull(str, &end, 16);
    } else {
        parsed = strtoull(str, &end, 10);
    }
    if (*end != '\0') {
        return false;
    }
    *value = (UINT64)parsed;
    return true;
}

static bool ParsePortNumber(const char *str, int *value) {
    UINT64 parsed = 0;
    if (!value || !ParseU64Value(str, &parsed) || parsed > 65535) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool ParseInterruptSpec(const char *spec, InterruptSpec *out) {
    char buffer[128];
    char *first = nullptr;
    char *second = nullptr;
    char *third = nullptr;
    char *end = nullptr;
    unsigned long step = 0;

    if (!spec || !out || strlen(spec) >= sizeof(buffer)) {
        return false;
    }
    snprintf(buffer, sizeof(buffer), "%s", spec);
    first = strchr(buffer, ':');
    if (!first) {
        return false;
    }
    *first++ = '\0';
    second = strchr(first, ':');
    if (second) {
        *second++ = '\0';
        third = second;
    }

    if (strcmp(buffer, "nmi") == 0) {
        out->nmi = true;
    } else if (strcmp(buffer, "int") == 0 || strcmp(buffer, "irq") == 0) {
        out->nmi = false;
    } else {
        return false;
    }

    step = strtoull(first, &end, 10);
    if (*first == '\0' || *end != '\0') {
        return false;
    }
    out->step = (UINT64)step;
    out->vector = 0xFF;
    out->delivered = false;

    if (third && !ParseByteValue(third, &out->vector)) {
        return false;
    }
    return true;
}

static bool ParseTimerSpec(const char *spec, TimerSpec *out) {
    char buffer[128];
    char *parts[4] = {nullptr, nullptr, nullptr, nullptr};
    char *cursor = nullptr;
    size_t count = 0;
    UINT64 period = 0;
    UINT64 start = 0;

    if (!spec || !out || strlen(spec) >= sizeof(buffer)) {
        return false;
    }
    snprintf(buffer, sizeof(buffer), "%s", spec);
    cursor = buffer;
    while (count < 4) {
        parts[count++] = cursor;
        char *colon = strchr(cursor, ':');
        if (!colon) {
            break;
        }
        *colon = '\0';
        cursor = colon + 1;
    }

    if (count < 2) {
        return false;
    }

    if (strcmp(parts[0], "nmi") == 0) {
        out->nmi = true;
    } else if (strcmp(parts[0], "int") == 0 || strcmp(parts[0], "irq") == 0) {
        out->nmi = false;
    } else {
        return false;
    }

    if (!ParseU64Value(parts[1], &period) || period == 0) {
        return false;
    }
    start = period;
    if (count >= 3 && parts[2] && parts[2][0] != '\0') {
        if (!ParseU64Value(parts[2], &start)) {
            return false;
        }
    }

    out->period = period;
    out->nextFire = start;
    out->vector = 0xFF;

    if (count >= 4 && parts[3] && parts[3][0] != '\0' && !ParseByteValue(parts[3], &out->vector)) {
        return false;
    }
    return true;
}

static bool ParseDump(char *str, MemoryDump *dump) {
    char *colon = strchr(str, ':');
    if (!colon) {
        return false;
    }

    *colon = '\0';
    UINT16 start = 0;
    bool ok = ParseHex(str, &start);
    *colon = ':';
    if (!ok) {
        return false;
    }

    char *end = nullptr;
    unsigned long length;
    if (colon[1] == '0' && (colon[2] == 'x' || colon[2] == 'X')) {
        length = strtoul(colon + 1, &end, 16);
    } else {
        length = strtoul(colon + 1, &end, 10);
    }

    if (*end != '\0' || length == 0 || length > 0x10000) {
        return false;
    }

    dump->start = start;
    dump->length = (UINT16)length;
    return true;
}

static void AddTracepoint(std::vector<Tracepoint> &tracepoints, UINT16 addr) {
    for (const auto &tp : tracepoints) {
        if (tp.pc == addr) {
            return;
        }
    }

    Tracepoint tp;
    tp.pc = addr;
    tracepoints.push_back(tp);
}

static bool ParseTracepointFile(const char *path, std::vector<Tracepoint> &tracepoints) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == '#' || *p == '\0' || *p == '\n') {
            continue;
        }

        char *end = nullptr;
        unsigned long value;
        if (strncmp(p, "0x", 2) == 0 || strncmp(p, "0X", 2) == 0) {
            value = strtoul(p + 2, &end, 16);
        } else {
            value = strtoul(p, &end, 16);
        }

        if ((*end != '\0' && *end != '\n') || value > 0xFFFF) {
            fclose(f);
            return false;
        }

        AddTracepoint(tracepoints, (UINT16)value);
    }

    fclose(f);
    return true;
}

static bool LoadBinary(StateZ80 *state, const char *filename, UINT16 loadAddr) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fprintf(stderr, "Error: Failed to get file size\n");
        fclose(f);
        return false;
    }

    if (loadAddr + size > 0x10000) {
        fprintf(stderr, "Error: Program too large (0x%04X + %ld bytes)\n", loadAddr, size);
        fclose(f);
        return false;
    }

    size_t read = fread(&state->memory[loadAddr], 1, (size_t)size, f);
    fclose(f);

    if ((long)read != size) {
        fprintf(stderr, "Error: Short read (%zu of %ld bytes)\n", read, size);
        return false;
    }

    return true;
}

static void ExtractMnemonic(const char *disasm, char *out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }

    const char *p = disasm;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }

    const char *space = strpbrk(p, " \t");
    size_t len = space ? (size_t)(space - p) : strlen(p);
    if (len >= outLen) {
        len = outLen - 1;
    }

    memcpy(out, p, len);
    out[len] = '\0';
}

static void OutputTrace(FILE *out, const TraceState &t, const StateZ80 *state) {
    fprintf(out, "{\"step\":%" PRIu64 ",\"pc\":\"%04X\",\"sp\":\"%04X\",\"f\":\"%02X\",\"clk\":%" PRIu64 ",",
            t.step, t.pc, t.sp, t.flags, t.clocks);
    fprintf(out,
            "\"op\":\"%s\",\"asm\":\"%s\",\"ix\":\"%04X\",\"iy\":\"%04X\",\"i\":\"%02X\",\"r8\":\"%02X\","
            "\"im\":%u,\"iff1\":%u,\"iff2\":%u,\"af2\":\"%04X\",\"bc2\":\"%04X\",\"de2\":\"%04X\",\"hl2\":\"%04X\",",
            t.mnemonic, t.disasm, state->ix, state->iy, state->i, state->r, state->im, state->iff1, state->iff2,
            (unsigned)((state->a_alt << 8) | state->f_alt), (unsigned)((state->b_alt << 8) | state->c_alt),
            (unsigned)((state->d_alt << 8) | state->e_alt), (unsigned)((state->h_alt << 8) | state->l_alt));
    fprintf(out, "\"r\":[\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\"]}\n", state->a, state->b,
            state->c, state->d, state->e, state->h, state->l);
}

static void OutputBusEvent(FILE *out, UINT64 cycle, const Z80BusEvent &event) {
    int m1 = 0;
    int mreq = 0;
    int iorq = 0;
    int rd = 0;
    int wr = 0;
    const char *kind = "read";

    switch (event.kind) {
    case Z80_BUS_EVENT_FETCH:
        kind = "fetch";
        m1 = 1;
        mreq = 1;
        rd = 1;
        break;
    case Z80_BUS_EVENT_READ:
        kind = "read";
        mreq = 1;
        rd = 1;
        break;
    case Z80_BUS_EVENT_WRITE:
        kind = "write";
        mreq = 1;
        wr = 1;
        break;
    case Z80_BUS_EVENT_IO_READ:
        kind = "io-read";
        iorq = 1;
        rd = 1;
        break;
    case Z80_BUS_EVENT_IO_WRITE:
        kind = "io-write";
        iorq = 1;
        wr = 1;
        break;
    case Z80_BUS_EVENT_INTERRUPT_ACK:
        kind = "intack";
        m1 = 1;
        iorq = 1;
        break;
    }

    fprintf(out,
            "BUS cycle=%" PRIu64 " kind=%s addr=0x%04X data=0x%02X m1=%d mreq=%d iorq=%d rd=%d wr=%d rfsh=0 halt=0 "
            "busak=0\n",
            cycle, kind, event.addr, event.data, m1, mreq, iorq, rd, wr);
    if (event.kind == Z80_BUS_EVENT_WRITE) {
        fprintf(out, "WRITE cycle=%" PRIu64 " addr=0x%04X data=0x%02X\n", cycle, event.addr, event.data);
    }
}

static void OutputBusStep(FILE *out, UINT64 *cycle, UINT64 *writes, const Z80StepEvents &events) {
    for (size_t i = 0; i < events.count; ++i) {
        OutputBusEvent(out, *cycle, events.events[i]);
        if (events.events[i].kind == Z80_BUS_EVENT_WRITE) {
            *writes += 1;
        }
        *cycle += 1;
    }
}

static void OutputIoTrace(FILE *err, UINT64 step, const Z80StepEvents &events, bool pluginLoaded) {
    const char *source = pluginLoaded ? "plugin" : "io-space";
    for (size_t i = 0; i < events.count; ++i) {
        const Z80BusEvent &event = events.events[i];
        const char *kind = nullptr;
        if (event.kind == Z80_BUS_EVENT_IO_READ) {
            kind = "read";
        } else if (event.kind == Z80_BUS_EVENT_IO_WRITE) {
            kind = "write";
        }
        if (!kind) {
            continue;
        }
        fprintf(err,
                "{\"type\":\"io\",\"step\":%" PRIu64 ",\"kind\":\"%s\",\"port\":\"%04X\",\"data\":\"%02X\","
                "\"source\":\"%s\"}\n",
                step, kind, event.addr, event.data, source);
    }
}

static void OutputHexDump(FILE *err, const StateZ80 *state, const MemoryDump &dump) {
    fprintf(err, "\nMemory dump 0x%04X - 0x%04X (%u bytes):\n", dump.start, (UINT16)(dump.start + dump.length - 1),
            dump.length);
    for (UINT16 offset = 0; offset < dump.length; offset += 16) {
        fprintf(err, "  %04X:", (UINT16)(dump.start + offset));
        for (UINT16 i = 0; i < 16 && offset + i < dump.length; ++i) {
            UINT16 addr = (UINT16)(dump.start + offset + i);
            fprintf(err, " %02X", state->memory[addr]);
        }
        fprintf(err, "  |");
        for (UINT16 i = 0; i < 16 && offset + i < dump.length; ++i) {
            UINT8 byte = state->memory[(UINT16)(dump.start + offset + i)];
            fprintf(err, "%c", (byte >= 32 && byte < 127) ? byte : '.');
        }
        fprintf(err, "|\n");
    }
}

static void OutputBusDump(FILE *out, const StateZ80 *state, const MemoryDump &dump) {
    for (UINT16 offset = 0; offset < dump.length; ++offset) {
        UINT16 addr = (UINT16)(dump.start + offset);
        fprintf(out, "MEM addr=0x%04X data=0x%02X\n", addr, state->memory[addr]);
    }
}

static void WriteCoverage(const char *path, UINT64 steps, const std::vector<UINT64> &pcHits,
                          const std::vector<UINT64> &opHits) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Error: Cannot open coverage file '%s'\n", path);
        return;
    }

    fprintf(f, "{\"steps\":%" PRIu64 ",\"pc_hits\":[", steps);
    bool first = true;
    for (size_t i = 0; i < pcHits.size(); ++i) {
        if (pcHits[i] == 0) {
            continue;
        }
        if (!first) {
            fprintf(f, ",");
        }
        fprintf(f, "{\"pc\":\"%04X\",\"count\":%" PRIu64 "}", (unsigned)i, pcHits[i]);
        first = false;
    }

    fprintf(f, "],\"op_hits\":[");
    first = true;
    for (size_t i = 0; i < opHits.size(); ++i) {
        if (opHits[i] == 0) {
            continue;
        }
        if (!first) {
            fprintf(f, ",");
        }
        fprintf(f, "{\"op\":\"%02X\",\"count\":%" PRIu64 "}", (unsigned)i, opHits[i]);
        first = false;
    }

    fprintf(f, "]}\n");
    fclose(f);
}

static bool IsStopAddress(UINT16 pc, const std::vector<UINT16> &stopAddrs) {
    return std::find(stopAddrs.begin(), stopAddrs.end(), pc) != stopAddrs.end();
}

static bool AllTracepointsHit(const std::vector<Tracepoint> &tracepoints) {
    if (tracepoints.empty()) {
        return false;
    }
    for (const auto &tp : tracepoints) {
        if (tp.hits == 0) {
            return false;
        }
    }
    return true;
}

static Tracepoint *FindTracepoint(std::vector<Tracepoint> &tracepoints, UINT16 pc) {
    for (auto &tp : tracepoints) {
        if (tp.pc == pc) {
            return &tp;
        }
    }
    return nullptr;
}

static LoopState CaptureLoopState(const StateZ80 *state) {
    LoopState snapshot;
    snapshot.pc = state->pc;
    snapshot.sp = state->sp;
    snapshot.a = state->a;
    snapshot.f = state->f;
    snapshot.a_alt = state->a_alt;
    snapshot.f_alt = state->f_alt;
    snapshot.b = state->b;
    snapshot.c = state->c;
    snapshot.b_alt = state->b_alt;
    snapshot.c_alt = state->c_alt;
    snapshot.d = state->d;
    snapshot.e = state->e;
    snapshot.d_alt = state->d_alt;
    snapshot.e_alt = state->e_alt;
    snapshot.h = state->h;
    snapshot.l = state->l;
    snapshot.h_alt = state->h_alt;
    snapshot.l_alt = state->l_alt;
    snapshot.ix = state->ix;
    snapshot.iy = state->iy;
    snapshot.i = state->i;
    snapshot.r = state->r;
    snapshot.im = state->im;
    snapshot.iff1 = state->iff1;
    snapshot.iff2 = state->iff2;
    snapshot.halted = state->halted;
    return snapshot;
}

static void ArmInterrupts(std::vector<InterruptSpec> &interrupts, StateZ80 *state, UINT64 step) {
    for (auto &irq : interrupts) {
        if (irq.delivered || irq.step != step) {
            continue;
        }
        if (irq.nmi) {
            state->nmi_pending = 1;
        } else {
            state->irq_pending = 1;
            state->irq_vector = irq.vector;
        }
        irq.delivered = true;
    }
}

static void ArmTimers(std::vector<TimerSpec> &timers, StateZ80 *state, UINT64 clocks) {
    for (auto &timer : timers) {
        bool due = false;
        if (timer.period == 0) {
            continue;
        }
        while (clocks >= timer.nextFire) {
            due = true;
            timer.nextFire += timer.period;
        }
        if (!due) {
            continue;
        }
        if (timer.nmi) {
            state->nmi_pending = 1;
        } else {
            state->irq_pending = 1;
            state->irq_vector = timer.vector;
        }
    }
}

static bool HasFutureInterrupts(const std::vector<InterruptSpec> &interrupts, UINT64 step) {
    for (const auto &irq : interrupts) {
        if (!irq.delivered && irq.step >= step) {
            return true;
        }
    }
    return false;
}

static bool HasActiveTimers(const std::vector<TimerSpec> &timers) {
    for (const auto &timer : timers) {
        if (timer.period != 0) {
            return true;
        }
    }
    return false;
}

static void ArmPluginInterrupt(IoPluginHost *plugin, StateZ80 *state, UINT64 step, UINT64 clocks) {
    bool nmi = false;
    UINT8 vector = 0xFF;
    if (!plugin || !plugin->loaded() || !plugin->AfterInstruction(step, clocks, &nmi, &vector)) {
        return;
    }
    if (nmi) {
        state->nmi_pending = 1;
    } else {
        state->irq_pending = 1;
        state->irq_vector = vector;
    }
}

struct DebugRuntime {
    Config *cfg = nullptr;
    StateZ80 *state = nullptr;
    ExecutionStatsZ80 *stats = nullptr;
    std::vector<UINT64> *pcHits = nullptr;
    std::vector<UINT64> *opHits = nullptr;
    UINT64 *step = nullptr;
    IoPluginHost *ioPlugin = nullptr;
};

static char HexDigit(unsigned value) {
    static const char kDigits[] = "0123456789abcdef";
    return kDigits[value & 0x0F];
}

static bool ParseHexDigits(std::string_view text, UINT64 *value) {
    UINT64 result = 0;
    if (!value || text.empty()) {
        return false;
    }
    for (char ch : text) {
        unsigned digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = (unsigned)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = (unsigned)(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = (unsigned)(ch - 'A' + 10);
        } else {
            return false;
        }
        result = (result << 4) | digit;
    }
    *value = result;
    return true;
}

static void AppendByteHex(std::string &out, UINT8 value) {
    out.push_back(HexDigit(value >> 4));
    out.push_back(HexDigit(value));
}

static void AppendWordHexLE(std::string &out, UINT16 value) {
    AppendByteHex(out, (UINT8)(value & 0xFF));
    AppendByteHex(out, (UINT8)(value >> 8));
}

static bool ParseHexBytePair(std::string_view text, UINT8 *value) {
    UINT64 parsed = 0;
    if (!value || text.size() != 2 || !ParseHexDigits(text, &parsed) || parsed > 0xFF) {
        return false;
    }
    *value = (UINT8)parsed;
    return true;
}

static bool DecodeHexBytes(std::string_view text, std::vector<UINT8> *out) {
    if (!out || (text.size() & 1u) != 0u) {
        return false;
    }
    out->clear();
    out->reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        UINT8 value = 0;
        if (!ParseHexBytePair(text.substr(i, 2), &value)) {
            return false;
        }
        out->push_back(value);
    }
    return true;
}

static std::string EncodeMemory(const UINT8 *data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        AppendByteHex(out, data[i]);
    }
    return out;
}

static UINT16 GetDebugRegister(const StateZ80 *state, unsigned reg) {
    switch (reg) {
    case 0:
        return (UINT16)((state->a << 8) | state->f);
    case 1:
        return (UINT16)((state->b << 8) | state->c);
    case 2:
        return (UINT16)((state->d << 8) | state->e);
    case 3:
        return (UINT16)((state->h << 8) | state->l);
    case 4:
        return state->sp;
    case 5:
        return state->pc;
    case 6:
        return state->ix;
    case 7:
        return state->iy;
    case 8:
        return (UINT16)((state->a_alt << 8) | state->f_alt);
    case 9:
        return (UINT16)((state->b_alt << 8) | state->c_alt);
    case 10:
        return (UINT16)((state->d_alt << 8) | state->e_alt);
    case 11:
        return (UINT16)((state->h_alt << 8) | state->l_alt);
    case 12:
        return (UINT16)((state->i << 8) | state->r);
    default:
        return 0;
    }
}

static bool SetDebugRegister(StateZ80 *state, unsigned reg, UINT16 value) {
    switch (reg) {
    case 0:
        state->a = (UINT8)(value >> 8);
        state->f = (UINT8)value;
        return true;
    case 1:
        state->b = (UINT8)(value >> 8);
        state->c = (UINT8)value;
        return true;
    case 2:
        state->d = (UINT8)(value >> 8);
        state->e = (UINT8)value;
        return true;
    case 3:
        state->h = (UINT8)(value >> 8);
        state->l = (UINT8)value;
        return true;
    case 4:
        state->sp = value;
        return true;
    case 5:
        state->pc = value;
        return true;
    case 6:
        state->ix = value;
        return true;
    case 7:
        state->iy = value;
        return true;
    case 8:
        state->a_alt = (UINT8)(value >> 8);
        state->f_alt = (UINT8)value;
        return true;
    case 9:
        state->b_alt = (UINT8)(value >> 8);
        state->c_alt = (UINT8)value;
        return true;
    case 10:
        state->d_alt = (UINT8)(value >> 8);
        state->e_alt = (UINT8)value;
        return true;
    case 11:
        state->h_alt = (UINT8)(value >> 8);
        state->l_alt = (UINT8)value;
        return true;
    case 12:
        state->i = (UINT8)(value >> 8);
        state->r = (UINT8)value;
        return true;
    default:
        return false;
    }
}

static std::string EncodeRegisterPacket(const StateZ80 *state) {
    std::string out;
    out.reserve(13 * 4);
    for (unsigned reg = 0; reg < 13; ++reg) {
        AppendWordHexLE(out, GetDebugRegister(state, reg));
    }
    return out;
}

static bool DecodeRegisterPacket(const std::string &payload, StateZ80 *state) {
    if (payload.size() != 13 * 4) {
        return false;
    }
    for (unsigned reg = 0; reg < 13; ++reg) {
        UINT8 lo = 0;
        UINT8 hi = 0;
        if (!ParseHexBytePair(std::string_view(payload).substr(reg * 4, 2), &lo) ||
            !ParseHexBytePair(std::string_view(payload).substr(reg * 4 + 2, 2), &hi)) {
            return false;
        }
        if (!SetDebugRegister(state, reg, (UINT16)((hi << 8) | lo))) {
            return false;
        }
    }
    return true;
}

static bool SendAll(int fd, const std::string &data) {
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t written = send(fd, data.data() + offset, data.size() - offset, 0);
        if (written <= 0) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static bool SendPacket(int fd, const std::string &payload) {
    unsigned checksum = 0;
    std::string packet;
    packet.reserve(payload.size() + 4);
    packet.push_back('$');
    for (char ch : payload) {
        checksum = (checksum + (unsigned char)ch) & 0xFFu;
        packet.push_back(ch);
    }
    packet.push_back('#');
    packet.push_back(HexDigit(checksum >> 4));
    packet.push_back(HexDigit(checksum));
    return SendAll(fd, packet);
}

static bool ReadPacket(int fd, bool noAck, std::string *payload, bool *interrupt) {
    if (payload) {
        payload->clear();
    }
    if (interrupt) {
        *interrupt = false;
    }
    for (;;) {
        char ch = 0;
        ssize_t got = recv(fd, &ch, 1, 0);
        if (got <= 0) {
            return false;
        }
        if (ch == '+' || ch == '-') {
            continue;
        }
        if (ch == '\x03') {
            if (interrupt) {
                *interrupt = true;
            }
            return true;
        }
        if (ch != '$') {
            continue;
        }

        std::string data;
        unsigned checksum = 0;
        while (true) {
            got = recv(fd, &ch, 1, 0);
            if (got <= 0) {
                return false;
            }
            if (ch == '#') {
                break;
            }
            checksum = (checksum + (unsigned char)ch) & 0xFFu;
            data.push_back(ch);
        }

        char hex[2] = {0, 0};
        if (recv(fd, &hex[0], 1, 0) <= 0 || recv(fd, &hex[1], 1, 0) <= 0) {
            return false;
        }
        UINT8 expected = 0;
        if (!ParseHexBytePair(std::string_view(hex, 2), &expected) || expected != (UINT8)checksum) {
            if (!noAck) {
                (void)SendAll(fd, "-");
            }
            continue;
        }
        if (!noAck) {
            (void)SendAll(fd, "+");
        }
        if (payload) {
            *payload = std::move(data);
        }
        return true;
    }
}

static bool PollCtrlC(int fd) {
    char ch = 0;
    ssize_t got = recv(fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
    if (got == 1 && ch == '\x03') {
        (void)recv(fd, &ch, 1, 0);
        return true;
    }
    if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return true;
    }
    return false;
}

static bool ParseOptionalAddress(std::string_view text, std::optional<UINT16> *addr) {
    UINT64 value = 0;
    if (!addr) {
        return false;
    }
    if (text.empty()) {
        *addr = std::nullopt;
        return true;
    }
    if (!ParseHexDigits(text, &value) || value > 0xFFFF) {
        return false;
    }
    *addr = (UINT16)value;
    return true;
}

static bool ParseAddrLen(std::string_view text, UINT64 *addr, UINT64 *len) {
    size_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return false;
    }
    return ParseHexDigits(text.substr(0, comma), addr) && ParseHexDigits(text.substr(comma + 1), len);
}

static std::string StepDebug(DebugRuntime *runtime, std::optional<UINT16> newPc) {
    if (newPc.has_value()) {
        runtime->state->pc = *newPc;
    }

    ArmInterrupts(runtime->cfg->interrupts, runtime->state, *runtime->step);
    ArmTimers(runtime->cfg->timers, runtime->state, runtime->stats->total_tstates);

    if (runtime->state->halted && !runtime->state->nmi_pending && !runtime->state->irq_pending &&
        HasActiveTimers(runtime->cfg->timers)) {
        runtime->stats->total_tstates += 4;
        return "S05";
    }

    UINT16 pc = runtime->state->pc;
    UINT8 opcode = runtime->state->memory[pc];
    (*runtime->pcHits)[pc] += 1;
    (*runtime->opHits)[opcode] += 1;

    Z80StepEvents events = {};
    Z80StepResult result =
        EmulateZ80Op(runtime->state, runtime->stats, runtime->cfg->ioTrace ? &events : nullptr);
    if (runtime->cfg->ioTrace) {
        OutputIoTrace(stderr, *runtime->step, events, runtime->ioPlugin->loaded());
    }
    *runtime->step += 1;
    ArmPluginInterrupt(runtime->ioPlugin, runtime->state, *runtime->step, runtime->stats->total_tstates);

    if (result == Z80_STEP_UNIMPLEMENTED) {
        if (!runtime->cfg->quiet) {
            fprintf(stderr, "Error: %s\n", GetZ80LastError(runtime->state));
        }
        return "S04";
    }
    return "S05";
}

static std::string ContinueDebug(DebugRuntime *runtime, const std::set<UINT16> &breakpoints, int clientFd,
                                 std::optional<UINT16> newPc) {
    if (newPc.has_value()) {
        runtime->state->pc = *newPc;
    }

    UINT16 initialPc = runtime->state->pc;
    bool ignoreInitialBreakpoint = true;

    while (*runtime->step < runtime->cfg->maxSteps) {
        if (PollCtrlC(clientFd)) {
            return "S02";
        }

        ArmInterrupts(runtime->cfg->interrupts, runtime->state, *runtime->step);
        ArmTimers(runtime->cfg->timers, runtime->state, runtime->stats->total_tstates);

        if (runtime->state->halted && !runtime->state->nmi_pending && !runtime->state->irq_pending &&
            HasActiveTimers(runtime->cfg->timers)) {
            runtime->stats->total_tstates += 4;
            continue;
        }

        if (!runtime->state->halted && breakpoints.count(runtime->state->pc) != 0 &&
            !(ignoreInitialBreakpoint && runtime->state->pc == initialPc)) {
            return "S05";
        }
        ignoreInitialBreakpoint = false;

        UINT16 pc = runtime->state->pc;
        UINT8 opcode = runtime->state->memory[pc];
        (*runtime->pcHits)[pc] += 1;
        (*runtime->opHits)[opcode] += 1;

        Z80StepEvents events = {};
        Z80StepResult result =
            EmulateZ80Op(runtime->state, runtime->stats, runtime->cfg->ioTrace ? &events : nullptr);
        if (runtime->cfg->ioTrace) {
            OutputIoTrace(stderr, *runtime->step, events, runtime->ioPlugin->loaded());
        }
        *runtime->step += 1;
        ArmPluginInterrupt(runtime->ioPlugin, runtime->state, *runtime->step, runtime->stats->total_tstates);

        if (result == Z80_STEP_UNIMPLEMENTED) {
            if (!runtime->cfg->quiet) {
                fprintf(stderr, "Error: %s\n", GetZ80LastError(runtime->state));
            }
            return "S04";
        }
        if (result == Z80_STEP_HALT) {
            if (runtime->state->nmi_pending || runtime->state->irq_pending ||
                HasFutureInterrupts(runtime->cfg->interrupts, *runtime->step) ||
                HasActiveTimers(runtime->cfg->timers)) {
                continue;
            }
            return "S05";
        }
    }

    return "S05";
}

static int RunGdbServer(DebugRuntime *runtime, int requestedPort) {
    static const char kTargetXml[] =
        "<?xml version=\"1.0\"?>"
        "<target version=\"1.0\">"
        "<architecture>z80</architecture>"
        "<feature name=\"org.gnu.gdb.z80.cpu\">"
        "<reg name=\"af\" bitsize=\"16\"/>"
        "<reg name=\"bc\" bitsize=\"16\"/>"
        "<reg name=\"de\" bitsize=\"16\"/>"
        "<reg name=\"hl\" bitsize=\"16\"/>"
        "<reg name=\"sp\" bitsize=\"16\"/>"
        "<reg name=\"pc\" bitsize=\"16\"/>"
        "<reg name=\"ix\" bitsize=\"16\"/>"
        "<reg name=\"iy\" bitsize=\"16\"/>"
        "<reg name=\"af'\" bitsize=\"16\"/>"
        "<reg name=\"bc'\" bitsize=\"16\"/>"
        "<reg name=\"de'\" bitsize=\"16\"/>"
        "<reg name=\"hl'\" bitsize=\"16\"/>"
        "<reg name=\"ir\" bitsize=\"16\"/>"
        "</feature>"
        "</target>";

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        fprintf(stderr, "Error: Failed to create GDB socket\n");
        return 1;
    }

    int one = 1;
    (void)setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)requestedPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(serverFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "Error: Failed to bind GDB port %d\n", requestedPort);
        close(serverFd);
        return 1;
    }
    if (listen(serverFd, 1) != 0) {
        fprintf(stderr, "Error: Failed to listen for GDB connection\n");
        close(serverFd);
        return 1;
    }

    sockaddr_in bound = {};
    socklen_t boundLen = sizeof(bound);
    if (getsockname(serverFd, reinterpret_cast<sockaddr *>(&bound), &boundLen) != 0) {
        fprintf(stderr, "Error: Failed to query GDB port\n");
        close(serverFd);
        return 1;
    }
    fprintf(stderr, "GDB listening on 127.0.0.1:%u\n", (unsigned)ntohs(bound.sin_port));

    int clientFd = accept(serverFd, nullptr, nullptr);
    close(serverFd);
    if (clientFd < 0) {
        fprintf(stderr, "Error: Failed to accept GDB connection\n");
        return 1;
    }

    bool noAck = false;
    std::set<UINT16> breakpoints;
    for (;;) {
        std::string packet;
        bool interrupt = false;
        if (!ReadPacket(clientFd, noAck, &packet, &interrupt)) {
            break;
        }

        std::string reply;
        bool detach = false;
        bool sendReply = true;

        if (interrupt) {
            reply = "S02";
        } else if (packet == "?") {
            reply = "S05";
        } else if (packet.rfind("qSupported", 0) == 0) {
            reply = "PacketSize=4000;QStartNoAckMode+;qXfer:features:read+;swbreak+";
        } else if (packet == "vMustReplyEmpty" || packet == "qTStatus" || packet.rfind("qHostInfo", 0) == 0 ||
                   packet.rfind("qThreadExtraInfo", 0) == 0) {
            reply.clear();
        } else if (packet == "QStartNoAckMode") {
            reply = "OK";
            if (!SendPacket(clientFd, reply)) {
                close(clientFd);
                return 1;
            }
            noAck = true;
            continue;
        } else if (packet == "qAttached") {
            reply = "1";
        } else if (packet == "qC") {
            reply = "QC0";
        } else if (packet == "qfThreadInfo") {
            reply = "m0";
        } else if (packet == "qsThreadInfo") {
            reply = "l";
        } else if (packet == "qOffsets") {
            reply = "Text=0;Data=0;Bss=0";
        } else if (packet == "qSymbol::") {
            reply = "OK";
        } else if (packet.rfind("qXfer:features:read:target.xml:", 0) == 0) {
            UINT64 offset = 0;
            UINT64 length = 0;
            if (!ParseAddrLen(std::string_view(packet).substr(strlen("qXfer:features:read:target.xml:")), &offset, &length)) {
                reply = "E01";
            } else {
                size_t xmlLen = strlen(kTargetXml);
                size_t start = (size_t)std::min<UINT64>(offset, xmlLen);
                size_t count = (size_t)std::min<UINT64>(length, xmlLen - start);
                reply.push_back(start + count < xmlLen ? 'm' : 'l');
                reply.append(kTargetXml + start, count);
            }
        } else if (packet == "g") {
            reply = EncodeRegisterPacket(runtime->state);
        } else if (packet.size() > 1 && packet[0] == 'p') {
            UINT64 reg = 0;
            if (!ParseHexDigits(std::string_view(packet).substr(1), &reg) || reg >= 13) {
                reply = "E01";
            } else {
                reply.reserve(4);
                AppendWordHexLE(reply, GetDebugRegister(runtime->state, (unsigned)reg));
            }
        } else if (packet.size() > 3 && packet[0] == 'P') {
            size_t eq = packet.find('=');
            UINT64 reg = 0;
            if (eq == std::string::npos || !ParseHexDigits(std::string_view(packet).substr(1, eq - 1), &reg) || reg >= 13 ||
                packet.size() != eq + 5) {
                reply = "E01";
            } else {
                UINT8 lo = 0;
                UINT8 hi = 0;
                if (!ParseHexBytePair(std::string_view(packet).substr(eq + 1, 2), &lo) ||
                    !ParseHexBytePair(std::string_view(packet).substr(eq + 3, 2), &hi) ||
                    !SetDebugRegister(runtime->state, (unsigned)reg, (UINT16)((hi << 8) | lo))) {
                    reply = "E01";
                } else {
                    reply = "OK";
                }
            }
        } else if (packet.size() > 1 && packet[0] == 'G') {
            reply = DecodeRegisterPacket(packet.substr(1), runtime->state) ? "OK" : "E01";
        } else if (packet.size() > 1 && packet[0] == 'm') {
            UINT64 addrValue = 0;
            UINT64 lenValue = 0;
            if (!ParseAddrLen(std::string_view(packet).substr(1), &addrValue, &lenValue) || lenValue > 0x10000ULL) {
                reply = "E01";
            } else {
                reply.clear();
                reply.reserve((size_t)lenValue * 2);
                for (UINT64 i = 0; i < lenValue; ++i) {
                    AppendByteHex(reply, runtime->state->memory[(UINT16)(addrValue + i)]);
                }
            }
        } else if (packet.size() > 1 && packet[0] == 'M') {
            size_t colon = packet.find(':');
            UINT64 addrValue = 0;
            UINT64 lenValue = 0;
            std::vector<UINT8> bytes;
            if (colon == std::string::npos || !ParseAddrLen(std::string_view(packet).substr(1, colon - 1), &addrValue, &lenValue) ||
                lenValue > 0x10000ULL || !DecodeHexBytes(std::string_view(packet).substr(colon + 1), &bytes) ||
                bytes.size() != lenValue) {
                reply = "E01";
            } else {
                for (size_t i = 0; i < bytes.size(); ++i) {
                    runtime->state->memory[(UINT16)(addrValue + i)] = bytes[i];
                }
                reply = "OK";
            }
        } else if (packet == "vCont?") {
            reply = "vCont;c;s";
        } else if (packet.rfind("vCont;", 0) == 0) {
            std::string_view action = std::string_view(packet).substr(6);
            if (action == "c") {
                reply = ContinueDebug(runtime, breakpoints, clientFd, std::nullopt);
            } else if (action == "s") {
                reply = StepDebug(runtime, std::nullopt);
            } else {
                reply.clear();
            }
        } else if (packet[0] == 'c') {
            std::optional<UINT16> addrOverride;
            if (!ParseOptionalAddress(std::string_view(packet).substr(1), &addrOverride)) {
                reply = "E01";
            } else {
                reply = ContinueDebug(runtime, breakpoints, clientFd, addrOverride);
            }
        } else if (packet[0] == 's') {
            std::optional<UINT16> addrOverride;
            if (!ParseOptionalAddress(std::string_view(packet).substr(1), &addrOverride)) {
                reply = "E01";
            } else {
                reply = StepDebug(runtime, addrOverride);
            }
        } else if ((packet.rfind("Z0,", 0) == 0) || (packet.rfind("z0,", 0) == 0)) {
            size_t comma = packet.find(',', 3);
            UINT64 addrValue = 0;
            if (comma == std::string::npos || !ParseHexDigits(std::string_view(packet).substr(3, comma - 3), &addrValue) ||
                addrValue > 0xFFFF) {
                reply = "E01";
            } else {
                if (packet[0] == 'Z') {
                    breakpoints.insert((UINT16)addrValue);
                } else {
                    breakpoints.erase((UINT16)addrValue);
                }
                reply = "OK";
            }
        } else if ((packet.rfind("Hg", 0) == 0) || (packet.rfind("Hc", 0) == 0)) {
            reply = "OK";
        } else if (packet == "D") {
            reply = "OK";
            detach = true;
        } else if (packet == "k") {
            sendReply = false;
            detach = true;
        } else {
            reply.clear();
        }

        if (sendReply && !SendPacket(clientFd, reply)) {
            close(clientFd);
            return 1;
        }
        if (detach) {
            break;
        }
    }

    close(clientFd);
    return 0;
}

int main(int argc, char *argv[]) {
    Config cfg;

    static struct option longOpts[] = {
        {"load", required_argument, nullptr, 'l'},
        {"entry", required_argument, nullptr, 'e'},
        {"sp", required_argument, nullptr, 'p'},
        {"max-steps", required_argument, nullptr, 'n'},
        {"stop-at", required_argument, nullptr, 's'},
        {"no-loop-detect", no_argument, nullptr, 1000},
        {"output", required_argument, nullptr, 'o'},
        {"dump", required_argument, nullptr, 'd'},
        {"cov", required_argument, nullptr, 'C'},
        {"tracepoint", required_argument, nullptr, 't'},
        {"tracepoint-file", required_argument, nullptr, 'T'},
        {"tracepoint-max", required_argument, nullptr, 'M'},
        {"tracepoint-stop", no_argument, nullptr, 'P'},
        {"bus-trace", no_argument, nullptr, 1007},
        {"irq", required_argument, nullptr, 1001},
        {"timer", required_argument, nullptr, 1002},
        {"io-plugin", required_argument, nullptr, 1003},
        {"io-plugin-config", required_argument, nullptr, 1004},
        {"io-trace", no_argument, nullptr, 1005},
        {"gdb", required_argument, nullptr, 1006},
        {"quiet", no_argument, nullptr, 'q'},
        {"summary", no_argument, nullptr, 'S'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "l:e:p:n:s:o:d:C:t:T:M:PqSh", longOpts, nullptr)) != -1) {
        switch (opt) {
        case 'l':
            if (!ParseHex(optarg, &cfg.loadAddr)) {
                fprintf(stderr, "Error: Invalid load address '%s'\n", optarg);
                return 1;
            }
            break;
        case 'e':
            if (!ParseHex(optarg, &cfg.entryAddr)) {
                fprintf(stderr, "Error: Invalid entry address '%s'\n", optarg);
                return 1;
            }
            cfg.entrySet = true;
            break;
        case 'p':
            if (!ParseHex(optarg, &cfg.spAddr)) {
                fprintf(stderr, "Error: Invalid SP address '%s'\n", optarg);
                return 1;
            }
            break;
        case 'n':
            cfg.maxSteps = strtoull(optarg, nullptr, 10);
            break;
        case 's': {
            UINT16 addr = 0;
            if (!ParseHex(optarg, &addr)) {
                fprintf(stderr, "Error: Invalid stop address '%s'\n", optarg);
                return 1;
            }
            cfg.stopAddrs.push_back(addr);
            break;
        }
        case 1000:
            cfg.loopDetect = false;
            break;
        case 'o':
            cfg.outputFile = optarg;
            break;
        case 'd': {
            MemoryDump dump;
            if (!ParseDump(optarg, &dump)) {
                fprintf(stderr, "Error: Invalid dump spec '%s' (use START:LENGTH, e.g., 0x2000:32)\n", optarg);
                return 1;
            }
            cfg.dumps.push_back(dump);
            break;
        }
        case 'C':
            cfg.coverageFile = optarg;
            break;
        case 't': {
            UINT16 addr = 0;
            if (!ParseHex(optarg, &addr)) {
                fprintf(stderr, "Error: Invalid tracepoint address '%s'\n", optarg);
                return 1;
            }
            AddTracepoint(cfg.tracepoints, addr);
            break;
        }
        case 'T':
            if (!ParseTracepointFile(optarg, cfg.tracepoints)) {
                fprintf(stderr, "Error: Failed to read tracepoint file '%s'\n", optarg);
                return 1;
            }
            break;
        case 'M':
            cfg.tracepointMax = strtoull(optarg, nullptr, 10);
            break;
        case 'P':
            cfg.tracepointStop = true;
            break;
        case 1007:
            cfg.busTrace = true;
            break;
        case 1001: {
            InterruptSpec irq;
            if (!ParseInterruptSpec(optarg, &irq)) {
                fprintf(stderr, "Error: Invalid interrupt spec '%s' (use nmi:STEP or int:STEP[:0xVV])\n", optarg);
                return 1;
            }
            cfg.interrupts.push_back(irq);
            break;
        }
        case 1002: {
            TimerSpec timer;
            if (!ParseTimerSpec(optarg, &timer)) {
                fprintf(stderr,
                        "Error: Invalid timer spec '%s' (use nmi:PERIOD[:START] or int:PERIOD[:START[:0xVV]])\n",
                        optarg);
                return 1;
            }
            cfg.timers.push_back(timer);
            break;
        }
        case 1003:
            cfg.ioPluginPath = optarg;
            break;
        case 1004:
            cfg.ioPluginConfig = optarg;
            break;
        case 1005:
            cfg.ioTrace = true;
            break;
        case 1006:
            if (!ParsePortNumber(optarg, &cfg.gdbPort)) {
                fprintf(stderr, "Error: Invalid GDB port '%s'\n", optarg);
                return 1;
            }
            break;
        case 'q':
            cfg.quiet = true;
            break;
        case 'S':
            cfg.summary = true;
            break;
        case 'h':
            PrintUsage(argv[0]);
            return 0;
        default:
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: No input file specified\n\n");
        PrintUsage(argv[0]);
        return 1;
    }

    cfg.inputFile = argv[optind];
    if (!cfg.entrySet) {
        cfg.entryAddr = cfg.loadAddr;
    }

    if (!cfg.tracepoints.empty() && !cfg.summary) {
        fprintf(stderr, "Error: Tracepoints require -S / --summary mode\n");
        return 1;
    }

    if (cfg.busTrace && cfg.summary) {
        fprintf(stderr, "Error: --bus-trace cannot be combined with --summary\n");
        return 1;
    }

    if (cfg.ioPluginConfig && !cfg.ioPluginPath) {
        fprintf(stderr, "Error: --io-plugin-config requires --io-plugin\n");
        return 1;
    }

    if (cfg.gdbPort >= 0) {
        if (!cfg.stopAddrs.empty() || !cfg.tracepoints.empty() || cfg.busTrace || cfg.outputFile || cfg.summary) {
            fprintf(stderr,
                    "Error: --gdb cannot be combined with stop addresses, tracepoints, --summary, --bus-trace, or --output\n");
            return 1;
        }
    }

    StateZ80 *state = InitZ80();
    if (!state) {
        fprintf(stderr, "Error: Failed to allocate CPU state\n");
        return 1;
    }

    IoPluginHost ioPlugin;
    if (cfg.ioPluginPath) {
        std::string error;
        if (!ioPlugin.Load(cfg.ioPluginPath, cfg.ioPluginConfig ? cfg.ioPluginConfig : "", &error)) {
            fprintf(stderr, "Error: Failed to load I/O plugin '%s': %s\n", cfg.ioPluginPath, error.c_str());
            FreeZ80(state);
            return 1;
        }
        ioPlugin.Attach(state);
    }

    if (!LoadBinary(state, cfg.inputFile, cfg.loadAddr)) {
        FreeZ80(state);
        return 1;
    }

    ResetZ80(state, cfg.entryAddr, cfg.spAddr);
    ioPlugin.Reset();

    FILE *out = stdout;
    if (cfg.outputFile) {
        out = fopen(cfg.outputFile, "w");
        if (!out) {
            fprintf(stderr, "Error: Cannot open output file '%s'\n", cfg.outputFile);
            FreeZ80(state);
            return 1;
        }
    }

    if (!cfg.quiet && !cfg.summary && !cfg.busTrace) {
        fprintf(stderr, "Z80 Trace\n");
        fprintf(stderr, "  Input:  %s\n", cfg.inputFile);
        fprintf(stderr, "  Load:   0x%04X\n", cfg.loadAddr);
        fprintf(stderr, "  Entry:  0x%04X\n", cfg.entryAddr);
        fprintf(stderr, "  SP:     0x%04X\n", cfg.spAddr);
        fprintf(stderr, "  Max:    %" PRIu64 " steps\n", cfg.maxSteps);
        if (!cfg.stopAddrs.empty()) {
            fprintf(stderr, "  Stop:");
            for (UINT16 addr : cfg.stopAddrs) {
                fprintf(stderr, " 0x%04X", addr);
            }
            fprintf(stderr, "\n");
        }
        if (!cfg.tracepoints.empty()) {
            fprintf(stderr, "  Tracepoints:");
            for (const auto &tp : cfg.tracepoints) {
                fprintf(stderr, " 0x%04X", tp.pc);
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
    }

    ExecutionStatsZ80 stats = {};
    stats.min_sp = state->sp;
    stats.min_sp_set = true;

    std::vector<UINT64> pcHits(0x10000, 0);
    std::vector<UINT64> opHits(0x100, 0);

    UINT64 step = 0;
    LoopState lastState = {};
    bool haveLastState = false;
    const char *haltReason = "max";
    int exitCode = 0;
    UINT64 totalTracepointHits = 0;
    UINT64 busCycle = 0;
    UINT64 totalBusWrites = 0;

    if (cfg.gdbPort >= 0) {
        DebugRuntime runtime = {&cfg, state, &stats, &pcHits, &opHits, &step, &ioPlugin};
        exitCode = RunGdbServer(&runtime, cfg.gdbPort);
        haltReason = exitCode == 0 ? (state->halted ? "hlt" : "gdb") : "gdb-error";
    } else {
        while (step < cfg.maxSteps) {
            ArmInterrupts(cfg.interrupts, state, step);
            ArmTimers(cfg.timers, state, stats.total_tstates);

            if (state->halted && !state->nmi_pending && !state->irq_pending && HasActiveTimers(cfg.timers)) {
                stats.total_tstates += 4;
                continue;
            }

            UINT16 pc = state->pc;
            UINT16 sp = state->sp;
            UINT8 flags = state->f;
            UINT64 clocks = stats.total_tstates;
            UINT8 opcode = state->memory[pc];

            char disasmBuf[128];
            DisassembleZ80Op(state->memory, pc, disasmBuf, sizeof(disasmBuf));
            char mnemonicBuf[32];
            ExtractMnemonic(disasmBuf, mnemonicBuf, sizeof(mnemonicBuf));

            Tracepoint *tp = FindTracepoint(cfg.tracepoints, pc);

            if (IsStopAddress(pc, cfg.stopAddrs)) {
                haltReason = "stop";
                break;
            }

            bool waitingForScheduledInterrupt = state->halted && !state->nmi_pending && !state->irq_pending &&
                                               HasFutureInterrupts(cfg.interrupts, step);

            if (cfg.loopDetect && haveLastState && !waitingForScheduledInterrupt) {
                LoopState currentState = CaptureLoopState(state);
                if (memcmp(&currentState, &lastState, sizeof(currentState)) == 0) {
                    haltReason = "loop";
                    break;
                }
                lastState = currentState;
            } else if (cfg.loopDetect && !waitingForScheduledInterrupt) {
                lastState = CaptureLoopState(state);
                haveLastState = true;
            }

            pcHits[pc] += 1;
            opHits[opcode] += 1;

            Z80StepEvents events = {};
            Z80StepResult result = EmulateZ80Op(state, &stats, (cfg.busTrace || cfg.ioTrace) ? &events : nullptr);
            TraceState trace = {step, pc, sp, flags, clocks, mnemonicBuf, disasmBuf};

            if (cfg.busTrace) {
                OutputBusStep(out, &busCycle, &totalBusWrites, events);
            } else if (!cfg.summary || tp != nullptr) {
                OutputTrace(out, trace, state);
            }
            if (cfg.ioTrace) {
                OutputIoTrace(stderr, step, events, ioPlugin.loaded());
            }

            if (tp != nullptr) {
                tp->hits += 1;
                totalTracepointHits += 1;
            }

            step += 1;

            ArmPluginInterrupt(&ioPlugin, state, step, stats.total_tstates);

            if (result == Z80_STEP_HALT) {
                if (state->nmi_pending || state->irq_pending || HasFutureInterrupts(cfg.interrupts, step) ||
                    HasActiveTimers(cfg.timers)) {
                    continue;
                }
                haltReason = "hlt";
                break;
            }

            if (result == Z80_STEP_UNIMPLEMENTED) {
                haltReason = "unimplemented";
                exitCode = 1;
                if (!cfg.quiet) {
                    fprintf(stderr, "Error: %s\n", GetZ80LastError(state));
                }
                break;
            }

            if (cfg.tracepointMax > 0 && totalTracepointHits >= cfg.tracepointMax) {
                haltReason = "tracepoint-max";
                break;
            }

            if (cfg.tracepointStop && AllTracepointsHit(cfg.tracepoints)) {
                haltReason = "tracepoint-stop";
                break;
            }
        }
    }

    if (!cfg.quiet && !cfg.summary && !cfg.busTrace) {
        fprintf(stderr, "\nExecution complete:\n");
        fprintf(stderr, "  Instructions: %" PRIu64 "\n", step);
        fprintf(stderr, "  Clocks:       %" PRIu64 " (rough estimate)\n", stats.total_tstates);
        fprintf(stderr, "  Final PC:     0x%04X\n", state->pc);
        fprintf(stderr, "  Final SP:     0x%04X\n", state->sp);
        fprintf(stderr, "  Halt reason:  %s\n", haltReason);
    }

    if (cfg.summary) {
        fprintf(out,
                "{\"pc\":\"%04X\",\"sp\":\"%04X\",\"f\":\"%02X\",\"clk\":%" PRIu64 ",\"steps\":%" PRIu64
                ",\"halt\":\"%s\",\"ix\":\"%04X\",\"iy\":\"%04X\",\"i\":\"%02X\",\"r8\":\"%02X\","
                "\"im\":%u,\"iff1\":%u,\"iff2\":%u,\"af2\":\"%04X\",\"bc2\":\"%04X\",\"de2\":\"%04X\","
                "\"hl2\":\"%04X\","
                "\"r\":[\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\"]}\n",
                state->pc, state->sp, state->f, stats.total_tstates, step, haltReason, state->ix, state->iy,
                state->i, state->r, state->im, state->iff1, state->iff2, (unsigned)((state->a_alt << 8) | state->f_alt),
                (unsigned)((state->b_alt << 8) | state->c_alt), (unsigned)((state->d_alt << 8) | state->e_alt),
                (unsigned)((state->h_alt << 8) | state->l_alt), state->a, state->b, state->c, state->d, state->e,
                state->h, state->l);
    }

    if (cfg.coverageFile) {
        WriteCoverage(cfg.coverageFile, step, pcHits, opHits);
    }

    for (const auto &dump : cfg.dumps) {
        if (cfg.busTrace) {
            OutputBusDump(out, state, dump);
        } else {
            OutputHexDump(stderr, state, dump);
        }
    }

    if (cfg.busTrace && strcmp(haltReason, "hlt") == 0 && exitCode == 0) {
        fprintf(out, "HALT cycle=%" PRIu64 " addr=0x%04X\n", busCycle, state->pc);
        fprintf(out, "PASS cycles=%" PRIu64 " writes=%" PRIu64 "\n", stats.total_tstates, totalBusWrites);
    }

    if (cfg.outputFile) {
        fclose(out);
    }

    FreeZ80(state);
    return exitCode;
}
