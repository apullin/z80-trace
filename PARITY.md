# `z80-trace` Parity Checklist Against `i8085-trace`

This checklist compares `z80-trace` against the reference tool's architecture and user-facing capabilities. It is intentionally feature-oriented rather than emulator-comparison-oriented.

## Architecture and Workflow

- [x] Standalone repository with one CMake-built executable.
- [x] CPU core, disassembler, state management, and CLI split into separate source files.
- [x] Lightweight Python end-to-end test runner.
- [x] Flat 64K CPU-only memory model with no baked-in machine emulation.
- [x] README that explains scope, build/test usage, architecture, and current gaps.

## Trace and Harness Features

- [x] Load a raw binary at a configurable address.
- [x] Choose separate entry point and stack pointer values.
- [x] Emit per-instruction NDJSON trace records.
- [x] Emit a final summary JSON record in summary mode.
- [x] Write sparse coverage JSON for PC and opcode hit counts.
- [x] Dump memory ranges on exit.
- [x] Support stop-at addresses.
- [x] Support loop detection and opt-out.
- [x] Support tracepoints, tracepoint files, `--tracepoint-max`, and `--tracepoint-stop`.
- [ ] Match the full `i8085-trace` option surface with working implementations for interrupts, timers, runtime I/O plugins, I/O tracing, and GDB RSP.

## CPU Completeness

- [x] Working baseline for a coherent subset of unprefixed Z80 instructions.
- [ ] Full unprefixed Z80 opcode coverage.
- [ ] `CB` prefixed opcodes.
- [ ] `ED` prefixed opcodes.
- [ ] `DD` and `FD` indexed opcodes.
- [ ] Alternate register set handling and trace visibility.
- [ ] Interrupt mode behavior, `IFF1/IFF2`, and related control instructions.
- [ ] Refresh-register and timing fidelity beyond the current rough baseline.
- [ ] Undocumented Z80 behavior coverage where relevant to compatibility goals.

## Validation State

- [x] Automated tests cover trace emission, summary mode, coverage writing, memory dumps, and tracepoint stopping.
- [ ] Tests demonstrate broad instruction-family coverage across the full Z80 ISA.
- [ ] Tests cross-check behavior against primary-source Z80 documentation or later golden-reference corpora.

## Readiness Statement

The project has reached architectural parity with the reference repo's first-order shape and developer workflow. It has not yet reached verification parity because the implemented Z80 execution surface is still intentionally partial.
