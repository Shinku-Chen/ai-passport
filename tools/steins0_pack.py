#!/usr/bin/env python3
"""Build the SG0PK01 resource pack for the Steins;Gate 0 (RW5) reader.

Source: the already converted `steins0-rw5` Xiaomi Vela port snapshot.  The
pack is read directly from flash by the firmware: no runtime JSON, no file
system, no PNG decoder.  Dialogue text is stored per source chunk (zlib) in the
source's private-use encoding (U+E000 + SC3 glyph id): this packer does not
invent an OCR/Unicode transcription.  Images are re-encoded to JPEG (opaque) or
JPEG + 1bpp alpha mask (bodies, expression patches, masks).

Pack layout v1 (little endian), header 20 B and 16 B per section:
 0 TEXT     named string blob       1 STRT {off u32,len u16,pad u16}
 2 NODE     {chunk_first u16,chunk_count u16,choice u16,condition u16,
             ending i8,flags u8,break_first u16,break_count u8,
             chapter_first u16,chapter_count u8,event_first u16,
             event_count u16,start_first u16,start_count u8,pad u16} = 24 B
 3 CHUNK    {zoff u32,zlen u32,raw_len u16,frames u8,pad u8} = 12 B
 4 FRAME    {text_off u16,text_len u16,view u16,speaker u8,group u8,
             date u16,flags u8,pad u8} = 12 B
 5 VIEW     {ref_first u16,layer_count u8,pad u8} = 4 B
 6 LAYER    {asset u16,x i16,y i16,w u16,h u16,opacity u16} = 12 B
 7 CHOICE   {kind u8,critical u8,heading u16,category u16,count u8,pad u8,
             to[2] u8,action[2] u8,text[2] u16,pad u16} = 18 B
 8 COND     {flag u8,yes u8,no u8,pad u8} = 4 B
 9 ENDING   {name u16,ending_id u8,pad u8} = 4 B
10 ENDLIST  u16 string index per clear-list entry
11 BREAK    u16 node-local cursor
12 CHGRP    {cursor u16,group u8,pad u8} = 4 B
13 EVENT    {at u16,type u8,flags u8,a u16,b u16,c u16,d u16} = 12 B
14 START    {node u8,group u8,script u16,index u16,date u16,name u16,pad u16}
15 ASSETID  {part_first u16,part_count u8,kind u8,name u16,pad u16} = 8 B
16 PART     {off u32,len u32,mask_off u32,mask_len u32,w u16,h u16,
             ax i16,ay i16,ow u16,oh u16,flags u8,pad u8} = 30 B
17 BLOB     chunk zlib streams (4-byte aligned) + image/mask payloads
18 GG       {label u16,first u16,count u8,pad u8} = 8 B
19 GE       {asset u16,pad u16} = 4 B
20 META     key=value UTF-8 text
21 SPK      u16 string index per speaker slot (FRAME.speaker indexes this)
22 DATE     u32 source story date per slot (FRAME.date indexes this)
23 LAYERREF u16 layer index per view layer slot (VIEW.ref_first + index)

Usage:
  python tools/steins0_pack.py --source <steins0-rw5 checkout> \
      --out assets/steins0/steins0_pack.bin --report build/steins0_pack_report.json
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

MAGIC = b"SG0PK001"
VERSION = 1
HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
STRT = struct.Struct("<IHH")
NODE = struct.Struct("<HHHHbBHBHBHHHBB")
CHUNK = struct.Struct("<IIHBB")
FRAME = struct.Struct("<HHHBBHBx")
VIEW = struct.Struct("<HBB")
LAYER = struct.Struct("<HhhHHH")
CHOICE = struct.Struct("<BBHHBBBBBBHHH")
COND = struct.Struct("<BBBx")
ENDING = struct.Struct("<HBB")
CHGRP = struct.Struct("<HBB")
EVENT = struct.Struct("<HBBHHHHHH")
START = struct.Struct("<BBHHHHH")
ASSETID = struct.Struct("<HBBHH")
PART = struct.Struct("<IIIIHHhhHHBx")
GG = struct.Struct("<HHBBxx")
GE = struct.Struct("<Hxx")
RMSG = struct.Struct("<HHHBBBBHHxx")
RREPLY = struct.Struct("<HBBHH")
RCHOICE = struct.Struct("<HHBBH")
RCONTACT = struct.Struct("<BHH3x")
CARD = struct.Struct("<H")

SEC_TEXT, SEC_STRT, SEC_NODE, SEC_CHUNK, SEC_FRAME, SEC_VIEW, SEC_LAYER = range(7)
SEC_CHOICE, SEC_COND, SEC_ENDING, SEC_ENDLIST, SEC_BREAK, SEC_CHGRP = range(7, 13)
SEC_EVENT, SEC_START, SEC_ASSETID, SEC_PART, SEC_BLOB, SEC_GG, SEC_GE = range(13, 20)
SEC_META, SEC_SPK, SEC_DATE, SEC_LAYERREF, SEC_TITLE = 20, 21, 22, 23, 24
SEC_RMSG, SEC_RREPLY, SEC_RCHOICE, SEC_RSEQ, SEC_RCONTACT, SEC_RBASE, SEC_CARD = range(25, 32)
SECTION_COUNT = 32

assert NODE.size == 24, NODE.size
assert CHUNK.size == 12 and FRAME.size == 12
assert VIEW.size == 4 and LAYER.size == 12
assert CHOICE.size == 18 and COND.size == 4 and ENDING.size == 4 and CHGRP.size == 4
assert EVENT.size == 16 and START.size == 12 and ASSETID.size == 8 and PART.size == 30
assert GG.size == 8 and GE.size == 4 and STRT.size == 8
assert RMSG.size == 16 and RREPLY.size == 8 and RCHOICE.size == 8 and RCONTACT.size == 8
assert CARD.size == 2

KIND_PHOTO, KIND_SPRITE = 0, 1
NONE16 = 0xFFFF


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_js_object(text: str, marker: str) -> dict:
    start = text.index(marker) + len(marker)
    while text[start] != "{":
        start += 1
    depth = 0
    instr = False
    esc = False
    for pos in range(start, len(text)):
        ch = text[pos]
        if instr:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                instr = False
            continue
        if ch == '"':
            instr = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return json.loads(text[start:pos + 1])
    raise ValueError("unbalanced JSON object")


def psnr(a: Image.Image, b: Image.Image, alpha: Image.Image | None = None) -> float:
    a = a.convert("RGB")
    b = b.convert("RGB")
    total = 0.0
    count = 0
    pa, pb = a.load(), b.load()
    for y in range(a.height):
        for x in range(a.width):
            if alpha is not None and alpha.getpixel((x, y)) < 128:
                continue
            for k in range(3):
                d = pa[x, y][k] - pb[x, y][k]
                total += d * d
                count += 1
    if not count:
        return 99.0
    mse = total / count
    return 99.0 if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)


class TextTable:
    def __init__(self):
        self.buf = bytearray()
        self.index: dict[str, int] = {}
        self.items: list[str] = []

    def add(self, s: str) -> int:
        idx = self.index.get(s)
        if idx is None:
            idx = len(self.items)
            self.items.append(s)
            self.index[s] = idx
            self.buf += s.encode("utf-8")
        return idx

    def offsets(self):
        out = []
        off = 0
        for s in self.items:
            raw = s.encode("utf-8")
            out.append((off, len(raw)))
            off += len(raw)
        return out


class Packer:
    def __init__(self, root, args):
        self.root = root
        self.common = root / "src" / "common"
        self.args = args
        self.graph_blob = (self.common / "graph.txt").read_bytes()
        self.graph = json.loads(self.graph_blob.decode("utf-8"))
        self.nodes = self.graph["nodes"]
        js = (self.common / "assets.js").read_bytes().decode("utf-8")
        self.assets = parse_js_object(js, "const assets=")
        self.parts = parse_js_object(js, "const parts=")
        self.gallery = json.loads((self.common / "gallery.txt").read_bytes().decode("utf-8"))
        self.asset_names = list(self.assets.keys())
        self.asset_index = {name: i for i, name in enumerate(self.asset_names)}
        self.text = TextTable()
        self.speakers: list[str] = []
        self.speaker_index: dict[str, int] = {}
        self.dates: list[int] = []
        self.date_index: dict[int, int] = {}
        self.image_blob = bytearray()
        self.part_records: list[tuple] = []
        self.part_cache: dict[str, tuple] = {}
        self.unique_image_files: set[str] = set()
        self.part_psnr: dict[str, list[float]] = {"body": [], "face": [], "photo": []}
        self.asset_rows: list[tuple] = []
        self.layer_records: list[tuple] = []
        self.layer_index: dict[tuple, int] = {}
        self.layer_ref: list[int] = []
        self.view_records: list[tuple] = []
        self.view_index: dict[tuple, int] = {}
        self.stats = {
            "photo": {"files": 0, "jpeg": 0},
            "body": {"files": 0, "jpeg": 0, "mask": 0, "mask_raw": 0},
            "face": {"files": 0, "jpeg": 0, "mask": 0, "mask_raw": 0},
            "title": {"files": 0, "jpeg": 0},
            "stamp": {"files": 0, "jpeg": 0, "mask": 0, "mask_raw": 0},
            "card": {"files": 0, "jpeg": 0},
        }
        self.title_asset = NONE16
        self.stamp_assets = {}
        self.card_assets = {}
        self.rine_msg = []          # catalog records (dicts)
        self.rine_msg_index = {}    # delivery key -> catalog index
        self.rine_replies = []      # reply window records
        self.rine_reply_index = {}  # reply key -> index
        self.rine_choices = []      # reply choice records
        self.rine_seq = []          # catalog refs for reply sequences
        self.node_order = sorted(self.nodes, key=lambda n: int(n[1:]))
        self.node_index = {name: i for i, name in enumerate(self.node_order)}
        if [int(n[1:]) for n in self.node_order] != list(range(len(self.node_order))):
            raise SystemExit("node ids must be contiguous n0..nN")
        self.report = {
            "source": str(root),
            "source_graph_sha256": sha256(self.graph_blob),
            "source_assets_sha256": sha256((self.common / "assets.js").read_bytes()),
            "source_gallery_sha256": sha256((self.common / "gallery.txt").read_bytes()),
            "source_font_sha256": sha256((self.common / "sg-original.ttf").read_bytes()),
            "quality": {
                "photo": args.quality_photo, "body": args.quality_body,
                "face": args.quality_face, "title": args.quality_title,
                "mask_bits": 1, "zlib_level": args.zlib_level,
                "jpeg_subsampling": "4:2:0", "jpeg_progressive": False,
            },
        }

    # ------------------------------------------------------------- string ids
    def string(self, s: str) -> int:
        return self.text.add(s)

    def speaker(self, s: str) -> int:
        idx = self.speaker_index.get(s)
        if idx is None:
            idx = len(self.speakers)
            self.speakers.append(s)
            self.speaker_index[s] = idx
            self.string(s)
        return idx

    def date(self, value: int) -> int:
        idx = self.date_index.get(value)
        if idx is None:
            idx = len(self.dates)
            self.dates.append(value)
            self.date_index[value] = idx
        return idx

    # ---------------------------------------------------------------- images
    def _role_map(self):
        if hasattr(self, "_roles"):
            return self._roles
        roles: dict[str, str] = {}
        for _pid, files in self.parts.items():
            for i, rel in enumerate(files):
                prev = roles.get(rel)
                want = "face" if i > 0 else "body"
                if prev == "face":
                    continue
                roles[rel] = want
        for asset, rel in self.assets.items():
            want = "body" if asset.startswith("p") else "photo"
            if roles.get(rel) == "face":
                continue
            if rel not in roles or roles[rel] == "photo":
                roles[rel] = want
        self._roles = roles
        return roles

    def _part_from_rgba(self, rel: str, quality: int, role: str) -> int:
        im = Image.open(self.common / "img" / rel)
        if im.mode != "RGBA":
            im = im.convert("RGBA")
        alpha = im.getchannel("A")
        bbox = alpha.getbbox()
        ow, oh = im.size
        if not bbox:
            rgb = Image.new("RGB", (1, 1), (0, 0, 0))
            mask_img = Image.new("L", (1, 1), 0)
            ax = ay = 0
            w = h = 1
        else:
            ax, ay, x1, y1 = bbox
            w, h = x1 - ax, y1 - ay
            crop = im.crop(bbox)
            alpha_c = crop.getchannel("A")
            rgb = crop.convert("RGB")
            mask_img = alpha_c
            # Fill the transparent area with the mean opaque colour: fewer JPEG
            # bits and no black fringe when the mask is applied at draw time.
            px = crop.load()
            step = max(1, int(math.sqrt(max(1, w * h // 4096))))
            r = g = b = n = 0
            for y in range(0, h, step):
                for x in range(0, w, step):
                    p = px[x, y]
                    if p[3] >= 128:
                        r += p[0]
                        g += p[1]
                        b += p[2]
                        n += 1
            fill = (r // n, g // n, b // n) if n else (0, 0, 0)
            low = mask_img.point(lambda v: 255 if v < 16 else 0)
            rgb.paste(Image.new("RGB", (w, h), fill), (0, 0), low)
        buf = io.BytesIO()
        rgb.save(buf, "JPEG", quality=quality, optimize=True, subsampling=2,
                 progressive=False)
        jpeg = buf.getvalue()
        stride = (w + 7) // 8
        mask = bytearray(stride * h)
        mp = mask_img.load()
        for y in range(h):
            base = y * stride
            for x in range(w):
                if mp[x, y] >= 128:
                    mask[base + (x >> 3)] |= 0x80 >> (x & 7)
        mask_z = zlib.compress(bytes(mask), 9)
        self._align(self.image_blob)
        off = len(self.image_blob)
        self.image_blob += jpeg
        self._align(self.image_blob)
        mask_off = len(self.image_blob)
        self.image_blob += mask_z
        stats = self.stats[role]
        stats["files"] += 1
        stats["jpeg"] += len(jpeg)
        stats["mask"] += len(mask_z)
        stats["mask_raw"] += len(mask)
        if len(self.part_psnr[role]) < 12 and rgb.width * rgb.height > 0:
            decoded = Image.open(io.BytesIO(jpeg)).convert("RGB")
            self.part_psnr[role].append(psnr(rgb, decoded, mask_img))
        self.part_cache[rel] = (off, len(jpeg), mask_off, len(mask_z), w, h,
                                ax, ay, ow, oh, 1)

    def _part_from_scaled_rgba(self, rel: str, quality: int, max_w: int, max_h: int,
                               role: str) -> int:
        im = Image.open(self.root / rel)
        if im.mode != "RGBA":
            im = im.convert("RGBA")
        alpha = im.getchannel("A")
        bbox = alpha.getbbox() or (0, 0, im.width, im.height)
        ax, ay, x1, y1 = bbox
        crop = im.crop(bbox)
        cw, ch = crop.size
        scale = min(max_w / max(1, cw), max_h / max(1, ch))
        w = max(1, int(round(cw * scale)))
        h = max(1, int(round(ch * scale)))
        crop = crop.resize((w, h), Image.LANCZOS)
        alpha_c = crop.getchannel("A")
        rgb = crop.convert("RGB")
        buf = io.BytesIO()
        rgb.save(buf, "JPEG", quality=quality, optimize=True, subsampling=2,
                 progressive=False)
        jpeg = buf.getvalue()
        stride = (w + 7) // 8
        mask = bytearray(stride * h)
        ap = alpha_c.load()
        for y in range(h):
            base = y * stride
            for x in range(w):
                if ap[x, y] >= 128:
                    mask[base + (x >> 3)] |= 0x80 >> (x & 7)
        mask_z = zlib.compress(bytes(mask), 9)
        self._align(self.image_blob)
        off = len(self.image_blob)
        self.image_blob += jpeg
        self._align(self.image_blob)
        mask_off = len(self.image_blob)
        self.image_blob += mask_z
        stats = self.stats[role]
        stats["files"] += 1
        stats["jpeg"] += len(jpeg)
        stats["mask"] += len(mask_z)
        stats["mask_raw"] += len(mask)
        idx = len(self.part_records)
        self.part_records.append((off, len(jpeg), mask_off, len(mask_z), w, h,
                                  0, 0, w, h, 1))
        return idx

    def _part_from_photo_scaled(self, rel: str, quality: int, w: int, h: int, role: str) -> int:
        im = Image.open(self.root / rel).convert("RGB").resize((w, h), Image.LANCZOS)
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=quality, optimize=True, subsampling=2,
                progressive=False)
        jpeg = buf.getvalue()
        self._align(self.image_blob)
        off = len(self.image_blob)
        self.image_blob += jpeg
        stats = self.stats[role]
        stats["files"] += 1
        stats["jpeg"] += len(jpeg)
        idx = len(self.part_records)
        self.part_records.append((off, len(jpeg), 0, 0, w, h, 0, 0, w, h, 0))
        return idx

    def _part_from_photo(self, rel: str, quality: int) -> int:
        im = Image.open(self.common / "img" / rel).convert("RGB")
        w, h = im.size
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=quality, optimize=True, subsampling=2,
                progressive=False)
        jpeg = buf.getvalue()
        self._align(self.image_blob)
        off = len(self.image_blob)
        self.image_blob += jpeg
        self.stats["photo"]["files"] += 1
        self.stats["photo"]["jpeg"] += len(jpeg)
        if len(self.part_psnr["photo"]) < 12:
            decoded = Image.open(io.BytesIO(jpeg)).convert("RGB")
            self.part_psnr["photo"].append(psnr(im, decoded))
        self.part_cache[rel] = (off, len(jpeg), 0, 0, w, h, 0, 0, w, h, 0)

    @staticmethod
    def _align(buf: bytearray):
        while len(buf) % 4:
            buf.append(0)

    def part_for(self, rel: str) -> int:
        if rel not in self.part_cache:
            self.unique_image_files.add(rel)
            role = self._role_map()[rel]
            if role in ("body", "face"):
                q = self.args.quality_body if role == "body" else self.args.quality_face
                self._part_from_rgba(rel, q, role)
            else:
                self._part_from_photo(rel, self.args.quality_photo)
        idx = len(self.part_records)
        self.part_records.append(self.part_cache[rel])
        return idx

    def build_assets(self):
        for name in self.asset_names:
            files = self.parts.get(name, [self.assets[name]])
            ids = [self.part_for(rel) for rel in files]
            first = ids[0]
            if ids != list(range(first, first + len(ids))):
                raise SystemExit(f"{name}: parts are not contiguous")
            kind = KIND_SPRITE if any(self.part_records[i][10] for i in ids) else KIND_PHOTO
            self.asset_rows.append((first, len(ids), kind, self.string(name)))
        if self.args.title_art:
            self._add_title()
        for stamp_id in sorted(int(p.stem) for p in
                               (self.common / "phone-stamps").glob("*.png")):
            rel = f"src/common/phone-stamps/{stamp_id}.png"
            part = self._part_from_scaled_rgba(rel, 62, 96, 96, "stamp")
            asset = len(self.asset_rows)
            self.stamp_assets[stamp_id] = asset
            self.asset_rows.append((part, 1, KIND_SPRITE, self.string(f"stamp_{stamp_id}")))
        card_paths = sorted((self.common / "chapter-cards").glob("*.jpg"),
                            key=lambda p: int(p.stem))
        if not card_paths:
            raise SystemExit(
                "missing tracked chapter cards: " +
                str(self.common / "chapter-cards") +
                " (the pack would contain invalid NONE card assets)")
        for path in card_paths:
            rel = f"src/common/chapter-cards/{path.name}"
            part = self._part_from_photo_scaled(rel, 46, 320, 180, "card")
            asset = len(self.asset_rows)
            self.card_assets[int(path.stem)] = asset
            self.asset_rows.append((part, 1, KIND_PHOTO, self.string(f"card_{path.stem}")))

    def _add_title(self):
        im = Image.open(self.root / "docs" / "title-static.png").convert("RGBA")
        art = im.crop((0, 0, 432, 243)).convert("RGB").resize((320, 180), Image.LANCZOS)
        buf = io.BytesIO()
        art.save(buf, "JPEG", quality=self.args.quality_title, optimize=True,
                 subsampling=2, progressive=False)
        jpeg = buf.getvalue()
        self._align(self.image_blob)
        off = len(self.image_blob)
        self.image_blob += jpeg
        idx = len(self.part_records)
        self.part_records.append((off, len(jpeg), 0, 0, 320, 180, 0, 0, 320, 180, 0))
        self.stats["title"]["files"] += 1
        self.stats["title"]["jpeg"] += len(jpeg)
        self.title_asset = len(self.asset_rows)
        self.asset_rows.append((idx, 1, KIND_PHOTO, self.string("@title")))

    # ----------------------------------------------------------------- views
    def build_views(self, chunk_data):
        for data in chunk_data:
            for layers in data["views_raw"]:
                key = tuple(tuple(layer) for layer in layers)
                if key in self.view_index:
                    continue
                first = len(self.layer_ref)
                for layer in layers:
                    asset, x, y, w, h, opacity, _z = layer
                    lkey = (self.asset_index[asset], x, y, w, h, opacity)
                    lidx = self.layer_index.get(lkey)
                    if lidx is None:
                        lidx = len(self.layer_records)
                        self.layer_index[lkey] = lidx
                        self.layer_records.append((self.asset_index[asset], x, y, w, h,
                                                   opacity))
                    self.layer_ref.append(lidx)
                self.view_index[key] = len(self.view_records)
                self.view_records.append((first, len(layers)))

    # ---------------------------------------------------------------- chunks
    def _phone_text(self, group: int, index: int) -> str:
        if not hasattr(self, "_phone_cache"):
            self._phone_cache = {}
        rows = self._phone_cache.get(group)
        if rows is None:
            path = self.common / "phone" / f"{group}.txt"
            rows = json.loads(path.read_bytes().decode("utf-8")) if path.exists() else []
            self._phone_cache[group] = rows
        return rows[index] if 0 <= index < len(rows) else ""

    def _add_rine_msg(self, key, msg_id, base, contact, sender, date, flags, kind,
                      text, stamp_id=None):
        if key in self.rine_msg_index:
            return self.rine_msg_index[key]
        text_idx = 0xFFFF if not text else self.string(text)
        stamp_asset = 0xFFFF
        if stamp_id is not None:
            stamp_asset = self.stamp_assets.get(stamp_id, 0xFFFF)
        date_idx = self.date(int(date)) if date else self.date(0)
        idx = len(self.rine_msg)
        self.rine_msg.append({
            "id": int(msg_id) & 0xFFFF, "base": int(base) & 0xFFFF,
            "date": date_idx, "contact": int(contact) & 0xFF,
            "sender": int(sender) & 0xFF,
            "flags": (1 if flags & 1 else 0) | (2 if flags & 2 else 0),
            "kind": kind, "text": text_idx, "stamp": stamp_asset,
        })
        self.rine_msg_index[key] = idx
        return idx

    def build_rine(self):
        self.rine_msg = []
        self.rine_msg_index = {}
        self.rine_choices = []
        self.rine_seq = []
        self.rine_replies = []
        self.rine_reply_index = {}
        # 1. Event messages and expire fallbacks, in node/event order.
        for node_id in self.node_order:
            for ei, ev in enumerate(self.nodes[node_id].get("events", [])):
                if ev.get("type") == "message":
                    stamp = int(ev["id"]) if ev.get("stamp") else None
                    text = "" if stamp is not None else self._phone_text(int(ev["id"]) // 256,
                                                                        int(ev["id"]) % 256)
                    base = int(ev.get("base", (int(ev["id"]) // 256) * 256))
                    self._add_rine_msg(f"s:{node_id}:{ei}", int(ev["id"]), base,
                                       int(ev.get("contact", 0)), int(ev.get("sender", 0)),
                                       int(ev.get("date", 0)),
                                       (1 if ev.get("outgoing") else 0) |
                                       (2 if ev.get("stamp") else 0),
                                       0, text, stamp)
                elif ev.get("type") == "expireRine":
                    for i, m in enumerate(ev.get("readMessages", [])):
                        stamp = int(m["id"]) if m.get("stamp") else None
                        text = "" if stamp is not None else self._phone_text(
                            int(m["id"]) // 256, int(m["id"]) % 256)
                        base = int(ev.get("base", (int(m["id"]) // 256) * 256))
                        self._add_rine_msg(f"f:{node_id}:{ei}:{i}", int(m["id"]), base,
                                           int(m.get("contact", 0)), int(m.get("sender", 1)),
                                           int(m.get("date", ev.get("date", 0))),
                                           (1 if m.get("outgoing") else 0) |
                                           (2 if m.get("stamp") else 0),
                                           3, text, stamp)
        # 2. Reply windows, choices and sequences (stable source-file order).
        for node_id in self.node_order:
            for ei, ev in enumerate(self.nodes[node_id].get("events", [])):
                if ev.get("type") != "rineReply":
                    continue
                key = ev.get("key", "")
                if key in self.rine_reply_index:
                    continue
                path = self.common / "reply" / f"{key}.txt"
                if not path.exists():
                    raise SystemExit(f"missing reply file {path}")
                record = json.loads(path.read_bytes().decode("utf-8"))
                base = int(record.get("base", ev.get("base", 0)))
                contact = int(record.get("contact", ev.get("contact", 0)))
                stamp_choices = bool(record.get("stampChoices"))
                choice_first = len(self.rine_choices)
                choices = []
                for ci, rep in enumerate(record.get("replies", [])):
                    sel_id = int(rep["id"])
                    label = "" if stamp_choices else self._phone_text(base // 256, sel_id % 256)
                    sel = self._add_rine_msg(f"r:{base}:{ci}:sel", sel_id, base, contact, 0,
                                             0, 1 | (2 if stamp_choices else 0), 1,
                                             label, sel_id if stamp_choices else None)
                    first_seq = len(self.rine_seq)
                    for si, row in enumerate(rep.get("sequence", [])):
                        stamp = int(row["id"]) if row.get("stamp") else None
                        text = "" if stamp is not None else self._phone_text(
                            int(row["id"]) // 256, int(row["id"]) % 256)
                        ref = self._add_rine_msg(f"r:{base}:{ci}:{si}", int(row["id"]), base,
                                                 contact, int(row.get("sender", 1)),
                                                 int(row.get("date", 0)),
                                                 (1 if row.get("outgoing") else 0) |
                                                 (2 if row.get("stamp") else 0),
                                                 2, text, stamp)
                        self.rine_seq.append(ref)
                    self.rine_choices.append((sel, first_seq,
                                              len(rep.get("sequence", [])),
                                              1 if stamp_choices else 0))
                    choices.append(ci)
                self.rine_reply_index[key] = len(self.rine_replies)
                self.rine_replies.append((base, contact, len(choices), choice_first,
                                          self.string(key)))
        # 3. Contact table (source order; id is the original contact id).
        self.rine_contacts = []
        names = self.graph["phone"].get("names", {})
        numbers = self.graph["phone"].get("contactNumbers", {})
        for cid in self.graph["phone"].get("contacts", []):
            name = names.get(str(cid)) or self.graph["phone"].get("contactNames", {}).get(str(cid), "")
            number = numbers.get(str(cid), "")
            self.rine_contacts.append((int(cid) & 0xFF, self.string(name),
                                       self.string(number)))
        self.rine_bases = [int(x) & 0xFFFF for x in
                           self.graph["phone"].get("replyBases", [])]
        self.card_rows = [self.card_assets.get(g, 0xFFFF) for g in range(14)]

    def build_chunks(self):
        unique: dict[str, dict] = {}
        chunk_data = []
        for node_id in self.node_order:
            for part in self.nodes[node_id]["chunks"]:
                rel = part["file"]
                if rel in unique:
                    continue
                raw = (self.common / "story" / rel).read_bytes()
                data = json.loads(raw.decode("utf-8"))
                if len(data["frames"]) != part["count"]:
                    raise SystemExit(
                        f"{rel}: {len(data['frames'])} frames != graph count {part['count']}")
                item = {"file": rel, "views_raw": data["views"], "frames": data["frames"]}
                unique[rel] = item
                chunk_data.append(item)
        self.build_assets()
        self.build_views(chunk_data)
        # Encode one zlib stream per unique chunk: FRAME table + UTF-8 text.
        for item in chunk_data:
            frames = bytearray()
            text = bytearray()
            for row in item["frames"]:
                speaker, body, view, date, group, _site, _script, _contacts, _world = row
                encoded = body.encode("utf-8")
                view_key = tuple(tuple(layer) for layer in item["views_raw"][view])
                frames += FRAME.pack(len(text), len(encoded), self.view_index[view_key],
                                     self.speaker(speaker), group, self.date(date), 0)
                text += encoded
            blob = bytes(frames) + bytes(text)
            if len(blob) > 0xFFFF:
                raise SystemExit(f"{item['file']}: raw chunk {len(blob)} > 65535")
            item["raw"] = blob
            item["z"] = zlib.compress(blob, self.args.zlib_level)
        self.chunk_data = chunk_data
        self.chunk_by_file = unique
        self.report["counts_text"] = {
            "chunks_unique": len(unique),
            "chunks_referenced": sum(len(self.nodes[n]["chunks"]) for n in self.node_order),
            "frames_unique": sum(len(d["frames"]) for d in chunk_data),
            "text_raw_bytes": sum(len(d["raw"]) for d in chunk_data),
            "text_zlib_bytes": sum(len(d["z"]) for d in chunk_data),
        }

    # ------------------------------------------------------------ graph data
    def build_graph(self):
        chunk_rows = []
        node_chunk_refs = {}
        for node_id in self.node_order:
            first = len(chunk_rows)
            rels = [p["file"] for p in self.nodes[node_id]["chunks"]]
            for rel in rels:
                item = self.chunk_by_file[rel]
                chunk_rows.append((item, len(item["z"]), len(item["raw"]),
                                   len(item["frames"])))
            node_chunk_refs[node_id] = (first, len(rels))
        self.chunk_rows = chunk_rows
        # starts: keep source order and attach to nodes
        starts = self.graph["starts"]
        self.start_list = list(starts.items())
        start_ranges: dict[str, list[int]] = {}
        for i, (_key, item) in enumerate(self.start_list):
            start_ranges.setdefault(item["node"], []).append(i)
        node_records = []
        choice_rows = []
        cond_rows = []
        ending_rows = []
        break_rows = []
        chgrp_rows = []
        event_rows = []
        for node_id in self.node_order:
            node = self.nodes[node_id]
            choice_idx = NONE16
            if node.get("choice"):
                choice = node["choice"]
                items = choice["items"]
                if len(items) != 2:
                    raise SystemExit(f"{node_id}: choice item count {len(items)}")
                choice_idx = len(choice_rows)
                texts = [self.string(it["text"]) for it in items]
                actions = [0 if it.get("action") == "accept" else 1 for it in items]
                targets = [self.node_index[it["to"]] for it in items]
                choice_rows.append(CHOICE.pack(
                    0 if choice.get("kind") == "call" else 1,
                    1 if choice.get("critical") else 0,
                    self.string(choice.get("heading", "")),
                    self.string(choice.get("category", "")), 2, 0,
                    targets[0], targets[1], actions[0], actions[1],
                    texts[0], texts[1], 0))
            cond_idx = NONE16
            if node.get("condition"):
                cond = node["condition"]
                cond_idx = len(cond_rows)
                cond_rows.append(COND.pack(0, self.node_index[cond["yes"]],
                                           self.node_index[cond["no"]]))
            ending_idx = -1
            if "endingId" in node:
                ending_idx = len(ending_rows)
                ending_rows.append(ENDING.pack(self.string(node["ending"]),
                                               node["endingId"], 0))
            brk_first = len(break_rows)
            brk = node.get("breaks", [])
            break_rows.extend(brk)
            ch_first = len(chgrp_rows)
            chs = node.get("chapters", [])
            for at, group in chs:
                chgrp_rows.append(CHGRP.pack(at, group, 0))
            ev_first = len(event_rows)
            for ei, ev in enumerate(node.get("events", [])):
                event_rows.append(self._event(ev, node_id, ei))
            st_first, st_count = (start_ranges[node_id][0], len(start_ranges[node_id])) \
                if node_id in start_ranges else (0, 0)
            cr = node_chunk_refs[node_id]
            node_records.append(NODE.pack(
                cr[0], cr[1], choice_idx, cond_idx, ending_idx, 0,
                brk_first, len(brk), ch_first, len(chs), ev_first,
                len(node.get("events", [])), st_first, st_count, 0))
        self.node_records = node_records
        self.choice_rows = choice_rows
        self.cond_rows = cond_rows
        self.ending_rows = ending_rows
        self.break_rows = break_rows
        self.chgrp_rows = chgrp_rows
        self.event_rows = event_rows
        self.start_rows = []
        for _key, item in self.start_list:
            self.start_rows.append(START.pack(
                self.node_index[item["node"]], item["group"], item["script"],
                item["index"], self.date(item["date"]), self.string(item.get("name", "")), 0))
        self.endlist_rows = [self.string(name) for name in self.graph.get("endings", [])]

    def _event(self, ev, node_id, event_index):
        kind = ev.get("type")
        at = ev.get("at", 0)
        if kind == "closeChoice":
            return EVENT.pack(at, 0, 0, 0, 0, 0, 0, 0, 0)
        if kind == "message":
            flags = (1 if ev.get("outgoing") else 0) | (2 if ev.get("stamp") else 0)
            a = (int(ev.get("contact", 0)) & 0xFF) | ((int(ev.get("sender", 0)) & 0xFF) << 8)
            data = self.rine_msg_index[f"s:{node_id}:{event_index}"]
            return EVENT.pack(at, 1, flags, a, int(ev.get("id", 0)) & 0xFFFF,
                              int(ev.get("base", 0)) & 0xFFFF,
                              self.date(int(ev.get("date", 0))), data, 0)
        if kind == "rineReset":
            return EVENT.pack(at, 2, 1 if ev.get("scope") == "display" else 0, 0, 0, 0, 0,
                              0, 0)
        if kind == "rineReply":
            data = self.rine_reply_index[ev.get("key", "")]
            return EVENT.pack(at, 3, 0, int(ev.get("contact", 0)) & 0xFF,
                              self.string(ev.get("key", "")),
                              int(ev.get("base", 0)) & 0xFFFF, 0, data, 0)
        if kind == "expireRine":
            count = len(ev.get("readMessages", []))
            first = (self.rine_msg_index[f"f:{node_id}:{event_index}:0"]
                     if count else 0xFFFF)
            # JS truthiness: an empty `readMessages` array still suppresses the
            # default message, so presence is recorded independently of count.
            flags = 2 if "readMessages" in ev else 0
            return EVENT.pack(at, 4, flags, 0, count,
                              int(ev.get("base", 0)) & 0xFFFF, 0, first, 0)
        raise SystemExit(f"unknown event type {kind}")

    # --------------------------------------------------------------- assemble
    def assemble(self):
        self.build_chunks()
        self.build_rine()
        self.build_graph()
        self.rine_msg_rows = [RMSG.pack(m["id"], m["base"], m["date"], m["contact"],
                                        m["sender"], m["flags"], m["kind"], m["text"],
                                        m["stamp"]) for m in self.rine_msg]
        self.rine_reply_rows = [RREPLY.pack(base, contact, count, first, key)
                                for base, contact, count, first, key in self.rine_replies]
        self.rine_choice_rows = [RCHOICE.pack(sel, first, count, stamp, 0)
                                 for sel, first, count, stamp in self.rine_choices]
        self.rine_seq_rows = [struct.pack("<H", ref) for ref in self.rine_seq]
        self.rine_contact_rows = [RCONTACT.pack(cid, name, number)
                                  for cid, name, number in self.rine_contacts]
        self.rine_base_rows = [struct.pack("<H", base) for base in self.rine_bases]
        self.card_rows_bin = [CARD.pack(asset) for asset in self.card_rows]
        # gallery
        gallery_groups = []
        gallery_entries = []
        for group in self.gallery:
            first = len(gallery_entries)
            for asset in group["images"]:
                gallery_entries.append(GE.pack(self.asset_index[asset]))
            gallery_groups.append(GG.pack(self.string(group["label"]), first,
                                          len(group["images"]), 0))
        self.gallery_group_count = len(gallery_groups)
        self.gallery_entry_count = len(gallery_entries)
        # chunk stream area
        chunk_area = bytearray()
        for item in self.chunk_data:
            item["zoff"] = len(chunk_area)
            chunk_area += item["z"]
        self._align(chunk_area)
        image_base = len(chunk_area)
        blob = bytearray(chunk_area) + bytearray(self.image_blob)
        chunk_rows = []
        for item, zlen, raw_len, frames in self.chunk_rows:
            chunk_rows.append(CHUNK.pack(item["zoff"], zlen, raw_len, frames, 0))
        # PART offsets are relative to the image area, rebase them onto BLOB.
        part_rows = [PART.pack(p[0] + image_base, p[1],
                               (p[2] + image_base) if p[3] else 0, p[3],
                               p[4], p[5], p[6], p[7], p[8], p[9], p[10])
                     for p in self.part_records]
        asset_rows = [ASSETID.pack(a, b, c, d, 0) for a, b, c, d in self.asset_rows]
        view_rows = [VIEW.pack(first, count, 0) for first, count in self.view_records]
        layer_ref = [struct.pack("<H", idx) for idx in self.layer_ref]
        layer_rows = [LAYER.pack(*row) for row in self.layer_records]
        # text/string/speaker/date tables
        text_buf = self.text.buf
        offsets = self.text.offsets()
        strt_rows = [STRT.pack(off, ln, 0) for off, ln in offsets]
        speaker_rows = [struct.pack("<H", self.string(name)) for name in self.speakers]
        date_rows = [struct.pack("<I", value) for value in self.dates]
        self.report["counts"] = self._counts()
        meta = self._meta()
        sections = {
            SEC_TEXT: (bytes(text_buf), len(strt_rows)),
            SEC_STRT: (b"".join(strt_rows), len(strt_rows)),
            SEC_NODE: (b"".join(self.node_records), len(self.node_records)),
            SEC_CHUNK: (b"".join(chunk_rows), len(chunk_rows)),
            SEC_FRAME: (b"", 0),
            SEC_VIEW: (b"".join(view_rows), len(view_rows)),
            SEC_LAYER: (b"".join(layer_rows), len(layer_rows)),
            SEC_CHOICE: (b"".join(self.choice_rows), len(self.choice_rows)),
            SEC_COND: (b"".join(self.cond_rows), len(self.cond_rows)),
            SEC_ENDING: (b"".join(self.ending_rows), len(self.ending_rows)),
            SEC_ENDLIST: (b"".join(struct.pack("<H", s) for s in self.endlist_rows),
                          len(self.endlist_rows)),
            SEC_BREAK: (b"".join(struct.pack("<H", v) for v in self.break_rows),
                        len(self.break_rows)),
            SEC_CHGRP: (b"".join(self.chgrp_rows), len(self.chgrp_rows)),
            SEC_EVENT: (b"".join(self.event_rows), len(self.event_rows)),
            SEC_START: (b"".join(self.start_rows), len(self.start_rows)),
            SEC_ASSETID: (b"".join(asset_rows), len(asset_rows)),
            SEC_PART: (b"".join(part_rows), len(part_rows)),
            SEC_BLOB: (bytes(blob), len(blob)),
            SEC_GG: (b"".join(gallery_groups), len(gallery_groups)),
            SEC_GE: (b"".join(gallery_entries), len(gallery_entries)),
            SEC_META: (meta.encode("utf-8"), len(meta)),
            SEC_SPK: (b"".join(speaker_rows), len(speaker_rows)),
            SEC_DATE: (b"".join(date_rows), len(date_rows)),
            SEC_LAYERREF: (b"".join(layer_ref), len(layer_ref)),
            SEC_TITLE: (b"".join(struct.pack("<H", self.string(t))
                                 for t in self.graph.get("titles", [])),
                        len(self.graph.get("titles", []))),
            SEC_RMSG: (b"".join(self.rine_msg_rows), len(self.rine_msg_rows)),
            SEC_RREPLY: (b"".join(self.rine_reply_rows), len(self.rine_reply_rows)),
            SEC_RCHOICE: (b"".join(self.rine_choice_rows), len(self.rine_choice_rows)),
            SEC_RSEQ: (b"".join(self.rine_seq_rows), len(self.rine_seq_rows)),
            SEC_RCONTACT: (b"".join(self.rine_contact_rows), len(self.rine_contact_rows)),
            SEC_RBASE: (b"".join(self.rine_base_rows), len(self.rine_base_rows)),
            SEC_CARD: (b"".join(self.card_rows_bin), len(self.card_rows_bin)),
        }
        # header + table + payloads
        header_size = HEADER.size + SECTION.size * SECTION_COUNT
        payload = bytearray()
        table = []
        for sec_type in range(SECTION_COUNT):
            data, count = sections[sec_type]
            while len(payload) % 4:
                payload.append(0)
            offset = header_size + len(payload)
            payload += data
            table.append(SECTION.pack(sec_type, offset, count, len(data)))
        total = header_size + len(payload)
        out = bytearray(HEADER.pack(MAGIC, VERSION, HEADER.size, SECTION_COUNT, 0, total))
        out += b"".join(table)
        out += payload
        self.section_sizes = {
            str(t): {"offset": struct.unpack_from("<I", table[t], 4)[0],
                     "count": sections[t][1], "bytes": len(sections[t][0])}
            for t in range(SECTION_COUNT)
        }
        self.stats_out = {
            "chunk_streams": len(self.chunk_data),
            "chunk_stream_bytes": sum(len(d["z"]) for d in self.chunk_data),
            "image_blob_bytes": len(self.image_blob),
            "parts": len(self.part_records),
            "views": len(self.view_records),
            "layers_unique": len(self.layer_records),
            "layer_refs": len(self.layer_ref),
        }
        self.report["images"] = {
            role: dict(stats, psnr_avg=round(sum(self.part_psnr.get(role, [])) /
                                             len(self.part_psnr[role]), 2)
                       if self.part_psnr.get(role) else None)
            for role, stats in self.stats.items()
        }
        self.report["layout"] = self.stats_out
        self.report["title_asset"] = self.title_asset
        return bytes(out)

    def _counts(self):
        return {
            "nodes": len(self.node_records),
            "chunks_unique": len(self.chunk_data),
            "chunks_referenced": len(self.chunk_rows),
            "frames_unique": sum(len(d["frames"]) for d in self.chunk_data),
            "frames_referenced": sum(row[3] for row in self.chunk_rows),
            "units_unique": sum(1 for d in self.chunk_data for row in d["frames"]
                                for cell in (row[0], row[1]) if cell),
            "codepoints_all_unique": sum(len(row[0]) + len(row[1])
                                         for d in self.chunk_data for row in d["frames"]),
            "pua_glyphs_unique": len({ord(ch) - 0xE000 for d in self.chunk_data
                                      for row in d["frames"] for cell in (row[0], row[1])
                                      for ch in cell if 0xE000 <= ord(ch) <= 0xF8FF}),
            "choices": len(self.choice_rows),
            "conditions": len(self.cond_rows),
            "endings": len(self.endlist_rows),
            "ending_nodes": len(self.ending_rows),
            "starts": len(self.start_rows),
            "events": len(self.event_rows),
            "speakers": len(self.speakers),
            "dates": len(self.dates),
            "gallery_groups": self.gallery_group_count,
            "gallery_entries": self.gallery_entry_count,
            "asset_ids": len(self.asset_rows),
            "asset_files": len(self.unique_image_files),
            "views": len(self.view_records),
            "layers_unique": len(self.layer_records),
            "titles": len(self.graph.get("titles", [])),
            "rine_msgs": len(self.rine_msg),
            "rine_replies": len(self.rine_replies),
            "rine_choices": len(self.rine_choices),
            "rine_seq": len(self.rine_seq),
            "rine_contacts": len(self.rine_contacts),
            "rine_bases": len(self.rine_bases),
            "chapter_cards": len(self.card_rows),
        }

    def _meta(self):
        c = self.report.get("counts", {})
        lines = [
            "format=1",
            "source_graph_sha256=" + self.report["source_graph_sha256"],
            "source_assets_sha256=" + self.report["source_assets_sha256"],
            "source_gallery_sha256=" + self.report["source_gallery_sha256"],
            "source_font_sha256=" + self.report["source_font_sha256"],
            f"quality_photo={self.args.quality_photo}",
            f"quality_body={self.args.quality_body}",
            f"quality_face={self.args.quality_face}",
            f"quality_title={self.args.quality_title}",
            f"title_asset={self.title_asset}",
            f"count_nodes={c.get('nodes', 0)}",
            f"count_chunks_unique={c.get('chunks_unique', 0)}",
            f"count_chunks_referenced={c.get('chunks_referenced', 0)}",
            f"count_frames_unique={c.get('frames_unique', 0)}",
            f"count_frames_referenced={c.get('frames_referenced', 0)}",
            f"count_text_strings_unique={c.get('frames_unique', 0)}",
            f"count_units_unique={c.get('units_unique', 0)}",
            f"count_codepoints_all_unique={c.get('codepoints_all_unique', 0)}",
            f"count_pua_glyphs_unique={c.get('pua_glyphs_unique', 0)}",
            f"count_choices={c.get('choices', 0)}",
            f"count_conditions={c.get('conditions', 0)}",
            f"count_endings={c.get('endings', 0)}",
            f"count_ending_nodes={c.get('ending_nodes', 0)}",
            f"count_starts={c.get('starts', 0)}",
            f"count_events={c.get('events', 0)}",
            f"count_speakers={c.get('speakers', 0)}",
            f"count_dates={c.get('dates', 0)}",
            f"count_gallery_groups={c.get('gallery_groups', 0)}",
            f"count_gallery_entries={c.get('gallery_entries', 0)}",
            f"count_asset_ids={c.get('asset_ids', 0)}",
            f"count_asset_files={c.get('asset_files', 0)}",
            f"count_views={c.get('views', 0)}",
            f"count_layers_unique={c.get('layers_unique', 0)}",
            f"count_titles={c.get('titles', 0)}",
            f"count_rine_msgs={c.get('rine_msgs', 0)}",
            f"count_rine_replies={c.get('rine_replies', 0)}",
            f"count_rine_choices={c.get('rine_choices', 0)}",
            f"count_rine_seq={c.get('rine_seq', 0)}",
            f"count_rine_contacts={c.get('rine_contacts', 0)}",
            f"count_rine_bases={c.get('rine_bases', 0)}",
            f"count_chapter_cards={c.get('chapter_cards', 0)}",
        ]
        return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, help="steins0-rw5 checkout")
    ap.add_argument("--out", default="assets/steins0/steins0_pack.bin")
    ap.add_argument("--report", default="build/steins0_pack_report.json")
    ap.add_argument("--quality-photo", type=int, default=38)
    ap.add_argument("--quality-body", type=int, default=48)
    ap.add_argument("--quality-face", type=int, default=58)
    ap.add_argument("--quality-title", type=int, default=70)
    ap.add_argument("--zlib-level", type=int, default=9)
    ap.add_argument("--no-title", action="store_true")
    args = ap.parse_args()
    args.title_art = not args.no_title
    root = Path(args.source)
    if not (root / "src" / "common" / "graph.txt").exists():
        sys.exit(f"{root} is not a steins0-rw5 checkout")
    packer = Packer(root, args)
    blob = packer.assemble()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    report = packer.report
    report["pack"] = {
        "path": str(out), "bytes": len(blob), "sha256": sha256(blob),
        "sections": packer.section_sizes,
    }
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n",
                                 encoding="utf-8")
    print(f"pack: {out} {len(blob)} B sha256 {sha256(blob)}")
    print(f"chunks {packer.report['counts']['chunks_unique']} "
          f"frames {packer.report['counts']['frames_unique']} "
          f"assets {packer.report['counts']['asset_ids']} "
          f"views {packer.report['counts']['views']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
