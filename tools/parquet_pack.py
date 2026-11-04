#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""《PARQUET》资源打包器：RW5 源快照 -> AI Passport 直读资源包。

用法:
  python tools/parquet_pack.py --source assets/parquet-source \
      --images <parquet-rw5>/src --out main/parquet_data

输入（只读）:
  assets/parquet-source/story/<scene>/<chunk>.txt   RW5 运行时剧本（每块 32 条 op）
  assets/parquet-source/catalog.json                场景/章节/路线/结局/入口/clearFlag
  assets/parquet-source/image-map.json              资源 id -> 图文件、立绘 base/patch 与脸部锚点
  assets/parquet-source/cutin-map.json              cut-in 尺寸与位置
  assets/parquet-source/gallery.txt                 CG 鉴赏分组（37 组 / 367 张）
  <images>/common/images/p014|s015/*                立绘与背景/CG（不入库，由源提供）
  <images>/common/cutins/*                          cut-in 图
  <images>/common/ui/title-{0,1,2}.jpg, title-logo.png  标题背景/Logo

输出:
  main/parquet_data/parquet_scn.bin   剧本包（场景表 + 16B 定长 op + 压缩正文块 + 名称表）
  main/parquet_data/parquet_img.bin   图片包（JPEG 背景/CG、BLOCK 立绘/cut-in、PATCH 表情）
  assets/parquet-source/parquet_symbols.txt  字库字符集（交给 tools/gal_font.py）
  build/reports/parquet-pack.json     本次打包统计与 SHA-256

格式与 main/parquet/parquet_pack.h 一一对应；独立源审计见 tools/parquet_audit.py
（审计工具不 import 本文件）。
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import struct
import sys
import zlib
from collections import OrderedDict

from PIL import Image

SCN_MAGIC = b"PQSCN1\0\0"
IMG_MAGIC = b"PQIMG1\0\0"
VERSION = 1

ART_W, ART_H = 320, 150       # 画面区；下方 90px 对话框
PORTRAIT_CANVAS_H = 262       # 源立绘画布高（432x286 美术空间）
PORTRAIT_SCALE = ART_H / PORTRAIT_CANVAS_H
CUTIN_CANVAS_H = 206
CUTIN_SCALE = ART_H / 286
CUTIN_TOP = round(51 * ART_H / 286)
TITLE_W, TITLE_H = 320, 150
TITLE_ANCHOR = 0.35

BLOCK_ROWS = 8
MAX_COLORS = 255
PATCH_MAX_PX = 4096
JPEG_QUALITY_DEFAULT = 62
TEXT_BLOCK_OPS = 32
TEXT_MAX_BYTES = 512   # 与 parquet_game.h 的 PQ_TEXT_MAX 一致

OP_TEXT, OP_JUMP, OP_CHOICE, OP_END = 0, 1, 2, 3
IMG_JPEG, IMG_BLOCK, IMG_PATCH = 0, 1, 2
CHOICE_ITEM_SIZE = 18

SCN_HEADER = 128
IMG_HEADER = 64


def die(msg: str) -> None:
    print(f"parquet_pack: {msg}", file=sys.stderr)
    sys.exit(1)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_json(path: str):
    with io.open(path, encoding="utf-8") as fh:
        return json.load(fh)


def load_story(source: str) -> "OrderedDict[int, list]":
    """按源运行时结构读剧本：story/<scene>/<chunk>.txt，每块最多 32 条 op。"""
    root = os.path.join(source, "story")
    if not os.path.isdir(root):
        die(f"缺少剧本目录 {root}")
    scenes = OrderedDict()
    for name in sorted((n for n in os.listdir(root) if n.isdigit()), key=int):
        d = os.path.join(root, name)
        if not os.path.isdir(d):
            continue
        ops = []
        chunks = sorted((n for n in os.listdir(d) if n.endswith(".txt")),
                        key=lambda n: int(n[:-4]))
        if not chunks:
            die(f"{d}: 没有剧本块")
        for chunk in chunks:
            data = load_json(os.path.join(d, chunk))
            if not isinstance(data, list) or not data:
                die(f"{d}/{chunk}: 剧本块不是非空数组")
            ops.extend(data)
        scenes[int(name)] = ops
    if not scenes:
        die("剧本目录为空")
    return scenes


def validate_ops(scenes, catalog) -> None:
    for sid, ops in scenes.items():
        meta = catalog["scenes"].get(str(sid))
        if not meta:
            die(f"场景 {sid} 不在 catalog.scenes")
        if meta["length"] != len(ops):
            die(f"场景 {sid} 长度 {len(ops)} != catalog {meta['length']}")
        for i, op in enumerate(ops):
            kind = op.get("op")
            if kind == "text":
                if not isinstance(op.get("text"), str) or not op["text"]:
                    die(f"场景 {sid} op {i}: text 为空")
                if len(op["text"].encode("utf-8")) > TEXT_MAX_BYTES:
                    die(f"场景 {sid} op {i}: 正文超过 {TEXT_MAX_BYTES} 字节")
                if op.get("view") not in ("normal", "another"):
                    die(f"场景 {sid} op {i}: view 非法 {op.get('view')!r}")
                if not isinstance(op.get("bodies"), list) or len(op["bodies"]) > 3:
                    die(f"场景 {sid} op {i}: bodies 非法")
            elif kind == "jumpScene":
                tgt = catalog["scenes"].get(str(op.get("scene")))
                if not tgt:
                    die(f"场景 {sid} op {i}: 跳转目标 {op.get('scene')} 不存在")
                if not isinstance(op.get("to"), int) or not (0 <= op["to"] < tgt["length"]):
                    die(f"场景 {sid} op {i}: 跳转位置 {op.get('to')} 越界")
            elif kind == "choice":
                items = op.get("items") or []
                if not (1 <= len(items) <= 4):
                    die(f"场景 {sid} op {i}: 选项数量 {len(items)} 非法")
                for it in items:
                    tgt = catalog["scenes"].get(str(it.get("scene")))
                    if not tgt or not (0 <= it.get("to", -1) < tgt["length"]):
                        die(f"场景 {sid} op {i}: 选项目标非法")
                    if not it.get("text"):
                        die(f"场景 {sid} op {i}: 选项文本为空")
                    if len(it["text"].encode("utf-8")) > TEXT_MAX_BYTES:
                        die(f"场景 {sid} op {i}: 选项文本超过 {TEXT_MAX_BYTES} 字节")
                    eff = it.get("effect")
                    if eff is not None and (not isinstance(eff, list) or len(eff) != 2
                                            or not isinstance(eff[0], str)
                                            or not isinstance(eff[1], int)):
                        die(f"场景 {sid} op {i}: 选项 effect 非法")
            elif kind == "end":
                pass
            else:
                die(f"场景 {sid} op {i}: 未知 op {kind!r}")


# ----------------------------------------------------------------- 图片编码
def fit_in(img: Image.Image, w: int, h: int) -> Image.Image:
    s = min(w / img.width, h / img.height, 1.0)
    return img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))),
                      Image.LANCZOS)


def cover_band(img: Image.Image, w: int, h: int, anchor: float) -> Image.Image:
    s = max(w / img.width, h / img.height)
    nw, nh = max(w, round(img.width * s)), max(h, round(img.height * s))
    r = img.resize((nw, nh), Image.LANCZOS)
    left = (nw - w) // 2
    top = int(max(0, min(nh - h, round((nh - h) * anchor))))
    return r.crop((left, top, left + w, top + h))


def jpeg_bytes(img: Image.Image, quality: int) -> bytes:
    buf = io.BytesIO()
    img.convert("RGB").save(buf, "JPEG", quality=quality, optimize=True, subsampling=2)
    return buf.getvalue()


_FILTERS = ("none", "sub", "up", "average", "paeth")


def _filter_row(row, prev, kind: str) -> bytes:
    w = len(row)
    out = bytearray(w)
    if kind == "none":
        return bytes(row)
    for i in range(w):
        x = row[i]
        a = row[i - 1] if i else 0
        b = prev[i]
        c = prev[i - 1] if i else 0
        if kind == "sub":
            pred = a
        elif kind == "up":
            pred = b
        elif kind == "average":
            pred = (a + b) >> 1
        else:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
        out[i] = (x - pred) & 0xFF
    return bytes(out)


def quantize_rgba(img: Image.Image):
    rgba = img.convert("RGBA")
    q = rgba.quantize(colors=MAX_COLORS, method=Image.FASTOCTREE)
    idx = q.load()
    w, h = rgba.size
    px = rgba.load()
    sums = {}
    for y in range(h):
        for x in range(w):
            k = idx[x, y]
            a = px[x, y][3]
            s = sums.setdefault(k, [0, 0])
            s[0] += a
            s[1] += 1
    pal = q.getpalette() or []
    n = max(idx[x, y] for y in range(h) for x in range(w)) + 1
    palette, alphas = [], []
    for i in range(n):
        r = pal[3 * i] if 3 * i < len(pal) else 0
        g = pal[3 * i + 1] if 3 * i + 1 < len(pal) else 0
        b = pal[3 * i + 2] if 3 * i + 2 < len(pal) else 0
        s = sums.get(i)
        a = int(s[0] / s[1]) if s and s[1] else 0
        palette.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
        alphas.append(a)
    return idx, palette, alphas, (w, h)


def encode_block(img: Image.Image) -> bytes:
    """BLOCK：调色板 + 每索引 8bit alpha + 8 行一块 zlib（parquet_gfx.c 对应）。"""
    idx, palette, alphas, (w, h) = quantize_rgba(img)
    blocks, lens = [], []
    prev = bytes(w)
    nblocks = (h + BLOCK_ROWS - 1) // BLOCK_ROWS
    for b in range(nblocks):
        chunk = bytearray()
        for y in range(b * BLOCK_ROWS, min(h, (b + 1) * BLOCK_ROWS)):
            row = bytes(idx[x, y] for x in range(w))
            best, best_key, best_name = None, None, "none"
            for k in _FILTERS:
                cand = _filter_row(row, prev, k)
                key = sum(abs(v - 256 if v > 127 else v) for v in cand)
                if best_key is None or key < best_key:
                    best, best_key, best_name = cand, key, k
            chunk.append(_FILTERS.index(best_name))
            chunk.extend(best)
            prev = row
        comp = zlib.compress(bytes(chunk), 9)
        lens.append(len(comp))
        blocks.append(comp)
    head = struct.pack("<6HI", w, h, len(palette), BLOCK_ROWS, nblocks, 0,
                       BLOCK_ROWS * (w + 1))
    out = bytearray(head)
    out += struct.pack(f"<{len(palette)}H", *palette)
    out += bytes(alphas)
    out += struct.pack(f"<{nblocks}I", *lens)
    for c in blocks:
        out += c
    return bytes(out)


def encode_patch(img: Image.Image) -> bytes:
    """PATCH：RGB565 + 8bit alpha，8 行一块 zlib。裁剪后的表情小图专用。"""
    rgba = img.convert("RGBA")
    w, h = rgba.size
    px = rgba.load()
    alpha = bytearray(w * h)
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b, a = px[x, y]
            row += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
            alpha[y * w + x] = a
        rows.append(bytes(row))
    nblocks = (h + BLOCK_ROWS - 1) // BLOCK_ROWS
    a_z = zlib.compress(bytes(alpha), 9)
    body = bytearray()
    lens = []
    for b in range(nblocks):
        chunk = b"".join(rows[b * BLOCK_ROWS:(b + 1) * BLOCK_ROWS])
        comp = zlib.compress(chunk, 9)
        lens.append(len(comp))
        body += comp
    head = struct.pack("<4HI", w, h, BLOCK_ROWS, nblocks, len(a_z))
    out = bytearray(head)
    out += a_z
    out += struct.pack(f"<{nblocks}I", *lens)
    out += body
    return bytes(out)


class ImagePack:
    def __init__(self):
        self.entries = []
        self.by_key = {}
        self.data = bytearray()
        self.offsets = []

    def add(self, key: str, kind: int, w: int, h: int, data: bytes) -> int:
        if key in self.by_key:
            return self.by_key[key]
        idx = len(self.entries)
        self.by_key[key] = idx
        self.entries.append({"kind": kind, "w": w, "h": h, "len": len(data)})
        self.offsets.append(len(self.data))
        self.data += data
        return idx

    def serialize(self, sprites, cutins, titles, asset_map, asset_cutin) -> bytes:
        off_entries = IMG_HEADER
        off_sprites = off_entries + 16 * len(self.entries)
        off_cutins = off_sprites + 16 * len(sprites)
        off_titles = off_cutins + 12 * len(cutins)
        off_asset = off_titles + 10 * len(titles)
        off_asset_cutin = off_asset + 2 * len(asset_map)
        off_data = off_asset_cutin + 2 * len(asset_cutin)
        head = struct.pack(
            "<8s7H8I",
            IMG_MAGIC, VERSION, len(self.entries), len(sprites), len(cutins),
            len(asset_map), len(titles), 0,
            off_entries, off_sprites, off_cutins, off_titles, off_asset,
            off_asset_cutin, off_data, len(self.data))
        out = bytearray(head)
        out += b"\0" * (IMG_HEADER - len(out))
        for i, e in enumerate(self.entries):
            out += struct.pack("<BBHHIIH", e["kind"], 0, e["w"], e["h"],
                               off_data + self.offsets[i], e["len"], 0)
        for s in sprites:
            out += struct.pack("<HHhhhhhH", *s)
        for c in cutins:
            out += struct.pack("<HhhHHH", *c)
        for t in titles:
            out += struct.pack("<HHHHH", *t)
        out += struct.pack(f"<{len(asset_map)}H", *asset_map)
        out += struct.pack(f"<{len(asset_cutin)}H", *asset_cutin)
        out += self.data
        return bytes(out)


def build_image_pack(source: str, images: str, quality: int, report: dict):
    image_map = load_json(os.path.join(source, "image-map.json"))
    cutin_map = load_json(os.path.join(source, "cutin-map.json"))
    gallery = load_json(os.path.join(source, "gallery.txt"))
    scenes = load_story(source)
    ids = [int(k[1:]) for k in image_map if k.startswith("a") and k[1:].isdigit()]
    if not ids:
        die("image-map 里没有资源 id")
    max_id = max(ids)
    ui_dir = os.path.join(images, "common", "ui")

    used_bg, used_body, used_cutin = set(), set(), set()
    for ops in scenes.values():
        for op in ops:
            if op["op"] != "text":
                continue
            if op.get("background"):
                used_bg.add(op["background"])
            used_body.update(op.get("bodies") or [])
            if op.get("cutIn"):
                used_cutin.add(op["cutIn"])
    gallery_ids = {item["image"] for item in gallery}
    if not used_body:
        die("剧本没有引用任何立绘")
    if not used_bg:
        die("剧本没有引用任何背景")

    pack = ImagePack()
    asset_map = [0xFFFF] * (max_id + 1)
    asset_cutin = [0xFFFF] * (max_id + 1)
    sprites, cutins, titles = [], [], []

    base_entries, patch_entries = {}, {}
    base_bytes = patch_bytes = 0
    for aid in sorted(used_body):
        meta = image_map.get(aid)
        if not meta or not meta.get("portrait"):
            die(f"{aid}: 不是立绘资源")
        base_path = os.path.join(images, meta["src"].lstrip("/"))
        patch_path = os.path.join(images, meta["patch"].lstrip("/"))
        if base_path not in base_entries:
            img = Image.open(base_path).convert("RGBA")
            if img.height != PORTRAIT_CANVAS_H:
                die(f"{base_path}: 立绘画布高 {img.height} != {PORTRAIT_CANVAS_H}")
            sw = max(1, round(img.width * PORTRAIT_SCALE))
            sh = max(1, round(img.height * PORTRAIT_SCALE))
            scaled = img.resize((sw, sh), Image.LANCZOS)
            bbox = scaled.getchannel("A").point(lambda a: 255 if a >= 128 else 0).getbbox()
            if not bbox:
                die(f"{base_path}: 立绘全透明")
            crop = scaled.crop(bbox)
            payload = encode_block(crop)
            e = pack.add("B:" + base_path, IMG_BLOCK, crop.width, crop.height, payload)
            base_entries[base_path] = (e, bbox[0], bbox[1], sw)
            base_bytes += len(payload)
        if patch_path not in patch_entries:
            img = Image.open(patch_path).convert("RGBA")
            if img.size[1] != PORTRAIT_CANVAS_H:
                die(f"{patch_path}: 表情画布高 {img.height} != {PORTRAIT_CANVAS_H}")
            sw = max(1, round(img.width * PORTRAIT_SCALE))
            sh = max(1, round(img.height * PORTRAIT_SCALE))
            scaled = img.resize((sw, sh), Image.LANCZOS)
            bbox = scaled.getchannel("A").point(lambda a: 255 if a >= 128 else 0).getbbox()
            if not bbox:
                patch_entries[patch_path] = (0xFFFF, 0, 0)
            else:
                crop = scaled.crop(bbox)
                if crop.width * crop.height > PATCH_MAX_PX:
                    die(f"{patch_path}: 表情补丁 {crop.size} 超过 {PATCH_MAX_PX}px")
                payload = encode_patch(crop)
                e = pack.add("P:" + patch_path, IMG_PATCH, crop.width, crop.height, payload)
                patch_entries[patch_path] = (e, bbox[0], bbox[1])
                patch_bytes += len(payload)
        body = base_entries[base_path]
        pat = patch_entries[patch_path]
        anchor = meta.get("faceAnchor") or [0, 0]
        anchor_x = int(round(anchor[0] * PORTRAIT_SCALE))
        sprites.append((body[0], pat[0], body[1], body[2], pat[1], pat[2],
                        anchor_x, body[3]))
        asset_map[int(aid[1:])] = 0x8000 | (len(sprites) - 1)

    cutin_index = {}
    cutin_bytes = 0
    for aid in sorted(used_cutin):
        cm = cutin_map.get(aid)
        if not cm:
            die(f"{aid}: cut-in 缺少 cutin-map 条目")
        path = os.path.join(images, cm["src"].lstrip("/"))
        if path in cutin_index:
            asset_cutin[int(aid[1:])] = cutin_index[path]
            continue
        img = Image.open(path).convert("RGBA")
        # cut-in 源 PNG 是 190px 高的内容裁图，显示盒是 cutin-map 里的 206px 高：
        # 统一按显示盒高度换算，保持等比与美术空间比例。
        sh = max(1, round(CUTIN_CANVAS_H * CUTIN_SCALE))
        sw = max(1, round(img.width * (sh / img.height)))
        scaled = img.resize((sw, sh), Image.LANCZOS)
        bbox = scaled.getchannel("A").point(lambda a: 255 if a >= 128 else 0).getbbox()
        if not bbox:
            die(f"{path}: cut-in 全透明")
        crop = scaled.crop(bbox)
        payload = encode_block(crop)
        e = pack.add("C:" + path, IMG_BLOCK, crop.width, crop.height, payload)
        x = ART_W // 2 - sw // 2 + bbox[0]
        y = CUTIN_TOP + bbox[1]
        cutin_index[path] = len(cutins)
        cutins.append((e, x, y, crop.width, crop.height, 0))
        asset_cutin[int(aid[1:])] = len(cutins) - 1
        cutin_bytes += len(payload)

    bg_entries = {}
    bg_count = 0
    bg_bytes = 0
    for aid in sorted(used_bg | gallery_ids):
        meta = image_map.get(aid)
        if not meta:
            die(f"{aid}: image-map 缺少条目")
        if meta.get("portrait"):
            die(f"{aid}: 背景/CG 条目却是立绘")
        path = os.path.join(images, meta["src"].lstrip("/"))
        if path in bg_entries:
            asset_map[int(aid[1:])] = bg_entries[path]
            continue
        img = Image.open(path).convert("RGB")
        fitted = fit_in(img, ART_W, ART_H)
        payload = jpeg_bytes(fitted, quality)
        e = pack.add("J:" + path, IMG_JPEG, fitted.width, fitted.height, payload)
        bg_entries[path] = e
        asset_map[int(aid[1:])] = e
        bg_count += 1
        bg_bytes += len(payload)

    title_bytes = 0
    for name in ("title-0.jpg", "title-1.jpg", "title-2.jpg"):
        path = os.path.join(ui_dir, name)
        img = Image.open(path).convert("RGB")
        fitted = cover_band(img, TITLE_W, TITLE_H, TITLE_ANCHOR)
        payload = jpeg_bytes(fitted, max(quality, 72))
        e = pack.add("T:" + path, IMG_JPEG, fitted.width, fitted.height, payload)
        titles.append((e, 0, 0, fitted.width, fitted.height))
        title_bytes += len(payload)
    logo_path = os.path.join(ui_dir, "title-logo.png")
    logo = Image.open(logo_path).convert("RGBA")
    lh = 34
    logo = logo.resize((max(1, round(logo.width * lh / logo.height)), lh), Image.LANCZOS)
    bbox = logo.getchannel("A").point(lambda a: 255 if a >= 128 else 0).getbbox()
    logo_crop = logo.crop(bbox)
    logo_payload = encode_block(logo_crop)
    logo_entry = pack.add("L:" + logo_path, IMG_BLOCK, logo_crop.width, logo_crop.height,
                          logo_payload)
    titles.append((logo_entry, 10 + bbox[0], 156 + bbox[1], logo_crop.width, logo_crop.height))
    title_bytes += len(logo_payload)

    blob = pack.serialize(sprites, cutins, titles, asset_map, asset_cutin)
    report["images"] = {
        "entries": len(pack.entries),
        "sprites": len(sprites),
        "bases": len(base_entries),
        "patches": len(patch_entries),
        "patch_empty": sum(1 for v in patch_entries.values() if v[0] == 0xFFFF),
        "cutins": len(cutins),
        "backgrounds": bg_count,
        "titles": len(titles),
        "asset_map": len(asset_map),
        "bytes": len(blob),
        "base_bytes": base_bytes,
        "patch_bytes": patch_bytes,
        "cutin_bytes": cutin_bytes,
        "bg_bytes": bg_bytes,
        "title_bytes": title_bytes,
        "jpeg_quality": quality,
    }
    return blob


def build_scn_pack(source: str, report: dict) -> bytes:
    catalog = load_json(os.path.join(source, "catalog.json"))
    gallery = load_json(os.path.join(source, "gallery.txt"))
    scenes_src = load_story(source)
    validate_ops(scenes_src, catalog)

    strings = bytearray()
    str_off = {}

    def add_str(text: str) -> int:
        if text in str_off:
            return str_off[text]
        off = len(strings)
        strings.extend(text.encode("utf-8"))
        strings.append(0)
        str_off[text] = off
        return off

    tag_labels = catalog["chapterLabels"]
    tag_keys = sorted({op.get("chapterTag", "") for ops in scenes_src.values()
                       for op in ops if op["op"] == "text" and op.get("chapterTag")})
    tag_index = {t: i for i, t in enumerate(tag_keys)}
    tag_offs = [add_str(tag_labels.get(t, t)) for t in tag_keys]

    speaker_names, speaker_index = [], {}
    for ops in scenes_src.values():
        for op in ops:
            if op["op"] == "text" and op.get("speaker"):
                name = op["speaker"]
                if name not in speaker_index:
                    speaker_index[name] = len(speaker_names)
                    speaker_names.append(name)
    speaker_offs = [add_str(n) for n in speaker_names]

    var_names, var_index = [], {}

    def add_var(name: str) -> int:
        if name not in var_index:
            var_index[name] = len(var_names)
            var_names.append(name)
        return var_index[name]

    for meta in catalog["scenes"].values():
        if meta.get("clearFlag"):
            add_var(meta["clearFlag"])
    for ops in scenes_src.values():
        for op in ops:
            if op["op"] == "choice":
                for it in op["items"]:
                    if it.get("effect"):
                        add_var(it["effect"][0])

    visuals, visual_index = [], {}
    for ops in scenes_src.values():
        for op in ops:
            if op["op"] != "text":
                continue
            bodies = list(op.get("bodies") or [])
            while len(bodies) < 3:
                bodies.append("")
            key = (op.get("background", ""), tuple(bodies), op.get("cutIn", ""),
                   op.get("view", "normal"), int(op.get("cutInOpacity", 0)))
            if key not in visual_index:
                visual_index[key] = len(visuals)
                visuals.append(key)

    chapters = [(add_str(tag_labels.get(ch["name"], ch["name"])), ch["id"], ch["route"])
                for ch in catalog["chapters"]]
    routes, first = [], 0
    for i, r in enumerate(catalog["routes"]):
        count = sum(1 for c in catalog["chapters"] if c["route"] == i)
        routes.append((add_str(r["name"]), first, count))
        first += count
    ending_index, ending_names, ending_rows = {}, [], []
    for sid in sorted(scenes_src):
        name = catalog["endings"].get(str(sid), "PARQUET")
        if name not in ending_index:
            ending_index[name] = len(ending_names)
            ending_names.append(name)
        ending_rows.append((sid, ending_index[name]))
    ending_offs = [add_str(n) for n in ending_names]

    by_group = OrderedDict()
    for item in gallery:
        by_group.setdefault(item["group"], []).append(item)
    groups, members = [], []
    for label, items in by_group.items():
        groups.append((add_str(label), len(members), len(items)))
        members.extend(int(it["image"][1:]) for it in items)

    text_blob = bytearray()
    text_blocks = []
    op_rows = []
    scene_rows = []
    logical = 0
    text_ops = 0
    choice_seq = []
    for sid in sorted(scenes_src):
        ops = scenes_src[sid]
        meta = catalog["scenes"][str(sid)]
        flag = meta.get("clearFlag") or ""
        tag = next((op.get("chapterTag", "") for op in ops if op["op"] == "text"), "")
        first_op = len(op_rows)
        for base in range(0, len(ops), TEXT_BLOCK_OPS):
            raw = bytearray()
            for op in ops[base:base + TEXT_BLOCK_OPS]:
                if op["op"] == "text":
                    op["_text_off"] = logical
                    b = op["text"].encode("utf-8")
                    raw.extend(b)
                    raw.append(0)
                    logical += len(b) + 1
                    text_ops += 1
            if not raw:
                continue   # 没有正文的块（纯跳转/选项）不占块表
            comp = zlib.compress(bytes(raw), 9)
            text_blocks.append((logical - len(raw), len(text_blob), len(comp), len(raw)))
            text_blob.extend(comp)
        for op in ops:
            kind = op["op"]
            if kind == "text":
                bodies = list(op.get("bodies") or [])
                while len(bodies) < 3:
                    bodies.append("")
                key = (op.get("background", ""), tuple(bodies), op.get("cutIn", ""),
                       op.get("view", "normal"), int(op.get("cutInOpacity", 0)))
                op_rows.append(struct.pack(
                    "<BBBBHHII", OP_TEXT, tag_index.get(op.get("chapterTag", ""), 0), 0, 0,
                    visual_index[key], speaker_index.get(op.get("speaker", ""), 0xFFFF),
                    op["_text_off"], len(op["text"].encode("utf-8"))))
            elif kind == "jumpScene":
                op_rows.append(struct.pack("<BBBBHHII", OP_JUMP, 0, 0, 0, op["scene"], 0,
                                           op["to"], 0))
            elif kind == "choice":
                op_rows.append(struct.pack("<BBBBHHII", OP_CHOICE, 0, 0, 0,
                                           len(choice_seq), len(op["items"]), 0, 0))
                choice_seq.append(op)
            else:
                op_rows.append(struct.pack("<BBBBHHII", OP_END, 0, 0, 0, 0, 0, 0, 0))
        scene_rows.append((sid, meta["route"], var_index.get(flag, 0xFF) if flag else 0xFF,
                           first_op, len(ops), tag_index.get(tag, 0), 0))
    # 选项文本也放在正文逻辑空间里（运行时统一走 pq_text_get），单独一块。
    choices, choice_items = [], []
    choice_raw = bytearray()
    choice_logical = logical
    for op in choice_seq:
        first_item = len(choice_items)
        for it in op["items"]:
            eff = it.get("effect")
            b = it["text"].encode("utf-8")
            choice_items.append((choice_logical, len(b), it["scene"], it["to"],
                                 add_var(eff[0]) if eff else 0, 1 if eff else 0,
                                 eff[1] if eff else 0))
            choice_raw.extend(b)
            choice_raw.append(0)
            choice_logical += len(b) + 1
        choices.append((first_item, len(op["items"])))
    if choice_raw:
        comp = zlib.compress(bytes(choice_raw), 9)
        text_blocks.append((logical, len(text_blob), len(comp), len(choice_raw)))
        text_blob.extend(comp)
        logical = choice_logical
    var_offs = [add_str(n) for n in var_names]

    op_blob = b"".join(op_rows)
    strings = bytes(strings)
    off_scenes = SCN_HEADER
    off_ops = off_scenes + 12 * len(scene_rows)
    off_visuals = off_ops + len(op_blob)
    off_text_blocks = off_visuals + 12 * len(visuals)
    off_text_data = off_text_blocks + 12 * len(text_blocks)
    off_tags = off_text_data + len(text_blob)
    off_speakers = off_tags + 4 * len(tag_offs)
    off_chapters = off_speakers + 4 * len(speaker_offs)
    off_routes = off_chapters + 8 * len(chapters)
    off_choices = off_routes + 8 * len(routes)
    off_choice_items = off_choices + 4 * len(choices)
    off_endings = off_choice_items + CHOICE_ITEM_SIZE * len(choice_items)
    off_gallery = off_endings + 4 * len(ending_rows)
    off_members = off_gallery + 8 * len(groups)
    off_ending_names = off_members + 2 * len(members)
    off_vars = off_ending_names + 4 * len(ending_offs)
    off_strings = off_vars + 4 * len(var_offs)
    head = struct.pack(
        "<8s19H18I",
        SCN_MAGIC, VERSION, len(scene_rows), len(op_rows), len(text_blocks),
        len(speaker_offs), len(chapters), len(routes), len(tag_offs), len(var_offs),
        len(visuals), len(groups), len(choices), len(ending_rows), len(choice_items),
        len(members), catalog["initial"], catalog["flowScene"],
        catalog["sideInitial"], catalog["afterInitial"],
        off_scenes, off_ops, off_visuals, off_text_blocks, off_text_data, off_tags,
        off_speakers, off_chapters, off_routes, off_choices, off_choice_items,
        off_endings, off_gallery, off_members, off_ending_names, off_vars, off_strings,
        len(strings))
    out = bytearray(head)
    out += b"\0" * (SCN_HEADER - len(out))
    for row in scene_rows:
        out += struct.pack("<HBBHHHH", *row)
    out += op_blob
    for vis in visuals:
        bg = int(vis[0][1:]) if vis[0] else 0xFFFF
        bodies = [int(b[1:]) if b else 0xFFFF for b in vis[1]]
        cut = int(vis[2][1:]) if vis[2] else 0xFFFF
        out += struct.pack("<HHHHHBB", bg, bodies[0], bodies[1], bodies[2], cut,
                           1 if vis[3] == "another" else 0, vis[4])
    for lo, co, cl, rl in text_blocks:
        out += struct.pack("<IIHH", lo, co, cl, rl)
    out += text_blob
    out += struct.pack(f"<{len(tag_offs)}I", *tag_offs)
    out += struct.pack(f"<{len(speaker_offs)}I", *speaker_offs)
    for off, sid, route in chapters:
        out += struct.pack("<IHBB", off, sid, route, 0)
    for off, first_ch, count in routes:
        out += struct.pack("<IHH", off, first_ch, count)
    for first_item, count in choices:
        out += struct.pack("<HBB", first_item, count, 0)
    for off, ln, sid, to, var, has, val in choice_items:
        out += struct.pack("<IHHIBBi", off, ln, sid, to, var, has, val)
    for sid, eidx in ending_rows:
        out += struct.pack("<HH", sid, eidx)
    for off, first_m, count in groups:
        out += struct.pack("<IHH", off, first_m, count)
    out += struct.pack(f"<{len(members)}H", *members)
    out += struct.pack(f"<{len(ending_offs)}I", *ending_offs)
    out += struct.pack(f"<{len(var_offs)}I", *var_offs)
    out += strings
    if len(out) != off_strings + len(strings):
        die(f"剧本包长度 {len(out)} != 预期 {off_strings + len(strings)}")
    report["script"] = {
        "scenes": len(scene_rows),
        "ops": len(op_rows),
        "text_ops": text_ops,
        "visuals": len(visuals),
        "text_blocks": len(text_blocks),
        "text_bytes": logical,
        "text_compressed": len(text_blob),
        "speakers": len(speaker_offs),
        "chapters": len(chapters),
        "routes": len(routes),
        "galleries": len(groups),
        "gallery_members": len(members),
        "choices": len(choices),
        "choice_items": len(choice_items),
        "vars": len(var_names),
        "bytes": len(out),
        "entries": [catalog["initial"], catalog["flowScene"],
                    catalog["sideInitial"], catalog["afterInitial"]],
    }
    return bytes(out)


def collect_symbols(source: str) -> str:
    scenes = load_story(source)
    chars = set()
    for ops in scenes.values():
        for op in ops:
            if op["op"] == "text":
                chars |= set(op["text"])
                chars |= set(op.get("speaker") or "")
            elif op["op"] == "choice":
                for it in op["items"]:
                    chars |= set(it["text"])
    for ch in "\ufffc\r\n\t":
        chars.discard(ch)
    header = ("# 《PARQUET》剧本与选项字符集（由 tools/parquet_pack.py 从源快照生成）。\n"
              "# 重新生成字体时必须与本文件一致。\n")
    return header + "".join(sorted(chars)) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", default="assets/parquet-source")
    ap.add_argument("--images", required=True, help="parquet-rw5 的 src 目录（资源路径以 /common 开头）")
    ap.add_argument("--out", default="main/parquet_data")
    ap.add_argument("--jpeg-quality", type=int, default=JPEG_QUALITY_DEFAULT)
    ap.add_argument("--report", default="build/reports/parquet-pack.json")
    args = ap.parse_args()

    report = {"version": VERSION, "source": os.path.abspath(args.source),
              "jpeg_quality": args.jpeg_quality}
    img_blob = build_image_pack(args.source, args.images, args.jpeg_quality, report)
    scn_blob = build_scn_pack(args.source, report)
    symbols = collect_symbols(args.source)

    os.makedirs(args.out, exist_ok=True)
    img_path = os.path.join(args.out, "parquet_img.bin")
    scn_path = os.path.join(args.out, "parquet_scn.bin")
    with open(img_path, "wb") as fh:
        fh.write(img_blob)
    with open(scn_path, "wb") as fh:
        fh.write(scn_blob)
    sym_path = os.path.join(args.source, "parquet_symbols.txt")
    with open(sym_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(symbols)
    report["files"] = {
        "img": {"path": img_path, "bytes": len(img_blob), "sha256": sha256_bytes(img_blob)},
        "scn": {"path": scn_path, "bytes": len(scn_blob), "sha256": sha256_bytes(scn_blob)},
        "symbols": {"path": sym_path, "chars": len(symbols.splitlines()[-1])},
    }
    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2, sort_keys=True)
        fh.write("\n")
    print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    print(f"parquet_pack: ok -> {img_path} ({len(img_blob)} B), "
          f"{scn_path} ({len(scn_blob)} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
