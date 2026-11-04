#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""《纸上的魔法使》资源打包器：源目录 -> AI Passport 可直读的资源包。

用法:
  python tools/paper_pack.py --source assets/paper-source --out main/paper_data \
      [--quality 40] [--font-otf assets/paper-source/paper-extra.otf]

源目录要求（paper-rw5 的 src/common 子集）:
  story/<scene>/<chunk>.txt   ops JSON（text/background/sprite/set/ifFalse/jumpScene/choice/...）
  images/<hash>.(jpg|png)     已转换的图（背景/CG 最大 320x240，立绘带 alpha）
  assets.js                   id -> 文件名 的映射
  catalog.js                  场景名/路线/结局/章节
  gallery.txt                 CG 鉴赏分组

产物:
  main/paper_data/paper_scn.bin   剧本包（场景表 + 8 字节定长 op + 压缩正文块 + 说话人/CG/章节表）
  main/paper_data/paper_img.bin   图片包（JPEG 场景图 / BLOCK 立绘 / PATCH 表情差分）
  assets/fonts/paper_cjk_symbols.txt  字库字符集（交给 tools/gal_font.py 生成字体）

格式细节见 docs/development/paper-pack-format.md（本工具与 main/paper/paper_pack.c 一一对应）。
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
from collections import Counter, defaultdict

from PIL import Image

# ----------------------------------------------------------------- 常量
IMG_MAGIC = b"PAPRIMG1"
SCN_MAGIC = b"PAPRSCN1"
TEXT_BLOCK_RAW = 16 * 1024          # 每个正文块的解压后字节上限
SPRITE_MAX_H = 150                  # 画面区高 150：立绘完整等比内接，绝不裁身体/脸
SPRITE_MAX_W = 190                  # 宽度上限，限制解码/绘制缓冲（150 高时通常不触发）
BLOCK_ROWS = 8
MAX_COLORS = 255
PATCH_MAX_PX = 4096                # 单块补丁的像素上限（引擎侧 24 KB 缓冲）
JPEG_QUALITY_DEFAULT = 40

# op kinds（与 paper_pack.h 的 enum 一致）
OP_TEXT, OP_SPRITE, OP_BACKGROUND, OP_SET, OP_IFFALSE, OP_JUMPSCENE = 0, 1, 2, 3, 4, 5
OP_CLEARSPRITES, OP_END, OP_CHOICE = 6, 7, 8


# ----------------------------------------------------------------- 基础
def die(msg: str) -> None:
    print(f"paper_pack: {msg}", file=sys.stderr)
    sys.exit(1)


def load_js_object(path: str) -> dict:
    """读取 `export const xxx={...}` 形式的 JS 里那个纯 JSON 对象。"""
    with io.open(path, encoding="utf-8") as f:
        s = f.read()
    i = s.index("{")
    depth, in_str, esc = 0, False, False
    for j in range(i, len(s)):
        ch = s[j]
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return json.loads(s[i:j + 1])
    die(f"{path}: 找不到完整的 JSON 对象")


def sha1_file(path: str) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


# ----------------------------------------------------------------- 图片编码
def jpeg_bytes(img: Image.Image, quality: int) -> bytes:
    buf = io.BytesIO()
    img.convert("RGB").save(buf, "JPEG", quality=quality, optimize=True, subsampling=2)
    return buf.getvalue()


def fit_in(img: Image.Image, w: int, h: int) -> Image.Image:
    """等比缩到 w×h 内，不放大；保留完整构图（横屏目标）。"""
    s = min(w / img.width, h / img.height, 1.0)
    return img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))),
                      Image.LANCZOS)


def cover(img: Image.Image, w: int, h: int) -> Image.Image:
    s = max(w / img.width, h / img.height)
    nw, nh = max(w, round(img.width * s)), max(h, round(img.height * s))
    r = img.resize((nw, nh), Image.LANCZOS)
    left, top = (nw - w) // 2, (nh - h) // 2
    return r.crop((left, top, left + w, top + h))


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
        else:  # paeth
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
        out[i] = (x - pred) & 0xFF
    return bytes(out)


def quantize_rgba(img: Image.Image):
    """RGBA -> (indices, palette RGB565 list, alpha list)。≤255 色。"""
    rgba = img.convert("RGBA")
    q = rgba.quantize(colors=MAX_COLORS, method=Image.FASTOCTREE)
    idx = q.load()
    w, h = rgba.size
    px = rgba.load()
    # 每个量化索引实际对应的平均 alpha（调色板模式丢掉了 alpha）
    sums = defaultdict(lambda: [0, 0])
    for y in range(h):
        for x in range(w):
            k = idx[x, y]
            a = px[x, y][3]
            s = sums[k]
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


