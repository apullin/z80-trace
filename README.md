# Z80 Trace

Standalone Z80 CPU tracer modeled on the `i8085-trace` project structure and workflow.

## Scope

`z80-trace` is a CPU-focused golden-reference tracer for future Z80 core verification. It loads a flat 64K binary image, executes instructions one at a time, and emits machine-readable trace data. It still deliberately does not model a full machine, ROM layout, or board peripherals, but it now implements the full documented Z80 opcode families, alternate register state, interrupt-mode bookkeeping, periodic timer injection, runtime port-I/O plugins, explicit I/O trace output, and a GDB remote-debugging mode alongside the harness-facing bus-event mode used by the current `z80new` RTL differential flow.

This repository is the implementation target described by `z80new` ticket `APU-7`. The goal of this first delivery is to reproduce the reference tool's architecture and developer workflow while establishing a clear baseline for future instruction-complete work.

## Architecture

The layout mirrors `i8085-trace` on purpose:

- `CMakeLists.txt` builds a single standalone executable.
- `include/z80_cpu.h` defines the CPU state and step/disassembly interfaces used by the CLI.
- `src/z80_exec.c` holds instruction execution for the currently implemented Z80 subset.
- `src/z80_state.c` handles allocation and reset.
- `src/z80_disasm.c` provides disassembly strings for trace output.
- `include/z80_io_plugin.h` defines the small shared-library ABI used by runtime I/O plugins.
- `src/io_plugin.cpp` loads and mediates runtime I/O plugins.
- `src/main.cpp` owns CLI parsing, binary loading, the trace loop, timers, summary output, dumps, coverage, I/O tracing, tracepoints, and GDB RSP.
- `test/run_tests.py` is the harness-oriented end-to-end validation entrypoint.

## Implemented Core

The current core executes the documented Z80 instruction families across the ordinary and prefixed opcode spaces:

- full unprefixed instruction decode and execution,
- `CB` rotate/shift and bit-manipulation instructions,
- `ED` extended arithmetic, block transfer/search, and interrupt-control instructions,
- `DD` and `FD` indexed forms, including indexed memory addressing and indexed register high/low byte access,
- indexed `CB` forms (`DDCB` and `FDCB`),
- alternate register handling through `EX AF,AF'` and `EXX`,
- interrupt state via `IFF1`, `IFF2`, `IM 0/1/2`, `RETI`, `RETN`, `DI`, `EI`, and HALT release by interrupt,
- refresh-register bookkeeping and instruction-accurate T-state totals based on the checked-in Z80 manuals.

The CLI now supports `--bus-trace`, which emits harness-comparable `BUS`, `WRITE`, `HALT`, `MEM`, and `PASS` lines, `--irq=SPEC`, which injects NMI or maskable interrupts on a chosen instruction step for black-box testing, `--timer=SPEC`, which injects a periodic NMI or maskable interrupt based on accumulated T-states, `--io-plugin` and `--io-plugin-config`, which load a runtime shared-library device model for Z80 port accesses, `--io-trace`, which emits machine-readable I/O records to `stderr`, and `--gdb=PORT`, which exposes the running tracer through the GDB Remote Serial Protocol on `127.0.0.1`.

The tracer remains instruction-timed rather than cycle-pin-accurate. `clk` and `PASS cycles=` reflect per-instruction T-state totals, not a wait-state-accurate bus waveform model.

## Building

From the `z80-trace` repository root:

```bash
cmake -S . -B build
cmake --build build
```

## Testing

```bash
python3 test/run_tests.py
```

From the `z80new` superproject root, the current harness-facing proofs are:

```bash
make sim-z80-trace-check
make sim-check
```

## Usage

```text
z80-trace [options] <binary.bin>

Memory Options:
  -l, --load=ADDR
  -e, --entry=ADDR
  -p, --sp=ADDR

Execution Options:
  -n, --max-steps=N
  -s, --stop-at=ADDR
  --no-loop-detect
  --irq=SPEC            SPEC is nmi:STEP or int:STEP[:0xVV]
  --timer=SPEC          SPEC is nmi:PERIOD[:START] or int:PERIOD[:START[:0xVV]] in T-states

Output Options:
  -o, --output=FILE
  -q, --quiet
  -S, --summary
  --bus-trace
  --io-trace
  -d, --dump=START:LEN
  --cov=FILE

Runtime Device and Debugger Options:
  --io-plugin=PATH
  --io-plugin-config=STRING
  --gdb=PORT

Tracepoint Options (require -S):
  -t, --tracepoint=ADDR
  -T, --tracepoint-file=FILE
  --tracepoint-max=N
  --tracepoint-stop
```

`--timer` is periodic rather than one-shot. The first interrupt fires at `START` T-states if `START` is given, otherwise after one full `PERIOD`. The timer then repeats every `PERIOD` T-states. Because the tracer is instruction-timed rather than wait-state-accurate, timer delivery occurs at the first instruction boundary at or after the requested T-state count.

