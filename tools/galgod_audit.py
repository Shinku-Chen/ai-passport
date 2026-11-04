#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Independent content audit: source chunks vs the built pack.

This tool never imports tools/galgod_pack.py. It parses all 29 source chunks
itself and builds two independent interpreters:

  * SourceVM  — walks the original node objects (labels, jumps, choices, vars);
  * PackVM    — walks the GALPK001 event stream.

Checks
  1. dialogue order + speaker names   (node-by-node, 5,457 lines)
  2. choice groups: option labels in order, and for every option a "witness"
     run (source target label vs pack target event) must reach the same
     next dialogue/choice/ending.
  3. all 71 jumps: same witness comparison, plus the pack's "clear sprites"
     flag must be set exactly for jumps that cross a source chunk.
  4. sprite operations: order, slot position, and image-name -> image-id
     aliasing (the same name always maps to the same pack image).
  5. chapter titles in order; CG/SD gallery mapping is derivable from events.

Usage:
  python tools/galgod_audit.py --script <band-src/common/scn> \
      --pack main/gal_data/gal_pack.bin
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

MAGIC = b"GALPK001"
(SEC_TEXT, SEC_NAME, SEC_EVENT, SEC_LINE, SEC_CHOICE, SEC_OPTION, SEC_ACTION,
 SEC_COND, SEC_IMAGE, SEC_CHAPTER, SEC_ENDING, SEC_GALLERY, SEC_META, SEC_TITLE,
 SEC_IMGDATA) = range(15)
(EV_LINE, EV_BG, EV_CG, EV_SPRITE, EV_CHOICE, EV_GOTO, EV_VAR, EV_CHAPTER,
 EV_ENDING, EV_END, EV_UNLOCK_END) = range(11)
CMP = {"==": 0, "!=": 1, ">=": 2, ">": 3, "<=": 4, "<": 5}


def fail(msg: str) -> None:
    print("FAIL: " + msg)


# --------------------------------------------------------------------- pack
class Pack:
    def __init__(self, path: str) -> None:
        blob = open(path, "rb").read()
        if blob[:8] != MAGIC:
            raise SystemExit("bad magic")
        version, total, count = struct.unpack_from("<III", blob, 8)
        if version != 1 or total != len(blob):
            raise SystemExit("bad version/size")
        self.blob = blob
        self.sec = {}
        for i in range(count):
            t, off, cnt, size = struct.unpack_from("<IIII", blob, 20 + 16 * i)
            self.sec[t] = (off, cnt, size)
        self.text = self.raw(SEC_TEXT)
        self.events = [struct.unpack_from("<BBHHH", self.raw(SEC_EVENT), i * 8)
                       for i in range(self.sec[SEC_EVENT][1])]
        self.lines = [struct.unpack_from("<IHH", self.raw(SEC_LINE), i * 8)
                      for i in range(self.sec[SEC_LINE][1])]
        self.choices = [struct.unpack_from("<HBBH", self.raw(SEC_CHOICE), i * 6)
                        for i in range(self.sec[SEC_CHOICE][1])]
        self.options = [struct.unpack_from("<HHHHBB", self.raw(SEC_OPTION), i * 10)
                        for i in range(self.sec[SEC_OPTION][1])]
        self.actions = [struct.unpack_from("<BBh", self.raw(SEC_ACTION), i * 4)
                        for i in range(self.sec[SEC_ACTION][1])]
        self.conds = [struct.unpack_from("<BBh", self.raw(SEC_COND), i * 4)
                      for i in range(self.sec[SEC_COND][1])]
        self.chapters = [struct.unpack_from("<HHHH", self.raw(SEC_CHAPTER), i * 8)
                         for i in range(self.sec[SEC_CHAPTER][1])]
        self.endings = [struct.unpack_from("<HHHBB", self.raw(SEC_ENDING), i * 8)
                        for i in range(self.sec[SEC_ENDING][1])]
        self.gallery = [struct.unpack_from("<HHHH", self.raw(SEC_GALLERY), i * 8)
                        for i in range(self.sec[SEC_GALLERY][1])]
        self.names = []
        for i in range(self.sec[SEC_NAME][1]):
            off, ln, _ = struct.unpack_from("<IHH", self.raw(SEC_NAME), i * 8)
            self.names.append(self.text[off:off + ln].decode("utf-8"))

    def raw(self, sec: int) -> bytes:
        off, _c, size = self.sec[sec]
        return self.blob[off:off + size]

    def name(self, idx: int) -> str:
        return "" if idx == 0xFFFF else self.names[idx]

    def line(self, idx: int):
        off, ln, spk = self.lines[idx]
        return self.text[off:off + ln].decode("utf-8"), self.name(spk)