def block_from_palette(idx, palette, alphas, size, rows=BLOCK_ROWS) -> bytes:
    """用显式调色板/alpha 编码 BLOCK（不再二次量化），保证与补丁像素一致。"""
    w, h = size
    blocks = []
    lens = []
    prev = bytes(w)
    nblocks = (h + rows - 1) // rows
    for b in range(nblocks):
        chunk = bytearray()
        for y in range(b * rows, min(h, (b + 1) * rows)):
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
    head = struct.pack("<6HI", w, h, len(palette), rows, nblocks, 0, rows * (w + 1))
    out = bytearray(head)
    out += struct.pack(f"<{len(palette)}H", *palette)
    out += bytes(alphas)
    out += struct.pack(f"<{nblocks}I", *lens)
    for c in blocks:
        out += c
    return bytes(out)


def palette_image(palette, alphas, size) -> Image.Image:
    """把调色板做成 Pillow 的 P 模式图，用于把同簇成员映射到同一套颜色。"""
    w, h = size
    pal = []
    for c in palette:
        r = ((c >> 11) & 0x1F) << 3
        g = ((c >> 5) & 0x3F) << 2
        b = (c & 0x1F) << 3
        pal += [r | (r >> 5), g | (g >> 6), b | (b >> 5)]
    im = Image.new("P", (1, 1))
    im.putpalette(pal + [0] * (768 - len(pal)))
    im.info["alphas"] = list(alphas)
    return im


def map_to_palette(img: Image.Image, pal_img: Image.Image, palette, alphas):
    """把 RGBA 图映射到给定调色板：颜色取最近调色板项，alpha 用本图二值 alpha。"""
    rgb = img.convert("RGB").quantize(palette=pal_img, dither=Image.NONE)
    idx = rgb.load()
    w, h = img.size
    alpha = img.getchannel("A").point(lambda a: 255 if a >= 128 else 0)
    ap = alpha.load()
    out = Image.new("RGBA", (w, h))
    op = out.load()
    for y in range(h):
        for x in range(w):
            i = idx[x, y]
            c = palette[i] if i < len(palette) else palette[0]
            r = ((c >> 11) & 0x1F) << 3
            g = ((c >> 5) & 0x3F) << 2
            b = (c & 0x1F) << 3
            op[x, y] = (r | (r >> 5), g | (g >> 6), b | (b >> 5), ap[x, y])
    return out, idx


def encode_block(img: Image.Image) -> bytes:
    """BLOCK 载荷：调色板 + 逐行滤波 + 8 行一块 zlib（与 senren 打包器同构）。"""
    idx, palette, alphas, (w, h) = quantize_rgba(img)
    blocks = []
    prev = bytes(w)
    nblocks = (h + BLOCK_ROWS - 1) // BLOCK_ROWS
    lens = []
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
    return bytes(out), (w, h)


def dequantize(idx, palette, alphas, size) -> Image.Image:
    """把 BLOCK 的量化结果还原成 RGBA —— 补丁按"解码后的像素"计算，
    保证 base+patch 与整块 BLOCK 解码逐像素一致。"""
    w, h = size
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            i = idx[x, y]
            c = palette[i]
            r = ((c >> 11) & 0x1F) << 3
            g = ((c >> 5) & 0x3F) << 2
            b = (c & 0x1F) << 3
            px[x, y] = (r | (r >> 5), g | (g >> 6), b | (b >> 5), alphas[i])
    return im


def sprite_prepare(img: Image.Image) -> Image.Image:
    """裁 alpha 透明边，按画面区高 150（宽 ≤190）缩放，只缩不放；alpha 二值化。

    二值 alpha 让"基准 + 补丁"的合成语义成立：变更点要么整像素替换（variant 不透明），
    要么退回整张 BLOCK（涉及挖空/半透明变化时），不会把半透明像素混错。
    """
    rgba = img.convert("RGBA")
    rgba.putalpha(rgba.getchannel("A").point(lambda a: 255 if a >= 128 else 0))
    bbox = rgba.getchannel("A").getbbox()
    if bbox:
        rgba = rgba.crop(bbox)
    s = min(1.0, SPRITE_MAX_H / rgba.height)
    if rgba.width * s > SPRITE_MAX_W:
        s = SPRITE_MAX_W / rgba.width
    if s < 1.0:
        rgba = rgba.resize((max(1, round(rgba.width * s)), max(1, round(rgba.height * s))),
                           Image.LANCZOS)
    return rgba


def sprite_mask(img: Image.Image, size=(32, 64)):
    a = img.getchannel("A").resize(size, Image.BILINEAR)
    return [v > 96 for v in a.getdata()]


def iou(a, b) -> float:
    inter = sum(1 for x, y in zip(a, b) if x and y)
    union = sum(1 for x, y in zip(a, b) if x or y)
    return inter / union if union else 0.0


