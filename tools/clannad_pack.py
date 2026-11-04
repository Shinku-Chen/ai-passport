#!/usr/bin/env python3
"""Build the CLANNAD reader content pack from the RW5 source snapshot.

The pack is a single little-endian binary embedded in the firmware image. It
contains the complete current-source story (all 411 chunks of 212 scenes), the
scene/entry/chapter tables, all 309 choices, every referenced background and
character image, the 68 CG gallery groups and the title art.

Image policy (see docs/reference/shinku-chen/clannad/content.md):
  * story backgrounds  224x168 JPEG q40 (4:3 kept; the art window is 224x168)
  * gallery CG         320x240 JPEG q30 (full-screen CG viewer, no crop)
  * character sprites  alpha-trimmed, height 160, JPEG q55 + 1bpp mask
  * title art          320x240 crop of the source 432x514 title painting
  * logo/subtitle      alpha-trimmed JPEG q75 + 1bpp mask

Masks are deduplicated by exact content: 386 sprite files share ~107 masks
because expression variants keep the same silhouette.

Usage:
  python3 tools/clannad_pack.py --source <clannad-rw5> \
      --out assets/clannad/clannad_pack.bin --report build/clannad_pack_report.json
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import struct
import sys
import zlib
from collections import OrderedDict

from PIL import Image

BS = chr(92)
MAGIC = b"CLANPK01"
VERSION = 1
BLOCK = 512
ENTRY_KEEP = 33          # source uses entry indices 0..32

SEC_TEXT, SEC_CATALOG, SEC_ENTRY, SEC_ASSETNAME, SEC_ASSET, SEC_GALLERY, \
    SEC_BLOB, SEC_OPS, SEC_META, SEC_ALIAS, SEC_SPEAKER = range(11)
SEC_NAMES = ["TEXT", "CATALOG", "ENTRY", "ASSETNAME", "ASSET", "GALLERY",
             "BLOB", "OPS", "META", "ALIAS", "SPEAKER"]

KIND_BG, KIND_SPRITE, KIND_GALLERY, KIND_TITLE, KIND_UI = range(5)

OP_TEXT, OP_CHOICE, OP_DYNAMIC_CHOICE, OP_SET, OP_NATIVE, OP_GOTO, \
    OP_IF_FALSE, OP_IF_TRUE, OP_CALL, OP_JUMP_SCENE, OP_CALL_SCENE, \
    OP_RETURN, OP_END, OP_BACKGROUND, OP_BODIES, OP_BODY_ADD = range(16)

EXPR_CONST0, EXPR_CONST1, EXPR_I8, EXPR_I32, EXPR_STR_REF = range(5)
EXPR_VAR_STATIC, EXPR_VAR_NOINDEX, EXPR_VAR_DYNAMIC = range(0x10, 0x13)
EXPR_BINOP, EXPR_UNOP, EXPR_CALL, EXPR_AND, EXPR_OR = range(0x20, 0x25)

BINOPS = {"+": 0, "-": 1, "*": 2, "/": 3, "%": 4, "==": 5, "!=": 6,
          "<": 7, "<=": 8, ">": 9, ">=": 10, "&": 11, "|": 12, "^": 13,
          "<<": 14, ">>": 15}
UNOPS = {"-": 0, "+": 1, "!": 2, "~": 3}
VAR_KINDS = {
    "f": 0, "g": 1, "s": 2, "a": 3, "z": 4, "cur_call.l": 5,
    "cur_call.k": 6, "counter": 7, "front.mwnd": 8, "c": 9, "pcmch": 10,
    "cur_call.$psc_no": 11, "cur_call.$obj_no": 12,
    "cur_call.$obj_no1": 13, "cur_call.$obj_no2": 14,
}
NATIVES = {
    "counter.start": 0, "counter.get": 1, "global.random": 2,
    "global.get_scene_name": 3, "global.get_line_no": 4,
    "$filename_translate": 5, "global.namae": 6, "check": 7,
    "timewait_key": 8, "$disp_dialog": 9, "auto_mode_onoff": 10,
    "anime_type": 11, "anime_time": 12, "no_wipe": 13, "koe_dont_stop": 14,
    "filter_color": 15, "$search_chara_center": 16, "str.search": 17,
    "str.len": 18, "str.mid": 19, "str.left": 20, "str.tonum": 21,
}


def balanced_json(text: str, marker: str):
    start = text.index(marker) + len(marker)
    while text[start] not in "{[":
        start += 1
    depth = 0
    instr = False
    esc = False
    for j in range(start, len(text)):
        c = text[j]
        if instr:
            if esc:
                esc = False
            elif c == BS:
                esc = True
            elif c == '"':
                instr = False
            continue
        if c == '"':
            instr = True
        elif c in "{[":
            depth += 1
        elif c in "}]":
            depth -= 1
            if depth == 0:
                return json.loads(text[start:j + 1])
    raise ValueError("unbalanced JSON at " + marker)


class TextPool:
    def __init__(self):
        self.buf = bytearray()
        self.index = {}

    def add(self, text: str) -> int:
        if text in self.index:
            return self.index[text]
        off = len(self.buf)
        self.buf += text.encode("utf-8")
        self.buf.append(0)   # NUL-terminated so the firmware can print it
        self.index[text] = off
        return off

    def walk(self) -> bytes:
        return bytes(self.buf)


class Packer:
    def __init__(self, source: str, bg_quality: int, gallery_quality: int,
                 sprite_quality: int, sprite_height: int):
        self.root = source
        self.common = os.path.join(source, "src/common")
        self.catalog = balanced_json(
            open(os.path.join(self.common, "catalog.js"), encoding="utf-8").read(),
            "export const catalog=")
        self.asset_files = {str(k).lower(): v for k, v in balanced_json(
            open(os.path.join(self.common, "assets.js"), encoding="utf-8").read(),
            "const assets=").items()}
        self.gallery = json.load(open(os.path.join(self.common, "gallery.txt"),
                                     encoding="utf-8"))
        self.scenes = {int(k): v for k, v in self.catalog["scenes"].items()}
        self.bg_quality = bg_quality
        self.gallery_quality = gallery_quality
        self.sprite_quality = sprite_quality
        self.sprite_height = sprite_height

        self.chunks = {}
        for dirpath, _, files in os.walk(os.path.join(self.common, "story")):
            for name in files:
                if not name.endswith(".txt"):
                    continue
                block = int(name.split(".")[0])
                scene = int(os.path.basename(dirpath))
                self.chunks[(scene, block)] = json.load(
                    open(os.path.join(dirpath, name), encoding="utf-8"))
        self.speakers = []
        for sid in sorted(self.scenes):
            for op in self.chunks[(sid, 0)] + self._ops_after_first(sid):
                if op["op"] == "text":
                    speaker = op.get("speaker") or ""
                    if speaker not in self.speakers:
                        self.speakers.append(speaker)
        self.text = TextPool()
        self.assets = []
        self.asset_index = {}
        self.names = {}
        self.sprite_names = {}
        self.blob = bytearray()
        self.masks = {}
        self.chars = set()
        self.stats = {"bg_encoded": 0, "gallery_encoded": 0, "sprite_encoded": 0,
                      "mask_unique": 0, "mask_shared": 0, "dedup_assets": 0,
                      "unique_mask_bytes": 0, "jpeg_bytes": 0}

    def _ops_after_first(self, sid):
        length = self.scenes[sid]["length"]
        ops = []
        for b in range(1, (length + BLOCK - 1) // BLOCK):
            ops.extend(self.chunks[(sid, b)])
        return ops

    # ---------------------------------------------------------------- images
    def _jpeg(self, im: Image.Image, quality: int) -> bytes:
        out = io.BytesIO()
        im.save(out, "JPEG", quality=quality, optimize=True, subsampling=2)
        return out.getvalue()

    def _blob_add(self, data: bytes) -> int:
        while len(self.blob) % 4:
            self.blob.append(0)
        off = len(self.blob)
        self.blob += data
        return off

    def _mask(self, alpha: Image.Image):
        w, h = alpha.size
        stride = (w + 7) // 8
        packed = bytearray(stride * h)
        raw = alpha.tobytes()
        for y in range(h):
            base = y * w
            row = y * stride
            for x in range(w):
                if raw[base + x] >= 128:
                    packed[row + (x >> 3)] |= 0x80 >> (x & 7)
        data = bytes(packed)
        key = (w, h, hashlib.sha256(data).hexdigest())
        if key in self.masks:
            self.stats["mask_shared"] += 1
            return self.masks[key], data
        off = self._blob_add(data)
        self.masks[key] = (off, data)
        self.stats["mask_unique"] += 1
        self.stats["unique_mask_bytes"] += len(data)
        return self.masks[key], data

    def _add_asset(self, kind: int, file: str, jpeg: bytes, w: int, h: int,
                   mask=None, ow=None, oh=None, ax=0, ay=0, name=""):
        key = (kind, file)
        if key in self.asset_index:
            return self.asset_index[key]
        digest = hashlib.sha256(jpeg + (mask[1] if mask else b"")).hexdigest()
        for i, a in enumerate(self.assets):
            if a["digest"] == digest and a["kind"] == kind:
                self.asset_index[key] = i
                self.stats["dedup_assets"] += 1
                return i
        off = self._blob_add(jpeg)
        mask_off, mask_len = (mask[0], len(mask[1])) if mask else (0, 0)
        rec = {"jpeg_off": off, "jpeg_len": len(jpeg), "w": w, "h": h,
               "mask_off": mask_off, "mask_len": mask_len,
               "ow": ow if ow is not None else w, "oh": oh if oh is not None else h,
               "ax": ax, "ay": ay, "kind": kind, "digest": digest, "name": name,
               "file": file}
        idx = len(self.assets)
        self.assets.append(rec)
        self.asset_index[key] = idx
        self.stats["jpeg_bytes"] += len(jpeg)
        return idx

    def _source_image(self, name: str) -> str:
        low = str(name).lower()
        if low not in self.asset_files:
            raise SystemExit("missing image mapping for %r" % name)
        path = os.path.join(self.common, "images", self.asset_files[low])
        if not os.path.isfile(path):
            raise SystemExit("missing image file " + path)
        return path

    def _encode_bg(self, name: str, gallery: bool) -> int:
        path = self._source_image(name)
        im = Image.open(path).convert("RGB")
        if gallery:
            kind = KIND_GALLERY
            target = (320, 240)
            quality = self.gallery_quality
        else:
            kind = KIND_BG
            target = (224, 168)
            quality = self.bg_quality
        if im.size != target:
            im = im.resize(target, Image.LANCZOS)
        jpeg = self._jpeg(im, quality)
        idx = self._add_asset(kind, path, jpeg, target[0], target[1], name=name)
        self.stats["gallery_encoded" if gallery else "bg_encoded"] += 1
        return idx

    def _encode_sprite(self, name: str) -> int:
        path = self._source_image(name)
        im = Image.open(path).convert("RGBA")
        ow, oh = im.size
        bbox = im.getchannel("A").getbbox() or (0, 0, ow, oh)
        ax, ay = bbox[0], bbox[1]
        crop = im.crop(bbox)
        cw, ch = crop.size
        scale = min(self.sprite_height / ch, 300.0 / cw, 2.0)
        w = max(1, round(cw * scale))
        h = max(1, round(ch * scale))
        if (w, h) != crop.size:
            crop = crop.resize((w, h), Image.LANCZOS)
        alpha = crop.getchannel("A")
        rgb = Image.new("RGB", (w, h), (0, 0, 0))
        rgb.paste(crop, mask=alpha)
        jpeg = self._jpeg(rgb, self.sprite_quality)
        mask, _ = self._mask(alpha)
        idx = self._add_asset(KIND_SPRITE, path, jpeg, w, h, mask=mask,
                              ow=ow, oh=oh, ax=ax, ay=ay, name=name)
        self.stats["sprite_encoded"] += 1
        return idx

    def _encode_ui(self, name: str, file: str, quality: int) -> int:
        path = os.path.join(self.common, "ui", file)
        im = Image.open(path).convert("RGBA")
        ow, oh = im.size
        bbox = im.getchannel("A").getbbox() or (0, 0, ow, oh)
        crop = im.crop(bbox)
        cw, ch = crop.size
        rgb = Image.new("RGB", (cw, ch), (0, 0, 0))
        rgb.paste(crop, mask=crop.getchannel("A"))
        jpeg = self._jpeg(rgb, quality)
        mask, _ = self._mask(crop.getchannel("A"))
        return self._add_asset(KIND_UI, path, jpeg, cw, ch, mask=mask,
                               ow=ow, oh=oh, ax=bbox[0], ay=bbox[1], name=name)

    def _encode_title(self, name: str, file: str) -> int:
        path = os.path.join(self.common, "ui", file)
        im = Image.open(path).convert("RGB")
        w, h = im.size
        if w < 320 or h < 240:
            im = im.resize((320, 240), Image.LANCZOS)
        else:
            im = im.crop((0, h - 240, 320, h))
        jpeg = self._jpeg(im, 45)
        return self._add_asset(KIND_TITLE, path, jpeg, 320, 240, name=name)

    # ---------------------------------------------------------------- assets
    def build_assets(self):
        bg_names = set()
        body_names = set()
        for ops in self.chunks.values():
            for op in ops:
                if op["op"] == "background":
                    bg_names.add(op["image"])
                elif op["op"] in ("bodies", "bodyAdd"):
                    for name in (op.get("images") or
                                 ([op["image"]] if op.get("image") else [])):
                        if name:
                            body_names.add(name)
        gallery_names = [item["image"] for item in self.gallery]
        gallery_set = {str(n).lower() for n in gallery_names}
        bg_names.add("kuro")
        bg_names |= set(gallery_names)
        for name in sorted(bg_names):
            gallery = str(name).lower() in gallery_set
            self.names[str(name).lower()] = self._encode_bg(name, gallery)
        for name in sorted(body_names):
            self.sprite_names[str(name).lower()] = self._encode_sprite(name)
        self.title_before = self._encode_title("title_before", "title-before.jpg")
        self.title_after = self._encode_title("title_after", "title.jpg")
        self.logo = self._encode_ui("logo", "logo.png", 75)
        self.subtitle = self._encode_ui("subtitle", "subtitle.png", 75)

    def asset(self, name: str) -> int:
        low = str(name).lower()
        if low not in self.names:
            raise SystemExit("unmapped background %r" % name)
        return self.names[low]

    def sprite(self, name: str) -> int:
        low = str(name).lower()
        if low not in self.sprite_names:
            raise SystemExit("unmapped portrait %r" % name)
        return self.sprite_names[low]

    def note_chars(self, text: str):
        if text:
            self.chars.update(ch for ch in text if ch not in "\r\n\t")

    # ------------------------------------------------------------ expressions
    def compile_expr(self, e, extras: bytearray, textpool: TextPool) -> int:
        off = len(extras)
        if isinstance(e, bool):
            e = int(e)
        if isinstance(e, int):
            if e == 0:
                extras.append(EXPR_CONST0)
            elif e == 1:
                extras.append(EXPR_CONST1)
            elif -128 <= e <= 127:
                extras += struct.pack("<Bb", EXPR_I8, e)
            else:
                extras += struct.pack("<Bi", EXPR_I32, e)
            return off
        if isinstance(e, str):
            self.note_chars(e)
            toff = textpool.add(e)
            extras += struct.pack("<BHH", EXPR_STR_REF, toff, len(e.encode("utf-8")))
            return off
        if not isinstance(e, list) or not e:
            raise SystemExit("unsupported expression: %r" % (e,))
        op = e[0]
        if op == "v":
            kind = VAR_KINDS.get(e[1])
            if kind is None:
                raise SystemExit("unknown variable kind %r" % (e[1],))
            if len(e) == 2:
                extras += struct.pack("<BB", EXPR_VAR_NOINDEX, kind)
            elif isinstance(e[2], int):
                extras += struct.pack("<BBH", EXPR_VAR_STATIC, kind, e[2])
            else:
                extras += struct.pack("<BB", EXPR_VAR_DYNAMIC, kind)
                self.compile_expr(e[2], extras, textpool)
            return off
        if op == "call":
            name, args = e[1], e[2]
            extras += struct.pack("<BBB", EXPR_CALL, self.native_id(name), len(args))
            for arg in args:
                self.compile_expr(arg, extras, textpool)
            if len(e) > 3:
                extras.append(1)
                self.compile_expr(e[3], extras, textpool)
            else:
                extras.append(0)
            return off
        if op in ("+", "-") and len(e) == 2:
            extras += struct.pack("<BB", EXPR_UNOP, UNOPS[op])
            self.compile_expr(e[1], extras, textpool)
            return off
        if op in BINOPS and len(e) == 3:
            extras += struct.pack("<BB", EXPR_BINOP, BINOPS[op])
            self.compile_expr(e[1], extras, textpool)
            self.compile_expr(e[2], extras, textpool)
            return off
        if op in UNOPS and len(e) == 2:
            extras += struct.pack("<BB", EXPR_UNOP, UNOPS[op])
            self.compile_expr(e[1], extras, textpool)
            return off
        if op == "&&":
            extras.append(EXPR_AND)
            self.compile_expr(e[1], extras, textpool)
            self.compile_expr(e[2], extras, textpool)
            return off
        if op == "||":
            extras.append(EXPR_OR)
            self.compile_expr(e[1], extras, textpool)
            self.compile_expr(e[2], extras, textpool)
            return off
        raise SystemExit("unsupported expression op %r" % (op,))

    def native_id(self, name: str) -> int:
        if re.search(r"\.counter\[\]\.start", name):
            return NATIVES["counter.start"]
        if re.search(r"\.counter\[\]\.get$", name):
            return NATIVES["counter.get"]
        if name == "global.random":
            return NATIVES["global.random"]
        if name == "global.get_scene_name":
            return NATIVES["global.get_scene_name"]
        if name == "global.get_line_no":
            return NATIVES["global.get_line_no"]
        if name == "$filename_translate":
            return NATIVES["$filename_translate"]
        if name == "global.namae":
            return NATIVES["global.namae"]
        if re.search(r"\.check$|\.check\(\)$|\.on_down$|\.check_skip$", name):
            return NATIVES["check"]
        if name == "global.timewait_key":
            return NATIVES["timewait_key"]
        if name == "$disp_dialog":
            return NATIVES["$disp_dialog"]
        if re.search(r"\.get_auto_mode_onoff_flag$", name):
            return NATIVES["auto_mode_onoff"]
        if re.search(r"\.get_(?:open|close)_anime_type$", name):
            return NATIVES["anime_type"]
        if re.search(r"\.get_(?:open|close)_anime_time$", name):
            return NATIVES["anime_time"]
        if re.search(r"\.get_no_wipe_anime_onoff$", name):
            return NATIVES["no_wipe"]
        if re.search(r"\.get_koe_dont_stop_onoff$", name):
            return NATIVES["koe_dont_stop"]
        if re.search(r"\.get_filter_color_[rgba]$", name):
            return NATIVES["filter_color"]
        if name == "$search_chara_center":
            return NATIVES["$search_chara_center"]
        m = re.search(r"\.(?:s|cur_call\.k)\[\]\.(search|len|cnt|mid|left|tonum)$", name)
        if m:
            return {"search": NATIVES["str.search"], "len": NATIVES["str.len"],
                    "cnt": NATIVES["str.len"], "mid": NATIVES["str.mid"],
                    "left": NATIVES["str.left"], "tonum": NATIVES["str.tonum"]}[m.group(1)]
        raise SystemExit("unsupported native %r" % name)

    # ---------------------------------------------------------------- blocks
    def encode_block(self, ops):
        textpool = TextPool()
        extras = bytearray()
        records = []
        for o in ops:
            op = o["op"]
            r = dict(type=0, n=0, x=0, y=0, z=0, t_off=0, t_len=0, extra=0)
            if op == "text":
                speaker = o.get("speaker") or ""
                text = o.get("text") or ""
                self.note_chars(speaker)
                self.note_chars(text)
                r["type"] = OP_TEXT
                r["x"] = self.speakers.index(speaker)
                r["t_off"] = textpool.add(text)
                r["t_len"] = len(text.encode("utf-8"))
            elif op in ("choice", "dynamicChoice"):
                r["y"] = self.compile_expr(o["target"], extras, textpool)
                if op == "choice":
                    items = o["items"]
                    r["type"] = OP_CHOICE
                    r["n"] = len(items)
                    r["x"] = len(extras)
                    extras.append(len(items))
                    for item in items:
                        text = item.get("text") or ""
                        self.note_chars(text)
                        toff = textpool.add(text)
                        extras += struct.pack("<iiHH", int(item.get("value", 0)),
                                              int(item.get("serial", 0)), toff,
                                              len(text.encode("utf-8")))
                else:
                    labels = o["labels"]
                    r["type"] = OP_DYNAMIC_CHOICE
                    r["n"] = len(labels)
                    r["x"] = len(extras)
                    extras.append(len(labels))
                    offset_pos = len(extras)
                    extras += bytes(2 * len(labels))
                    label_offsets = []
                    for label in labels:
                        label_offsets.append(self.compile_expr(label, extras, textpool))
                    for i, off in enumerate(label_offsets):
                        struct.pack_into("<H", extras, offset_pos + i * 2, off)
            elif op == "set":
                r["type"] = OP_SET
                r["x"] = self.compile_expr(o["target"], extras, textpool)
                r["y"] = self.compile_expr(o["value"], extras, textpool)
            elif op == "native":
                r["type"] = OP_NATIVE
                r["x"] = self.compile_expr(o["call"], extras, textpool)
            elif op == "goto":
                r["type"] = OP_GOTO
                r["x"] = o["to"]
            elif op == "ifFalse":
                r["type"] = OP_IF_FALSE
                r["x"] = o["to"]
                r["y"] = self.compile_expr(o["condition"], extras, textpool)
            elif op == "ifTrue":
                r["type"] = OP_IF_TRUE
                r["x"] = o["to"]
                r["y"] = self.compile_expr(o["condition"], extras, textpool)
            elif op == "call":
                r["type"] = OP_CALL
                r["x"] = o["to"]
            elif op == "jumpScene":
                r["type"] = OP_JUMP_SCENE
                r["x"] = o["scene"]
                r["n"] = o.get("entry", 0)
            elif op == "callScene":
                r["type"] = OP_CALL_SCENE
                r["x"] = o["scene"]
                r["n"] = o.get("entry", 0)
            elif op == "return":
                r["type"] = OP_RETURN
            elif op == "end":
                r["type"] = OP_END
            elif op == "background":
                r["type"] = OP_BACKGROUND
                r["x"] = self.asset(o["image"])
                r["n"] = 1 if o.get("clearBody") else 0
            elif op == "bodies":
                images = o.get("images") or []
                r["type"] = OP_BODIES
                r["n"] = min(3, len(images))
                for i, name in enumerate(images[:3]):
                    r["xyz"[i]] = self.sprite(name)
            elif op == "bodyAdd":
                r["type"] = OP_BODY_ADD
                r["x"] = self.sprite(o["image"])
            else:
                raise SystemExit("unsupported op %r" % (op,))
            records.append(r)
        text = textpool.walk()
        head = struct.pack("<HHHH", len(records), len(text), len(extras), 0)
        body = bytearray(head)
        for r in records:
            body += struct.pack("<BBHHHHHH", r["type"], r["n"], r["x"], r["y"],
                                r["z"], r["t_off"], r["t_len"], r["extra"])
        body += extras
        body += text
        return bytes(body), len(text), len(extras)

    # ------------------------------------------------------------------ pack
    def build(self, out_path: str, report_path: str):
        self.build_assets()
        text = TextPool()
        speaker_off = [text.add(s) for s in self.speakers]
        scene_ids = sorted(self.scenes)
        scene_name_off = {sid: text.add(self.scenes[sid].get("name") or "")
                          for sid in scene_ids}
        scene_source_off = {sid: text.add(self.scenes[sid].get("source") or "")
                            for sid in scene_ids}
        route_name_off = [text.add(r["name"]) for r in self.catalog["routes"]]
        chapter_name_off = [text.add(c["name"]) for c in self.catalog["chapters"]]
        for route in self.catalog["routes"]:
            self.note_chars(route["name"])
        for chapter in self.catalog["chapters"]:
            self.note_chars(chapter["name"])
        for item in self.gallery:
            self.note_chars(item["label"])
        for scene in scene_ids:
            self.note_chars(self.scenes[scene].get("name") or "")

        block_records = []
        ops_section = bytearray()
        max_raw = max_text = max_extra = max_ops = 0
        for sid in scene_ids:
            length = self.scenes[sid]["length"]
            for b in range((length + BLOCK - 1) // BLOCK):
                ops = self.chunks.get((sid, b))
                if ops is None:
                    raise SystemExit("missing chunk %s/%s" % (sid, b))
                raw, text_bytes, extra_bytes = self.encode_block(ops)
                max_raw = max(max_raw, len(raw))
                max_text = max(max_text, text_bytes)
                max_extra = max(max_extra, extra_bytes)
                max_ops = max(max_ops, len(ops))
                if text_bytes > 65000 or extra_bytes > 65000:
                    raise SystemExit("block %s/%s exceeds u16 text/extra" % (sid, b))
                cobj = zlib.compressobj(9, zlib.DEFLATED, -15)
                comp = cobj.compress(raw) + cobj.flush()
                while len(ops_section) % 4:
                    ops_section.append(0)
                block_records.append({"off": len(ops_section), "comp": len(comp),
                                      "raw": len(raw), "op_count": len(ops),
                                      "scene": sid, "block": b})
                ops_section += comp

        entries = bytearray()
        for sid in scene_ids:
            table = self.scenes[sid]["entries"]
            if len(table) > ENTRY_KEEP:
                table = table[:ENTRY_KEEP]
            for i in range(ENTRY_KEEP):
                entries += struct.pack("<H", table[i] if i < len(table) else 0)

        catalog = bytearray()
        catalog += struct.pack("<HHHH", len(scene_ids), len(block_records),
                               len(self.catalog["routes"]), len(self.catalog["chapters"]))
        block_base = 0
        for si, sid in enumerate(scene_ids):
            length = self.scenes[sid]["length"]
            blocks = (length + BLOCK - 1) // BLOCK
            scene = self.scenes[sid]
            catalog += struct.pack("<HHHHHBBHH", sid, scene_name_off[sid],
                                   scene_source_off[sid], length,
                                   si * ENTRY_KEEP, ENTRY_KEEP,
                                   int(scene.get("route", 0)), block_base, blocks)
            block_base += blocks
        for rec in block_records:
            catalog += struct.pack("<IIIHH", rec["off"], rec["comp"], rec["raw"],
                                   rec["op_count"], 0)
        chapter_first = 0
        for ri, route in enumerate(self.catalog["routes"]):
            count = sum(1 for c in self.catalog["chapters"] if int(c["route"]) == ri)
            catalog += struct.pack("<HHHH", route_name_off[ri], chapter_first, count, 0)
            chapter_first += count
        order = sorted(range(len(self.catalog["chapters"])),
                       key=lambda i: (int(self.catalog["chapters"][i]["route"]), i))
        for ci in order:
            chapter = self.catalog["chapters"][ci]
            catalog += struct.pack("<HHBBH", chapter["id"], chapter_name_off[ci],
                                   int(chapter["route"]), 0, 0)

        asset_section = bytearray()
        asset_names = bytearray()
        for a in self.assets:
            asset_section += struct.pack("<IIHHIIHHhhBBHHH",
                                         a["jpeg_off"], a["jpeg_len"], a["w"], a["h"],
                                         a["mask_off"], a["mask_len"], a["ow"], a["oh"],
                                         a["ax"], a["ay"], a["kind"], 0, 0, 0, 0)
            asset_names += struct.pack("<H", text.add(a["name"]))

        groups = OrderedDict()
        for item in self.gallery:
            key = item.get("group") or item["source"]
            groups.setdefault(key, []).append(item)
        gallery_section = bytearray()
        gallery_entries = bytearray()
        first = 0
        for label, items in groups.items():
            gallery_section += struct.pack("<HHHH", text.add(label), first,
                                           len(items), self.names[items[0]["image"].lower()])
            for item in items:
                gallery_entries += struct.pack("<H", self.names[item["image"].lower()])
            first += len(items)
        if first != len(self.gallery):
            raise SystemExit("gallery entry count mismatch")

        # 别名表按“用途”区分：同一源名可能同时是背景和立绘（两张独立资源）。
        alias = bytearray()
        for name in sorted(self.names):
            alias += struct.pack("<HHBB", text.add(name), self.names[name], 0, 0)
        for name in sorted(self.sprite_names):
            alias += struct.pack("<HHBB", text.add(name), self.sprite_names[name], 1, 0)

        meta = {
            "format": "CLANNAD AI Passport pack",
            "version": str(VERSION),
            "source": "xuanyide01/clannad-rw5",
            "source_commit": "2c67720807db9d9761c22ddbc1f11e6a90cc2adc",
            "flow_scene": str(self.catalog["flowScene"]),
            "initial_scene": str(self.catalog["initial"]),
            "block_size": str(BLOCK),
            "scenes": str(len(scene_ids)),
            "chunks": str(len(block_records)),
            "chapters": str(len(self.catalog["chapters"])),
            "routes": str(len(self.catalog["routes"])),
            "gallery_groups": str(len(groups)),
            "gallery_entries": str(len(self.gallery)),
            "assets": str(len(self.assets)),
            "bg_quality": str(self.bg_quality),
            "gallery_quality": str(self.gallery_quality),
            "sprite_quality": str(self.sprite_quality),
            "sprite_height": str(self.sprite_height),
            "kuro_asset": str(self.names["kuro"]),
            "title_before": str(self.title_before),
            "title_after": str(self.title_after),
            "logo": str(self.logo),
            "subtitle": str(self.subtitle),
            "max_raw_block": str(max_raw),
            "max_block_text": str(max_text),
            "max_block_extra": str(max_extra),
            "max_block_ops": str(max_ops),
        }
        meta_section = bytes(
            "".join("%s=%s\n" % (k, v) for k, v in meta.items()), "utf-8")

        sections = [
            (SEC_TEXT, text.walk(), 0),
            (SEC_SPEAKER, b"".join(struct.pack("<H", o) for o in speaker_off),
             len(speaker_off)),
            (SEC_CATALOG, bytes(catalog), len(scene_ids)),
            (SEC_ENTRY, bytes(entries), len(entries) // 2),
            (SEC_ASSETNAME, bytes(asset_names), len(self.assets)),
            (SEC_ASSET, bytes(asset_section), len(self.assets)),
            (SEC_GALLERY, bytes(gallery_section) + bytes(gallery_entries),
             len(groups) * 8),
            (SEC_BLOB, bytes(self.blob), len(self.assets)),
            (SEC_OPS, bytes(ops_section), len(block_records)),
            (SEC_META, meta_section, 0),
            (SEC_ALIAS, bytes(alias), len(alias) // 6),
        ]
        header_size = 24 + 16 * len(sections)
        offsets = {}
        pos = header_size
        for sec_id, data, _ in sections:
            while pos % 4:
                pos += 1
            offsets[sec_id] = pos
            pos += len(data)
        total = pos
        out = bytearray(total)
        out[0:8] = MAGIC
        struct.pack_into("<IIHHI", out, 8, VERSION, total, len(sections), 0, 0)
        for i, (sec_id, data, count) in enumerate(sections):
            off = offsets[sec_id]
            struct.pack_into("<IIII", out, 24 + i * 16, sec_id, off, count, len(data))
            out[off:off + len(data)] = data
        out_dir = os.path.dirname(os.path.abspath(out_path))
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        with open(out_path, "wb") as fh:
            fh.write(out)

        chars = "".join(sorted(self.chars))
        chars_path = os.path.join(out_dir, "clannad_chars.txt")
        with open(chars_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# Characters used by the CLANNAD pack (story, names, choices,\n")
            fh.write("# string constants, routes, chapters, gallery labels).\n")
            fh.write(chars + "\n")

        report = {
            "source": os.path.abspath(self.root),
            "pack_bytes": len(out),
            "sections": {SEC_NAMES[s]: {"offset": offsets[s], "size": len(d)}
                         for s, d, _ in sections},
            "meta": meta,
            "stats": self.stats,
            "max_block": {"raw": max_raw, "text": max_text, "extra": max_extra,
                          "ops": max_ops},
            "assets": {
                "count": len(self.assets),
                "jpeg_bytes": self.stats["jpeg_bytes"],
                "unique_mask_bytes": self.stats["unique_mask_bytes"],
                "bg": sum(1 for a in self.assets if a["kind"] == KIND_BG),
                "gallery": sum(1 for a in self.assets if a["kind"] == KIND_GALLERY),
                "sprites": sum(1 for a in self.assets if a["kind"] == KIND_SPRITE),
                "title": sum(1 for a in self.assets if a["kind"] == KIND_TITLE),
                "ui": sum(1 for a in self.assets if a["kind"] == KIND_UI),
            },
            "aliases": len(self.names) + len(self.sprite_names),
            "char_count": len(chars),
            "chars_file": chars_path,
            "sha256": hashlib.sha256(out).hexdigest(),
        }
        with open(report_path, "w", encoding="utf-8") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=1)
        print(json.dumps({k: report[k] for k in
                          ("pack_bytes", "sha256", "aliases", "char_count",
                           "assets", "max_block")}, ensure_ascii=False, indent=1))
        return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--out", default="assets/clannad/clannad_pack.bin")
    ap.add_argument("--report", default="build/clannad_pack_report.json")
    ap.add_argument("--bg-quality", type=int, default=40)
    ap.add_argument("--gallery-quality", type=int, default=30)
    ap.add_argument("--sprite-quality", type=int, default=55)
    ap.add_argument("--sprite-height", type=int, default=160)
    args = ap.parse_args()
    packer = Packer(args.source, args.bg_quality, args.gallery_quality,
                    args.sprite_quality, args.sprite_height)
    packer.build(args.out, args.report)


if __name__ == "__main__":
    main()
