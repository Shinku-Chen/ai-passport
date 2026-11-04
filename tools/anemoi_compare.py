#!/usr/bin/env python3
"""严格全流程对照：源引擎转录 vs 生产 C 转录。

两侧都必须是同一种 tag 行格式（reviewer 的 oracle_walk.mjs 与 build 里的 probe_vm.c
输出的一致）：

    V|<cursor>|<vars>                变量抽查点（可选，缺省不参与）
    T|<scene>|<op>|<speaker>|<text>  一句台词/旁白（speaker 为 0 表示旁白）
    B|<scene>|<count>|<item>|<item>  委托板：count + 每项 "preview:scene:entry:lock"
    C|<scene>|<n>|<label>|<label>    选择：选项数与标签
    E|lines=<n> choices=<n> boards=<n>   终止（源 END）
    X|<chapter>                      本卷路线边界（C 侧截断点）

比较规则（任何一条不满足都非零退出）：
  * 长度必须相同（逐停点，不允许"取共同前缀也算过"）；
  * 每个停点的 kind 必须相同；
  * T: scene/op/speaker/text 全等；
  * B: scene/count 全等，且每项 preview/scene/entry/lock 全等；
  * C: scene/n 全等，且每个标签、每个 enabled 全等；
  * 终止：两侧都必须是 E，且 lines/choices/boards 计数相等；
  * 任一侧出现 X（边界/截断）而另一侧到 E：失败（"以边界当完成"被禁止）。

用法：
  python tools/anemoi_compare.py --oracle O.txt --c C.txt [--json out.json]
负例自检：--selftest 会构造截断/改字/错 END/板项/选项变异，必须全部被拒。
"""
from __future__ import annotations

import argparse
import json
import sys

TAGS = {"V", "VARS", "T", "B", "C", "E", "X"}


def parse(path: str):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for ln, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            tag = line.split("|", 1)[0]
            if tag not in TAGS:
                raise SystemExit("%s:%d 未知行标签 %r" % (path, ln, tag[:16]))
            rows.append((tag, line))
    return rows


def canon(tag: str, line: str):
    parts = line.split("|")
    if tag == "T":
        if len(parts) != 5:
            raise SystemExit("T 行字段数错误: %r" % line[:80])
        return ("T", parts[1], parts[2], parts[3], parts[4])
    if tag == "B":
        return ("B", parts[1], parts[2], tuple(parts[3:]))
    if tag == "C":
        return ("C", parts[1], parts[2], tuple(parts[3:]))
    if tag == "E":
        return ("E", parts[1])
    if tag == "X":
        return ("X", parts[1] if len(parts) > 1 else "")
    return ("V", line)


def compare(oracle_rows, c_rows, ignore_vars=True):
    diffs = []
    o = [canon(t, l) for t, l in oracle_rows if not (ignore_vars and t in ("V", "VARS"))]
    c = [canon(t, l) for t, l in c_rows if not (ignore_vars and t in ("V", "VARS"))]
    if len(o) != len(c):
        diffs.append({
            "kind": "length",
            "oracle": len(o),
            "c": len(c),
            "detail": "转录停点数不同（截断/漏段/多段都算失败）",
        })
    for i in range(min(len(o), len(c))):
        if o[i] != c[i]:
            diffs.append({
                "kind": "row",
                "index": i,
                "oracle": list(o[i]),
                "c": list(c[i]),
            })
            break
    if o and c:
        if o[-1][0] != "E" or c[-1][0] != "E":
            diffs.append({
                "kind": "termination",
                "oracle_last": list(o[-1]),
                "c_last": list(c[-1]),
                "detail": "两侧都必须以 E（源 END）结束；X 是本卷边界，不能当完成",
            })
        elif o[-1] != c[-1]:
            diffs.append({"kind": "end-counts", "oracle": list(o[-1]), "c": list(c[-1])})
    stats = {
        "oracle_stops": len(o),
        "c_stops": len(c),
        "oracle_T": sum(1 for r in o if r[0] == "T"),
        "c_T": sum(1 for r in c if r[0] == "T"),
        "oracle_B": sum(1 for r in o if r[0] == "B"),
        "c_B": sum(1 for r in c if r[0] == "B"),
        "oracle_C": sum(1 for r in o if r[0] == "C"),
        "c_C": sum(1 for r in c if r[0] == "C"),
        "oracle_end": list(o[-1]) if o and o[-1][0] == "E" else None,
        "c_end": list(c[-1]) if c and c[-1][0] == "E" else None,
    }
    return diffs, stats