def patch_payload(base: Image.Image, var: Image.Image):
    """基准图 + 变更像素补丁。

    约束（保证合成语义正确）：补丁只覆盖 **变体像素完全不透明（alpha>=250）** 的变更点。
    半透明像素无法在"直接合成到画布"的链路里被正确替换（会与基准图混合），
    因此只要变更点里出现半透明像素，就整张退回整块 BLOCK —— 不静默丢 alpha。
    返回 (payload, rect) 或 None（不划算/含半透明变更）。
    """
    if base.size != var.size:
        return None
    b = base.load()
    v = var.load()
    w, h = base.size
    xs0, ys0, xs1, ys1 = w, h, -1, -1
    changed = 0
    for y in range(h):
        for x in range(w):
            if b[x, y] != v[x, y]:
                changed += 1
                if v[x, y][3] < 128:
                    return None          # 需要"挖空"的变更：退回整张图（二值 alpha）
                if x < xs0:
                    xs0 = x
                if x > xs1:
                    xs1 = x
                if y < ys0:
                    ys0 = y
                if y > ys1:
                    ys1 = y
    if changed == 0 or xs1 < 0:
        return None
    rw, rh = xs1 - xs0 + 1, ys1 - ys0 + 1
    if rw * rh > PATCH_MAX_PX:
        return None
    mask_bits = bytearray((rw + 7) // 8 * rh)
    px = bytearray()
    for y in range(ys0, ys1 + 1):
        for x in range(xs0, xs1 + 1):
            if b[x, y] == v[x, y]:
                continue
            i = (y - ys0) * rw + (x - xs0)
            mask_bits[i >> 3] |= 0x80 >> (i & 7)
            r, g, bl = v[x, y][:3]
            px += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3))
    mz = zlib.compress(bytes(mask_bits), 9)
    pz = zlib.compress(bytes(px), 9)
    payload = struct.pack("<II", len(mz), len(pz)) + mz + pz
    return payload, (xs0, ys0, rw, rh)


def block_from_palette(idx, palette, alphas, size, rows=BLOCK_ROWS) -> bytes:
    """用显式调色板/alpha 编码 BLOCK（不再二次量化），保证与补丁像素一致。"""
    w, h = size
    blocks = []
    lens = []
    prev = bytes(w)
    nblocks = (h + rows - 1) // rows
    for b in range(nblocks):
        chunk = bytearray()
        for y in range(b * rows, min(h, (b + 1) * rows)):
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
    head = struct.pack("<6HI", w, h, len(palette), rows, nblocks, 0, rows * (w + 1))
    out = bytearray(head)
    out += struct.pack(f"<{len(palette)}H", *palette)
    out += bytes(alphas)
    out += struct.pack(f"<{nblocks}I", *lens)
    for c in blocks:
        out += c
    return bytes(out)


def palette_image(palette, alphas, size) -> Image.Image:
    """把调色板做成 Pillow 的 P 模式图，用于把同簇成员映射到同一套颜色。"""
    w, h = size
    pal = []
    for c in palette:
        r = ((c >> 11) & 0x1F) << 3
        g = ((c >> 5) & 0x3F) << 2
        b = (c & 0x1F) << 3
        pal += [r | (r >> 5), g | (g >> 6), b | (b >> 5)]
    im = Image.new("P", (1, 1))
    im.putpalette(pal + [0] * (768 - len(pal)))
    im.info["alphas"] = list(alphas)
    return im


def map_to_palette(img: Image.Image, pal_img: Image.Image, palette, alphas):
    """把 RGBA 图映射到给定调色板：颜色取最近调色板项，alpha 用本图二值 alpha。"""
    rgb = img.convert("RGB").quantize(palette=pal_img, dither=Image.NONE)
    idx = rgb.load()
    w, h = img.size
    alpha = img.getchannel("A").point(lambda a: 255 if a >= 128 else 0)
    ap = alpha.load()
    out = Image.new("RGBA", (w, h))
    op = out.load()
    for y in range(h):
        for x in range(w):
            i = idx[x, y]
            c = palette[i] if i < len(palette) else palette[0]
            r = ((c >> 11) & 0x1F) << 3
            g = ((c >> 5) & 0x3F) << 2
            b = (c & 0x1F) << 3
            op[x, y] = (r | (r >> 5), g | (g >> 6), b | (b >> 5), ap[x, y])
    return out, idx


def encode_block(img: Image.Image) -> bytes:
    """BLOCK 载荷：调色板 + 逐行滤波 + 8 行一块 zlib（与 senren 打包器同构）。"""
    idx, palette, alphas, (w, h) = quantize_rgba(img)
    blocks = []
    prev = bytes(w)
    nblocks = (h + BLOCK_ROWS - 1) // BLOCK_ROWS
    lens = []
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
    return bytes(out), (w, h)


def dequantize(idx, palette, alphas, size) -> Image.Image:
    """把 BLOCK 的量化结果还原成 RGBA —— 补丁按"解码后的像素"计算，
    保证 base+patch 与整块 BLOCK 解码逐像素一致。"""
    w, h = size
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            i = idx[x, y]
            c = palette[i]
            r = ((c >> 11) & 0x1F) << 3
            g = ((c >> 5) & 0x3F) << 2
            b = (c & 0x1F) << 3
            px[x, y] = (r | (r >> 5), g | (g >> 6), b | (b >> 5), alphas[i])
    return im


