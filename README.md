# Z80 Trace

Standalone Z80 CPU tracer modeled on the `i8085-trace` project structure and workflow.

## Scope

`z80-trace` is a CPU-focused golden-reference tracer for future Z80 core verification. It loads a flat 64K binary image, executes instructions one at a time, and emits machine-readable trace data. It deliberately does not model a full machine, ROM layout, or board peripherals in this first baseline, but it now does provide a bounded harness-facing bus-event mode for the current `z80new` RTL differential flow.

This repository is the implementation target described by `z80new` ticket `APU-7`. The goal of this first delivery is to reproduce the reference tool's architecture and developer workflow while establishing a clear baseline for future instruction-complete work.

## Architecture

The layout mirrors `i8085-trace` on purpose:

- `CMakeLists.txt` builds a single standalone executable.
- `include/z80_cpu.h` defines the CPU state and step/disassembly interfaces used by the CLI.
- `src/z80_exec.c` holds instruction execution for the currently implemented Z80 subset.
- `src/z80_state.c` handles allocation and reset.
- `src/z80_disasm.c` provides disassembly strings for trace output.
- `src/main.cpp` owns CLI parsing, binary loading, the trace loop, summary output, dumps, coverage, and tracepoints.
- `test/run_tests.py` is the harness-oriented end-to-end validation entrypoint.

## Implemented Baseline

The current baseline supports enough unprefixed Z80 instructions to prove both the standalone tracer flow and the first bounded `z80new` harness differential flow:

- register and immediate loads,
- register-to-register loads including `(HL)` access,
- `INC` and `DEC` on 8-bit registers and `(HL)`,
- `DJNZ`,
- `ADD`, `SUB`, `XOR`, `OR`, and `CP` in register and immediate forms,
- `JR` and conditional `JR`,
- `JP`, `CALL`, `RET`,
- `PUSH` and `POP` for `BC`, `DE`, `HL`, and `AF`,
- `EX DE,HL`,
- `HALT`,
- `LD (nn),A`, `LD A,(nn)`, `LD (nn),HL`, and `LD HL,(nn)`.

The CLI now also supports `--bus-trace`, which emits harness-comparable `BUS`, `WRITE`, `HALT`, `MEM`, and `PASS` lines for the currently supported subset. That output is intended for normalized architectural differential checks against the current `z80new` RTL harness rather than for cycle-accurate timing claims.

Cycle counts remain rough per-instruction estimates, just like the reference repo's documented approximation. They are useful for relative harness checks but are not timing-accurate.

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

From the `z80new` superproject root, the current bounded harness-use proof is:

```bash
make sim-z80-trace-check
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

Output Options:
  -o, --output=FILE
  -q, --quiet
  -S, --summary
  --bus-trace
  -d, --dump=START:LEN
  --cov=FILE

Tracepoint Options (require -S):
  -t, --tracepoint=ADDR
  -T, --tracepoint-file=FILE
  --tracepoint-max=N
  --tracepoint-stop
```

The CLI also recognizes `--irq`, `--timer`, `--io-plugin`, `--io-plugin-config`, `--io-trace`, and `--gdb`, but the baseline exits with a clear error because those parity features have not been implemented yet.

## Trace Output

Normal execution emits one NDJSON object per instruction. The trace records the pre-execution `pc`, `sp`, flags, and clock count, plus the post-execution general registers:

```json
{"step":0,"pc":"0000","sp":"4000","f":"00","clk":0,"op":"LD","asm":"LD SP,$4000","ix":"0000","iy":"0000","i":"00","r8":"01","r":["00","00","00","00","00","00","00"]}
```

Summary mode emits one final JSON line:

```json
{"pc":"000C","sp":"4000","f":"00","clk":43,"steps":6,"halt":"hlt","ix":"0000","iy":"0000","i":"00","r8":"06","r":["42","00","00","00","00","20","00"]}
```

`--bus-trace` replaces the NDJSON stream with harness-oriented text lines. The output records architectural fetches, operand reads, memory writes, the final halt address, and any requested memory dumps:

```text
BUS cycle=9 kind=fetch addr=0x0009 data=0xC5 m1=1 mreq=1 iorq=0 rd=1 wr=0 rfsh=0 halt=0 busak=0
BUS cycle=10 kind=write addr=0x3FFF data=0x5A m1=0 mreq=1 iorq=0 rd=0 wr=1 rfsh=0 halt=0 busak=0
WRITE cycle=10 addr=0x3FFF data=0x5A
HALT cycle=18 addr=0x000D
PASS cycles=62 writes=3
```

Coverage output matches the `i8085-trace` shape: total steps plus sparse `pc_hits` and `op_hits` arrays. In normal mode memory dumps go to `stderr` in hex plus ASCII. In `--bus-trace` mode dumps emit one `MEM` line per byte so the output stays machine-comparable.

## Example

This example creates a tiny binary, runs the tracer, and prints six trace lines plus the final dump:

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

The current baseline does not yet provide:

- full Z80 instruction coverage, including prefixed opcode families (`CB`, `ED`, `DD`, `FD`) and undocumented behavior,
- interrupt injection or periodic timer simulation,
- runtime I/O plugins,
- GDB remote debugging,
- alternate register set tracing,
- cycle-accurate timing or cycle-faithful bus-level behavior.

The parity status is tracked in [PARITY.md](PARITY.md).

## What Still Blocks Harness Use

`z80-trace` has now reached a first bounded harness-usable baseline for `z80new`: the repo-local `make sim-z80-trace-check` flow compares the current RTL harness against tracer-emitted `--bus-trace` output across a directed fixture suite covering fetch-only streams, immediate loads, direct stores, `JR`, `(HL)` roundtrips, `DJNZ`, `EX DE,HL`, and stack push/pop traffic.

The remaining blockers are explicit and still matter:

- prefixed opcode families (`CB`, `ED`, `DD`, `FD`) remain largely unimplemented,
- alternate register sets and interrupt behavior are still absent,
- timing is still approximate rather than cycle-accurate,
- validation is still bounded to the directed fixture suite rather than broad source-backed Z80 conformance.