def selftest(base_rows):
    """变异负例：每一种都必须被 compare() 判失败。"""
    cases = []

    def mutate(fn, name):
        rows = [(t, l) for t, l in base_rows]
        fn(rows)
        cases.append((name, rows))

    def trunc(rows):
        rows.pop(len(rows) // 2)

    def one_char(rows):
        for i, (t, l) in enumerate(rows):
            if t == "T":
                rows[i] = (t, l[:-1] + ("X" if l[-1] != "X" else "Y"))
                return
        raise SystemExit("selftest 需要至少一行 T")

    def wrong_end(rows):
        for i, (t, l) in enumerate(rows):
            if t == "E":
                rows[i] = (t, l.replace("lines=", "lines=1"))
                return
        raise SystemExit("selftest 需要 E 行")

    def board_item(rows):
        for i, (t, l) in enumerate(rows):
            if t == "B":
                rows[i] = (t, l + "|extra:0:0:0")
                return
        raise SystemExit("selftest 需要 B 行")

    def choice_label(rows):
        for i, (t, l) in enumerate(rows):
            if t == "C":
                parts = l.split("|")
                parts[-1] = parts[-1] + "!"
                rows[i] = (t, "|".join(parts))
                return
        raise SystemExit("selftest 需要 C 行")

    def to_boundary(rows):
        rows[-1] = ("X", "boundary")

    for fn, name in ((trunc, "truncated"), (one_char, "one-char"),
                     (wrong_end, "wrong-end"), (board_item, "board-item"),
                     (choice_label, "choice-label"), (to_boundary, "boundary-as-end")):
        mutate(fn, name)

    ok = True
    results = []
    for name, rows in cases:
        diffs, _ = compare(base_rows, rows)
        passed = bool(diffs)
        results.append({"case": name, "rejected": passed,
                        "first": diffs[0]["kind"] if diffs else None})
        ok = ok and passed
    return ok, results


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--oracle")
    ap.add_argument("--c")
    ap.add_argument("--json")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--keep-vars", action="store_true", help="把 V 行也纳入比较")
    args = ap.parse_args()

    if args.selftest:
        if not args.oracle:
            raise SystemExit("--selftest 需要 --oracle 提供基线转录")
        rows = parse(args.oracle)
        ok, results = selftest(rows)
        for r in results:
            print("%s | selftest | %s | first=%s" % (
                "PASS" if r["rejected"] else "FAIL", r["case"], r["first"]))
        print("SUMMARY | selftest | %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1

    if not args.oracle or not args.c:
        raise SystemExit("需要 --oracle 与 --c（或 --selftest --oracle）")
    o = parse(args.oracle)
    c = parse(args.c)
    diffs, stats = compare(o, c, ignore_vars=not args.keep_vars)
    result = {"equal": not diffs, "diffs": diffs, "stats": stats}
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(result, fh, ensure_ascii=False, indent=1)
    if not stats["oracle_end"] or not stats["c_end"]:
        print("FAIL | terminator | 任一侧没有 E（源 END） | oracle=%s c=%s" % (
            stats["oracle_end"], stats["c_end"]))
    elif diffs:
        d = diffs[0]
        print("FAIL | %s | %s" % (d["kind"], json.dumps(d, ensure_ascii=False)[:400]))
    else:
        print("PASS | transcript | stops=%d T=%d B=%d C=%d end=%s" % (
            stats["oracle_stops"], stats["oracle_T"], stats["oracle_B"],
            stats["oracle_C"], stats["oracle_end"]))
    print("STATS | oracle_stops=%d c_stops=%d oracle_T=%d c_T=%d oracle_B=%d c_B=%d oracle_C=%d c_C=%d"
          % (stats["oracle_stops"], stats["c_stops"], stats["oracle_T"], stats["c_T"],
             stats["oracle_B"], stats["c_B"], stats["oracle_C"], stats["c_C"]))
    return 0 if not diffs else 1


if __name__ == "__main__":
    sys.exit(main())
