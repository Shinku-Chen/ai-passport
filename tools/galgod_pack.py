#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build the GalGod resource pack for the AI Passport port.

Inputs are external source checkouts passed on the command line, so the repo
never stores machine-specific paths:

  --script <dir>   band port script dir (`src/common/scn`, chunk*.txt)
  --images <dir>   upstream asset dir (`src/common/img`, with b/ c/ s/)
  --out <file>     output pack (default main/gal_data/gal_pack.bin)
  --chars <file>   charset inventory for the font subsetter

Script model: chunk001..chunk029 concatenated; labels are `L<name>` and may be
qualified `028@L5158` (chunk + label). Labels are unique, so the prefix is
stripped and every jump resolves through one name -> node map.  Two jumps go
backwards (5214->L4801, 5402->L5091); they stay runtime gotos, never expanded.

Images all become one 320x240 canvas split into 320x150 (art area) + 320x90
(strip behind the text box):
  bg  672x480 -> cover-crop
  cg  854x480 -> fit width 320 (=320x180), centred on black (whole frame kept)
  ch  143x380 -> scale to screen height 240 (w=90); JPEG + 1bpp alpha mask
JPEGs are baseline 4:2:0 (TJpgDec decodes baseline only).

Pack layout (little endian), magic GALPK001:
  header : magic[8] version u32 total u32 section_count u32
  table  : {type u32, offset u32, count u32, size u32} * section_count
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import struct
import sys
from typing import Dict, List, Optional, Tuple

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("需要 Pillow: python -m pip install pillow")

MAGIC = b"GALPK001"
VERSION = 1

SEC_TEXT, SEC_NAME, SEC_EVENT, SEC_LINE, SEC_CHOICE, SEC_OPTION, SEC_ACTION, SEC_COND, \
    SEC_IMAGE, SEC_CHAPTER, SEC_ENDING, SEC_GALLERY, SEC_META, SEC_TITLE, \
    SEC_IMGDATA = range(15)

EV_LINE, EV_BG, EV_CG, EV_SPRITE, EV_CHOICE, EV_GOTO, EV_VAR, EV_CHAPTER, \
    EV_ENDING, EV_END, EV_UNLOCK_END = range(11)

POS_LEFT, POS_CENTER, POS_RIGHT = 0, 1, 2
POS_OF = {"left": POS_LEFT, "center": POS_CENTER, "right": POS_RIGHT,
          "c": POS_CENTER, "l": POS_LEFT, "r": POS_RIGHT}

OP_SET, OP_ADD, OP_SUB = 0, 1, 2
CMP_EQ, CMP_NE, CMP_GE, CMP_GT, CMP_LE, CMP_LT = range(6)

ART_W, ART_H = 320, 240
TOP_H, BOT_H = 150, 90
# 条带(文本框背后的 90 行)以 1/2 分辨率存:它被 55%~95% 不透明的面板盖住,
# 半分辨率在屏幕上看不出来,却省下 43KB DRAM 与约 250KB Flash。
STRIP_W, STRIP_H = 160, 45
BG_Q, CG_Q, SPR_Q = 80, 80, 82
SPRITE_SCREEN_H = 240
NONE16 = 0xFFFF
GOTO_CLEAR_SPRITES = 1 << 0

TERMINAL_ENDINGS = {
    "Bad End · 未能触碰的世界": "天选试落榜。那个世界，终究没能触碰到。",
    "—— 全剧终 ——": "月幕之下，故事走到了它的尽头。",
}
ROUTE_ENDINGS = {"结局 · 二人的世界": "二人的世界", "结局 · 回归日常": "回归日常"}


