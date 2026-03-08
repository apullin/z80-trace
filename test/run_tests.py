#!/usr/bin/env python3

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN_CANDIDATES = [
    ROOT / "build" / "z80-trace",
    ROOT / "z80-trace",
]


def find_binary() -> Path | None:
    for path in BIN_CANDIDATES:
        if path.exists() and os.access(path, os.X_OK):
            return path
    return None


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def write_bytes(path: Path, data: bytes):
    path.write_bytes(data)


def parse_dump_bytes(stderr: str, addr: int, count: int):
    marker = f"{addr:04X}:"
    for line in stderr.splitlines():
        if marker not in line:
            continue
        after = line.split(marker, 1)[1]
        if "|" in after:
            after = after.split("|", 1)[0]
        parts = [p for p in after.strip().split() if re.fullmatch(r"[0-9A-Fa-f]{2}", p)]
        if len(parts) >= count:
            return [int(p, 16) for p in parts[:count]]
    return None


def test_trace_and_dump(bin_path: Path):
    program = bytes([
        0x31, 0x00, 0x40,  # LD SP,0x4000
        0x21, 0x00, 0x20,  # LD HL,0x2000
        0x3E, 0x42,        # LD A,0x42
        0x77,              # LD (HL),A
        0xEE, 0x00,        # XOR 0x00
        0x7E,              # LD A,(HL)
        0x76,              # HALT
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        prog_path = Path(tmpdir) / "trace.bin"
        write_bytes(prog_path, program)
        res = run([str(bin_path), "-q", "--dump=0x2000:1", str(prog_path)])
        if res.returncode != 0:
            raise AssertionError(f"trace test failed: {res.stderr}")

        lines = [json.loads(line) for line in res.stdout.splitlines() if line.strip()]
        if len(lines) != 7:
            raise AssertionError(f"expected 7 trace lines, got {len(lines)}")
        if lines[0]["pc"] != "0000" or lines[0]["op"] != "LD":
            raise AssertionError(f"unexpected first trace line: {lines[0]}")
        if lines[-1]["op"] != "HALT":
            raise AssertionError(f"expected final op HALT, got {lines[-1]['op']}")
        if lines[-1]["r"][0] != "42":
            raise AssertionError(f"expected A=42 at halt, got {lines[-1]['r'][0]}")

        dumped = parse_dump_bytes(res.stderr, 0x2000, 1)
        if dumped != [0x42]:
            raise AssertionError(f"expected dump [42], got {dumped}")


def test_summary_and_coverage(bin_path: Path):
    program = bytes([
        0x3E, 0x01,  # LD A,0x01
        0x06, 0x02,  # LD B,0x02
        0x80,        # ADD A,B
        0x76,        # HALT
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        prog_path = Path(tmpdir) / "summary.bin"
        cov_path = Path(tmpdir) / "cov.json"
        write_bytes(prog_path, program)
        res = run([str(bin_path), "-q", "-S", f"--cov={cov_path}", str(prog_path)])
        if res.returncode != 0:
            raise AssertionError(f"summary test failed: {res.stderr}")

        summary = json.loads(res.stdout.strip().splitlines()[-1])
        if summary["halt"] != "hlt":
            raise AssertionError(f"expected halt reason hlt, got {summary['halt']}")
        if summary["r"][0] != "03":
            raise AssertionError(f"expected A=03, got {summary['r'][0]}")
        if summary["steps"] != 4:
            raise AssertionError(f"expected 4 steps, got {summary['steps']}")

        cov = json.loads(cov_path.read_text())
        if cov["steps"] != 4:
            raise AssertionError(f"expected coverage steps 4, got {cov['steps']}")
        op_hits = {entry["op"]: entry["count"] for entry in cov["op_hits"]}
        for opcode in ("3E", "06", "80", "76"):
            if op_hits.get(opcode) != 1:
                raise AssertionError(f"expected opcode {opcode} hit once, got {op_hits.get(opcode)}")


def test_tracepoint_max(bin_path: Path):
    program = bytes([
        0x00,        # NOP
        0x18, 0xFE,  # JR -2 -> PC 0x0001
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        prog_path = Path(tmpdir) / "tracepoint.bin"
        write_bytes(prog_path, program)
        res = run([
            str(bin_path),
            "-q",
            "-S",
            "--tracepoint=0x0001",
            "--tracepoint-max=2",
            "--no-loop-detect",
            str(prog_path),
        ])
        if res.returncode != 0:
            raise AssertionError(f"tracepoint test failed: {res.stderr}")

        lines = [json.loads(line) for line in res.stdout.splitlines() if line.strip()]
        if len(lines) != 3:
            raise AssertionError(f"expected 3 JSON lines, got {len(lines)}")
        if lines[0]["pc"] != "0001" or lines[1]["pc"] != "0001":
            raise AssertionError(f"expected tracepoint hits at 0001, got {lines[:2]}")
        if lines[-1]["halt"] != "tracepoint-max":
            raise AssertionError(f"expected halt tracepoint-max, got {lines[-1]['halt']}")


def main():
    bin_path = find_binary()
    if not bin_path:
        print("Error: z80-trace binary not found. Build with cmake first.", file=sys.stderr)
        return 1

    test_trace_and_dump(bin_path)
    test_summary_and_coverage(bin_path)
    test_tracepoint_max(bin_path)
    print("All tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
