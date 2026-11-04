#!/usr/bin/env python3
"""Diff the unmodified source engine trace against the production C VM trace.

This is the tracked regression for the map-preview/date/speaker/label fixes.
It imports the source snapshot only through tests/summer_source_trace.mjs and
the production C through tests/summer_c_trace.c; it does not read
build/review-model or any old report.

    SUMMER_SOURCE=<snapshot> python tests/summer_trace_compare.py \
        --pack main/summer_data/summer_pack.bin --c-trace <binary> \
        --log-dir build/logs/summer-trace
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NODE_TRACE = ROOT / "tests/summer_source_trace.mjs"

CASES = [
    ("story_ff", ["story", "first", "first", None]),
    ("story_ll", ["story", "last", "last", None]),
    ("story_fl", ["story", "first", "last", None]),
    ("story_lf", ["story", "last", "first", None]),
    ("alka", ["alka", "first", "first", None]),
    ("pocket", ["pocket", "first", "first", None]),
    ("chapter_137", ["chapter", "first", "first", "137"]),
    ("chapter_285", ["chapter", "first", "first", "285"]),
    ("chapter_287", ["chapter", "first", "first", "287"]),
    ("labels", ["labels", "first", "first", None]),
]


def run(cmd, env, log):
    proc = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                          errors="replace")
    log.write_text(proc.stdout, encoding="utf-8")
    return proc.returncode, proc.stdout


def normalize(text):
    return [line.rstrip("\r\n") for line in text.splitlines()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=os.environ.get("SUMMER_SOURCE", ""))
    ap.add_argument("--pack", default="main/summer_data/summer_pack.bin")
    ap.add_argument("--c-trace", required=True)
    ap.add_argument("--log-dir", default="build/logs/summer-trace")
    args = ap.parse_args()
    if not args.source or not Path(args.source).is_dir():
        print("SKIP: SUMMER_SOURCE is not a directory", file=sys.stderr)
        return 77
    source = Path(args.source)
    if not (source / "src/common/engine.js").exists():
        print("SKIP: source snapshot lacks src/common/engine.js", file=sys.stderr)
        return 77
    pack = Path(args.pack)
    if not pack.is_absolute():
        pack = ROOT / pack
    c_trace = Path(args.c_trace)
    if not c_trace.is_absolute():
        c_trace = ROOT / c_trace
    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["SUMMER_SOURCE"] = str(source)
    env["PYTHONIOENCODING"] = "utf-8"

    failures = 0
    for name, (mode, choice, map_mode, scene) in CASES:
        js_cmd = ["node", str(NODE_TRACE), "--mode", mode, "--choice", choice,
                  "--map", map_mode]
        if scene:
            js_cmd += ["--scene", scene]
        c_cmd = [str(c_trace), str(pack), mode, choice, map_mode]
        if scene:
            c_cmd.append(scene)
        js_rc, js_text = run(js_cmd, env, log_dir / f"{name}.js.log")
        c_rc, c_text = run(c_cmd, env, log_dir / f"{name}.c.log")
        js_lines = normalize(js_text)
        c_lines = normalize(c_text)
        if js_rc != 0 or c_rc != 0:
            print(f"FAIL {name}: node rc={js_rc} c rc={c_rc} "
                  f"logs={log_dir}", file=sys.stderr)
            failures += 1
            continue
        if js_lines == c_lines:
            print(f"PASS {name}: {len(js_lines)} lines identical")
            continue
        failures += 1
        first = 0
        while first < min(len(js_lines), len(c_lines)) and js_lines[first] == c_lines[first]:
            first += 1
        diff_path = log_dir / f"{name}.diff.txt"
        with diff_path.open("w", encoding="utf-8") as fh:
            fh.write(f"first mismatch line {first + 1}\n")
            for i in range(max(0, first - 3), min(len(js_lines), first + 8)):
                fh.write(f"JS {i+1}: {js_lines[i]}\n")
            for i in range(max(0, first - 3), min(len(c_lines), first + 8)):
                fh.write(f"C  {i+1}: {c_lines[i]}\n")
        print(f"FAIL {name}: first mismatch at {first + 1}; logs={log_dir}; "
              f"diff={diff_path}", file=sys.stderr)
        for i in range(max(0, first - 1), min(len(js_lines), first + 3)):
            print(f"  JS {i+1}: {js_lines[i]}", file=sys.stderr)
        for i in range(max(0, first - 1), min(len(c_lines), first + 3)):
            print(f"  C  {i+1}: {c_lines[i]}", file=sys.stderr)

    if failures:
        print(f"summer_trace_compare: FAIL ({failures} case(s))", file=sys.stderr)
        return 1
    print("summer_trace_compare: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
