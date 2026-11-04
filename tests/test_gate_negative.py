#!/usr/bin/env python3
"""Negative gate: a compile failure in the image test must be a hard failure.

Evidence is written to a unique directory under build/gate-evidence/ and is
kept; nothing is deleted and no user directory is touched.
"""
import argparse
import os
import subprocess
import sys
import tempfile

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--include", default="main")
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    base = os.path.join(root, "build", "gate-evidence")
    os.makedirs(base, exist_ok=True)
    td = tempfile.mkdtemp(prefix="image-negative-", dir=base)
    out = os.path.join(td, "broken.o")
    cc = args.cc
    if cc.endswith(".sh"):
        cc = ["bash", cc]
    else:
        cc = [cc]
    proc = subprocess.run(
        cc + ["-std=c11", "-I", args.include, "-c",
              "tests/fixtures/gal_image_broken.c", "-o", out],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    with open(os.path.join(td, "compile.log"), "w", encoding="utf-8") as fh:
        fh.write("exit=%d\n" % proc.returncode)
        fh.write(proc.stdout)
        fh.write(proc.stderr)
    print("negative gate evidence: %s" % td)
    if proc.returncode == 0:
        print("ERROR: negative gate accepted a broken image test", file=sys.stderr)
        return 1
    if "error" not in (proc.stderr + proc.stdout).lower():
        print("ERROR: compiler failed without an error diagnostic", file=sys.stderr)
        return 1
    print("negative gate: compile failure correctly propagated")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