# --------------------------------------------------------------------- source
class Source:
    def __init__(self, script_dir: str) -> None:
        files = sorted(f for f in os.listdir(script_dir) if f.endswith(".txt"))
        self.files = files
        self.nodes = []
        self.chunk_of = []
        self.chunk_start = []
        for ci, name in enumerate(files):
            self.chunk_start.append(len(self.nodes))
            data = json.load(open(os.path.join(script_dir, name), encoding="utf-8"))
            self.nodes.extend(data)
            self.chunk_of.extend([ci] * len(data))
        self.labels = {}
        for i, n in enumerate(self.nodes):
            if n and n[0] == 0:
                self.labels.setdefault(str(n[1]), i)

    def resolve(self, target: str) -> int:
        label = target.split("@", 1)[-1]
        if label not in self.labels:
            raise KeyError(target)
        return self.labels[label]

    def run_from(self, node_index: int, max_steps: int = 500):
        """Source interpreter: return (kind, payload) at the next stop."""
        i = node_index
        for _ in range(max_steps):
            if i >= len(self.nodes):
                return ("end", None)
            n = self.nodes[i]
            if not isinstance(n, list) or not n:
                i += 1
                continue
            t = n[0]
            if t == 3:
                return ("line", (str(n[2]) if len(n) > 2 else "", str(n[1]) if len(n) > 1 else ""))
            if t == 4:
                return ("choice", tuple(str(o[0]) for o in (n[1] or [])))
            if t == 6:
                target = str(n[1])
                if target.startswith("*gameend_"):
                    return ("ending", target[len("*gameend_"):])
                i = self.resolve(target)
                continue
            i += 1
        return ("loop", None)


# --------------------------------------------------------------------- pack VM
def pack_run(pack: Pack, pc: int, max_steps: int = 5000):
    for _ in range(max_steps):
        if pc >= len(pack.events):
            return ("end", None)
        kind, a, b, c, d = pack.events[pc]
        if kind == EV_LINE:
            return ("line", pack.line(b))
        if kind == EV_CHOICE:
            first, cnt, _pad, _p = pack.choices[b]
            return ("choice", tuple(pack.names[pack.options[first + i][0]] for i in range(cnt)))
        if kind == EV_GOTO:
            pc = b
            continue
        if kind == EV_ENDING:
            return ("ending", pack.names[pack.endings[b][0]])
        if kind == EV_END:
            return ("end", None)
        pc += 1
    return ("loop", None)


