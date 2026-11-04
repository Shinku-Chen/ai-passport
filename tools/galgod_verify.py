#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Structural verification of a GalGod pack (read-only).

Checks every cross-reference inside the pack, prints a summary, and exits
non-zero on the first structural error.  Used both while converting and as a
release gate; the firmware repeats the cheap range checks at load time.

  python tools/galgod_verify.py main/gal_data/gal_pack.bin [--expect-lines 5457]
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import deque

MAGIC = b"GALPK001"
(SEC_TEXT, SEC_NAME, SEC_EVENT, SEC_LINE, SEC_CHOICE, SEC_OPTION, SEC_ACTION,
 SEC_COND, SEC_IMAGE, SEC_CHAPTER, SEC_ENDING, SEC_GALLERY, SEC_META, SEC_TITLE,
 SEC_IMGDATA) = range(15)
(EV_LINE, EV_BG, EV_CG, EV_SPRITE, EV_CHOICE, EV_GOTO, EV_VAR, EV_CHAPTER,
 EV_ENDING, EV_END, EV_UNLOCK_END) = range(11)

EVENT_FMT = "<BBHHH"
EVENT_SIZE = struct.calcsize(EVENT_FMT)
LINE_SIZE = struct.calcsize("<IHH")
OPTION_SIZE = struct.calcsize("<HHHHBB")
IMAGE_SIZE = struct.calcsize("<HHIIIIIIII")


def fail(msg: str) -> None:
    print("FAIL: " + msg)
    sys.exit(1)


