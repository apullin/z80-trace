#include "z80_cpu.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <string>
#include <vector>

struct MemoryDump {
    UINT16 start;
    UINT16 length;
};

struct Tracepoint {
    UINT16 pc;
    UINT32 hits = 0;
};

struct Config {
    const char *inputFile = nullptr;
    const char *outputFile = nullptr;
    const char *coverageFile = nullptr;
    UINT16 loadAddr = 0x0000;
    UINT16 entryAddr = 0x0000;
    UINT16 spAddr = 0xFFFF;
    UINT64 maxSteps = 1000000;
    bool loopDetect = true;
    bool quiet = false;
    bool summary = false;
    bool entrySet = false;
    std::vector<UINT16> stopAddrs;
    std::vector<MemoryDump> dumps;
    std::vector<Tracepoint> tracepoints;
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
    fprintf(stderr, "  -d, --dump=START:LEN  Dump memory range at exit (hex, can repeat)\n");
    fprintf(stderr, "  --cov=FILE            Write coverage JSON (pc/opcode hit counts)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Tracepoint Options (require -S):\n");
    fprintf(stderr, "  -t, --tracepoint=ADDR       Trace only this address (hex, can repeat)\n");
    fprintf(stderr, "  -T, --tracepoint-file=FILE  Load tracepoint addresses from file\n");
    fprintf(stderr, "  --tracepoint-max=N          Stop after N total tracepoint hits\n");
    fprintf(stderr, "  --tracepoint-stop           Stop when all tracepoints hit at least once\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Parity Placeholder Options (recognized, not implemented yet):\n");
    fprintf(stderr, "  --irq=SPEC            Not implemented in the baseline Z80 tracer\n");
    fprintf(stderr, "  --timer=SPEC          Not implemented in the baseline Z80 tracer\n");
    fprintf(stderr, "  --io-plugin=PATH      Not implemented in the baseline Z80 tracer\n");
    fprintf(stderr, "  --io-plugin-config=S  Not implemented in the baseline Z80 tracer\n");
    fprintf(stderr, "  --io-trace            Not implemented in the baseline Z80 tracer\n");
    fprintf(stderr, "  --gdb=PORT            Not implemented in the baseline Z80 tracer\n");
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
    fprintf(out, "\"op\":\"%s\",\"asm\":\"%s\",\"ix\":\"%04X\",\"iy\":\"%04X\",\"i\":\"%02X\",\"r8\":\"%02X\",",
            t.mnemonic, t.disasm, state->ix, state->iy, state->i, state->r);
    fprintf(out, "\"r\":[\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\"]}\n", state->a, state->b,
            state->c, state->d, state->e, state->h, state->l);
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
        case 1001:
        case 1002:
        case 1003:
        case 1004:
        case 1005:
        case 1006:
            fprintf(stderr, "Error: This baseline does not implement the requested parity option yet.\n");
            return 1;
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

    StateZ80 *state = InitZ80();
    if (!state) {
        fprintf(stderr, "Error: Failed to allocate CPU state\n");
        return 1;
    }

    if (!LoadBinary(state, cfg.inputFile, cfg.loadAddr)) {
        FreeZ80(state);
        return 1;
    }

    ResetZ80(state, cfg.entryAddr, cfg.spAddr);

    FILE *out = stdout;
    if (cfg.outputFile) {
        out = fopen(cfg.outputFile, "w");
        if (!out) {
            fprintf(stderr, "Error: Cannot open output file '%s'\n", cfg.outputFile);
            FreeZ80(state);
            return 1;
        }
    }

    if (!cfg.quiet && !cfg.summary) {
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
    UINT16 lastPC = 0xFFFF;
    UINT16 lastSP = 0xFFFF;
    const char *haltReason = "max";
    int exitCode = 0;
    UINT64 totalTracepointHits = 0;

    while (step < cfg.maxSteps) {
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

        if (cfg.loopDetect && step > 0 && pc == lastPC && sp == lastSP) {
            haltReason = "loop";
            break;
        }

        pcHits[pc] += 1;
        opHits[opcode] += 1;

        Z80StepResult result = EmulateZ80Op(state, &stats);
        TraceState trace = {step, pc, sp, flags, clocks, mnemonicBuf, disasmBuf};

        if (!cfg.summary || tp != nullptr) {
            OutputTrace(out, trace, state);
        }

        if (tp != nullptr) {
            tp->hits += 1;
            totalTracepointHits += 1;
        }

        lastPC = pc;
        lastSP = sp;
        step += 1;

        if (result == Z80_STEP_HALT) {
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

    if (!cfg.quiet && !cfg.summary) {
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
                "\"r\":[\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\",\"%02X\"]}\n",
                state->pc, state->sp, state->f, stats.total_tstates, step, haltReason, state->ix, state->iy,
                state->i, state->r, state->a, state->b, state->c, state->d, state->e, state->h, state->l);
    }

    if (cfg.coverageFile) {
        WriteCoverage(cfg.coverageFile, step, pcHits, opHits);
    }

    for (const auto &dump : cfg.dumps) {
        fprintf(stderr, "\nMemory dump 0x%04X - 0x%04X (%u bytes):\n", dump.start,
                (UINT16)(dump.start + dump.length - 1), dump.length);
        for (UINT16 offset = 0; offset < dump.length; offset += 16) {
            fprintf(stderr, "  %04X:", (UINT16)(dump.start + offset));
            for (UINT16 i = 0; i < 16 && offset + i < dump.length; ++i) {
                UINT16 addr = (UINT16)(dump.start + offset + i);
                fprintf(stderr, " %02X", state->memory[addr]);
            }
            fprintf(stderr, "  |");
            for (UINT16 i = 0; i < 16 && offset + i < dump.length; ++i) {
                UINT8 byte = state->memory[(UINT16)(dump.start + offset + i)];
                fprintf(stderr, "%c", (byte >= 32 && byte < 127) ? byte : '.');
            }
            fprintf(stderr, "|\n");
        }
    }

    if (cfg.outputFile) {
        fclose(out);
    }

    FreeZ80(state);
    return exitCode;
}