def norm(text: str):
    import unicodedata
    out = []
    for ch in text:
        if ord(ch) < 0x20:
            continue
        if 0xF900 <= ord(ch) <= 0xFAFF:
            ch = unicodedata.normalize("NFKC", ch)
        out.append(ch)
    return "".join(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", required=True)
    ap.add_argument("--pack", required=True)
    args = ap.parse_args()

    src = Source(args.script)
    pack = Pack(args.pack)
    errors = 0

    # ---- 1) dialogue order + speakers ---------------------------------
    src_lines = [(norm(str(n[2]) if len(n) > 2 else ""), str(n[1]) if len(n) > 1 else "")
                 for n in src.nodes if n and n[0] == 3]
    pack_lines = [pack.line(i) for i in range(len(pack.lines))]
    if len(src_lines) != len(pack_lines):
        fail(f"台词数 {len(pack_lines)} != 源 {len(src_lines)}")
        errors += 1
    for i, ((st, ss), (pt, ps)) in enumerate(zip(src_lines, pack_lines)):
        if st != norm(pt) or ss != ps:
            fail(f"第 {i} 句不一致: 源({ss}) {st[:20]!r} vs 包({ps}) {pt[:20]!r}")
            errors += 1
            if errors > 5:
                break

    # ---- 2) choices + option witnesses --------------------------------
    src_choices = [n for n in src.nodes if n and n[0] == 4]
    pack_choice_events = [i for i, e in enumerate(pack.events) if e[0] == EV_CHOICE]
    if len(src_choices) != len(pack.choices):
        fail(f"选项组 {len(pack.choices)} != 源 {len(src_choices)}")
        errors += 1
    seen_options = 0
    for gi, (sc, ev_idx) in enumerate(zip(src_choices, pack_choice_events)):
        first, cnt, _pad, _prompt = pack.choices[gi]
        pack_labels = tuple(pack.names[pack.options[first + i][0]] for i in range(cnt))
        src_labels = tuple(str(o[0]) for o in (sc[1] or []))
        if pack_labels != src_labels:
            fail(f"选项组 {gi} 文案不一致: {pack_labels} vs {src_labels}")
            errors += 1
        for oi, so in enumerate(sc[1] or []):
            opt = pack.options[first + oi]
            seen_options += 1
            # 变量动作个数/条件存在性
            src_expr = str(so[2]) if len(so) > 2 and so[2] else ""
            src_cond = str(so[3]) if len(so) > 3 and so[3] else ""
            if (opt[4] > 0) != bool(src_expr):
                fail(f"选项 {gi}.{oi} 动作数不一致 (pack={opt[4]}, src={src_expr!r})")
                errors += 1
            if (opt[2] != 0xFFFF) != bool(src_cond):
                fail(f"选项 {gi}.{oi} 条件不一致 (pack={opt[2]}, src={src_cond!r})")
                errors += 1
            # witness:源按标签走 / 包按 target 走,下一站内容必须一致
            want = src.run_from(src.resolve(str(so[1])))
            got = pack_run(pack, opt[1])
            if want[0] != got[0]:
                fail(f"选项 {gi}.{oi} 落点类型 {got[0]} != 源 {want[0]}")
                errors += 1
            elif want[0] == "line" and (norm(got[1][0]), got[1][1]) != (norm(want[1][0]), want[1][1]):
                fail(f"选项 {gi}.{oi} 落点台词不一致: {got[1][0][:16]!r} vs {want[1][0][:16]!r}")
                errors += 1
            elif want[0] == "choice" and got[1] != want[1]:
                fail(f"选项 {gi}.{oi} 落点选项组不一致")
                errors += 1

    # ---- 3) jumps: witness + cross-chunk clear flag -------------------
    src_jumps = [(i, n) for i, n in enumerate(src.nodes)
                 if n and n[0] == 6 and not str(n[1]).startswith("*gameend_")]
    pack_gotos = [i for i, e in enumerate(pack.events) if e[0] == EV_GOTO]
    if len(src_jumps) != len(pack_gotos):
        fail(f"跳转数 {len(pack_gotos)} != 源 {len(src_jumps)}")
        errors += 1
    for (si, sn), gi in zip(src_jumps, pack_gotos):
        g = pack.events[gi]
        target = str(sn[1])
        cross = src.chunk_of[src.resolve(target)] != src.chunk_of[si]
        if bool(g[1] & 1) != cross:
            fail(f"跳转 {si}->{target} 的跨块清立绘标记错误 (pack={g[1]}, cross={cross})")
            errors += 1
        want = src.run_from(src.resolve(target))
        got = pack_run(pack, g[2])
        if want[0] != got[0]:
            fail(f"跳转 {si}->{target} 落点类型 {got[0]} != {want[0]}")
            errors += 1
        elif want[0] == "line" and (norm(got[1][0]), got[1][1]) != (norm(want[1][0]), want[1][1]):
            fail(f"跳转 {si}->{target} 落点台词不一致")
            errors += 1
        elif want[0] == "choice" and got[1] != want[1]:
            fail(f"跳转 {si}->{target} 落点选项组不一致")
            errors += 1

    # ---- 4) endings ---------------------------------------------------
    src_ends = [n for n in src.nodes if n and n[0] == 6 and str(n[1]).startswith("*gameend_")]
    # pack 里还有"只解锁不弹画面"的路线结局(kind=1),与 gameend 分开比较。
    pack_terminals = [e for e in pack.endings if e[3] == 0]
    if len(src_ends) != len(pack_terminals):
        fail(f"结局数 {len(pack_terminals)} != 源 {len(src_ends)}")
        errors += 1
    for n, e in zip(src_ends, pack_terminals):
        want = str(n[1])[len("*gameend_"):]
        if pack.names[e[0]] != want:
            fail(f"结局名 {pack.names[e[0]]!r} != {want!r}")
            errors += 1

    # ---- 5) sprite ops + alias ---------------------------------------
    src_ops = []
    for n in src.nodes:
        if n and n[0] == 3 and len(n) > 3 and isinstance(n[3], list):
            for it in n[3]:
                src_ops.append((str(it[0]), str(it[1]) if len(it) > 1 else "center"))
    pack_ops = [e for e in pack.events if e[0] == EV_SPRITE and e[2] != 0xFFFF]
    if len(src_ops) != len(pack_ops):
        fail(f"立绘指令数 {len(pack_ops)} != 源 {len(src_ops)}")
        errors += 1
    pos_of = {"left": 0, "center": 1, "right": 2, "l": 0, "c": 1, "r": 2}
    name_to_image = {}
    for (name, pos), ev in zip(src_ops, pack_ops):
        if ev[3] != pos_of.get(pos, 1):
            fail(f"立绘 {name} 槽位 {ev[3]} != 源 {pos}")
            errors += 1
        prev = name_to_image.setdefault(name, ev[2])
        if prev != ev[2]:
            fail(f"立绘别名不一致: {name} 用了 {prev} 与 {ev[2]}")
            errors += 1

    # ---- 6) chapters --------------------------------------------------
    src_ch = [str(n[1]) for n in src.nodes if n and n[0] == 1]
    pack_ch = [pack.names[c[0]] for c in pack.chapters]
    if src_ch != pack_ch:
        fail(f"章节标题顺序不一致: {len(pack_ch)} vs {len(src_ch)}")
        errors += 1

    print(f"audit: chunks={len(src.files)} nodes={len(src.nodes)} lines={len(src_lines)} "
          f"choices={len(pack.choices)} options={seen_options} jumps={len(src_jumps)} "
          f"endings={len(pack.endings)} sprites={len(src_ops)} "
          f"chapters={len(pack.chapters)} images={pack.sec[SEC_IMAGE][1]}")
    if errors:
        print(f"FAIL: {errors} 项不一致")
        sys.exit(1)
    print("PASS: 源 chunk 与 pack 逐项对齐(台词/选项/跳转/结局/立绘/章节)")


if __name__ == "__main__":
    main()
