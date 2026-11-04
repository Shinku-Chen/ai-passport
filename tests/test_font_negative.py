#!/usr/bin/env python3
"""Negative gate: a missing generated font must make --check exit non-zero.
Evidence is kept under build/gate-evidence/ (no cleanup, no user directory).
"""
import os
import subprocess
import sys
import tempfile

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    base = os.path.join(root, "build", "gate-evidence")
    os.makedirs(base, exist_ok=True)
    td = tempfile.mkdtemp(prefix="font-negative-", dir=base)
    proc = subprocess.run(
        [sys.executable, "tools/manosaba_font.py", "--check",
         "--pack", "main/gal_data/manosaba.pack",
         "--source-root", "main", "--out-dir", td],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=root)
    with open(os.path.join(td, "check.log"), "w", encoding="utf-8") as fh:
        fh.write("exit=%d\n" % proc.returncode)
        fh.write(proc.stdout)
        fh.write(proc.stderr)
    print("font negative gate evidence: %s" % td)
    if proc.returncode == 0:
        print("ERROR: font check accepted a missing font", file=sys.stderr)
        return 1
    print("font negative gate: missing font correctly rejected")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
