#!/usr/bin/env python3

import json
import os
import re
import socket
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
GDB_PORT_RE = re.compile(r"GDB listening on 127\.0\.0\.1:(?P<port>\d+)")


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


def parse_io_trace(stderr: str):
    records = []
    for line in stderr.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            parsed = json.loads(line)
        except json.JSONDecodeError:
            continue
        if parsed.get("type") == "io":
            records.append(parsed)
    return records


def build_test_plugin(tmpdir: Path) -> Path:
    src_path = tmpdir / "test_plugin.c"
    lib_name = "test_plugin.dylib" if sys.platform == "darwin" else "test_plugin.so"
    lib_path = tmpdir / lib_name
    src_path.write_text(
        """
#include "z80_io_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PluginState {
    UINT8 read_value;
    UINT16 last_write_port;
    UINT8 last_write_value;
} PluginState;

static void *plugin_create(const char *config, char *error_out, size_t error_out_len) {
    PluginState *state = (PluginState *)calloc(1, sizeof(PluginState));
    if (!state) {
        if (error_out && error_out_len > 0) {
            snprintf(error_out, error_out_len, "allocation failed");
        }
        return NULL;
    }
    state->read_value = 0x5A;
    if (config && *config) {
        state->read_value = (UINT8)strtoul(config, NULL, 0);
    }
    return state;
}

static void plugin_destroy(void *opaque) {
    free(opaque);
}

static void plugin_reset(void *opaque) {
    PluginState *state = (PluginState *)opaque;
    if (!state) {
        return;
    }
    state->last_write_port = 0;
    state->last_write_value = 0;
}

static UINT8 plugin_read_port(void *opaque, UINT16 port, UINT8 default_value) {
    PluginState *state = (PluginState *)opaque;
    if (state && (port & 0x00FFu) == 0x10u) {
        return state->read_value;
    }
    return default_value;
}

static void plugin_write_port(void *opaque, UINT16 port, UINT8 value) {
    PluginState *state = (PluginState *)opaque;
    if (!state) {
        return;
    }
    state->last_write_port = port;
    state->last_write_value = value;
}

static const Z80TraceIoPluginApi API = {
    Z80_TRACE_IO_PLUGIN_ABI_VERSION,
    "test-plugin",
    plugin_create,
    plugin_destroy,
    plugin_reset,
    plugin_read_port,
    plugin_write_port,
    NULL,
};

const Z80TraceIoPluginApi *z80_trace_get_io_plugin_api(void) {
    return &API;
}
""",
        encoding="ascii",
    )
    if sys.platform == "darwin":
        cmd = [
            "cc",
            "-dynamiclib",
            "-fPIC",
            "-std=c99",
            "-I",
            str(ROOT / "include"),
            str(src_path),
            "-o",
            str(lib_path),
        ]
    else:
        cmd = [
            "cc",
            "-shared",
            "-fPIC",
            "-std=c99",
            "-I",
            str(ROOT / "include"),
            str(src_path),
            "-o",
            str(lib_path),
        ]
    res = run(cmd)
    if res.returncode != 0:
        raise AssertionError(f"failed to build test plugin: {res.stderr}")
    return lib_path


def rsp_checksum(payload: str) -> str:
    return f"{sum(payload.encode('ascii')) & 0xFF:02x}"


class RspClient:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.no_ack = False

    def _read_exact(self, count: int) -> bytes:
        data = bytearray()
        while len(data) < count:
            chunk = self.sock.recv(count - len(data))
            if not chunk:
                raise AssertionError("short read from RSP server")
            data.extend(chunk)
        return bytes(data)

    def _recv_packet(self) -> str:
        while True:
            ch = self._read_exact(1)
            if ch in (b"+", b"-"):
                continue
            if ch != b"$":
                raise AssertionError(f"unexpected RSP prefix: {ch!r}")
            payload = bytearray()
            while True:
                ch = self._read_exact(1)
                if ch == b"#":
                    break
                payload.extend(ch)
            checksum = self._read_exact(2).decode("ascii")
            expected = rsp_checksum(payload.decode("ascii"))
            if checksum.lower() != expected:
                raise AssertionError(f"bad checksum: got {checksum}, expected {expected}")
            if not self.no_ack:
                self.sock.sendall(b"+")
            return payload.decode("ascii")

    def send(self, payload: str) -> str:
        packet = f"${payload}#{rsp_checksum(payload)}".encode("ascii")
        self.sock.sendall(packet)
        if not self.no_ack:
            ack = self._read_exact(1)
            if ack != b"+":
                raise AssertionError(f"expected ack '+', got {ack!r}")
        reply = self._recv_packet()
        if payload == "QStartNoAckMode":
            self.no_ack = True
        return reply


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


