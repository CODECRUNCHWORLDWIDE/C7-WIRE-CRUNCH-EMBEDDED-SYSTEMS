#!/usr/bin/env python3
"""
cc-debug.py - Host-side core-dump reader for the Week 12 mini-project.

Pulls the persisted crash dump out of a target Pico over SWD (via an already-
running OpenOCD GDB server) and pretty-prints a postmortem, mapping the
faulting PC and the heuristic backtrace to source lines with addr2line.

This is the off-target half of the Unix "gdb program core" workflow: the
device records its own core image into no-init SRAM (fault_handler.c); this
tool reads that image back and decodes it.

Two modes:
  1. --gdb : connect to the OpenOCD GDB server, dump the no-init SRAM region,
             parse the cc_crash_dump_t struct, and decode it. (Default.)
  2. --bin : decode a previously-saved binary dump (from GDB's
             `dump binary memory core.bin <start> <end>`), no live target.

Usage:
  cc-debug.py --elf build/debug_app.elf [--gdb localhost:3333]
  cc-debug.py --elf build/debug_app.elf --bin core.bin --dump-addr 0x20000000

Citations:
  - Lecture 2 (the cc_crash_dump_t layout) and debug_common.h.
  - GDB Remote Serial Protocol; arm-none-eabi-addr2line.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import socket
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# cc_crash_dump_t layout, mirroring exercises/debug_common.h.
#
#   uint32 magic
#   uint32 version
#   uint32 exc_number
#   8 x uint32 frame  (r0,r1,r2,r3,r12,lr,pc,xpsr)
#   uint32 stacked_sp
#   6 x uint32 extra_regs
#   8 x uint32 backtrace
#   uint32 backtrace_count
#   uint32 crc32
#   uint32 source
# ---------------------------------------------------------------------------

CRASH_MAGIC = 0xDEADF1A7
CRASH_VERSION = 1

_FMT = "<3I8I I6I8I I I I"   # magic..source
DUMP_SIZE = struct.calcsize(_FMT)

EXC_NAMES = {
    0: "thread mode", 2: "NMI", 3: "HardFault",
    11: "SVCall", 14: "PendSV", 15: "SysTick",
}
SOURCE_NAMES = {1: "no-init SRAM (full)", 2: "watchdog scratch (minimal)"}


def crc32_ieee(data: bytes) -> int:
    """IEEE 802.3 reflected CRC32, matching cc_crc32 in fault_handler.c."""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            mask = 0xFFFFFFFF if (crc & 1) else 0
            crc = (crc >> 1) ^ (0xEDB88320 & mask)
            crc &= 0xFFFFFFFF
    return crc ^ 0xFFFFFFFF


class CrashDump:
    def __init__(self, raw: bytes):
        if len(raw) < DUMP_SIZE:
            raise ValueError(f"need {DUMP_SIZE} bytes, got {len(raw)}")
        fields = struct.unpack(_FMT, raw[:DUMP_SIZE])
        self.magic = fields[0]
        self.version = fields[1]
        self.exc_number = fields[2]
        (self.r0, self.r1, self.r2, self.r3,
         self.r12, self.lr, self.pc, self.xpsr) = fields[3:11]
        self.stacked_sp = fields[11]
        self.extra_regs = list(fields[12:18])
        self.backtrace = list(fields[18:26])
        self.backtrace_count = fields[26]
        self.crc32 = fields[27]
        self.source = fields[28]
        self._raw = raw[:DUMP_SIZE]

    def valid(self) -> bool:
        if self.magic != CRASH_MAGIC:
            return False
        if self.version != CRASH_VERSION:
            return False
        # CRC covers everything up to (not including) crc32 at offset.
        crc_off = struct.calcsize("<3I8I I6I8I I")  # up to backtrace_count incl.
        return crc32_ieee(self._raw[:crc_off]) == self.crc32


def addr2line(elf: Path, addr: int) -> str:
    try:
        out = subprocess.check_output(
            ["arm-none-eabi-addr2line", "-f", "-e", str(elf), f"0x{addr:08x}"],
            stderr=subprocess.DEVNULL,
        ).decode().strip().replace("\n", " @ ")
        return out
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "(addr2line unavailable)"


def print_postmortem(d: CrashDump, elf: Optional[Path]) -> None:
    print("\n*** CRASH DUMP DECODED ***")
    print(f"  source      : {SOURCE_NAMES.get(d.source, 'unknown')}")
    print(f"  faulted in  : {EXC_NAMES.get(d.exc_number, 'IRQ/unknown')} "
          f"(exc #{d.exc_number})")
    print(f"  PC  = 0x{d.pc:08x}   LR  = 0x{d.lr:08x}   xPSR = 0x{d.xpsr:08x}")
    print(f"  R0  = 0x{d.r0:08x}   R1  = 0x{d.r1:08x}   "
          f"R2  = 0x{d.r2:08x}   R3  = 0x{d.r3:08x}")
    print(f"  R12 = 0x{d.r12:08x}   stacked SP = 0x{d.stacked_sp:08x}")
    print(f"  R8..R11 = " + " ".join(f"0x{r:08x}" for r in d.extra_regs[:4]))
    print(f"  R4..R7  = " + " ".join(f"0x{r:08x}" for r in d.extra_regs[4:6]))

    if elf:
        print(f"  faulting line: {addr2line(elf, d.pc)}")
        print("  backtrace:")
        for i in range(min(d.backtrace_count, len(d.backtrace))):
            a = d.backtrace[i]
            print(f"    0x{a:08x}  {addr2line(elf, a)}")
    else:
        print(f"  backtrace ({d.backtrace_count} entries): " +
              " ".join(f"0x{a:08x}" for a in d.backtrace[:d.backtrace_count]))
    print("*** end ***\n")


# ---------------------------------------------------------------------------
# Pull the dump live from a running OpenOCD GDB server, by scanning SRAM for
# the magic. We talk a tiny subset of the GDB Remote Serial Protocol: the
# 'm<addr>,<len>' memory-read packet.
# ---------------------------------------------------------------------------

def _gdb_rsp_checksum(payload: str) -> str:
    return f"{sum(payload.encode()) & 0xff:02x}"


def _gdb_send(sock: socket.socket, payload: str) -> str:
    pkt = f"${payload}#{_gdb_rsp_checksum(payload)}"
    sock.sendall(pkt.encode())
    # Read the '+' ack then the '$...#xx' reply.
    buf = b""
    while b"#" not in buf or buf.count(b"#") < 1 or len(buf) < buf.rfind(b"#") + 3:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
        if buf.startswith(b"+"):
            buf = buf[1:]
    sock.sendall(b"+")  # ack the reply
    text = buf.decode(errors="replace")
    start = text.find("$")
    end = text.find("#", start)
    return text[start + 1:end] if start >= 0 and end >= 0 else ""


def read_mem(sock: socket.socket, addr: int, length: int) -> bytes:
    reply = _gdb_send(sock, f"m{addr:x},{length:x}")
    if reply.startswith("E") or not reply:
        raise RuntimeError(f"memory read failed at 0x{addr:08x}: {reply!r}")
    return bytes.fromhex(reply)


def pull_dump_over_gdb(host: str, port: int) -> Optional[CrashDump]:
    with socket.create_connection((host, port), timeout=10) as sock:
        sock.sendall(b"+")
        # Scan the 264 KB SRAM window for the magic in 4 KB strides; the
        # no-init region is near the top, so scan downward from 0x20042000.
        magic_le = struct.pack("<I", CRASH_MAGIC)
        base, top, step = 0x20000000, 0x20042000, 0x1000
        for a in range(top - step, base - 1, -step):
            block = read_mem(sock, a, step)
            idx = block.find(magic_le)
            if idx != -1:
                addr = a + idx
                raw = read_mem(sock, addr, DUMP_SIZE)
                d = CrashDump(raw)
                if d.valid():
                    print(f"cc-debug: found valid dump at 0x{addr:08x}")
                    return d
    return None


def main() -> None:
    ap = argparse.ArgumentParser(description="Week 12 core-dump reader.")
    ap.add_argument("--elf", type=Path, help="matching ELF for addr2line")
    ap.add_argument("--gdb", type=str, default="localhost:3333",
                    help="OpenOCD GDB server host:port")
    ap.add_argument("--bin", type=Path, default=None,
                    help="decode a saved binary SRAM dump instead of going live")
    ap.add_argument("--dump-addr", type=lambda s: int(s, 0), default=0x20000000,
                    help="base address of the --bin image (for offset search)")
    args = ap.parse_args()

    if args.elf and not args.elf.exists():
        sys.exit(f"error: ELF {args.elf} not found")

    if args.bin:
        data = args.bin.read_bytes()
        idx = data.find(struct.pack("<I", CRASH_MAGIC))
        if idx == -1:
            sys.exit("error: no crash magic in the binary image")
        d = CrashDump(data[idx:idx + DUMP_SIZE])
        if not d.valid():
            print("warning: dump CRC/magic did not validate; decoding anyway")
        print_postmortem(d, args.elf)
        return

    host, _, port = args.gdb.partition(":")
    try:
        d = pull_dump_over_gdb(host, int(port or "3333"))
    except (OSError, RuntimeError) as e:
        sys.exit(f"error talking to OpenOCD GDB server: {e}")

    if d is None:
        print("cc-debug: no valid crash dump found in SRAM (clean device?).")
        return
    print_postmortem(d, args.elf)


if __name__ == "__main__":
    main()