def sprite_prepare(img: Image.Image) -> Image.Image:
    """裁 alpha 透明边，按画面区高 150（宽 ≤190）缩放，只缩不放；alpha 二值化。

    二值 alpha 让"基准 + 补丁"的合成语义成立：变更点要么整像素替换（variant 不透明），
    要么退回整张 BLOCK（涉及挖空/半透明变化时），不会把半透明像素混错。
    """
    rgba = img.convert("RGBA")
    rgba.putalpha(rgba.getchannel("A").point(lambda a: 255 if a >= 128 else 0))
    bbox = rgba.getchannel("A").getbbox()
    if bbox:
        rgba = rgba.crop(bbox)
    s = min(1.0, SPRITE_MAX_H / rgba.height)
    if rgba.width * s > SPRITE_MAX_W:
        s = SPRITE_MAX_W / rgba.width
    if s < 1.0:
        rgba = rgba.resize((max(1, round(rgba.width * s)), max(1, round(rgba.height * s))),
                           Image.LANCZOS)
    return rgba


def sprite_mask(img: Image.Image, size=(32, 64)):
    a = img.getchannel("A").resize(size, Image.BILINEAR)
    return [v > 96 for v in a.getdata()]


def iou(a, b) -> float:
    inter = sum(1 for x, y in zip(a, b) if x and y)
    union = sum(1 for x, y in zip(a, b) if x or y)
    return inter / union if union else 0.0


def collect_ops(story_dir: str):
    """按场景合并 chunk，返回 {scene_id: [op...]}（保持 chunk 顺序）。"""
    scenes = defaultdict(list)
    for name in os.listdir(story_dir):
        d = os.path.join(story_dir, name)
        if not os.path.isdir(d):
            continue
        scene_id = int(name)
        chunks = []
        for f in os.listdir(d):
            if f.endswith(".txt"):
                chunks.append((int(os.path.splitext(f)[0]), os.path.join(d, f)))
        for _, path in sorted(chunks):
            with io.open(path, encoding="utf-8") as fh:
                scenes[scene_id].extend(json.load(fh))
    return scenes


