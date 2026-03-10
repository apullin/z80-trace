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
- [x] Emit harness-comparable `BUS`, `WRITE`, `HALT`, `MEM`, and `PASS` lines in `--bus-trace` mode.
- [x] Expose alternate register pairs and interrupt bookkeeping in trace and summary output.
- [x] Support CLI-driven interrupt injection with `--irq=SPEC`.
- [x] Write sparse coverage JSON for PC and opcode hit counts.
- [x] Dump memory ranges on exit.
- [x] Support stop-at addresses.
- [x] Support loop detection and opt-out.
- [x] Support tracepoints, tracepoint files, `--tracepoint-max`, and `--tracepoint-stop`.
- [x] Match the full `i8085-trace` option surface with working implementations for timers, runtime I/O plugins, I/O tracing, and GDB RSP.

## CPU Completeness

- [x] Full unprefixed Z80 opcode coverage.
- [x] `CB` prefixed opcodes.
- [x] `ED` prefixed opcodes.
- [x] `DD` and `FD` indexed opcodes.
- [x] Alternate register set handling and trace visibility.
- [x] Interrupt mode behavior, `IFF1/IFF2`, and related control instructions.
- [x] Refresh-register bookkeeping and instruction-accurate T-state totals.
- [ ] Exhaustive undocumented Z80 behavior coverage where relevant to compatibility goals.

## Validation State

- [x] Automated tests cover trace emission, summary mode, coverage writing, memory dumps, tracepoint stopping, `--bus-trace`, indexed prefixes, indexed-`CB`, alternate registers, block moves, interrupt service, periodic timers, runtime I/O plugins, I/O tracing, and GDB RSP.
- [x] Repo-local differential checks compare the current `z80new` RTL harness against `z80-trace` on a documented directed fixture suite.
- [x] Tests demonstrate broad instruction-family coverage across the major Z80 opcode families exercised by the in-repo fixtures and black-box tracer suite.
- [x] The implementation and tests are grounded in the checked-in Z80 primary documents under `docs/`.
- [x] A real local `gdb` client can attach, install a breakpoint, continue, inspect registers, and detach through `--gdb=PORT`.

## Readiness Statement

The project has reached architectural parity with the reference repo's first-order shape and now also supports a broad CPU-reference role inside `z80new`. The remaining explicit gaps are concentrated in wait-state fidelity, full-machine peripheral modeling beyond the plugin seam, and exhaustive undocumented-behavior verification rather than in missing mainstream Z80 execution support or missing tracer runtime features.