class Strings:
    """UTF-8 blob with byte-exact dedup."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._idx: Dict[bytes, int] = {}

    def add(self, text: str) -> Tuple[int, int]:
        raw = text.encode("utf-8")
        if not raw:
            return 0, 0
        off = self._idx.get(raw)
        if off is None:
            off = len(self._buf)
            self._buf.extend(raw)
            self._buf.append(0)   # NUL-terminated for C
            self._idx[raw] = off
        return off, len(raw)

    @property
    def data(self) -> bytes:
        return bytes(self._buf)


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def clean_text(text: str) -> str:
    import unicodedata
    out = []
    for ch in text:
        if ord(ch) < 0x20:
            continue
        if 0xF900 <= ord(ch) <= 0xFAFF:
            ch = unicodedata.normalize("NFKC", ch)
        out.append(ch)
    return "".join(out)


def jpeg_bytes(im: Image.Image, quality: int) -> bytes:
    buf = io.BytesIO()
    im.convert("RGB").save(buf, "JPEG", quality=quality, optimize=True,
                           progressive=False, subsampling=2)
    return buf.getvalue()


def cover(im: Image.Image, w: int, h: int) -> Image.Image:
    iw, ih = im.size
    scale = max(w / iw, h / ih)
    im2 = im.resize((max(1, round(iw * scale)), max(1, round(ih * scale))), Image.LANCZOS)
    x = (im2.width - w) // 2
    y = (im2.height - h) // 2
    return im2.crop((x, y, x + w, y + h))


def fit_width(im: Image.Image, canvas_w: int, canvas_h: int) -> Image.Image:
    iw, ih = im.size
    h = max(1, round(ih * (canvas_w / iw)))
    canvas = Image.new("RGB", (canvas_w, canvas_h), (0, 0, 0))
    if h <= canvas_h:
        im2 = im.resize((canvas_w, h), Image.LANCZOS)
        canvas.paste(im2, (0, (canvas_h - h) // 2))
        return canvas
    im2 = im.resize((canvas_w, h), Image.LANCZOS)
    y = (h - canvas_h) // 2
    return im2.crop((0, y, canvas_w, y + canvas_h))


def pack_mask(alpha: Image.Image) -> bytes:
    if alpha.mode != "1":
        alpha = alpha.point(lambda v: 255 if v > 127 else 0).convert("1")
    return alpha.tobytes()


def parse_actions(expr: str) -> List[Tuple[str, int, int]]:
    out: List[Tuple[str, int, int]] = []
    for part in str(expr).split(","):
        part = part.strip()
        if not part:
            continue
        if not part.startswith("f."):
            raise SystemExit(f"不支持的表达式: {part}")
        body = part[2:]
        if body.endswith("++"):
            out.append((body[:-2].strip(), OP_ADD, 1))
        elif body.endswith("--"):
            out.append((body[:-2].strip(), OP_SUB, 1))
        elif "+=" in body:
            name, val = body.split("+=", 1)
            out.append((name.strip(), OP_ADD, int(val.strip())))
        elif "-=" in body:
            name, val = body.split("-=", 1)
            out.append((name.strip(), OP_SUB, int(val.strip())))
        elif "=" in body:
            name, val = body.split("=", 1)
            out.append((name.strip(), OP_SET, int(val.strip())))
        else:
            raise SystemExit(f"不支持的表达式: {part}")
    return out


def parse_condition(cond: str) -> Tuple[str, int, int]:
    body = str(cond).strip()
    if not body.startswith("f."):
        raise SystemExit(f"不支持的条件: {body}")
    body = body[2:]
    for token, op in ((">=", CMP_GE), ("<=", CMP_LE), ("==", CMP_EQ),
                      ("!=", CMP_NE), (">", CMP_GT), ("<", CMP_LT)):
        if token in body:
            name, val = body.split(token, 1)
            return name.strip(), op, int(val.strip())
    raise SystemExit(f"不支持的条件: {cond}")


def gallery_label(name: str) -> str:
    if name.startswith("cg_") and name[3:].isdigit():
        return f"CG {int(name[3:]):02d}"
    if name.startswith("sd_") and name[3:].isdigit():
        return f"SD {int(name[3:]):02d}"
    if name == "cg_noctella_descend":
        return "诺提拉 · 降临"
    if name == "cg_noctella_descend_full":
        return "诺提拉 · 降临（全）"
    return name


class Packer:
    def __init__(self, script_dir: str, image_dir: str, source_label: str) -> None:
        self.imgdir = image_dir
        self.source_label = source_label
        files = sorted(f for f in os.listdir(script_dir) if f.endswith(".txt"))
        if not files:
            raise SystemExit(f"剧本目录里没有 .txt: {script_dir}")
        self.chunk_files = files
        self.nodes: List[list] = []
        self.chunk_start: List[int] = []
        for name in files:
            self.chunk_start.append(len(self.nodes))
            data = json.load(open(os.path.join(script_dir, name), encoding="utf-8"))
            if not isinstance(data, list):
                raise SystemExit(f"{name} 不是数组")
            self.nodes.extend(data)
        self.labels: Dict[str, int] = {}
        for i, node in enumerate(self.nodes):
            if node and node[0] == 0 and len(node) > 1:
                self.labels.setdefault(str(node[1]), i)

        self.strings = Strings()
        self.names: List[Tuple[int, int]] = []
        self.name_index: Dict[str, int] = {}
        self.events: List[List[int]] = []
        self.lines: List[Tuple[int, int, int]] = []
        self.choices: List[Tuple[int, int, int]] = []
        self.options: List[List[int]] = []
        self.actions: List[Tuple[int, int, int]] = []
        self.conds: List[Tuple[int, int, int]] = []
        self.chapters: List[List[int]] = []
        self.endings: List[List[int]] = []
        self.gallery: List[Tuple[int, int, int]] = []
        self.images: List[dict] = []
        self.image_index: Dict[str, int] = {}
        self.image_md5: Dict[str, int] = {}      # 文件内容 -> image id(无损去重)
        self.vars: Dict[str, int] = {}
        self.var_names: List[str] = []
        self.stats = {"bg": 0, "cg": 0, "sprite": 0, "lines": 0, "choices": 0,
                      "options": 0, "chapters": 0, "endings": 0, "jumps": 0,
                      "cond_jumps": 0, "back_jumps": 0, "cross_chunk_jumps": 0}
        self.pending_jumps: List[Tuple[int, str]] = []
        self.pending_options: List[Tuple[int, str]] = []
        self.title_image = 0

    # -- small helpers ---------------------------------------------------
    def name_id(self, text: str) -> int:
        if text in self.name_index:
            return self.name_index[text]
        off, ln = self.strings.add(text)
        self.names.append((off, ln))
        idx = len(self.names) - 1
        self.name_index[text] = idx
        return idx

    def var_id(self, name: str) -> int:
        if name not in self.vars:
            if len(self.vars) >= 16:
                raise SystemExit("变量超过 16 个")
            self.vars[name] = len(self.var_names)
            self.var_names.append(name)
        return self.vars[name]

    def add_cond(self, cond: str) -> int:
        if not cond:
            return NONE16
        name, op, val = parse_condition(cond)
        self.conds.append((self.var_id(name), op, val))
        return len(self.conds) - 1

    def add_actions(self, expr: str) -> Tuple[int, int]:
        acts = parse_actions(expr)
        if not acts:
            return NONE16, 0
        first = len(self.actions)
        for name, op, val in acts:
            self.actions.append((self.var_id(name), op, val))
        return first, len(acts)

    def line_id(self, speaker: str, text: str) -> int:
        off, ln = self.strings.add(clean_text(text))
        self.lines.append((off, ln, self.name_id(speaker) if speaker else NONE16))
        return len(self.lines) - 1

    def chunk_of(self, node_index: int) -> int:
        lo, hi = 0, len(self.chunk_start) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.chunk_start[mid] <= node_index:
                lo = mid
            else:
                hi = mid - 1
        return lo

    def ending_id(self, raw: str) -> int:
        name = raw[len("*gameend_"):]
        nid = self.name_id(name)
        for i, rec in enumerate(self.endings):
            if rec[0] == nid:
                return i
        tid = self.name_id(TERMINAL_ENDINGS.get(name, ""))
        self.endings.append([nid, tid, len(self.events), 0])
        self.stats["endings"] += 1
        return len(self.endings) - 1

    # -- images ----------------------------------------------------------
    def image(self, key: str, path: str, kind: str) -> int:
        if key in self.image_index:
            return self.image_index[key]
        if not os.path.exists(path):
            raise SystemExit(f"找不到素材: {path}")
        digest = hashlib.md5(open(path, "rb").read()).hexdigest()
        if digest in self.image_md5:
            # 同一份像素被两个名字引用(例如 heavenly_altar / _focus):共用一条记录。
            self.image_index[key] = self.image_md5[digest]
            self.stats["dedup"] = self.stats.get("dedup", 0) + 1
            return self.image_md5[digest]
        with Image.open(path) as im0:
            im0.load()
            if kind == "sprite":
                rec = self._sprite(im0)
            else:
                canvas = cover(im0.convert("RGB"), ART_W, ART_H) if kind == "bg" \
                    else fit_width(im0.convert("RGB"), ART_W, ART_H)
                rec = self._scene(canvas, BG_Q if kind == "bg" else CG_Q)
        idx = len(self.images)
        self.images.append(rec)
        self.image_index[key] = idx
        self.image_md5[digest] = idx
        self.stats[kind] += 1
        return idx

    def _scene(self, canvas: Image.Image, quality: int) -> dict:
        strip = canvas.crop((0, TOP_H, ART_W, ART_H)).resize((STRIP_W, STRIP_H), Image.LANCZOS)
        return {"w": ART_W, "h": ART_H, "kind": "scene",
                "top": jpeg_bytes(canvas.crop((0, 0, ART_W, TOP_H)), quality),
                "bot": jpeg_bytes(strip, 75),
                "mask_top": b"", "mask_bot": b""}

    def _sprite(self, im0: Image.Image) -> dict:
        rgba = im0.convert("RGBA")
        w, h = rgba.size
        scale = SPRITE_SCREEN_H / h
        w2, h2 = max(1, round(w * scale)), min(ART_H, max(1, round(h * scale)))
        resized = rgba.resize((w2, h2), Image.LANCZOS)
        alpha = resized.getchannel("A")
        top = resized.crop((0, 0, w2, min(h2, TOP_H)))
        rec = {"w": w2, "h": h2, "kind": "sprite",
               "top": jpeg_bytes(top, SPR_Q), "bot": b"",
               "mask_top": pack_mask(alpha.crop((0, 0, w2, min(h2, TOP_H)))),
               "mask_bot": b""}
        if h2 > TOP_H:
            rec["bot"] = jpeg_bytes(resized.crop((0, TOP_H, w2, h2)), SPR_Q)
            rec["mask_bot"] = pack_mask(alpha.crop((0, TOP_H, w2, h2)))
        return rec

    # -- walk ------------------------------------------------------------
    def build(self) -> None:
        nodes = self.nodes
        node_event = [0] * len(nodes)
        chapter_of_node: Dict[int, int] = {}
        for i, node in enumerate(nodes):
            if node and node[0] == 1:
                chapter_of_node[i] = len(self.chapters)
                self.chapters.append([self.name_id(clean_text(str(node[1]))), 0, 0])
                self.stats["chapters"] += 1

        # Some chapter titles sit immediately before a label that jumps target
        # (the band chunking leaves the title unreachable).  Emit those at the
        # label instead, so the chapter card shows on the route it belongs to.
        defer_chapter = {}
        skip_chapter = set()
        for i, node in enumerate(nodes):
            if node and node[0] == 1 and i + 1 < len(nodes) and nodes[i + 1] and nodes[i + 1][0] == 0:
                defer_chapter[i + 1] = chapter_of_node[i]
                skip_chapter.add(i)

        chunk_starts = set(self.chunk_start)
        last_line = NONE16

        def clear_sprites() -> None:
            for slot in range(3):
                self.events.append([EV_SPRITE, slot, NONE16, 0, 0])

        for i, node in enumerate(nodes):
            node_event[i] = len(self.events)
            if not node:
                continue
            t = node[0]
            if t == 0:
                if i in defer_chapter:
                    cid = defer_chapter[i]
                    self.chapters[cid][1] = len(self.events)
                    self.events.append([EV_CHAPTER, 0, cid, 0, 0])
                    clear_sprites()
                    route = ROUTE_ENDINGS.get(self.strings.data[
                        self.names[self.chapters[cid][0]][0]:
                        self.names[self.chapters[cid][0]][0] + self.names[self.chapters[cid][0]][1]
                    ].decode("utf-8"))
                    if route:
                        self.endings.append([self.name_id(route), NONE16, len(self.events), 1])
                        self.stats["endings"] += 1
                        self.events.append([EV_UNLOCK_END, 0, len(self.endings) - 1, 0, 0])
                continue
            if i in chunk_starts and i != 0:
                clear_sprites()          # load() resets characters on chunk entry
                node_event[i] = len(self.events)
            if t == 1:
                if i in skip_chapter:
                    continue
                cid = chapter_of_node[i]
                self.chapters[cid][1] = len(self.events)
                self.events.append([EV_CHAPTER, 0, cid, 0, 0])
                clear_sprites()
                route = ROUTE_ENDINGS.get(str(node[1]))
                if route:
                    self.endings.append([self.name_id(route), NONE16, len(self.events), 1])
                    self.stats["endings"] += 1
                    self.events.append([EV_UNLOCK_END, 0, len(self.endings) - 1, 0, 0])
            elif t == 2:
                img = self.image("bg/" + str(node[1]), os.path.join(self.imgdir, "b", f"{node[1]}.png"), "bg")
                self.events.append([EV_BG, 0, img, 0, 0])
            elif t == 3:
                sprites = node[3] if len(node) > 3 and isinstance(node[3], list) else []
                for item in sprites:
                    if not isinstance(item, list) or not item:
                        continue
                    name = str(item[0])
                    pos = POS_OF.get(str(item[1]).lower() if len(item) > 1 else "center", POS_CENTER)
                    img = self.image("ch/" + name,
                                     os.path.join(self.imgdir, "s", name.split("_")[-1] + ".png"), "sprite")
                    self.events.append([EV_SPRITE, pos, img, pos, 0])
                last_line = self.line_id(str(node[1]) if len(node) > 1 else "",
                                         str(node[2]) if len(node) > 2 else "")
                self.events.append([EV_LINE, 0, last_line, 0, 0])
                self.stats["lines"] += 1
            elif t == 4:
                opts = node[1] if len(node) > 1 else []
                first = len(self.options)
                for o in opts:
                    label = str(o[0])
                    target = str(o[1]) if len(o) > 1 else ""
                    expr = str(o[2]) if len(o) > 2 and o[2] else ""
                    cond = str(o[3]) if len(o) > 3 and o[3] else ""
                    a_first, a_cnt = self.add_actions(expr)
                    self.options.append([self.name_id(label), NONE16, self.add_cond(cond),
                                         a_first, a_cnt])
                    self.pending_options.append((len(self.options) - 1, target, i))
                self.choices.append((first, len(opts), last_line))
                self.events.append([EV_CHOICE, 0, len(self.choices) - 1, 0, 0])
                self.stats["choices"] += 1
                self.stats["options"] += len(opts)
            elif t == 5:
                name = str(node[1])
                img = self.image("cg/" + name, os.path.join(self.imgdir, "c", f"{name}.png"), "cg")
                self.events.append([EV_CG, 0, img, 0, 0])
                clear_sprites()
            elif t == 6:
                target = str(node[1])
                cond = str(node[2]) if len(node) > 2 and node[2] else ""
                if target.startswith("*gameend_"):
                    self.events.append([EV_ENDING, 0, self.ending_id(target), 0, 0])
                    continue
                self.events.append([EV_GOTO, 0, 0, self.add_cond(cond), 0])
                self.pending_jumps.append((len(self.events) - 1, target, i))
                self.stats["jumps"] += 1
                if cond:
                    self.stats["cond_jumps"] += 1
            elif t == 7:
                raise SystemExit(f"未支持的立绘指令节点: {node}")
            elif t in (8, 9):
                self.stats[f"node_type_{t}"] = self.stats.get(f"node_type_{t}", 0) + 1
            elif t == 10:
                name, op, val = parse_actions(str(node[1]))[0]
                self.events.append([EV_VAR, self.var_id(name), op, val & 0xFFFF, 0])
            else:
                raise SystemExit(f"未知节点类型 {t}: {node}")

        # Resolve jump / option targets now that the event map is stable.
        def resolve(target: str, from_node: int) -> Tuple[int, bool]:
            label = target.split("@", 1)[-1]
            if label not in self.labels:
                raise SystemExit(f"跳转目标找不到: {target}")
            dst_node = self.labels[label]
            cross = self.chunk_of(dst_node) != self.chunk_of(from_node)
            return node_event[dst_node], cross

        for ev_idx, target, from_node in self.pending_jumps:
            dst, cross = resolve(target, from_node)
            self.events[ev_idx][1] = GOTO_CLEAR_SPRITES if cross else 0
            self.events[ev_idx][2] = dst
            if dst < ev_idx:
                self.stats["back_jumps"] += 1
            if cross:
                self.stats["cross_chunk_jumps"] += 1
        for opt_idx, target, from_node in self.pending_options:
            dst, cross = resolve(target, from_node)
            self.options[opt_idx][1] = dst
            if cross:
                self.stats["cross_chunk_jumps"] += 1

        for cid in range(len(self.chapters)):
            first = self.chapters[cid][1]
            last = len(self.events)
            for other in self.chapters[cid + 1:]:
                if other[1]:
                    last = other[1]
                    break
            self.chapters[cid][2] = max(0, last - first)

        self.events.append([EV_END, 0, 0, 0, 0])
        self._add_unreferenced_assets()
        self._build_gallery()
        # Title backdrop: the moon sky, else the first background.
        for name in ("moon", "moon_all"):
            if "bg/" + name in self.image_index:
                self.title_image = self.image_index["bg/" + name]
                break
        else:
            self.title_image = self.image_index.get("bg/moon", 0) if self.image_index else 0

    def _add_unreferenced_assets(self) -> None:
        """Keep the whole asset set in the pack, referenced or not (completeness)."""
        bdir = os.path.join(self.imgdir, "b")
        for name in sorted(os.listdir(bdir)):
            if name.endswith("_r.png") or not name.endswith(".png"):
                continue
            key = "bg/" + name[:-4]
            if key not in self.image_index:
                self.image(key, os.path.join(bdir, name), "bg")
        cdir = os.path.join(self.imgdir, "c")
        for name in sorted(os.listdir(cdir)):
            if not name.endswith(".png") or name.endswith(("_l.png", "_r.png")):
                continue
            key = "cg/" + name[:-4]
            if key not in self.image_index:
                self.image(key, os.path.join(cdir, name), "cg")
        sdir = os.path.join(self.imgdir, "s")
        for name in sorted(os.listdir(sdir)):
            if not name.endswith(".png"):
                continue
            key = "ch/sp_" + name[:-4]
            if key not in self.image_index:
                self.image(key, os.path.join(sdir, name), "sprite")

    def _build_gallery(self) -> None:
        seen: Dict[int, int] = {}
        for ev_idx, ev in enumerate(self.events):
            if ev[0] != EV_CG or ev[2] in seen:
                continue
            seen[ev[2]] = ev_idx
            key = next(k for k, v in self.image_index.items() if v == ev[2])
            self.gallery.append((self.name_id(gallery_label(key.split("/", 1)[1])), ev[2], ev_idx))
        self.stats["gallery"] = len(self.gallery)

    # -- serialise -------------------------------------------------------
    def write(self, out_path: str, chars_path: str) -> dict:
        text = self.strings.data
        img_blob = bytearray()
        img_records = []
        for rec in self.images:
            top_off = len(img_blob); img_blob += rec["top"]
            bot_off = len(img_blob); img_blob += rec["bot"]
            mt_off = len(img_blob); img_blob += rec["mask_top"]
            mb_off = len(img_blob); img_blob += rec["mask_bot"]
            img_records.append((rec["w"], rec["h"], top_off, len(rec["top"]),
                                bot_off, len(rec["bot"]), mt_off, len(rec["mask_top"]),
                                mb_off, len(rec["mask_bot"])))
        img_table = b"".join(struct.pack("<HHIIIIIIII", *r) for r in img_records)

        payloads = [
            (SEC_TEXT, len(text), text),
            (SEC_NAME, len(self.names), b"".join(struct.pack("<IHH", o, l, 0) for o, l in self.names)),
            (SEC_EVENT, len(self.events), b"".join(
                struct.pack("<BBHHH", *e) for e in self.events)),
            (SEC_LINE, len(self.lines), b"".join(
                struct.pack("<IHH", *r) for r in self.lines)),
            (SEC_CHOICE, len(self.choices), b"".join(
                struct.pack("<HBBH", first, cnt, 0, prompt) for first, cnt, prompt in self.choices)),
            (SEC_OPTION, len(self.options), b"".join(
                struct.pack("<HHHHBB", t, tgt, cond, af, ac, 0)
                for t, tgt, cond, af, ac in self.options)),
            (SEC_ACTION, len(self.actions), b"".join(
                struct.pack("<BBh", *r) for r in self.actions)),
            (SEC_COND, len(self.conds), b"".join(
                struct.pack("<BBh", *r) for r in self.conds)),
            (SEC_IMAGE, len(img_records), img_table),
            (SEC_CHAPTER, len(self.chapters), b"".join(
                struct.pack("<HHHH", n, first, cnt, 0) for n, first, cnt in self.chapters)),
            (SEC_ENDING, len(self.endings), b"".join(
                struct.pack("<HHHBB", n, t, ev, kind, 0) for n, t, ev, kind in self.endings)),
            (SEC_GALLERY, len(self.gallery), b"".join(
                struct.pack("<HHHH", n, img, ev, 0) for n, img, ev in self.gallery)),
            (SEC_META, 0, self._meta()),
            (SEC_TITLE, 1, struct.pack("<HH", self.title_image, 0)),
            (SEC_IMGDATA, len(img_blob), bytes(img_blob)),
        ]
        header_len = 20 + 16 * len(payloads)

        def aligned(n: int) -> int:
            return (n + 3) & ~3

        img_data_off = aligned(header_len + sum(aligned(len(p)) for t, _c, p in payloads
                                                if t != SEC_IMGDATA))
        # Patch absolute image offsets now that the data section position is known.
        patched = bytearray()
        for (w, h, top, top_len, bot, bot_len, mt, mt_len, mb, mb_len) in img_records:
            patched += struct.pack("<HHIIIIIIII", w, h,
                                   img_data_off + top, top_len,
                                   img_data_off + bot, bot_len,
                                   img_data_off + mt, mt_len,
                                   img_data_off + mb, mb_len)
        payloads = [(t, c, bytes(patched) if t == SEC_IMAGE else p) for t, c, p in payloads]

        offset = header_len
        table = []
        body = bytearray()
        section_sizes = {}
        for sec_type, count, payload in payloads:
            pad = aligned(len(body) + header_len) - (len(body) + header_len)
            body += bytes(pad)
            offset = header_len + len(body)
            table.append(struct.pack("<IIII", sec_type, offset, count, len(payload)))
            body += payload
            section_sizes[sec_type] = len(payload)
        blob = bytearray(MAGIC + struct.pack("<III", VERSION, 0, len(payloads)) +
                         b"".join(table) + bytes(body))
        struct.pack_into("<I", blob, 12, len(blob))
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        open(out_path, "wb").write(blob)

        chars = sorted({c for c in text.decode("utf-8") if c not in "\r\n\t"})
        open(chars_path, "w", encoding="utf-8").write("".join(chars))
        return {"bytes": len(blob), "sections": section_sizes, "chars": len(chars),
                "images": len(self.images),
                "image_bytes": len(img_blob),
                "sha256": hashlib.sha256(blob).hexdigest()}

    def _meta(self) -> bytes:
        meta = {
            "generator": "galgod_pack/1",
            "source": self.source_label,
            "chunks": len(self.chunk_files),
            "nodes": len(self.nodes),
            "vars": self.var_names,
            "counts": self.stats,
            "text_bytes": len(self.strings.data),
        }
        return json.dumps(meta, ensure_ascii=False, indent=1).encode("utf-8")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", required=True)
    ap.add_argument("--images", required=True)
    ap.add_argument("--out", default=os.path.join("main", "gal_data", "gal_pack.bin"))
    ap.add_argument("--chars", default=os.path.join("main", "gal_data", "chars.txt"))
    ap.add_argument("--manifest", default="")
    args = ap.parse_args()
    packer = Packer(args.script, args.images, os.path.basename(os.path.dirname(args.script)))
    packer.build()
    info = packer.write(args.out, args.chars)
    if args.manifest:
        json.dump({"info": info, "stats": packer.stats, "vars": packer.var_names,
                   "chapters": [packer.strings.data[packer.names[n[0]][0]:
                                                packer.names[n[0]][0] + packer.names[n[0]][1]
                                                ].decode("utf-8") for n in packer.chapters],
                   "gallery": len(packer.gallery), "endings": len(packer.endings)},
                  open(args.manifest, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    log(f"pack: {args.out} {info['bytes']} B, images {info['images']} "
        f"({info['image_bytes']} B), chars {info['chars']}, sha256 {info['sha256'][:16]}")
    log("stats: " + json.dumps(packer.stats, ensure_ascii=False))


if __name__ == "__main__":
    main()
