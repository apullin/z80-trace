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
BUS_RE = re.compile(
    r"^BUS cycle=(?P<cycle>\d+) kind=(?P<kind>[a-z-]+) "
    r"addr=0x(?P<addr>[0-9A-Fa-f]{4}) data=0x(?P<data>[0-9A-Fa-f]{2}) "
    r"m1=(?P<m1>[01]) mreq=(?P<mreq>[01]) iorq=(?P<iorq>[01]) rd=(?P<rd>[01]) "
    r"wr=(?P<wr>[01]) rfsh=(?P<rfsh>[01]) halt=(?P<halt>[01]) busak=(?P<busak>[01])$"
)
WRITE_RE = re.compile(r"^WRITE cycle=(?P<cycle>\d+) addr=0x(?P<addr>[0-9A-Fa-f]{4}) data=0x(?P<data>[0-9A-Fa-f]{2})$")
HALT_RE = re.compile(r"^HALT cycle=(?P<cycle>\d+) addr=0x(?P<addr>[0-9A-Fa-f]{4})$")
MEM_RE = re.compile(r"^MEM addr=0x(?P<addr>[0-9A-Fa-f]{4}) data=0x(?P<data>[0-9A-Fa-f]{2})$")
PASS_RE = re.compile(r"^PASS cycles=(?P<cycles>\d+) writes=(?P<writes>\d+)$")


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


def parse_bus_trace(stdout: str):
    parsed = {
        "bus": [],
        "writes": [],
        "halt": None,
        "mem": [],
        "pass": None,
    }
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        match = BUS_RE.match(line)
        if match:
            parsed["bus"].append(
                {
                    "cycle": int(match.group("cycle")),
                    "kind": match.group("kind"),
                    "addr": int(match.group("addr"), 16),
                    "data": int(match.group("data"), 16),
                }
            )
            continue
        match = WRITE_RE.match(line)
        if match:
            parsed["writes"].append(
                {
                    "cycle": int(match.group("cycle")),
                    "addr": int(match.group("addr"), 16),
                    "data": int(match.group("data"), 16),
                }
            )
            continue
        match = HALT_RE.match(line)
        if match:
            parsed["halt"] = {
                "cycle": int(match.group("cycle")),
                "addr": int(match.group("addr"), 16),
            }
            continue
        match = MEM_RE.match(line)
        if match:
            parsed["mem"].append(
                {
                    "addr": int(match.group("addr"), 16),
                    "data": int(match.group("data"), 16),
                }
            )
            continue
        match = PASS_RE.match(line)
        if match:
            parsed["pass"] = {
                "cycles": int(match.group("cycles")),
                "writes": int(match.group("writes")),
            }
    return parsed


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


def test_push_pop_bus_trace(bin_path: Path):
    program = bytes([
        0x31, 0x00, 0x40,  # LD SP,0x4000
        0x21, 0x00, 0x20,  # LD HL,0x2000
        0x01, 0x33, 0x5A,  # LD BC,0x5A33
        0xC5,              # PUSH BC
        0xF1,              # POP AF
        0x77,              # LD (HL),A
        0x76,              # HALT
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        prog_path = Path(tmpdir) / "push_pop.bin"
        write_bytes(prog_path, program)
        res = run([str(bin_path), "-q", "--bus-trace", "--dump=0x2000:1", str(prog_path)])
        if res.returncode != 0:
            raise AssertionError(f"push/pop bus trace failed: {res.stderr}")

        parsed = parse_bus_trace(res.stdout)
        bus_kinds = [entry["kind"] for entry in parsed["bus"]]
        if bus_kinds[:3] != ["fetch", "read", "read"]:
            raise AssertionError(f"unexpected first bus events: {parsed['bus'][:3]}")
        stack_writes = [(entry["addr"], entry["data"]) for entry in parsed["writes"][:2]]
        if stack_writes != [(0x3FFF, 0x5A), (0x3FFE, 0x33)]:
            raise AssertionError(f"unexpected PUSH writes: {stack_writes}")
        if parsed["mem"] != [{"addr": 0x2000, "data": 0x5A}]:
            raise AssertionError(f"unexpected bus-trace dump: {parsed['mem']}")
        if parsed["halt"] != {"cycle": parsed["halt"]["cycle"], "addr": 0x000D}:
            raise AssertionError(f"unexpected halt record: {parsed['halt']}")
        if parsed["pass"] is None or parsed["pass"]["writes"] != 3:
            raise AssertionError(f"unexpected PASS line: {parsed['pass']}")


def test_djnz_and_exchange(bin_path: Path):
    djnz_program = bytes([
        0x06, 0x03,        # LD B,0x03
        0x21, 0x00, 0x20,  # LD HL,0x2000
        0x3E, 0x0F,        # LD A,0x0F
        0x3C,              # INC A
        0x10, 0xFD,        # DJNZ back to INC A
        0x77,              # LD (HL),A
        0x76,              # HALT
    ])
    exchange_program = bytes([
        0x11, 0x00, 0x20,  # LD DE,0x2000
        0x21, 0x34, 0x12,  # LD HL,0x1234
        0x3E, 0x44,        # LD A,0x44
        0xEB,              # EX DE,HL
        0x77,              # LD (HL),A
        0x76,              # HALT
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        djnz_path = Path(tmpdir) / "djnz.bin"
        exchange_path = Path(tmpdir) / "exchange.bin"
        write_bytes(djnz_path, djnz_program)
        write_bytes(exchange_path, exchange_program)

        djnz_res = run([str(bin_path), "-q", "--dump=0x2000:1", str(djnz_path)])
        if djnz_res.returncode != 0:
            raise AssertionError(f"DJNZ test failed: {djnz_res.stderr}")
        djnz_lines = [json.loads(line) for line in djnz_res.stdout.splitlines() if line.strip()]
        if djnz_lines[-1]["r"][0] != "12":
            raise AssertionError(f"expected A=12 after DJNZ loop, got {djnz_lines[-1]['r'][0]}")
        dumped = parse_dump_bytes(djnz_res.stderr, 0x2000, 1)
        if dumped != [0x12]:
            raise AssertionError(f"expected DJNZ dump [12], got {dumped}")

        exchange_res = run([str(bin_path), "-q", "--dump=0x2000:1", str(exchange_path)])
        if exchange_res.returncode != 0:
            raise AssertionError(f"EX DE,HL test failed: {exchange_res.stderr}")
        exchange_lines = [json.loads(line) for line in exchange_res.stdout.splitlines() if line.strip()]
        if exchange_lines[-2]["asm"] != "LD (HL),A":
            raise AssertionError(f"unexpected penultimate instruction: {exchange_lines[-2]}")
        dumped = parse_dump_bytes(exchange_res.stderr, 0x2000, 1)
        if dumped != [0x44]:
            raise AssertionError(f"expected EX DE,HL dump [44], got {dumped}")


def main():
    bin_path = find_binary()
    if not bin_path:
        print("Error: z80-trace binary not found. Build with cmake first.", file=sys.stderr)
        return 1

    test_trace_and_dump(bin_path)
    test_summary_and_coverage(bin_path)
    test_tracepoint_max(bin_path)
    test_push_pop_bus_trace(bin_path)
    test_djnz_and_exchange(bin_path)
    print("All tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