class TextPool:
    """把剧本文本压成 16 KB 的逻辑块，行不跨块。"""

    def __init__(self):
        self.blocks = []          # [(logical_off, raw_bytes)]
        self.cur = bytearray()
        self.cur_off = 0
        self.logical = 0

    def add(self, s: str):
        b = s.encode("utf-8") + b"\x00"
        if self.cur and len(self.cur) + len(b) > TEXT_BLOCK_RAW:
            self.blocks.append((self.cur_off, bytes(self.cur)))
            self.cur_off += len(self.cur)
            self.cur = bytearray()
        off = self.logical
        self.cur += b
        self.logical += len(b)
        return off

    def finish(self):
        if self.cur:
            self.blocks.append((self.cur_off, bytes(self.cur)))
            self.cur = bytearray()
        return self.blocks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default="assets/paper-source")
    ap.add_argument("--out", default="main/paper_data")
    ap.add_argument("--quality", type=int, default=JPEG_QUALITY_DEFAULT)
    ap.add_argument("--font-otf", default=None)
    ap.add_argument("--no-patch", action="store_true", help="禁用差分（生成对照用整块图包）")
    ap.add_argument("--img-name", default="paper_img.bin")
    ap.add_argument("--dump-patch", default="tests/paper/fixtures/patch-expect.bin")
    ap.add_argument("--report", default="build/paper-pack-report.json")
    args = ap.parse_args()

    src = os.path.abspath(args.source)
    story_dir = os.path.join(src, "story")
    img_dir = os.path.join(src, "images")
    if not os.path.isdir(story_dir):
        die(f"找不到剧本目录: {story_dir}")

    assets = load_js_object(os.path.join(src, "assets.js"))
    catalog = load_js_object(os.path.join(src, "catalog.js"))
    gallery = json.loads(io.open(os.path.join(src, "gallery.txt"), encoding="utf-8").read())

    scenes = collect_ops(story_dir)
    print(f"scenes={len(scenes)} ops={sum(len(v) for v in scenes.values())}")

    # ---- 图片 id 收集
    bg_ids, sprite_ids, choice_ids = [], set(), set()
    for sid, ops in scenes.items():
        for op in ops:
            if op["op"] == "background":
                if op.get("image"):
                    bg_ids.append(op["image"])
            elif op["op"] == "sprite":
                if op.get("image"):
                    sprite_ids.add(op["image"])
            elif op["op"] == "choice":
                for it in op.get("items", []):
                    if it.get("image"):
                        choice_ids.add(it["image"])
    gallery_ids = [g["image"] for g in gallery if g.get("image")]
    frame_ids = sorted(set(bg_ids) | set(gallery_ids))
    sprite_ids = sorted(sprite_ids)
    all_ids = frame_ids + sprite_ids
    missing = [i for i in all_ids if i not in assets]
    if missing:
        die(f"assets.js 缺少 {len(missing)} 个资源: {missing[:8]}")
    print(f"frames={len(frame_ids)} sprites={len(sprite_ids)} choice_art={len(choice_ids)}")
    print(f"gallery groups={len({g['group'] for g in gallery})} images={len(gallery_ids)}")

    # ---- 图片编码
    name_idx = {}
    blobs = []
    entries = []
    kind_of = {}
    stats = Counter()

    def add_entry(iid: str, kind: int, w: int, h: int, payload: bytes, extra: bytes = b""):
        idx = len(entries)
        entries.append({"id": iid, "kind": kind, "w": w, "h": h,
                        "off": 0, "size": len(payload) + len(extra),
                        "extra": extra, "payload": payload})
        name_idx[iid] = idx
        return idx

    sprite_index = {iid: len(frame_ids) + i for i, iid in enumerate(sprite_ids)}
    cache = {}
    for iid in frame_ids:
        fn = assets[iid]
        path = os.path.join(img_dir, fn)
        try:
            with Image.open(path) as im:
                canvas = fit_in(im.convert("RGB"), 320, 150)
                payload = jpeg_bytes(canvas, args.quality)
        except Exception as e:
            die(f"编码失败 {iid} ({fn}): {e}")
        add_entry(iid, 0, canvas.width, canvas.height, payload)
        stats["frame_bytes"] += len(payload)
        cache[iid] = path

    # 立绘：逐图量化成 BLOCK；再按「同尺寸 + 掩码 IoU>=0.9」聚类，把变体写成
    # 基准图 + 不透明变更像素补丁（补丁只覆盖 variant alpha==255 的点，保证合成语义正确）。
    sprite_payload = {}
    sprite_img = {}
    for iid in sprite_ids:
        fn = assets[iid]
        path = os.path.join(img_dir, fn)
        try:
            with Image.open(path) as im:
                prepared = sprite_prepare(im)
        except Exception as e:
            die(f"立绘处理失败 {iid} ({fn}): {e}")
        payload, (w, h) = encode_block(prepared)
        sprite_payload[iid] = payload
        sprite_img[iid] = prepared
        stats["sprite_block_bytes"] += len(payload)

    masks, sizes = {}, {}
    for iid, im in sprite_img.items():
        masks[iid] = sprite_mask(im)
        sizes[iid] = im.size
    by_size = defaultdict(list)
    for iid, sz in sizes.items():
        by_size[sz].append(iid)
    clusters = []
    for sz, ids in sorted(by_size.items(), key=lambda kv: -len(kv[1])):
        remaining = list(ids)
        while remaining:
            seed = remaining.pop(0)
            group = [seed]
            for other in list(remaining):
                if iou(masks[seed], masks[other]) >= 0.90:
                    group.append(other)
                    remaining.remove(other)
            clusters.append(group)

    kind_of = {}
    patch_used = 0
    patch_bytes = 0
    patch_dump = []          # (iid, w, h, [(x, y, rgb565), ...]) 供 C 测试逐像素对照
    for group in (clusters if not args.no_patch else []):
        if len(group) < 2:
            continue
        base = group[0]
        for iid in group[1:]:
            r = patch_payload(sprite_img[base], sprite_img[iid])
            if not r:
                continue
            payload, (x, y, w, h) = r
            base_idx = sprite_index.get(base)
            if base_idx is None:
                continue
            full_len = len(sprite_payload[iid])
            if len(payload) + 10 < full_len:
                sprite_payload[iid] = payload
                kind_of[iid] = (2, base_idx, x, y, w, h)
                patch_used += 1
                patch_bytes += len(payload)
                # 记录"应当被写成什么样"的期望像素（真彩 RGB565）
                b = sprite_img[base].load()
                v = sprite_img[iid].load()
                exp = []
                for yy in range(y, y + h):
                    for xx in range(x, x + w):
                        if b[xx, yy] != v[xx, yy]:
                            r0, g0, b0 = v[xx, yy][:3]
                            exp.append((xx, yy,
                                        ((r0 >> 3) << 11) | ((g0 >> 2) << 5) | (b0 >> 3)))
                patch_dump.append({"id": iid, "index": sprite_index[iid],
                                   "w": sprite_img[iid].width, "h": sprite_img[iid].height,
                                   "pixels": exp})

    for iid in sprite_ids:
        kind, extra = 1, b""
        if iid in kind_of:
            kind = 2
            _, base_idx, x, y, w, h = kind_of[iid]
            extra = struct.pack("<HHHHH", base_idx, x, y, w, h)
        im = sprite_img[iid]
        add_entry(iid, kind, im.width, im.height, sprite_payload[iid], extra)
        stats["sprite_final_bytes"] += len(sprite_payload[iid])
    print(f"sprites: {len(sprite_ids)} block_bytes={stats['sprite_block_bytes']} "
          f"final={stats['sprite_final_bytes']} patches={patch_used} ({patch_bytes} B)")
    print(f"frames: {len(frame_ids)} bytes={stats['frame_bytes']}")

    # ---- 写图片包
    names = bytearray()
    name_off = {}
    for iid in [e["id"] for e in entries]:
        name_off[iid] = len(names)
        names += iid.encode("utf-8") + b"\x00"
    # 图片 -> CG 组（0xFFFF = 不属于任何 CG 组），按 gallery.txt 出现顺序编号
    gidx = {}
    for g in gallery:
        if g["group"] not in gidx:
            gidx[g["group"]] = len(gidx)
    group_of = {}
    for g in gallery:
        idx = name_idx.get(g.get("image"))
        if idx is not None and idx not in group_of:
            group_of[idx] = gidx[g["group"]]
    img_group = [group_of.get(i, 0xFFFF) for i in range(len(entries))]
    group_blob = struct.pack(f"<{len(img_group)}H", *img_group)

    hdr = struct.pack("<8sHHII", IMG_MAGIC, 1, len(entries), len(names), 0)
    table_size = 24 * len(entries)
    base_off = len(hdr) + table_size + len(names)
    off = base_off
    table = bytearray()
    body = bytearray()
    for e in entries:
        table += struct.pack("<IHHHHIII", name_off[e["id"]], e["kind"], e["w"], e["h"],
                             len(e["extra"]), off, e["size"], 0)
        off += e["size"]
        body += e["extra"] + e["payload"]
    os.makedirs(args.out, exist_ok=True)
    img_path = os.path.join(args.out, args.img_name)
    body_off = len(hdr) + len(table) + len(names)
    group_off = body_off + len(body)
    hdr = struct.pack("<8sHHII", IMG_MAGIC, 1, len(entries), len(names), group_off)
    with open(img_path, "wb") as f:
        f.write(hdr + bytes(table) + bytes(names) + bytes(body) + group_blob)
    img_size = os.path.getsize(img_path)

    # ---- 剧本包
    speakers = []
    speaker_idx = {}
    vars_order = ["tf.scene_mode", "sf.pv", "sf.loadp", "sf.special"]
    var_idx = {v: i for i, v in enumerate(vars_order)}
    for i in range(96):
        var_idx[f"sf.album_flag[{i}]"] = len(vars_order) + i
    for i in range(64):
        var_idx[f"sf.scene_flag[{i}]"] = len(vars_order) + 96 + i

    pool = TextPool()
    op_stream = bytearray()
    scene_table = []
    choice_blob = bytearray()

    def var_index(key: str) -> int:
        if key in var_idx:
            return var_idx[key]
        idx = len(var_idx)
        var_idx[key] = idx
        vars_order.append(key)
        return idx

    choice_entries = []
    for sid in sorted(scenes):
        ops = scenes[sid]
        first = len(op_stream) // 8
        for op in ops:
            k = op["op"]
            if k == "text":
                sp = op.get("speaker") or ""
                if sp not in speaker_idx:
                    speaker_idx[sp] = len(speakers)
                    speakers.append(sp)
                off = pool.add(op["text"])
                ln = len(op["text"].encode("utf-8"))
                if ln > 65535:
                    die(f"单句过长: {ln} B @ scene {sid}")
                op_stream += struct.pack("<BBHI", OP_TEXT, speaker_idx[sp], ln, off)
            elif k == "background":
                op_stream += struct.pack("<BBHI", OP_BACKGROUND, 1 if op.get("clearBody") else 0,
                                         name_idx[op["image"]], 0)
            elif k == "sprite":
                slot = int(op.get("slot", 0))
                img = name_idx.get(op.get("image")) if op.get("image") else 0xFFFF
                op_stream += struct.pack("<BBHI", OP_SPRITE, slot,
                                         img if img is not None else 0xFFFF, 0)
            elif k == "clearSprites":
                op_stream += struct.pack("<BBHI", OP_CLEARSPRITES, 0, 0, 0)
            elif k == "set":
                key = op["key"]
                val = op["value"]
                if isinstance(val, bool):
                    val = 1 if val else 0
                op_stream += struct.pack("<BBHI", OP_SET, 0, var_index(key), int(val) & 0xFFFFFFFF)
            elif k == "ifFalse":
                c = op["condition"]
                if c is False:
                    kind = 0
                elif isinstance(c, list) and c[0] == "v" and c[1] == "tf.scene_mode":
                    kind = 1
                elif isinstance(c, list) and c[0] == "==" and c[1] == ["v", "sf.pv"] and c[2] == 1:
                    kind = 2
                else:
                    die(f"未支持的 ifFalse 条件 @ scene {sid}: {c}")
                op_stream += struct.pack("<BBHI", OP_IFFALSE, kind, 0, int(op["to"]))
            elif k == "jumpScene":
                op_stream += struct.pack("<BBHI", OP_JUMPSCENE, 0, int(op["scene"]), int(op["to"]))
            elif k == "end":
                op_stream += struct.pack("<BBHI", OP_END, 0, 0, 0)
            elif k == "choice":
                items = op.get("items", [])
                if len(items) > 250:
                    die("选项过多")
                ci = len(choice_entries)
                blob = bytearray()
                blob += struct.pack("<B", len(items))
                for it in items:
                    off = pool.add(it["text"])
                    ln = len(it["text"].encode("utf-8"))
                    blob += struct.pack("<IHHI", off, ln, name_idx.get(it.get("image"), 0xFFFF),
                                        int(it["to"]))
                choice_entries.append(bytes(blob))
                op_stream += struct.pack("<BBHI", OP_CHOICE, len(items), ci, 0)
            elif k == "unsupportedJump":
                # 源脚本里唯一一处无效标签跳转，位于无条件跳转后的不可达位置
                # （paper-rw5 reports/control-flow-validation.json 已证正式流程与章节入口都到不了）。
                # 仍保持"到此终止"的语义，避免意外继续执行后面的指令。
                print(f"  WARN scene {sid}: unsupportedJump {op.get('destination')} "
                      f"-> 以 END 处理（不可达）")
                op_stream += struct.pack("<BBHI", OP_END, 0, 0, 0)
            else:
                die(f"未知 op {k} @ scene {sid}")
        op_count = len(op_stream) // 8 - first
        scene_table.append((sid, first, op_count))

    text_blocks = pool.finish()
    textidx = bytearray()
    textdata = bytearray()
    for logical_off, raw in text_blocks:
        comp = zlib.compress(raw, 9)
        textidx += struct.pack("<IIHHI", logical_off, len(textdata), len(comp), len(raw), 0)
        textdata += comp

    # 说话人表
    sp_blob = bytearray()
    sp_offs = []
    for name in speakers:
        sp_offs.append(len(sp_blob))
        sp_blob += name.encode("utf-8") + b"\x00"

    # CG 表：group -> 首图索引 + 组名
    groups = []
    seen = set()
    for g in gallery:
        if g["group"] in seen:
            continue
        seen.add(g["group"])
        groups.append((g["group"], g.get("label", g["group"]),
                       name_idx.get(g["image"], 0xFFFF)))
    cg_blob = bytearray()
    cg_offs = []
    for gid, label, first_img in groups:
        cg_offs.append(len(cg_blob))
        cg_blob += label.encode("utf-8") + b"\x00"
    # 每组的全部图片（用于鉴赏翻页）
    group_members = defaultdict(list)
    for g in gallery:
        group_members[g["group"]].append(name_idx.get(g["image"], 0xFFFF))
    members = bytearray()
    member_offs = []
    for gid, _, _ in groups:
        member_offs.append(len(members))
        imgs = group_members[gid]
        members += struct.pack("<H", len(imgs)) + struct.pack(f"<{len(imgs)}H", *imgs)

    # 章节表（catalog.chapters）
    ch_blob = bytearray()
    ch_offs = []
    chapters = catalog.get("chapters", [])
    for ch in chapters:
        ch_offs.append(len(ch_blob))
        ch_blob += ch["name"].encode("utf-8") + b"\x00"
    endings = catalog.get("endings", {})
    end_blob = bytearray()
    end_offs = []
    for sid, name in endings.items():
        end_offs.append((len(end_blob), int(sid)))
        end_blob += name.encode("utf-8") + b"\x00"

    # ---- 组装（v2：每段显式 offset，C 侧不做顺序推导）
    n_scenes = len(scene_table)
    n_names = len(entries)
    n_speakers = len(sp_offs)
    n_chapters = len(chapters)
    n_endings = len(endings)

    # 场景表 {id, route, first_op, op_count, name_off}
    scene_names = bytearray()
    scene_name_off = {}
    scene_tbl = bytearray()
    for sid, first, count in scene_table:
        meta = catalog["scenes"].get(str(sid), {})
        nm = str(meta.get("name", sid))
        route = int(meta.get("route", 0))
        if nm not in scene_name_off:
            scene_name_off[nm] = len(scene_names)
            scene_names += nm.encode("utf-8") + b"\x00"
        scene_tbl += struct.pack("<HHIIII", sid, route, first, count,
                                 scene_name_off[nm], 0)

    def offsets_blob(offs, blob):
        return struct.pack(f"<{len(offs)}I", *offs) + bytes(blob)

    sp_section = offsets_blob(sp_offs, sp_blob)
    cg_section = offsets_blob(cg_offs, cg_blob)
    ch_section = offsets_blob(ch_offs, ch_blob)
    end_section = offsets_blob([o for o, _ in end_offs], end_blob)
    member_section = offsets_blob(member_offs, members)
    choice_section = offsets_blob([sum(len(c) for c in choice_entries[:i])
                                   for i in range(len(choice_entries))],
                                  b"".join(choice_entries))
    ch_meta = bytearray()
    for ch in chapters:
        ch_meta += struct.pack("<HHHH", int(ch["id"]), int(ch.get("route", 0)),
                               int(ch.get("id", 0)), 0)
    end_scenes = bytearray()
    for _, sid in end_offs:
        end_scenes += struct.pack("<H", sid)

    sections = [
        ("scene_tbl", bytes(scene_tbl)),
        ("scene_names", bytes(scene_names)),
        ("speaker", sp_section),
        ("cg_names", cg_section),
        ("cg_members", member_section),
        ("chapters", ch_section),
        ("chapter_meta", bytes(ch_meta)),
        ("choices", choice_section),
        ("endings", end_section),
        ("ending_scenes", bytes(end_scenes)),
        ("ops", bytes(op_stream)),
        ("textidx", bytes(textidx)),
        ("textdata", bytes(textdata)),
    ]
    hdr_len = 8 + 12 * 2 + 15 * 4
    offs = {}
    cur = hdr_len
    for name, data in sections:
        cur = (cur + 3) & ~3
        offs[name] = cur
        cur += len(data)
    total = (cur + 3) & ~3

    flow_scene = int(catalog.get("flowScene", catalog.get("initial", 1)))
    initial_scene = int(catalog.get("initial", flow_scene))
    hdr = struct.pack("<8s12H15I", SCN_MAGIC, 3,
                      n_scenes, n_speakers, len(text_blocks), len(groups), n_names,
                      n_chapters, len(choice_entries), n_endings, (max(var_idx.values()) + 1 if var_idx else 0),
                      flow_scene, initial_scene,
                      offs["ops"], len(op_stream), offs["textidx"], offs["textdata"],
                      offs["scene_tbl"], offs["scene_names"], offs["speaker"],
                      offs["cg_names"], offs["cg_members"], offs["chapters"],
                      offs["chapter_meta"], offs["choices"], offs["endings"],
                      offs["ending_scenes"], total)
    assert len(hdr) == hdr_len, (len(hdr), hdr_len)

    buf = bytearray(hdr)
    for name, data in sections:
        while len(buf) < offs[name]:
            buf += b"\x00"
        buf += data
    while len(buf) < total:
        buf += b"\x00"
    scn = buf

    if args.dump_patch and patch_dump:
        with open(args.dump_patch, "wb") as f:
            f.write(struct.pack("<I", len(patch_dump)))
            for d in patch_dump:
                f.write(struct.pack("<IHHI", d["index"], d["w"], d["h"], len(d["pixels"])))
                for (px, py, c) in d["pixels"]:
                    f.write(struct.pack("<HHH", px, py, c))

    scn_path = os.path.join(args.out, "paper_scn.bin")
    with open(scn_path, "wb") as f:
        f.write(bytes(scn))
    scn_size = os.path.getsize(scn_path)

    # 头部还缺 speaker/cg 计数偏移信息：补一张小索引表在末尾更简单——这里改用固定布局的
    # 说明：CG/章节/选择/结局表的偏移已经写进头部之后由 C 侧按顺序推导。为避免歧义，
    # 额外写一份 JSON 清单供 C 侧常量与测试比对。
    manifest = {
        "scenes": len(scene_table), "ops": len(op_stream) // 8,
        "images": n_names, "frames": len(frame_ids), "sprites": len(sprite_ids),
        "patches": patch_used, "speakers": len(speakers), "text_blocks": len(text_blocks),
        "text_raw": sum(len(r) for _, r in text_blocks),
        "text_comp": len(textdata), "cg_groups": len(groups),
        "chapters": len(chapters), "choices": len(choice_entries),
        "offsets": offs, "img_bytes": img_size, "scn_bytes": scn_size,
        "img_quality": args.quality,
    }
    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    with io.open(args.report, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)

    # ---- 字库字符集
    charset = set()
    for sid, ops in scenes.items():
        for op in ops:
            if op["op"] == "text":
                charset.update(op["text"])
                if op.get("speaker"):
                    charset.update(op["speaker"])
            elif op["op"] == "choice":
                for it in op.get("items", []):
                    charset.update(it["text"])
    charset.update("　 ")
    out_font_dir = "assets/fonts"
    os.makedirs(out_font_dir, exist_ok=True)
    sym_path = os.path.join(out_font_dir, "paper_cjk_symbols.txt")
    with io.open(sym_path, "w", encoding="utf-8") as f:
        for ch in sorted(charset):
            f.write(ch + "\n")
    print(f"charset: {len(charset)} chars -> {sym_path}")

    print(f"image pack: {img_size} B  ({img_size/1048576:.2f} MiB)")
    print(f"script pack: {scn_size} B  ({scn_size/1048576:.2f} MiB)  "
          f"text {len(textdata)} B compressed / {manifest['text_raw']} B raw")
    total_pack = img_size + scn_size
    print(f"packs total: {total_pack} B ({total_pack/1048576:.2f} MiB)")


if __name__ == "__main__":
    main()