class Pack:
    def __init__(self, path: str) -> None:
        blob = open(path, "rb").read()
        if blob[:8] != MAGIC:
            fail("魔数不是 GALPK001")
        version, total, count = struct.unpack_from("<III", blob, 8)
        if version != 1:
            fail(f"版本不支持: {version}")
        if total != len(blob):
            fail(f"总长度不符: header={total} file={len(blob)}")
        self.blob = blob
        self.sec = {}
        for i in range(count):
            t, off, cnt, size = struct.unpack_from("<IIII", blob, 20 + 16 * i)
            if off + size > len(blob):
                fail(f"段 {t} 越界")
            self.sec[t] = (off, cnt, size)
        self.text = self._raw(SEC_TEXT)
        self.names = [struct.unpack_from("<IHH", self._raw(SEC_NAME), i * 8) for i in
                      range(self._count(SEC_NAME))]
        self.events = [struct.unpack_from(EVENT_FMT, self._raw(SEC_EVENT), i * EVENT_SIZE)
                       for i in range(self._count(SEC_EVENT))]
        self.lines = [struct.unpack_from("<IHH", self._raw(SEC_LINE), i * LINE_SIZE)
                      for i in range(self._count(SEC_LINE))]
        self.choices = [struct.unpack_from("<HBBH", self._raw(SEC_CHOICE), i * 6)
                        for i in range(self._count(SEC_CHOICE))]
        self.options = [struct.unpack_from("<HHHHBB", self._raw(SEC_OPTION), i * OPTION_SIZE)
                        for i in range(self._count(SEC_OPTION))]
        self.actions = [struct.unpack_from("<BBh", self._raw(SEC_ACTION), i * 4)
                        for i in range(self._count(SEC_ACTION))]
        self.conds = [struct.unpack_from("<BBh", self._raw(SEC_COND), i * 4)
                      for i in range(self._count(SEC_COND))]
        self.images = [struct.unpack_from("<HHIIIIIIII", self._raw(SEC_IMAGE), i * IMAGE_SIZE)
                       for i in range(self._count(SEC_IMAGE))]
        self.chapters = [struct.unpack_from("<HHHH", self._raw(SEC_CHAPTER), i * 8)
                         for i in range(self._count(SEC_CHAPTER))]
        self.endings = [struct.unpack_from("<HHHBB", self._raw(SEC_ENDING), i * 8)
                        for i in range(self._count(SEC_ENDING))]
        self.gallery = [struct.unpack_from("<HHHH", self._raw(SEC_GALLERY), i * 8)
                        for i in range(self._count(SEC_GALLERY))]
        self.meta = self._raw(SEC_META).decode("utf-8", "replace")
        self.title_image = struct.unpack_from("<HH", self._raw(SEC_TITLE), 0)[0]

    def _raw(self, sec: int) -> bytes:
        off, _cnt, size = self.sec[sec]
        return self.blob[off:off + size]

    def _count(self, sec: int) -> int:
        return self.sec[sec][1]

    def name(self, idx: int) -> str:
        if idx == 0xFFFF:
            return ""
        off, ln, _ = self.names[idx]
        return self.text[off:off + ln].decode("utf-8")

    def line_text(self, idx: int) -> str:
        off, ln, _ = self.lines[idx]
        return self.text[off:off + ln].decode("utf-8")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("--expect-lines", type=int, default=5457)
    ap.add_argument("--expect-choices", type=int, default=15)
    ap.add_argument("--expect-options", type=int, default=53)
    ap.add_argument("--expect-chapters", type=int, default=18)
    ap.add_argument("--expect-gallery", type=int, default=33)
    ap.add_argument("--expect-endings", type=int, default=4)
    args = ap.parse_args()
    p = Pack(args.pack)
    errs = []

    def chk(cond, msg):
        if not cond:
            errs.append(msg)

    chk(len(p.lines) == args.expect_lines, f"台词数 {len(p.lines)} != {args.expect_lines}")
    chk(len(p.choices) == args.expect_choices, f"选项组 {len(p.choices)}")
    chk(len(p.options) == args.expect_options, f"选项数 {len(p.options)}")
    chk(len(p.chapters) == args.expect_chapters, f"章节 {len(p.chapters)}")
    chk(len(p.gallery) == args.expect_gallery, f"鉴赏 {len(p.gallery)}")
    chk(len(p.endings) == args.expect_endings, f"结局 {len(p.endings)}")

    for i, (off, ln, spk) in enumerate(p.lines):
        if off + ln > len(p.text):
            errs.append(f"LINE {i} 文本越界")
        if spk != 0xFFFF and spk >= len(p.names):
            errs.append(f"LINE {i} 说话人越界")
    for i, (off, ln, _pad) in enumerate(p.names):
        if off + ln > len(p.text):
            errs.append(f"NAME {i} 越界")
    for i, (w, h, top, tl, bot, bl, mt, mtl, mb, mbl) in enumerate(p.images):
        for off, ln, what in ((top, tl, "top"), (bot, bl, "bot"), (mt, mtl, "mask_top"),
                              (mb, mbl, "mask_bot")):
            if ln and (off + ln > len(p.blob) or off < p.sec[SEC_IMGDATA][0]):
                errs.append(f"IMAGE {i} {what} 越界")
        if w == 0 or h == 0 or w > 320 or h > 240:
            errs.append(f"IMAGE {i} 尺寸异常 {w}x{h}")

    back_jumps = []
    for i, (kind, a, b, c, d) in enumerate(p.events):
        if kind == EV_LINE and b >= len(p.lines):
            errs.append(f"EVENT {i} 台词越界")
        elif kind in (EV_BG, EV_CG, EV_SPRITE) and b != 0xFFFF and b >= len(p.images):
            errs.append(f"EVENT {i} 图片越界")
        elif kind == EV_CHOICE and b >= len(p.choices):
            errs.append(f"EVENT {i} 选项越界")
        elif kind == EV_GOTO:
            if b >= len(p.events):
                errs.append(f"EVENT {i} 跳转越界 {b}")
            elif b < i:
                back_jumps.append((i, b))
            if c != 0xFFFF and c >= len(p.conds):
                errs.append(f"EVENT {i} 条件越界")
        elif kind == EV_CHAPTER and b >= len(p.chapters):
            errs.append(f"EVENT {i} 章节越界")
        elif kind in (EV_ENDING, EV_UNLOCK_END) and b >= len(p.endings):
            errs.append(f"EVENT {i} 结局越界")
        elif kind == EV_VAR and a >= 16:
            errs.append(f"EVENT {i} 变量越界")

    for i, (text, target, cond, af, ac, _pad) in enumerate(p.options):
        if text >= len(p.names):
            errs.append(f"OPTION {i} 文案越界")
        if target >= len(p.events):
            errs.append(f"OPTION {i} 目标越界 {target}")
        if cond != 0xFFFF and cond >= len(p.conds):
            errs.append(f"OPTION {i} 条件越界")
        if ac and af + ac > len(p.actions):
            errs.append(f"OPTION {i} 动作越界")

    for i, (first, cnt, _pad, prompt) in enumerate(p.choices):
        if first + cnt > len(p.options):
            errs.append(f"CHOICE {i} 选项越界")
        if prompt != 0xFFFF and prompt >= len(p.lines):
            errs.append(f"CHOICE {i} 提示台词越界")

    for i, ch in enumerate(p.chapters):
        name, first, count = ch[0], ch[1], ch[2]
        if first >= len(p.events) and len(p.events):
            errs.append(f"CHAPTER {i} 起始事件越界")
        if first + count > len(p.events):
            errs.append(f"CHAPTER {i} 范围越界")

    for i, g in enumerate(p.gallery):
        name, img, ev = g[0], g[1], g[2]
        if img >= len(p.images):
            errs.append(f"GALLERY {i} 图片越界")
        if ev >= len(p.events):
            errs.append(f"GALLERY {i} 解锁事件越界")

    # Reachability from event 0 (fallthrough + goto + option targets).
    seen = set()
    queue = deque([0])
    while queue:
        pc = queue.popleft()
        if pc in seen or pc >= len(p.events):
            continue
        seen.add(pc)
        kind, a, b, c, d = p.events[pc]
        if kind == EV_GOTO:
            queue.append(b)
            if c != 0xFFFF:
                queue.append(pc + 1)
        elif kind == EV_CHOICE:
            first, cnt, _pad, _ = p.choices[b]
            for opt in p.options[first:first + cnt]:
                queue.append(opt[1])
            queue.append(pc + 1)
        elif kind in (EV_ENDING, EV_END):
            continue
        else:
            queue.append(pc + 1)
    unreachable = [i for i in range(len(p.events)) if i not in seen]
    reachable_lines = {p.events[i][2] for i in seen if p.events[i][0] == EV_LINE}
    unreachable_lines = [i for i in range(len(p.lines)) if i not in reachable_lines]
    reachable_imgs = {p.events[i][2] for i in seen
                      if p.events[i][0] in (EV_BG, EV_CG, EV_SPRITE)
                      and p.events[i][2] != 0xFFFF}
    unused_images = [i for i in range(len(p.images)) if i not in reachable_imgs]

    print(f"pack         : {args.pack} ({len(p.blob)} B)")
    print(f"meta         : {json.loads(p.meta)['counts']}")
    print(f"events       : {len(p.events)}  lines {len(p.lines)}  choices {len(p.choices)} "
          f"options {len(p.options)}")
    print(f"chapters     : {len(p.chapters)}  endings {len(p.endings)}  gallery {len(p.gallery)}")
    print(f"images       : {len(p.images)}  image bytes {p.sec[SEC_IMGDATA][2]}  "
          f"title image {p.title_image}")
    print(f"back jumps   : {back_jumps}")
    print(f"unreachable  : events {len(unreachable)}  lines {len(unreachable_lines)}")
    print(f"unused images: {len(unused_images)}")
    chapter_events = [p.chapters[i][1] for i in range(len(p.chapters))]
    bad_chapters = [i for i, ev in enumerate(chapter_events) if ev not in seen]
    ending_events = [e[2] for e in p.endings]
    bad_endings = [i for i, ev in enumerate(ending_events) if ev not in seen]
    gallery_events = [g[2] for g in p.gallery]
    bad_gallery = [i for i, ev in enumerate(gallery_events) if ev not in seen]
    print(f"chapters reached : {len(p.chapters) - len(bad_chapters)}/{len(p.chapters)}"
          + (f"  missing {bad_chapters}" if bad_chapters else ""))
    print(f"endings reached  : {len(p.endings) - len(bad_endings)}/{len(p.endings)}"
          + (f"  missing {bad_endings}" if bad_endings else ""))
    print(f"gallery reached  : {len(p.gallery) - len(bad_gallery)}/{len(p.gallery)}"
          + (f"  missing {bad_gallery}" if bad_gallery else ""))
    for c in p.chapters[:3]:
        print("  chapter:", p.name(c[0]), c[1], c[2])
    for e in p.endings:
        print("  ending :", p.name(e[0]), "text=", repr(p.name(e[1])[:20]), "ev=", e[2], "kind=", e[3])
    if p.title_image >= len(p.images):
        errs.append("TITLE 图片越界")
    for i in bad_chapters:
        errs.append(f"CHAPTER {i} 不可达")
    for i in bad_endings:
        errs.append(f"ENDING {i} 不可达")
    for i in bad_gallery:
        errs.append(f"GALLERY {i} 不可达")
    if errs:
        print(f"FAIL: {len(errs)} error(s)")
        for e in errs[:20]:
            print("  -", e)
        sys.exit(1)
    print("PASS: structure ok")


if __name__ == "__main__":
    main()