`--io-plugin=PATH` loads a shared library that exports `z80_trace_get_io_plugin_api()` as defined in `include/z80_io_plugin.h`. The plugin can override port reads, observe port writes, reset its own state, and optionally request an interrupt after an instruction retires. When no plugin is loaded, the tracer falls back to the built-in flat `io_space` image.

`--io-trace` writes one JSON line per I/O access to `stderr` without changing the existing `stdout` trace and summary formats:

```json
{"type":"io","step":1,"kind":"read","port":"0010","data":"5A","source":"plugin"}
```

`--gdb=PORT` starts a localhost-only GDB server instead of the normal trace stream. `PORT=0` asks the OS to choose a free port and prints the chosen `127.0.0.1:PORT` endpoint to `stderr`.

## Trace Output

Normal execution emits one NDJSON object per instruction. The trace records the pre-execution `pc`, `sp`, flags, and clock count, plus the post-execution general registers, alternate register pairs, and interrupt bookkeeping:

```json
{"step":0,"pc":"0000","sp":"4000","f":"00","clk":0,"op":"LD","asm":"LD SP,$4000","ix":"0000","iy":"0000","i":"00","r8":"01","im":0,"iff1":0,"iff2":0,"af2":"0000","bc2":"0000","de2":"0000","hl2":"0000","r":["00","00","00","00","00","00","00"]}
```

Summary mode emits one final JSON line:

```json
{"pc":"000C","sp":"4000","f":"00","clk":43,"steps":6,"halt":"hlt","ix":"0000","iy":"0000","i":"00","r8":"06","im":0,"iff1":0,"iff2":0,"af2":"0000","bc2":"0000","de2":"0000","hl2":"0000","r":["42","00","00","00","00","20","00"]}
```

`--bus-trace` replaces the NDJSON stream with harness-oriented text lines. The output records architectural fetches, operand reads, memory writes, I/O reads and writes, interrupt acknowledge cycles, the final halt address, and any requested memory dumps:

```text
BUS cycle=9 kind=fetch addr=0x0009 data=0xC5 m1=1 mreq=1 iorq=0 rd=1 wr=0 rfsh=0 halt=0 busak=0
BUS cycle=10 kind=write addr=0x3FFF data=0x5A m1=0 mreq=1 iorq=0 rd=0 wr=1 rfsh=0 halt=0 busak=0
WRITE cycle=10 addr=0x3FFF data=0x5A
HALT cycle=18 addr=0x000D
PASS cycles=62 writes=3
```

Coverage output matches the `i8085-trace` shape: total steps plus sparse `pc_hits` and `op_hits` arrays. In normal mode memory dumps go to `stderr` in hex plus ASCII. In `--bus-trace` mode dumps emit one `MEM` line per byte so the output stays machine-comparable.

## GDB Mode

The remote-debugger mode reuses the same in-tree CPU core and memory image as the normal tracer path. It supports register reads and writes for the Z80 architectural register file (`AF`, `BC`, `DE`, `HL`, `SP`, `PC`, `IX`, `IY`, the shadow pairs, and `IR`), memory reads and writes over the flat 64K address space, single-step, continue, software breakpoints, and detach. The server binds to `127.0.0.1` only.

For example:

```bash
./build/z80-trace -q --gdb=0 sample.bin
```

The tracer prints a line like `GDB listening on 127.0.0.1:61327` to `stderr`. A separate terminal can then attach with:

```bash
gdb -q
(gdb) set architecture z80
(gdb) target remote 127.0.0.1:61327
```

## Example

This example creates a tiny binary, runs the tracer, and prints seven trace lines plus the final dump:

```bash
python3 - <<'PY'
from pathlib import Path
Path("sample.bin").write_bytes(bytes([
    0x31, 0x00, 0x40,  # LD SP,0x4000
    0x21, 0x00, 0x20,  # LD HL,0x2000
    0x3E, 0x42,        # LD A,0x42
    0x77,              # LD (HL),A
    0xEE, 0x00,        # XOR 0x00
    0x7E,              # LD A,(HL)
    0x76,              # HALT
]))
PY
./build/z80-trace -q --dump=0x2000:1 sample.bin
```

## Explicit Gaps Versus `i8085-trace`

The current tracer still does not provide:

- wait-state-accurate or pin-faithful bus timing,
- a built-in full-machine peripheral model beyond flat memory plus optional port-I/O plugins,
- exhaustive undocumented-behavior matching beyond the implemented core semantics and current tests.

The parity status is tracked in [PARITY.md](PARITY.md).

## What Still Blocks Harness Use

`z80-trace` now has a broad CPU core and a working repo-local harness path. The local Python suite covers prefixed execution, indexed addressing, alternate register exchange, block moves, and interrupt service. The repo-local `make sim-z80-trace-check` and `make sim-check` flows still pass against the current directed fixture suite.

What remains explicit is narrower:

- bus timing remains instruction-accurate rather than wait-state-accurate,
- runtime plugins operate only at the Z80 port-I/O boundary and optional post-instruction interrupt requests,
- verification is materially wider than the original baseline but still not an exhaustive undocumented-behavior corpus.
