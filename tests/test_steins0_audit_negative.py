#!/usr/bin/env python3
"""Negative test: unknown expireRine flag bits must fail the SG0 source audit.

Usage: test_steins0_audit_negative.py <source> <pack> [audit.py]

Mutates the first non-empty expireRine event's flags with 0x80 and 0x82.
The C reader rejects unknown bits; the audit must reject them too (legacy
flags=0 and new flags=2 are still valid).
"""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "tools" / "steins0_audit.py"
SEC_EVENT = 13


def find_expire(blob: bytes):
    version, header, sections = struct.unpack_from("<HHH", blob, 8)
    if version != 1 or header != 20:
        raise SystemExit(f"unexpected pack header {version}/{header}")
    for i in range(sections):
        typ, off, count, size = struct.unpack_from("<IIII", blob, 20 + i * 16)
        if typ != SEC_EVENT:
            continue
        for index in range(count):
            at = off + index * 16
            if blob[at + 2] == 4 and struct.unpack_from("<H", blob, at + 6)[0] > 0:
                return at
    raise SystemExit("no non-empty expireRine event in pack")


def run_audit(source: str, pack: Path):
    return subprocess.run(
        [sys.executable, str(AUDIT), source, "--pack", str(pack), "--quiet"],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def main() -> int:
    source, pack = sys.argv[1], Path(sys.argv[2])
    blob = bytearray(pack.read_bytes())
    offset = find_expire(blob)
    failures = 0
    for bit in (0x80, 0x82):
        mutated = bytearray(blob)
        mutated[offset + 3] |= bit
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad_pack.bin"
            bad.write_bytes(mutated)
            result = run_audit(source, bad)
        if result.returncode == 0:
            print(f"FAIL: expireRine flags 0x{bit:02X} was accepted")
            print(result.stdout[-500:])
            failures += 1
        elif "unknown_flags" not in result.stdout:
            print(f"FAIL: flags 0x{bit:02X} failed for the wrong reason")
            print(result.stdout[-500:])
            failures += 1
        else:
            print(f"PASS: expireRine flags 0x{bit:02X} rejected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
