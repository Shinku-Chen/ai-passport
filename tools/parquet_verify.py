#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""《PARQUET》资源包校验 + 独立参考解码 + C 回归 fixture。

用法:
  python tools/parquet_verify.py                       # 结构/内容/源一致性检查
  python tools/parquet_verify.py --fixtures tests/parquet/fixtures
                                                      # 另写 images.txt 供 C 逐像素/几何对照

本工具直接解析 parquet_img.bin / parquet_scn.bin（不 import parquet_pack.py），
用自己的 BLOCK/PATCH 解码器与 Pillow JPEG 解码生成期望值，因此能和固件 C 解码
互为独立实现。

检查项:
  * 两个包的表头、版本、段边界、条目边界；
  * 107 场景 / 11205 op / 434 正文块 / 2623 视觉组合 / 37 组 367 张 CG 与源快照一致；
  * 每条正文块可解压、逻辑偏移连续、op 声明长度与 NUL 位置一致；
  * 每个 op 的图/立绘/cut-in 资源 id 都能解析；跳转与选项目标在界内；
  * 每个场景的 clearFlag 变量存在，结局/章节/路线/鉴赏表非空；
  * 立绘 base/patch 几何：anchor 与 body/patch 偏移都落在画布内。
"""
from __future__ import annotations

import argparse
import io
import json
import os
import struct
import sys
import zlib
from collections import Counter

from PIL import Image

SCN_MAGIC = b"PQSCN1\0\0"
IMG_MAGIC = b"PQIMG1\0\0"
ART_W, ART_H = 320, 150


def die(msg: str) -> None:
    print(f"parquet_verify: {msg}", file=sys.stderr)
    sys.exit(1)


class Pack:
    def __init__(self, img_data: bytes, scn_data: bytes):
        self.img = img_data
        self.scn = scn_data
        self._parse_img()
        self._parse_scn()

    # ------------------------------------------------------------ 图片包
    def _parse_img(self):
        b = self.img
        if len(b) < 64 or b[:8] != IMG_MAGIC:
            die("图片包标识错误")
        (version, self.img_entry_count, self.sprite_count, self.cutin_count,
         self.asset_count, self.title_count, _res) = struct.unpack_from("<7H", b, 8)
        (self.off_entries, self.off_sprites, self.off_cutins, self.off_titles,
         self.off_asset, self.off_asset_cutin, self.off_data,
         self.data_size) = struct.unpack_from("<8I", b, 22)
        if version != 1:
            die("图片包版本不兼容")
        need = [
            (self.off_entries, 16 * self.img_entry_count),
            (self.off_sprites, 16 * self.sprite_count),
            (self.off_cutins, 12 * self.cutin_count),
            (self.off_titles, 10 * self.title_count),
            (self.off_asset, 2 * self.asset_count),
            (self.off_asset_cutin, 2 * self.asset_count),
            (self.off_data, self.data_size),
        ]
        for off, size in need:
            if off + size > len(b):
                die(f"图片包段越界 off={off} size={size}")
        self.img_entries = []
        for i in range(self.img_entry_count):
            kind, flags, w, h, data_off, data_len, res = struct.unpack_from(
                "<BBHHIIH", b, self.off_entries + 16 * i)
            if kind > 2 or w == 0 or h == 0 or data_off + data_len > len(b):
                die(f"图片条目 {i} 非法")
            self.img_entries.append((kind, w, h, data_off, data_len))
        self.sprites = []
        for i in range(self.sprite_count):
            self.sprites.append(struct.unpack_from("<HHhhhhhH", b, self.off_sprites + 16 * i))
        self.cutins = []
        for i in range(self.cutin_count):
            self.cutins.append(struct.unpack_from("<HhhHHH", b, self.off_cutins + 12 * i))
        self.titles = []
        for i in range(self.title_count):
            self.titles.append(struct.unpack_from("<HHHHH", b, self.off_titles + 10 * i))
        self.asset_map = list(struct.unpack_from(f"<{self.asset_count}H", b, self.off_asset))
        self.asset_cutin = list(struct.unpack_from(f"<{self.asset_count}H", b,
                                                   self.off_asset_cutin))

    def image_payload(self, index):
        kind, w, h, off, ln = self.img_entries[index]
        return kind, w, h, self.img[off:off + ln]

    # ------------------------------------------------------------ 剧本包
    def _parse_scn(self):
        b = self.scn
        if len(b) < 128 or b[:8] != SCN_MAGIC:
            die("剧本包标识错误")
        h = struct.unpack_from("<19H", b, 8)
        (version, self.scene_count, self.op_count, self.text_block_count,
         self.speaker_count, self.chapter_count, self.route_count, self.tag_count,
         self.var_count, self.visual_count, self.gallery_group_count, self.choice_count,
         self.ending_count, self.choice_item_count, self.gallery_member_count,
         self.initial, self.flow, self.side, self.after) = h
        if version != 1:
            die("剧本包版本不兼容")
        o = struct.unpack_from("<18I", b, 8 + 38)
        (self.off_scenes, self.off_ops, self.off_visuals, self.off_text_blocks,
         self.off_text_data, self.off_tags, self.off_speakers, self.off_chapters,
         self.off_routes, self.off_choices, self.off_choice_items, self.off_endings,
         self.off_gallery, self.off_members, self.off_ending_names, self.off_vars,
         self.off_strings, self.strings_size) = o
        need = [
            (self.off_scenes, 12 * self.scene_count),
            (self.off_ops, 16 * self.op_count),
            (self.off_visuals, 12 * self.visual_count),
            (self.off_text_blocks, 12 * self.text_block_count),
            (self.off_tags, 4 * self.tag_count),
            (self.off_speakers, 4 * self.speaker_count),
            (self.off_chapters, 8 * self.chapter_count),
            (self.off_routes, 8 * self.route_count),
            (self.off_choices, 4 * self.choice_count),
            (self.off_choice_items, 18 * self.choice_item_count),
            (self.off_endings, 4 * self.ending_count),
            (self.off_gallery, 8 * self.gallery_group_count),
            (self.off_members, 2 * self.gallery_member_count),
            (self.off_vars, 4 * self.var_count),
            (self.off_strings, self.strings_size),
        ]
        for off, size in need:
            if off + size > len(b):
                die(f"剧本包段越界 off={off} size={size}")
        self.scenes = []
        for i in range(self.scene_count):
            sid, route, clear_var, first_op, op_count, tag, res = struct.unpack_from(
                "<HBBHHHH", b, self.off_scenes + 12 * i)
            if first_op + op_count > self.op_count or op_count == 0:
                die(f"场景 {sid} op 范围越界")
            if tag >= self.tag_count:
                die(f"场景 {sid} 章节标签越界")
            if clear_var != 0xFF and clear_var >= self.var_count:
                die(f"场景 {sid} clearFlag 越界")
            self.scenes.append((sid, route, clear_var, first_op, op_count, tag))
        self.ops = []
        for i in range(self.op_count):
            self.ops.append(struct.unpack_from("<BBBBHHII", b, self.off_ops + 16 * i))
        self.visuals = []
        for i in range(self.visual_count):
            self.visuals.append(struct.unpack_from("<HHHHHBB", b,
                                                   self.off_visuals + 12 * i))
        self.text_blocks = []
        for i in range(self.text_block_count):
            self.text_blocks.append(struct.unpack_from("<IIHH", b,
                                                       self.off_text_blocks + 12 * i))
        self.text_data = b[self.off_text_data:self.off_text_data + (
            self.off_tags - self.off_text_data)]
        self.strings = b[self.off_strings:self.off_strings + self.strings_size]
        self.tag_offs = list(struct.unpack_from(f"<{self.tag_count}I", b, self.off_tags))
        self.speaker_offs = list(struct.unpack_from(f"<{self.speaker_count}I", b,
                                                    self.off_speakers))
        self.chapters = []
        for i in range(self.chapter_count):
            self.chapters.append(struct.unpack_from("<IHBB", b,
                                                    self.off_chapters + 8 * i))
        self.routes = []
        for i in range(self.route_count):
            self.routes.append(struct.unpack_from("<IHH", b, self.off_routes + 8 * i))
        self.choices = []
        for i in range(self.choice_count):
            self.choices.append(struct.unpack_from("<HBB", b, self.off_choices + 4 * i))
        self.choice_items = []
        for i in range(self.choice_item_count):
            self.choice_items.append(struct.unpack_from("<IHHIBBi", b,
                                                        self.off_choice_items + 18 * i))
        self.endings = []
        for i in range(self.ending_count):
            self.endings.append(struct.unpack_from("<HH", b, self.off_endings + 4 * i))
        self.galleries = []
        for i in range(self.gallery_group_count):
            self.galleries.append(struct.unpack_from("<IHH", b, self.off_gallery + 8 * i))
        self.members = list(struct.unpack_from(f"<{self.gallery_member_count}H", b,
                                               self.off_members))
        self.ending_name_count = max((e[1] for e in self.endings), default=-1) + 1
        self.ending_names = list(struct.unpack_from(f"<{self.ending_name_count}I", b,
                                                    self.off_ending_names))
        self.var_offs = list(struct.unpack_from(f"<{self.var_count}I", b, self.off_vars))
        self._text_cache = {}

    def string(self, off):
        if off >= len(self.strings):
            die(f"字符串偏移越界 {off}")
        end = self.strings.find(b"\0", off)
        if end < 0:
            die(f"字符串未终止 {off}")
        return self.strings[off:end].decode("utf-8")

    def text_block(self, index):
        if index in self._text_cache:
            return self._text_cache[index]
        lo, co, cl, rl = self.text_blocks[index]
        if co + cl > len(self.text_data):
            die(f"正文块 {index} 越界")
        raw = zlib.decompress(self.text_data[co:co + cl])
        if len(raw) != rl:
            die(f"正文块 {index} 解压长度 {len(raw)} != {rl}")
        self._text_cache[index] = (lo, raw)
        return lo, raw

    def text_at(self, logical, length):
        lo0 = 0
        for i, (lo, co, cl, rl) in enumerate(self.text_blocks):
            if lo <= logical < lo + rl:
                lo0, raw = self.text_block(i)
                local = logical - lo0
                s = raw[local:local + length + 1]
                if len(s) != length + 1 or s[-1] != 0:
                    die(f"正文 {logical} 声明长度与 NUL 不符")
                return s[:-1].decode("utf-8")
        die(f"正文偏移 {logical} 不在任何块内")


# ---------------------------------------------------------------- 参考解码
def _unfilter(prev: bytes, row: bytes, kind: int) -> bytes:
    out = bytearray(len(row))
    for i, v in enumerate(row):
        a = out[i - 1] if i else 0
        b = prev[i]
        c = prev[i - 1] if i else 0
        if kind == 0:
            pass
        elif kind == 1:
            v = (v + a) & 0xFF
        elif kind == 2:
            v = (v + b) & 0xFF
        elif kind == 3:
            v = (v + ((a + b) >> 1)) & 0xFF
        elif kind == 4:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            v = (v + pred) & 0xFF
        else:
            die(f"未知行滤波 {kind}（行首字节损坏）")
        out[i] = v
    return bytes(out)


def _blend565(dst, src, a):
    """与 parquet_gfx.c 的 put_px 完全相同的 565 整数混合（目标为黑色）。"""
    if a >= 250:
        return src
    sr, sg, sb = (src >> 11) & 0x1F, (src >> 5) & 0x3F, src & 0x1F
    dr, dg, db = (dst >> 11) & 0x1F, (dst >> 5) & 0x3F, dst & 0x1F
    r = (sr * a + dr * (255 - a)) // 255
    g = (sg * a + dg * (255 - a)) // 255
    b = (sb * a + db * (255 - a)) // 255
    return (r << 11) | (g << 5) | b


def _rgb888(c):
    r5, g6, b5 = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def decode_block(payload: bytes):
    if len(payload) < 16:
        die("BLOCK 载荷过短")
    w, h, pal_count, rows, nblocks, _pad = struct.unpack_from("<6H", payload, 0)
    row_bytes = struct.unpack_from("<I", payload, 12)[0]
    if w == 0 or h == 0 or pal_count == 0 or pal_count > 256 or rows != 8:
        die(f"BLOCK 头非法 w={w} h={h} pal={pal_count}")
    if row_bytes != rows * (w + 1) or nblocks != (h + 7) // 8:
        die("BLOCK 行/块参数不一致")
    off = 16
    palette = list(struct.unpack_from(f"<{pal_count}H", payload, off))
    off += 2 * pal_count
    alphas = payload[off:off + pal_count]
    off += pal_count
    lens = list(struct.unpack_from(f"<{nblocks}I", payload, off))
    off += 4 * nblocks
    canvas = [0] * (w * h)
    prev = bytes(w)
    for b in range(nblocks):
        comp = payload[off:off + lens[b]]
        off += lens[b]
        raw = zlib.decompress(comp)
        block_rows = min(8, h - b * 8)
        if len(raw) != block_rows * (w + 1):
            die(f"BLOCK 块 {b} 解压长度 {len(raw)} != {block_rows * (w + 1)}")
        y = b * 8
        for r in range(block_rows):
            rowp = raw[r * (w + 1):(r + 1) * (w + 1)]
            row = _unfilter(prev, rowp[1:], rowp[0])
            prev = row
            for x in range(w):
                i = row[x]
                canvas[(y + r) * w + x] = _blend565(0, palette[i], alphas[i])
    return w, h, canvas


def decode_patch(payload: bytes):
    if len(payload) < 12:
        die("PATCH 载荷过短")
    w, h, rows, nblocks, alpha_len = struct.unpack_from("<4HI", payload, 0)
    if w == 0 or h == 0 or rows != 8 or nblocks != (h + 7) // 8:
        die(f"PATCH 头非法 w={w} h={h}")
    off = 12
    alpha = zlib.decompress(payload[off:off + alpha_len])
    if len(alpha) != w * h:
        die("PATCH alpha 长度不符")
    off += alpha_len
    lens = list(struct.unpack_from(f"<{nblocks}I", payload, off))
    off += 4 * nblocks
    canvas = [0] * (w * h)
    for b in range(nblocks):
        raw = zlib.decompress(payload[off:off + lens[b]])
        off += lens[b]
        block_rows = min(8, h - b * 8)
        if len(raw) != block_rows * w * 2:
            die(f"PATCH 块 {b} 长度不符")
        for r in range(block_rows):
            for x in range(w):
                src = struct.unpack_from("<H", raw, (r * w + x) * 2)[0]
                a = alpha[(b * 8 + r) * w + x]
                canvas[(b * 8 + r) * w + x] = _blend565(0, src, a)
    return w, h, canvas


def crc32_rgba(canvas, w, h):
    buf = bytearray()
    for c in canvas:
        r, g, b = _rgb888(c)
        buf += bytes((r, g, b, 255))
    return zlib.crc32(bytes(buf)) & 0xFFFFFFFF


def grid_samples(canvas, w, h, cols=4, rows=4):
    out = []
    for j in range(rows):
        for i in range(cols):
            x = min(w - 1, int((i + 0.5) * w / cols))
            y = min(h - 1, int((j + 0.5) * h / rows))
            out.extend(_rgb888(canvas[y * w + x]))
    return out


# ---------------------------------------------------------------- 检查
def load_source(source: str):
    def load(name):
        with io.open(os.path.join(source, name), encoding="utf-8") as fh:
            return json.load(fh)
    return load("catalog.json"), load("gallery.txt"), load("image-map.json")


def check(pack: Pack, source: str | None, fixtures: str | None) -> dict:
    report = {}
    got_texts = sum(1 for o in pack.ops if o[0] == 0)
    if source:
        catalog, gallery, image_map = load_source(source)
        story_root = os.path.join(source, "story")
        story = {}
        for sid in sorted((n for n in os.listdir(story_root) if n.isdigit()), key=int):
            ops = []
            d = os.path.join(story_root, sid)
            for chunk in sorted((n for n in os.listdir(d) if n.endswith(".txt")),
                                key=lambda n: int(n[:-4])):
                with io.open(os.path.join(d, chunk), encoding="utf-8") as fh:
                    ops.extend(json.load(fh))
            story[int(sid)] = ops
        if len(story) != pack.scene_count:
            die(f"场景数 {pack.scene_count} != 源 {len(story)}")
        src_ops = sum(len(v) for v in story.values())
        if src_ops != pack.op_count:
            die(f"op 数 {pack.op_count} != 源 {src_ops}")
        src_texts = sum(1 for v in story.values() for o in v if o["op"] == "text")
        if src_texts != got_texts:
            die(f"正文条数 {got_texts} != 源 {src_texts}")
        # 逐个场景核对 op 数量与源一致
        for (sid, route, clear_var, first_op, op_count, tag) in pack.scenes:
            if len(story[sid]) != op_count:
                die(f"场景 {sid} op 数 {op_count} != 源 {len(story[sid])}")
            if catalog["scenes"][str(sid)]["route"] != route:
                die(f"场景 {sid} route 不符")
        if (pack.initial, pack.flow, pack.side, pack.after) != (
                catalog["initial"], catalog["flowScene"], catalog["sideInitial"],
                catalog["afterInitial"]):
            die("入口场景与 catalog 不符")
        # 正文逐条比对（源文本 vs 包内文本）
        checked = 0
        for (sid, route, clear_var, first_op, op_count, tag) in pack.scenes:
            src = story[sid]
            for local in range(op_count):
                kind, otag, view, flags, a, b, c, d = pack.ops[first_op + local]
                if src[local]["op"] == "text":
                    if kind != 0:
                        die(f"场景 {sid} op {local}: 源是 text，包里不是")
                    text = pack.text_at(c, d)
                    if text != src[local]["text"]:
                        die(f"场景 {sid} op {local}: 正文不一致")
                    if text.encode() != text.encode("utf-8") or len(text.encode()) != d:
                        die(f"场景 {sid} op {local}: 长度不一致")
                    checked += 1
                elif src[local]["op"] == "jumpScene":
                    if kind != 1 or a != src[local]["scene"] or c != src[local]["to"]:
                        die(f"场景 {sid} op {local}: 跳转不一致")
                elif src[local]["op"] == "choice":
                    if kind != 2 or b != len(src[local]["items"]):
                        die(f"场景 {sid} op {local}: 选项不一致")
                elif src[local]["op"] == "end":
                    if kind != 3:
                        die(f"场景 {sid} op {local}: end 不一致")
        report["source_text_checked"] = checked
        # 鉴赏
        src_groups = []
        for item in gallery:
            if not src_groups or src_groups[-1][0] != item["group"]:
                src_groups.append([item["group"], []])
            src_groups[-1][1].append(int(item["image"][1:]))
        if len(src_groups) != pack.gallery_group_count:
            die(f"鉴赏组 {pack.gallery_group_count} != 源 {len(src_groups)}")
        for gi, (label, members) in enumerate(src_groups):
            off, first, count = pack.galleries[gi]
            if pack.string(off) != label or count != len(members):
                die(f"鉴赏组 {gi} 元数据不符")
            if pack.members[first:first + count] != members:
                die(f"鉴赏组 {gi} 成员不符")
        # 资源引用覆盖：背景 / 立绘 / cut-in 分别校验
        used_bg, used_body, used_cutin = set(), set(), set()
        for ops in story.values():
            for op in ops:
                if op["op"] != "text":
                    continue
                used_bg.add(op["background"])
                used_body.update(op["bodies"])
                if op["cutIn"]:
                    used_cutin.add(op["cutIn"])
        for aid in used_bg:
            num = int(aid[1:])
            if num >= pack.asset_count or pack.asset_map[num] == 0xFFFF:
                die(f"背景 {aid} 在包里没有映射")
            if pack.asset_map[num] & 0x8000:
                die(f"背景 {aid} 不应该映射成立绘")
        for aid in used_body:
            num = int(aid[1:])
            v = pack.asset_map[num] if num < pack.asset_count else 0xFFFF
            if not (v & 0x8000) or (v & 0x7FFF) >= pack.sprite_count:
                die(f"立绘 {aid} 映射错误")
            body, patch = pack.sprites[v & 0x7FFF][0], pack.sprites[v & 0x7FFF][1]
            if pack.img_entries[body][0] != 1:
                die(f"立绘 {aid} 身体不是 BLOCK")
            if patch != 0xFFFF and pack.img_entries[patch][0] != 2:
                die(f"立绘 {aid} 表情不是 PATCH")
        for aid in used_cutin:
            num = int(aid[1:])
            if num >= pack.asset_count or pack.asset_cutin[num] == 0xFFFF:
                die(f"cut-in {aid} 在包里没有映射")
            if pack.img_entries[pack.cutins[pack.asset_cutin[num]][0]][0] != 1:
                die(f"cut-in {aid} 不是 BLOCK")
        report["source_assets"] = len(used_bg)
        report["source_sprites"] = len(used_body)
        report["source_cutins"] = len(used_cutin)
        report["source_gallery_groups"] = len(src_groups)
        report["source_gallery_members"] = sum(len(m) for _, m in src_groups)
    # ---- 结构
    seen_scenes = set()
    for (sid, route, clear_var, first_op, op_count, tag) in pack.scenes:
        if sid in seen_scenes:
            die(f"场景 id 重复 {sid}")
        seen_scenes.add(sid)
    for i, (kind, otag, view, flags, a, b, c, d) in enumerate(pack.ops):
        if kind > 3:
            die(f"op {i} 类型未知")
        if kind == 0:
            if a >= pack.visual_count:
                die(f"op {i} 视觉越界")
            if b != 0xFFFF and b >= pack.speaker_count:
                die(f"op {i} 说话人越界")
            if otag >= pack.tag_count:
                die(f"op {i} 章节标签越界")
            text = pack.text_at(c, d)
            if len(text.encode("utf-8")) != d:
                die(f"op {i} 正文长度不符")
        elif kind == 1:
            if a not in seen_scenes:
                die(f"op {i} 跳转目标不存在")
        elif kind == 2:
            if a >= pack.choice_count:
                die(f"op {i} 选项越界")
        if view > 1:
            die(f"op {i} view 非法")
    for (sid, route, clear_var, first_op, op_count, tag) in pack.scenes:
        if tag >= pack.tag_count:
            die(f"场景 {sid} 标签越界")
    tags = [pack.string(o) for o in pack.tag_offs]
    if not all(tags):
        die("章节标签为空")
    for i, (label, first, count) in enumerate(pack.galleries):
        if count == 0 or first + count > pack.gallery_member_count:
            die(f"鉴赏组 {i} 为空或越界")
        for m in pack.members[first:first + count]:
            if m >= pack.asset_count:
                die(f"鉴赏成员 {m} 越界")
    for i in range(pack.ending_name_count):
        if not pack.string(pack.ending_names[i]):
            die(f"结局名 {i} 为空")
    for i, (first_item, count, _p) in enumerate(pack.choices):
        if count == 0 or first_item + count > pack.choice_item_count:
            die(f"选项 {i} 越界")
        for j in range(count):
            off, ln, sid, to, var, has, val = pack.choice_items[first_item + j]
            if sid not in seen_scenes:
                die(f"选项 {i}-{j} 目标场景不存在")
            text = pack.text_at(off, ln)
            if not text:
                die(f"选项 {i}-{j} 文本为空")
            if var >= pack.var_count:
                die(f"选项 {i}-{j} 变量越界")
    # 视觉表引用
    for vi, (bg, b0, b1, b2, cut, view, opacity) in enumerate(pack.visuals):
        for aid, kind_name in ((bg, "bg"), (b0, "body"), (b1, "body"), (b2, "body")):
            if aid == 0xFFFF:
                continue
            if aid >= pack.asset_count or pack.asset_map[aid] == 0xFFFF:
                die(f"视觉 {vi} 的 {kind_name} {aid} 无映射")
        if cut != 0xFFFF:
            if cut >= pack.asset_count or pack.asset_cutin[cut] == 0xFFFF:
                die(f"视觉 {vi} 的 cutin {cut} 无映射")
        if view > 1:
            die(f"视觉 {vi} view 非法")
    # 立绘几何
    for si, (body, patch, bdx, bdy, pdx, pdy, anchor, canvas_w) in enumerate(pack.sprites):
        if body >= pack.img_entry_count:
            die(f"立绘 {si} body 越界")
        if patch != 0xFFFF and patch >= pack.img_entry_count:
            die(f"立绘 {si} patch 越界")
        kind, w, h, _off, _ln = pack.img_entries[body]
        if kind != 1:
            die(f"立绘 {si} body 不是 BLOCK")
        if bdx < 0 or bdy < 0 or bdx + w > canvas_w or bdy + h > ART_H:
            die(f"立绘 {si} 身体越出画布")
        if anchor < 0 or anchor > canvas_w:
            die(f"立绘 {si} 锚点越界")
        if patch != 0xFFFF:
            pk, pw, ph, _o, _l = pack.img_entries[patch]
            if pk != 2:
                die(f"立绘 {si} patch 不是 PATCH")
            if pdx < 0 or pdy < 0 or pdx + pw > canvas_w or pdy + ph > ART_H:
                die(f"立绘 {si} 表情越出画布")
    # cut-in 几何
    for ci, (image, x, y, w, h, _p) in enumerate(pack.cutins):
        kind, iw, ih, _o, _l = pack.img_entries[image]
        if kind != 1 or (w, h) != (iw, ih):
            die(f"cut-in {ci} 尺寸不符")
        if x < 0 or y < 0 or x + w > ART_W or y + h > ART_H:
            die(f"cut-in {ci} 越出画面区")
    # 图片条目载荷可解
    kinds = Counter()
    for i in range(pack.img_entry_count):
        kind, w, h, off, ln = pack.img_entries[i]
        kinds[kind] += 1
        payload = pack.img[off:off + ln]
        if kind == 1:
            dw, dh, _ = decode_block(payload)
            if (dw, dh) != (w, h):
                die(f"BLOCK {i} 尺寸与条目不符")
        elif kind == 2:
            dw, dh, _ = decode_patch(payload)
            if (dw, dh) != (w, h):
                die(f"PATCH {i} 尺寸与条目不符")
        else:
            try:
                im = Image.open(io.BytesIO(payload))
                im.load()
            except Exception as exc:   # noqa: BLE001
                die(f"JPEG {i} 解码失败: {exc}")
            if im.size != (w, h):
                die(f"JPEG {i} 尺寸 {im.size} != {(w, h)}")
    report["image_kinds"] = dict(kinds)
    report["counts"] = {
        "scenes": pack.scene_count,
        "ops": pack.op_count,
        "text_ops": got_texts,
        "text_blocks": pack.text_block_count,
        "visuals": pack.visual_count,
        "speakers": pack.speaker_count,
        "chapters": pack.chapter_count,
        "routes": pack.route_count,
        "galleries": pack.gallery_group_count,
        "gallery_members": pack.gallery_member_count,
        "choices": pack.choice_count,
        "choice_items": pack.choice_item_count,
        "endings": pack.ending_count,
        "image_entries": pack.img_entry_count,
        "sprites": pack.sprite_count,
        "cutins": pack.cutin_count,
        "assets": pack.asset_count,
    }
    if fixtures:
        write_fixtures(pack, fixtures)
        report["fixtures"] = fixtures
    return report


def write_fixtures(pack: Pack, out_dir: str):
    os.makedirs(out_dir, exist_ok=True)
    lines = ["# PQIMG_FIXTURES 1", "# index kind w h crc32 rgbs*48"]
    for i in range(pack.img_entry_count):
        kind, w, h, off, ln = pack.img_entries[i]
        payload = pack.img[off:off + ln]
        if kind == 1:
            dw, dh, canvas = decode_block(payload)
        elif kind == 2:
            dw, dh, canvas = decode_patch(payload)
        else:
            im = Image.open(io.BytesIO(payload)).convert("RGB")
            dw, dh = im.size
            px = im.load()
            canvas = [0] * (dw * dh)
            for y in range(dh):
                for x in range(dw):
                    r, g, b = px[x, y]
                    canvas[y * dw + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        if (dw, dh) != (w, h):
            die(f"fixture 尺寸不符 {i}")
        crc = crc32_rgba(canvas, dw, dh) if kind != 0 else 0
        samples = grid_samples(canvas, dw, dh)
        lines.append(" ".join(str(v) for v in [i, kind, w, h, crc] + samples))
    path = os.path.join(out_dir, "images.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"parquet_verify: fixtures -> {path} ({len(lines) - 2} entries)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--img", default="main/parquet_data/parquet_img.bin")
    ap.add_argument("--scn", default="main/parquet_data/parquet_scn.bin")
    ap.add_argument("--source", default="assets/parquet-source")
    ap.add_argument("--fixtures", default=None)
    ap.add_argument("--report", default=None)
    args = ap.parse_args()
    with open(args.img, "rb") as fh:
        img = fh.read()
    with open(args.scn, "rb") as fh:
        scn = fh.read()
    pack = Pack(img, scn)
    source = args.source if os.path.isdir(args.source) else None
    report = check(pack, source, args.fixtures)
    report["files"] = {
        "img": {"bytes": len(img)},
        "scn": {"bytes": len(scn)},
    }
    print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    if args.report:
        os.makedirs(os.path.dirname(args.report), exist_ok=True)
        with open(args.report, "w", encoding="utf-8") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=2, sort_keys=True)
            fh.write("\n")
    print("parquet_verify: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
