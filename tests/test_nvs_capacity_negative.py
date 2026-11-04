#!/usr/bin/env python3
"""Negative capacity contract: the old 64 KiB NVS layout must be rejected."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOL = os.path.join(ROOT, "tests", "test_nvs_capacity.py")


def main():
    if len(sys.argv) != 2:
        print("usage: test_nvs_capacity_negative.py <witness-log>")
        return 2
    witness = sys.argv[1]
    outdir = tempfile.mkdtemp(prefix="nvs-capacity-negative-", dir=os.path.join(ROOT, "build"))
    src = open(os.path.join(ROOT, "partitions.csv"), encoding="utf-8").read()
    small = src.replace("0x26000", "0x10000")
    part = os.path.join(outdir, "partitions-small.csv")
    open(part, "w", encoding="utf-8").write(small)
    cmd = [sys.executable, TOOL, "--witness-log", witness, "--partitions", part]
    run = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print("NVS_NEG_DIR:", outdir)
    print(run.stdout)
    if run.stderr:
        print(run.stderr)
    if run.returncode == 0:
        print("FAIL: 64 KiB small partition was accepted")
        return 1
    if "partition nvs expected" not in run.stdout:
        print("FAIL: rejection reason did not identify NVS layout")
        return 1
    print("NVS_NEGATIVE PASS old-64KiB rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
