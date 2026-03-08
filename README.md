# Z80 Trace

Standalone Z80 CPU tracer modeled on the `i8085-trace` project structure and workflow.

## Scope

`z80-trace` is a CPU-focused golden-reference tracer for future Z80 core verification. It loads a flat 64K binary image, executes instructions one at a time, and emits machine-readable trace data. It deliberately does not model a full machine, ROM layout, or board peripherals in this first baseline.

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

The current baseline supports enough unprefixed Z80 instructions to prove the harness flow end to end:

- register and immediate loads,
- register-to-register loads including `(HL)` access,
- `INC` and `DEC` on 8-bit registers and `(HL)`,
- `ADD`, `SUB`, `XOR`, `OR`, and `CP` in register and immediate forms,
- `JR` and conditional `JR`,
- `JP`, `CALL`, `RET`,
- `HALT`,
- `LD (nn),A`, `LD A,(nn)`, `LD (nn),HL`, and `LD HL,(nn)`.

Cycle counts are rough per-instruction estimates, just like the reference repo's documented approximation. They are useful for relative harness checks but are not yet timing-accurate.

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

Coverage output matches the `i8085-trace` shape: total steps plus sparse `pc_hits` and `op_hits` arrays. Memory dumps go to `stderr` in hex plus ASCII.

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
- cycle-accurate timing or bus-level behavior.

The parity status is tracked in [PARITY.md](PARITY.md).

## What Still Blocks Harness Use

`z80-trace` is not yet ready to act as the verification harness for a Z80 core because it still lacks instruction-complete execution, interrupt behavior, prefixed opcode coverage, and stronger validation against authoritative Z80 behavior. The current baseline is intended to lock in project shape, CLI contract, trace format, and test structure so future work can extend it without rethinking the harness architecture.