def test_indexed_prefixes(bin_path: Path):
    program = bytes([
        0xDD, 0x21, 0x00, 0x20,        # LD IX,0x2000
        0xFD, 0x21, 0x01, 0x20,        # LD IY,0x2001
        0x3E, 0x11,                    # LD A,0x11
        0xDD, 0x77, 0x02,              # LD (IX+2),A
        0xFD, 0x36, 0x03, 0x22,        # LD (IY+3),0x22
        0xDD, 0x7E, 0x02,              # LD A,(IX+2)
        0xFD, 0x86, 0x03,              # ADD A,(IY+3)
        0x76,                          # HALT
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        prog_path = Path(tmpdir) / "indexed.bin"
        write_bytes(prog_path, program)
        res = run([str(bin_path), "-q", "-S", "--dump=0x2002:3", str(prog_path)])
        if res.returncode != 0:
            raise AssertionError(f"indexed prefix test failed: {res.stderr}")

        summary = json.loads(res.stdout.strip().splitlines()[-1])
        if summary["r"][0] != "33":
            raise AssertionError(f"expected indexed A=33, got {summary['r'][0]}")
        if summary["ix"] != "2000" or summary["iy"] != "2001":
            raise AssertionError(f"unexpected index registers: {summary}")

        dumped = parse_dump_bytes(res.stderr, 0x2002, 3)
        if dumped != [0x11, 0x00, 0x22]:
            raise AssertionError(f"expected indexed dump [11,00,22], got {dumped}")


def test_indexed_cb_and_shadow_registers(bin_path: Path):
    indexed_cb_program = bytes([
        0xDD, 0x21, 0x00, 0x20,        # LD IX,0x2000
        0xDD, 0x36, 0x01, 0x81,        # LD (IX+1),0x81
        0xDD, 0xCB, 0x01, 0x06,        # RLC (IX+1)
        0xDD, 0x4E, 0x01,              # LD C,(IX+1)
        0x76,                          # HALT
    ])
    shadow_program = bytes([
        0x01, 0x22, 0x11,              # LD BC,0x1122
        0x11, 0x44, 0x33,              # LD DE,0x3344
        0x21, 0x66, 0x55,              # LD HL,0x5566
        0x3E, 0x12,                    # LD A,0x12
        0x08,                          # EX AF,AF'
        0x3E, 0x34,                    # LD A,0x34
        0xD9,                          # EXX
        0x01, 0x88, 0x77,              # LD BC,0x7788
        0x11, 0xAA, 0x99,              # LD DE,0x99AA
        0x21, 0xCC, 0xBB,              # LD HL,0xBBCC
        0x76,                          # HALT
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        indexed_path = Path(tmpdir) / "indexed_cb.bin"
        write_bytes(indexed_path, indexed_cb_program)
        indexed_res = run([str(bin_path), "-q", "-S", "--dump=0x2001:1", str(indexed_path)])
        if indexed_res.returncode != 0:
            raise AssertionError(f"indexed cb test failed: {indexed_res.stderr}")

        indexed_summary = json.loads(indexed_res.stdout.strip().splitlines()[-1])
        if indexed_summary["r"][2] != "03":
            raise AssertionError(f"expected C=03 after indexed CB op, got {indexed_summary['r'][2]}")
        dumped = parse_dump_bytes(indexed_res.stderr, 0x2001, 1)
        if dumped != [0x03]:
            raise AssertionError(f"expected indexed CB dump [03], got {dumped}")

        shadow_path = Path(tmpdir) / "shadow.bin"
        write_bytes(shadow_path, shadow_program)
        shadow_res = run([str(bin_path), "-q", "-S", str(shadow_path)])
        if shadow_res.returncode != 0:
            raise AssertionError(f"shadow register test failed: {shadow_res.stderr}")

        shadow_summary = json.loads(shadow_res.stdout.strip().splitlines()[-1])
        if shadow_summary["r"][0] != "34":
            raise AssertionError(f"expected main A=34, got {shadow_summary['r'][0]}")
        if shadow_summary["af2"] != "1200":
            raise AssertionError(f"expected AF' = 1200, got {shadow_summary['af2']}")
        if shadow_summary["bc2"] != "1122" or shadow_summary["de2"] != "3344" or shadow_summary["hl2"] != "5566":
            raise AssertionError(f"unexpected shadow register pairs: {shadow_summary}")
        if shadow_summary["r"][1] != "77" or shadow_summary["r"][2] != "88":
            raise AssertionError(f"unexpected main BC after EXX: {shadow_summary['r']}")


def test_block_move_and_interrupts(bin_path: Path):
    ldir_program = bytes([
        0x21, 0x00, 0x20,              # LD HL,0x2000
        0x36, 0x41,                    # LD (HL),0x41
        0x23,                          # INC HL
        0x36, 0x42,                    # LD (HL),0x42
        0x23,                          # INC HL
        0x36, 0x43,                    # LD (HL),0x43
        0x21, 0x00, 0x20,              # LD HL,0x2000
        0x11, 0x10, 0x20,              # LD DE,0x2010
        0x01, 0x03, 0x00,              # LD BC,0x0003
        0xED, 0xB0,                    # LDIR
        0x76,                          # HALT
    ])

    irq_program = bytearray([
        0xF3,                          # DI
        0xED, 0x56,                    # IM 1
        0xFB,                          # EI
        0x76,                          # HALT
        0x76,                          # HALT after RETI
    ])
    irq_program.extend(b"\x00" * (0x0038 - len(irq_program)))
    irq_program.extend([
        0x3E, 0x5A,                    # LD A,0x5A
        0xED, 0x4D,                    # RETI
    ])

    nmi_program = bytearray([
        0xFB,                          # EI
        0x76,                          # HALT
        0x76,                          # HALT after RETN
    ])
    nmi_program.extend(b"\x00" * (0x0066 - len(nmi_program)))
    nmi_program.extend([
        0x3E, 0xA5,                    # LD A,0xA5
        0xED, 0x45,                    # RETN
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        ldir_path = Path(tmpdir) / "ldir.bin"
        write_bytes(ldir_path, ldir_program)
        ldir_res = run([str(bin_path), "-q", "-S", "--dump=0x2010:3", str(ldir_path)])
        if ldir_res.returncode != 0:
            raise AssertionError(f"ldir test failed: {ldir_res.stderr}")
        ldir_summary = json.loads(ldir_res.stdout.strip().splitlines()[-1])
        if ldir_summary["r"][1] != "00" or ldir_summary["r"][2] != "00":
            raise AssertionError(f"expected BC=0000 after LDIR, got {ldir_summary['r'][1:3]}")
        dumped = parse_dump_bytes(ldir_res.stderr, 0x2010, 3)
        if dumped != [0x41, 0x42, 0x43]:
            raise AssertionError(f"expected LDIR dump [41,42,43], got {dumped}")

        irq_path = Path(tmpdir) / "irq.bin"
        write_bytes(irq_path, bytes(irq_program))
        irq_res = run([str(bin_path), "-q", "-S", "--irq=int:4:0xFF", str(irq_path)])
        if irq_res.returncode != 0:
            raise AssertionError(f"irq test failed: {irq_res.stderr}")
        irq_summary = json.loads(irq_res.stdout.strip().splitlines()[-1])
        if irq_summary["r"][0] != "5A":
            raise AssertionError(f"expected IM1 handler A=5A, got {irq_summary['r'][0]}")
        if irq_summary["im"] != 1:
            raise AssertionError(f"expected IM=1 after irq test, got {irq_summary['im']}")
        if irq_summary["iff1"] != 0 or irq_summary["iff2"] != 0:
            raise AssertionError(f"expected IFF cleared after RETI path, got {irq_summary}")

        nmi_path = Path(tmpdir) / "nmi.bin"
        write_bytes(nmi_path, bytes(nmi_program))
        nmi_res = run([str(bin_path), "-q", "-S", "--irq=nmi:2", str(nmi_path)])
        if nmi_res.returncode != 0:
            raise AssertionError(f"nmi test failed: {nmi_res.stderr}")
        nmi_summary = json.loads(nmi_res.stdout.strip().splitlines()[-1])
        if nmi_summary["r"][0] != "A5":
            raise AssertionError(f"expected NMI handler A=A5, got {nmi_summary['r'][0]}")
        if nmi_summary["iff1"] != 1:
            raise AssertionError(f"expected RETN to restore IFF1, got {nmi_summary['iff1']}")


def test_timer_and_io_plugin(bin_path: Path):
    timer_program = bytearray([
        0xED, 0x56,                    # IM 1
        0xFB,                          # EI
        0x00,                          # NOP
        0x76,                          # HALT
    ])
    timer_program.extend(b"\x00" * (0x0038 - len(timer_program)))
    timer_program.extend([
        0x3C,                          # INC A
        0xED, 0x4D,                    # RETI
    ])

    io_program = bytes([
        0xDB, 0x10,                    # IN A,(0x10)
        0xD3, 0x11,                    # OUT (0x11),A
        0x76,                          # HALT
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)

        timer_path = tmp / "timer.bin"
        write_bytes(timer_path, bytes(timer_program))
        timer_res = run(
            [
                str(bin_path),
                "-q",
                "-S",
                "--no-loop-detect",
                "--max-steps=7",
                "--timer=int:1000:12:0xFF",
                str(timer_path),
            ]
        )
        if timer_res.returncode != 0:
            raise AssertionError(f"timer test failed: {timer_res.stderr}")
        timer_summary = json.loads(timer_res.stdout.strip().splitlines()[-1])
        if timer_summary["r"][0] != "01":
            raise AssertionError(f"expected timer interrupt to increment A to 01, got {timer_summary['r'][0]}")
        if timer_summary["steps"] != 7:
            raise AssertionError(f"expected bounded timer run to stop at 7 steps, got {timer_summary['steps']}")

        plugin_path = build_test_plugin(tmp)
        io_path = tmp / "io.bin"
        write_bytes(io_path, io_program)
        io_res = run(
            [
                str(bin_path),
                "-q",
                "-S",
                "--io-trace",
                f"--io-plugin={plugin_path}",
                "--io-plugin-config=0x5A",
                str(io_path),
            ]
        )
        if io_res.returncode != 0:
            raise AssertionError(f"io plugin test failed: {io_res.stderr}")
        io_summary = json.loads(io_res.stdout.strip().splitlines()[-1])
        if io_summary["r"][0] != "5A":
            raise AssertionError(f"expected plugin-backed IN to load A=5A, got {io_summary['r'][0]}")
        io_trace = parse_io_trace(io_res.stderr)
        expected_trace = [
            {"kind": "read", "port": "0010", "data": "5A", "source": "plugin"},
            {"kind": "write", "port": "5A11", "data": "5A", "source": "plugin"},
        ]
        observed = [{k: entry[k] for k in ("kind", "port", "data", "source")} for entry in io_trace]
        if observed != expected_trace:
            raise AssertionError(f"unexpected io-trace records: {observed}")


def test_gdb_rsp(bin_path: Path):
    program = bytes([
        0x3E, 0x42,                    # LD A,0x42
        0x32, 0x00, 0x20,              # LD (0x2000),A
        0x76,                          # HALT
    ])

    with tempfile.TemporaryDirectory() as tmpdir:
        prog_path = Path(tmpdir) / "gdb.bin"
        write_bytes(prog_path, program)
        proc = subprocess.Popen(
            [str(bin_path), "-q", "--gdb=0", str(prog_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            port_line = proc.stderr.readline().strip()
            match = GDB_PORT_RE.search(port_line)
            if not match:
                raise AssertionError(f"failed to read gdb port line, got: {port_line!r}")
            port = int(match.group("port"))

            with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
                client = RspClient(sock)
                supported = client.send("qSupported:multiprocess+;swbreak+")
                if "QStartNoAckMode+" not in supported:
                    raise AssertionError(f"unexpected qSupported reply: {supported}")
                if client.send("QStartNoAckMode") != "OK":
                    raise AssertionError("failed to enter no-ack mode")
                if client.send("?") != "S05":
                    raise AssertionError("unexpected initial stop reply")
                if client.send("p5") != "0000":
                    raise AssertionError("expected initial PC=0000")
                if client.send("Z0,0002,1") != "OK":
                    raise AssertionError("failed to install software breakpoint")
                if client.send("c") != "S05":
                    raise AssertionError("continue did not stop at breakpoint")
                if client.send("p0") != "0042":
                    raise AssertionError("expected AF to reflect LD A,0x42 before store")
                if client.send("m2000,1") != "00":
                    raise AssertionError("memory changed before store breakpoint")
                if client.send("s") != "S05":
                    raise AssertionError("single-step did not stop")
                if client.send("p5") != "0500":
                    raise AssertionError("expected PC=0005 after stepping store")
                if client.send("m2000,1") != "42":
                    raise AssertionError("store did not update memory after single-step")
                if client.send("z0,0002,1") != "OK":
                    raise AssertionError("failed to clear software breakpoint")
                if client.send("M2001,1:55") != "OK":
                    raise AssertionError("memory write packet failed")
                if client.send("m2001,1") != "55":
                    raise AssertionError("memory write packet did not take effect")
                if client.send("c") != "S05":
                    raise AssertionError("continue to HALT did not stop")
                if client.send("D") != "OK":
                    raise AssertionError("detach failed")
        finally:
            stdout, stderr = proc.communicate(timeout=5)
            if proc.returncode != 0:
                raise AssertionError(f"gdb session failed: stdout={stdout!r} stderr={stderr!r}")


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
    test_indexed_prefixes(bin_path)
    test_indexed_cb_and_shadow_registers(bin_path)
    test_block_move_and_interrupts(bin_path)
    test_timer_and_io_plugin(bin_path)
    test_gdb_rsp(bin_path)
    print("All tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
