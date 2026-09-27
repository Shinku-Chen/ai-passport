#!/usr/bin/env python3
"""生成 AI Passport《天使☆騒々 RE-BOOT!》阅读器的中文字形子集(LVGL 9 位图字体)。

为什么不用 lv_font_conv:这个工具最后一版是 1.5.3(2021),在 Node v24 下写出的
字形位图是坏的 —— 同一个 OTF、同一套参数,`--no-compress` 与
`--no-compress --no-prefilter` 产出的字节完全相同,按 LVGL 的 PLAIN 4bpp 读法解码
全是噪点(LVGL 自带的 Montserrat 字体用同一解码器完全正常),设备上表现为"文字
全部乱码"。这里改成直接用 Pillow(FreeType)栅格化 + 自己写 LVGL 字体格式,不依赖
Node;生成后会回读自己写出的 C 文件,与栅格化结果逐个码位逐像素比对。

字体来源与许可:
  Noto Sans SC Regular,2.004-H2(SIL Open Font License 1.1,允许再分发与子集化)。
  本文件生成的是它的子集,许可证随字体一起记录(见 C 文件头)。源字体不提交进
  仓库,重新生成时用 --font 指向本地副本(LVGL 组件缓存里的
  tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf 就是完整字体)。

码位来源:
  assets/fonts/tsxx_symbols.txt —— tools/tsxx_pack.py --symbols-out 写出的资源包
  码位表(3417 个码位,顺序 = 资源包的词频序,与包里的符号码一一对应:前 255 个
  在正文里走 1 字节码)。
  输入清单的顺序原样保留;只有写进 cmap 的 unicode_list 必须升序 —— LVGL 用二分
  查找取码位,这一点与词频序无关。

字形格式(与 LVGL 9.5/9.6 的 PLAIN 解码一致,见 lv_font_fmt_txt.c):
  - 4bpp,字形内**连续** nibble 打包(行间不补齐),字形之间按字节对齐;
    font_dsc.stride == 0 时 LVGL 正是这样跨行连续读 nibble 的;
  - cmap 全部用 SPARSE_TINY:本字体只覆盖资源包出现的码位,ASCII 不连续,
    且 FORMAT0_FULL 在 9.6 里要求逐码位一张 uint8 偏移表、漏填会崩,不适用;
  - ofs_y 按 LVGL 的绘制公式给出:
      letter_y = line_top + (line_height - base_line) - box_h - ofs_y
    即 ofs_y = 基线 - 字形盒底,基线以下为负。

用法:
  python tools/tsxx_font.py --font <NotoSansSC-Regular.ttf>          # 生成 + 校验
  python tools/tsxx_font.py --check --font <NotoSansSC-Regular.ttf>  # 只自检(不写文件)
  python tools/tsxx_font.py --font <NotoSansSC-Regular.ttf> --estimate-size 20

生成物 assets/fonts/tsxx_cjk_16.c 提交进仓库;源字体不上传。
"""

from __future__ import annotations

import argparse
import io
import os
import re
import struct
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - tool dependency
    sys.exit("需要 Pillow: python -m pip install pillow")

SIZES = (16,)            # 生成的像素字号(正文/名字/菜单统一用 16px)
BPP = 4
# 行高固定为 字号+4:界面按"16px 字、20px 行距"排版。Noto Sans SC 的自然行高在
# 16px 字号下约 25px,直接用会一屏少放两行;多出来的行距在自然行框里上下各裁
# 一半,基线随之平移,字形本身不受影响(与 tools/atri_font.py 同一套约定)。
EXTRA_LEADING = 4
DEFAULT_SYMBOLS = "assets/fonts/tsxx_symbols.txt"
DEFAULT_OUT_DIR = "assets/fonts"
# lv_font_fmt_txt_glyph_dsc_t.bitmap_index 默认是 20 位(LV_FONT_FMT_TXT_LARGE == 0)。
BITMAP_INDEX_LIMIT = 1 << 20
# 单个 SPARSE_TINY cmap 里,码位相对 range_start 的偏移要装进 uint16
# (get_glyph_dsc_id 里 `uint16_t key = rcp`),而 range_length 也是 uint16,
# 所以一个块的最大跨度是 0xFFFE(range_length = 0xFFFF)。跨度超了就切多张 cmap。
CMAP_SPAN_LIMIT = 0xFFFE


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


# ---------------------------------------------------------------- 码位清单
def read_symbols(path: str) -> list:
    """读取码位清单,返回码位列表(保持文件里的顺序,就地去重)。

    文件是一整串字符(词频序),不是"每行一个码位",所以不能按行拆:U+000A 本身
    就是码位表里的一个符号(剧本正文的换行)。
    用通用换行读入很关键:仓库里这份文件是在 Windows 上写的,码位表中间那个
    U+000A 被写成了 CRLF(多出一个 U+000D 字节);通用换行把 CRLF 还原成单个
    U+000A,读出来正好是资源包里的 3417 个码位。
    """
    try:
        with open(path, "r", encoding="utf-8", newline=None) as handle:
            text = handle.read()
    except OSError as exc:
        raise SystemExit(f"读不到码位清单 {path}: {exc}")
    except UnicodeDecodeError as exc:
        raise SystemExit(f"码位清单不是 UTF-8: {exc}")

    codepoints = []
    seen = set()
    duplicates = 0
    for ch in text:
        cp = ord(ch)
        if cp < 0x20 and cp not in (0x09, 0x0A):
            raise SystemExit(f"码位清单里有意外控制字符 U+{cp:04X},先确认文件没被改坏")
        if cp in seen:
            duplicates += 1
            continue
        seen.add(cp)
        codepoints.append(cp)
    if duplicates:
        log(f"  提示: 清单里有 {duplicates} 个重复码位,已按首次出现去重")
    if not codepoints:
        raise SystemExit(f"码位清单是空的: {path}")
    return codepoints


def raster_codepoints(codepoints: list) -> list:
    """去掉控制码位:U+000A 是码位表里的换行符号,LVGL 自己换行,不需要字形。

    这不是“缺字”,所以不能让覆盖检查把它当成源字体缺字;但它也必须在码位表里,
    所以只在这里过滤,不修改输入文件。
    """
    kept = [cp for cp in codepoints if cp >= 0x20]
    skipped = [cp for cp in codepoints if cp < 0x20]
    if skipped:
        log("  控制码位不生成字形(码位表里的换行符号,不是缺字): "
            + " ".join(f"U+{cp:04X}" for cp in skipped))
    return kept


# ---------------------------------------------------------------- 源字体覆盖
def _read_cmap_subtable(blob: bytes, off: int) -> set:
    """读一个 cmap 子表,返回有字形的码位集合(只处理 Unicode 子表用到的格式)。"""
    fmt = struct.unpack_from(">H", blob, off)[0]
    found = set()
    if fmt == 14:
        # Unicode Variation Sequences:只描述变体选择符组合,本身不提供字形。
        return found
    if fmt == 0:
        for code, gid in enumerate(blob[off + 6 : off + 6 + 256]):
            if gid:
                found.add(code)
    elif fmt == 4:
        seg_x2 = struct.unpack_from(">H", blob, off + 6)[0]
        seg = seg_x2 // 2
        ends = struct.unpack_from(f">{seg}H", blob, off + 14)
        starts = struct.unpack_from(f">{seg}H", blob, off + 16 + seg_x2)
        deltas = struct.unpack_from(f">{seg}h", blob, off + 16 + 2 * seg_x2)
        range_off_base = off + 16 + 3 * seg_x2
        ranges = struct.unpack_from(f">{seg}H", blob, range_off_base)
        for i in range(seg):
            for code in range(starts[i], min(ends[i], 0xFFFE) + 1):
                if ranges[i] == 0:
                    gid = (code + deltas[i]) & 0xFFFF
                else:
                    addr = range_off_base + 2 * i + ranges[i] + 2 * (code - starts[i])
                    gid = struct.unpack_from(">H", blob, addr)[0]
                    if gid:
                        gid = (gid + deltas[i]) & 0xFFFF
                if gid:
                    found.add(code)
    elif fmt == 6:
        first, count = struct.unpack_from(">HH", blob, off + 6)
        for i in range(count):
            gid = struct.unpack_from(">H", blob, off + 10 + 2 * i)[0]
            if gid:
                found.add(first + i)
    elif fmt == 12:
        groups = struct.unpack_from(">I", blob, off + 12)[0]
        for i in range(groups):
            first, last, gid = struct.unpack_from(">III", blob, off + 16 + 12 * i)
            if gid:
                found |= set(range(first, last + 1))
    else:
        raise SystemExit(f"cmap 子表格式 {fmt} 不认识,换一个源字体")
    return found


def font_codepoints(path: str) -> set:
    """直接读 TTF/OTF 的 cmap,拿到源字体真正有字形的码位(不依赖 fontTools)。"""
    try:
        blob = open(path, "rb").read()
    except OSError as exc:
        raise SystemExit(f"读不到源字体 {path}: {exc}")
    if blob[:4] == b"ttcf":     # 字体集合:只用第一个字体
        blob = blob[struct.unpack_from(">I", blob, 12)[0] :]
    if blob[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true", b"typ1"):
        raise SystemExit(f"{path} 不是 TTF/OTF")
    num_tables = struct.unpack_from(">H", blob, 4)[0]
    cmap_off = None
    for i in range(num_tables):
        tag, _checksum, offset, _length = struct.unpack_from(">4sIII", blob, 12 + 16 * i)
        if tag == b"cmap":
            cmap_off = offset
    if cmap_off is None:
        raise SystemExit(f"{path} 没有 cmap 表")
    sub_count = struct.unpack_from(">H", blob, cmap_off + 2)[0]
    codepoints = set()
    for i in range(sub_count):
        platform, encoding, sub_off = struct.unpack_from(
            ">HHI", blob, cmap_off + 4 + 8 * i)
        # 只要 Unicode 子表:平台 0 全部(编码 5 是变体序列表,格式 14 已在下面跳过),
        # 平台 3 的 BMP(1)与全平面(10)。
        if platform == 0 or (platform == 3 and encoding in (1, 10)):
            codepoints |= _read_cmap_subtable(blob, cmap_off + sub_off)
    return codepoints


def check_font_coverage(font_path: str, codepoints: list) -> None:
    """源字体必须覆盖每一个码位:缺字不许静默生成,直接在覆盖数/缺失数上报错停下。"""
    covered = font_codepoints(font_path)
    missing = [cp for cp in codepoints if cp not in covered]
    log(f"  源字体覆盖 {len(codepoints) - len(missing)} / 缺失 {len(missing)}"
        f" (清单 {len(codepoints)} 个码位)")
    if missing:
        samples = " ".join(f"U+{cp:04X}" for cp in missing[:20])
        log(f"  源字体缺 {len(missing)} 个码位: {samples}")
        log("  停下,不生成不完整的字体。换完整字体(Noto Sans SC,SIL OFL 1.1)后重跑:")
        log("    python tools/tsxx_font.py --font <完整字体>")
        raise SystemExit(1)


# ---------------------------------------------------------------- 字形栅格化
def rasterize(font_path: str, size: int, codepoints: list) -> dict:
    """返回 {码位: (adv_w, box_w, box_h, ofs_x, ofs_y, 4bpp 像素列表)}。

    ofs_y 的约定来自 LVGL 的绘制公式(见 lv_draw_label.c):
      letter_y = line_top + (line_height - base_line) - box_h - ofs_y
    画布原点 (pad, pad) 就是行顶,基线落在 y = pad + ascent 处,因此
    ofs_y = ascent - 字形盒底(盒底在基线以上为正,以下为负)。
    """
    font = ImageFont.truetype(font_path, size, layout_engine=ImageFont.Layout.BASIC)
    ascent, _descent = font.getmetrics()
    pad = 4
    span = size * 2 + pad * 2
    glyphs = {}
    for cp in codepoints:
        ch = chr(cp)
        adv_w = max(0, min(0xFFF, int(round(font.getlength(ch) * 16))))
        canvas = Image.new("L", (span, span), 0)
        ImageDraw.Draw(canvas).text((pad, pad), ch, font=font, fill=255)
        bbox = canvas.getbbox()
        if bbox is None:                      # 空白字形(空格、换行)
            glyphs[cp] = (adv_w, 0, 0, 0, 0, [])
            continue
        x0, y0, x1, y1 = bbox
        ink = canvas.crop(bbox)
        bw, bh = ink.size
        if bw > 0xFF or bh > 0xFF:
            raise SystemExit(f"U+{cp:04X} 的字形盒 {bw}x{bh} 超过 uint8,字号太大")
        pixels = ink.load()
        nibbles = []
        for y in range(bh):
            for x in range(bw):
                nibbles.append(min(15, (pixels[x, y] * 15 + 127) // 255))
        ofs_x = x0 - pad
        ofs_y = ascent - (y1 - pad)
        if not -0x80 <= ofs_x <= 0x7F or not -0x80 <= ofs_y <= 0x7F:
            raise SystemExit(f"U+{cp:04X} 的偏移 ({ofs_x}, {ofs_y}) 超过 int8")
        glyphs[cp] = (adv_w, bw, bh, ofs_x, ofs_y, nibbles)
    return glyphs


def pack_nibbles(nibbles: list) -> bytes:
    """4bpp 连续打包:高半字节在前,字形之间按字节对齐(LVGL stride == 0 的读法)。"""
    out = bytearray()
    for i in range(0, len(nibbles), 2):
        hi = nibbles[i]
        lo = nibbles[i + 1] if i + 1 < len(nibbles) else 0
        out.append(((hi & 0xF) << 4) | (lo & 0xF))
    return bytes(out)


def glyph_bitmap_bytes(glyphs: dict) -> int:
    return sum(len(pack_nibbles(nibbles)) for _a, _w, _h, _x, _y, nibbles in glyphs.values())


def footprint(codepoints: list, glyphs: dict) -> dict:
    """编译进固件的 rodata 占用(位图 + unicode_list + glyph_dsc)。

    lv_font_fmt_txt_glyph_dsc_t 在 LV_FONT_FMT_TXT_LARGE == 0 时是 8 字节
    (bitmap_index 20 位 + adv_w 12 位 = 4 字节,再 4 个 8 位字段)。
    """
    ordered = sorted(codepoints)
    chunks = split_cmap_chunks(ordered)
    bitmap = glyph_bitmap_bytes(glyphs)
    unicode_list = 2 * len(ordered) + 2 * len(chunks)
    glyph_dsc = 8 * (len(ordered) + 1)
    return {"glyphs": len(ordered), "chunks": len(chunks), "bitmap": bitmap,
            "unicode_list": unicode_list, "glyph_dsc": glyph_dsc,
            "total": bitmap + unicode_list + glyph_dsc}


# ---------------------------------------------------------------- 生成 C 文件
def split_cmap_chunks(codepoints: list) -> list:
    """按升序切块,每块内 码位 - 块首码位 <= CMAP_SPAN_LIMIT(range_length 装进 uint16)。"""
    chunks = []
    for cp in codepoints:
        if not chunks or cp - chunks[-1][0] > CMAP_SPAN_LIMIT:
            chunks.append([])
        chunks[-1].append(cp)
    return chunks


def emit_font(name: str, size: int, codepoints: list, glyphs: dict, line_height: int,
              base_line: int, source_note: str) -> str:
    ordered = sorted(codepoints)             # LVGL 二分查找要求升序
    chunks = split_cmap_chunks(ordered)
    out = io.StringIO()
    w = out.write
    w("/*******************************************************************************\n")
    w(f" * Size: {size} px\n")
    w(f" * Bpp: {BPP}\n")
    w(" * 由 tools/tsxx_font.py 生成(自研生成器,不依赖 lv_font_conv)。格式要点:\n")
    w(" *   - 4bpp,字形内连续 nibble 打包(行间不补字节),字形之间按字节对齐:\n")
    w(" *     LVGL 9 在 font_dsc.stride == 0 时正是这样跨行连续读 nibble 的。\n")
    w(" *   - cmap 全部 SPARSE_TINY:本字体只覆盖资源包用到的码位,ASCII 不连续;\n")
    w(" *     码位按升序存放(LVGL 二分查找),与输入清单的词频序无关。\n")
    w(" * 源字体与许可: Noto Sans SC Regular 2.004-H2(SIL Open Font License 1.1);\n")
    w(" *   本文件是它的子集,许可随字体一起记录,源字体本身不提交进仓库。\n")
    w(" * 码位来源: assets/fonts/tsxx_symbols.txt(资源包码位表,词频序)。\n")
    w(f" * 码位数: {len(ordered)}\n")
    w(" * 重新生成(可重复执行,写完自己回读比对):\n")
    w(" *   python tools/tsxx_font.py --font <NotoSansSC-Regular.ttf> \\\n")
    w(f" *       --symbols {DEFAULT_SYMBOLS} --out-dir {DEFAULT_OUT_DIR}\n")
    w(" * 自检: python tools/tsxx_font.py --check --font <NotoSansSC-Regular.ttf>\n")
    if source_note:
        w(f" * {source_note}\n")
    w(" *****************************************************************************/\n\n")
    w('#include "lvgl.h"\n\n')
    w(f"#ifndef {name.upper()}\n#define {name.upper()} 1\n\n")
    w("/*-----------------\n *    BITMAPS\n *----------------*/\n\n")
    w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")

    index = 0
    dsc_lines = []
    for gid_cp in ordered:
        adv_w, bw, bh, ofs_x, ofs_y, nibbles = glyphs[gid_cp]
        gid = len(dsc_lines) + 1
        if bw == 0 or bh == 0:
            dsc_lines.append((gid, index, adv_w, 0, 0, ofs_x, ofs_y))
            continue
        data = pack_nibbles(nibbles)
        dsc_lines.append((gid, index, adv_w, bw, bh, ofs_x, ofs_y))
        w(f"    /* U+{gid_cp:04X} */\n")
        for j in range(0, len(data), 12):
            w("    " + ", ".join(f"0x{b:02x}" for b in data[j : j + 12]) + ",\n")
        index += len(data)
    w("};\n\n")
    if index >= BITMAP_INDEX_LIMIT:
        raise SystemExit(f"位图 {index} 字节超过 bitmap_index 的 20 位上限,"
                         "需要打开 LV_FONT_FMT_TXT_LARGE 或换更小的字号")

    w("/*-----------------\n *  GLYPH DESCRIPTION\n *----------------*/\n\n")
    w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
    w("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}"
      " /* id = 0 reserved */,\n")
    for gid, bi, adv_w, bw, bh, ofs_x, ofs_y in dsc_lines:
        w(f"    {{.bitmap_index = {bi}, .adv_w = {adv_w}, .box_w = {bw}, .box_h = {bh},"
          f" .ofs_x = {ofs_x}, .ofs_y = {ofs_y}}},\n")
    w("};\n\n")

    w("/*-----------------\n *  CHARACTER MAPPING\n *----------------*/\n\n")
    for i, chunk in enumerate(chunks):
        w(f"static const uint16_t unicode_list_{i + 1}[] = {{\n    ")
        for j, cp in enumerate(chunk):
            w(f"0x{cp - chunk[0]:x}, ")
            if (j + 1) % 8 == 0 and j + 1 < len(chunk):
                w("\n    ")
        w("\n};\n\n")

    w("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
    gid_start = 1
    for i, chunk in enumerate(chunks):
        w("    {\n")
        w(f"        .range_start = 0x{chunk[0]:x}, .range_length = {chunk[-1] - chunk[0] + 1},\n")
        w(f"        .glyph_id_start = {gid_start},\n")
        w(f"        .unicode_list = unicode_list_{i + 1}, .glyph_id_ofs_list = NULL,\n")
        w(f"        .list_length = {len(chunk)},"
          " .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n")
        w("    },\n" if i + 1 < len(chunks) else "    }\n")
        gid_start += len(chunk)
    w("};\n\n")

    w("/*-----------------\n *  ALL CUSTOM DATA\n *----------------*/\n\n")
    w("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
    w("    .glyph_bitmap = glyph_bitmap,\n")
    w("    .glyph_dsc = glyph_dsc,\n")
    w("    .cmaps = cmaps,\n")
    w("    .kern_dsc = NULL,\n")
    w("    .kern_scale = 0,\n")
    w(f"    .cmap_num = {len(chunks)},\n")
    w(f"    .bpp = {BPP},\n")
    w("    .kern_classes = 0,\n")
    w("    .bitmap_format = 0,   /* LV_FONT_FMT_TXT_PLAIN:未压缩、未做行 XOR */\n")
    w("    .stride = 0,          /* 0 = 行间不补齐,字形内连续 nibble */\n")
    w("};\n\n")

    w("/*-----------------\n *  PUBLIC FONT\n *----------------*/\n\n")
    w(f"const lv_font_t {name} = {{\n")
    w("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
    w("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
    w(f"    .line_height = {line_height},\n")
    w(f"    .base_line = {base_line},\n")
    w("#if LV_VERSION_CHECK(9, 6, 0)\n")
    w(f"    .cap_height = {size},\n")
    w(f"    .x_height = {size * 9 // 16},\n")
    w("#endif\n")
    w("    .subpx = LV_FONT_SUBPX_NONE,\n")
    w("    .underline_position = -1,\n")
    w("    .underline_thickness = 1,\n")
    w("#if LV_VERSION_CHECK(9, 3, 0)\n")
    w("    .static_bitmap = 1,\n")
    w("#endif\n")
    w("    .dsc = &font_dsc,\n")
    w("    .fallback = NULL,\n")
    w("    .user_data = NULL,\n")
    w("};\n\n")
    w(f"#endif /* {name.upper()} */\n")
    return out.getvalue()


# ---------------------------------------------------------------- 回读 C 文件
def parse_font(path: str) -> dict:
    """把写出的 C 文件解析回结构与位图,用于自检(不编译也能核对)。"""
    text = open(path, "r", encoding="utf-8").read()
    bitmap_text = re.search(r"const uint8_t glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not bitmap_text:
        raise SystemExit(f"{path}: 找不到 glyph_bitmap")
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", bitmap_text.group(1)))
    dsc = [
        tuple(int(x) for x in m)
        for m in re.findall(
            r"\{\s*\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),"
            r"\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\s*\}",
            text,
        )
    ]
    cmap = {}
    cmap_text = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};\n",
                          text, re.S)
    if not cmap_text:
        raise SystemExit(f"{path}: 找不到 cmaps[]")
    entries = re.findall(
        r"\{\s*\.range_start\s*=\s*(0x[0-9a-fA-F]+|\d+),\s*\.range_length\s*=\s*(\d+),"
        r"\s*\.glyph_id_start\s*=\s*(\d+),"
        r"\s*\.unicode_list\s*=\s*(\w+),\s*\.glyph_id_ofs_list\s*=\s*(\w+),"
        r"\s*\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*(\w+)",
        cmap_text.group(1), re.S)
    if not entries:
        raise SystemExit(f"{path}: 解析不出任何 cmap 条目")
    ranges = []
    for start_s, length_s, gid_start_s, ulist_name, ofs_name, list_len_s, ctype in entries:
        start = int(start_s, 16) if start_s.lower().startswith("0x") else int(start_s)
        length = int(length_s)
        gid_start = int(gid_start_s)
        list_len = int(list_len_s)
        if not 0 < length <= 0xFFFF:
            raise SystemExit(f"{path}: range_length {length} 装不进 uint16")
        if ofs_name != "NULL":
            raise SystemExit(f"{path}: 生成器不使用 glyph_id_ofs_list,实际 {ofs_name}")
        ranges.append((start, length))
        if ctype.endswith("FORMAT0_TINY"):
            for i in range(length):
                cmap[start + i] = gid_start + i
        elif ctype.endswith("SPARSE_TINY"):
            arr = re.search(rf"static const uint16_t {ulist_name}\[\]\s*=\s*\{{(.*?)\}};",
                            text, re.S)
            if not arr:
                raise SystemExit(f"{path}: 找不到 {ulist_name}")
            ulist = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", arr.group(1))]
            if len(ulist) != list_len:
                raise SystemExit(
                    f"{path}: {ulist_name} 长度 {len(ulist)} != list_length {list_len}")
            if ulist != sorted(ulist):
                raise SystemExit(f"{path}: {ulist_name} 不是升序,LVGL 二分查找会取不到码位")
            for i, off in enumerate(ulist):
                if not 0 <= off < length:
                    raise SystemExit(f"{path}: {ulist_name} 里 0x{off:x} 超出 range_length")
                cmap[start + off] = gid_start + i
        else:
            raise SystemExit(f"{path}: 不支持的 cmap 类型 {ctype}")

    # cmap 区间不能重叠:LVGL 只看第一个命中的区间,重叠会让后面的字形取不到。
    ranges.sort()
    for (a_start, a_len), (b_start, _b_len) in zip(ranges, ranges[1:]):
        if b_start < a_start + a_len:
            raise SystemExit(f"{path}: cmap 区间重叠(0x{a_start:x}+{a_len} 与 0x{b_start:x})")

    line_height = int(re.search(r"\.line_height\s*=\s*(\d+)", text).group(1))
    base_line = int(re.search(r"\.base_line\s*=\s*(\d+)", text).group(1))
    name_match = re.search(r"const lv_font_t (\w+) = \{", text)
    if not name_match:
        raise SystemExit(f"{path}: 找不到字体变量定义")
    return {
        "name": name_match.group(1),
        "bitmap": data,
        "dsc": dsc,
        "cmap": cmap,
        "line_height": line_height,
        "base_line": base_line,
    }


def decode_glyph(font: dict, cp: int):
    """按 LVGL 的 PLAIN 4bpp 读法取一个字形(连续 nibble,字形起点按字节对齐)。"""
    gid = font["cmap"].get(cp)
    if gid is None:
        return None
    bi, adv_w, bw, bh, ofs_x, ofs_y = font["dsc"][gid]
    nibbles = []
    for i in range(bw * bh):
        bitpos = bi * 8 + i * 4
        byte = font["bitmap"][bitpos // 8]
        nibbles.append((byte >> 4) if bitpos % 8 == 0 else (byte & 0xF))
    return (adv_w, bw, bh, ofs_x, ofs_y, nibbles)


def verify(path: str, codepoints: list, glyphs: dict) -> int:
    """逐像素比对:回读写出的 C 文件,与这次栅格化的结果核对。"""
    font = parse_font(path)
    errors = []
    missing = [cp for cp in codepoints if cp not in font["cmap"]]
    if missing:
        errors.append(f"{len(missing)} 个码位不在 cmap 里: "
                      + " ".join(f"U+{cp:04X}" for cp in missing[:8]))
    stale = [cp for cp in font["cmap"] if cp not in glyphs]
    if stale:
        errors.append(f"cmap 里有 {len(stale)} 个本次没生成的码位: "
                      + " ".join(f"U+{cp:04X}" for cp in stale[:8]))
    for cp in codepoints:
        want = glyphs[cp]
        got = decode_glyph(font, cp)
        if got is None:
            continue
        if (want[1], want[2]) != (got[1], got[2]):
            errors.append(f"U+{cp:04X} 盒尺寸不符: {got[1]}x{got[2]} != {want[1]}x{want[2]}")
            continue
        if (want[0], want[3], want[4]) != (got[0], got[3], got[4]):
            errors.append(f"U+{cp:04X} 度量不符: adv/ofs {got[0]},{got[3]},{got[4]} != "
                          f"{want[0]},{want[3]},{want[4]}")
            continue
        if want[5] != got[5]:
            diff = sum(1 for a, b in zip(want[5], got[5]) if a != b)
            errors.append(f"U+{cp:04X} 位图不一致({diff}/{len(want[5])} 像素不同)")
    if errors:
        for e in errors[:20]:
            log(f"    ERROR: {e}")
        log(f"  校验失败: {len(errors)} 项")
        return 1
    log(f"  校验通过: {len(codepoints)} 个字形逐像素与栅格化结果一致")
    return 0


# ---------------------------------------------------------------- 自检模式
def check(out_dir: str, codepoints: list, font_path: str, list_total: int = 0) -> int:
    """纯自检:重新解析生成的 C 文件,再和源字体的栅格化结果逐个码位比对。"""
    ok = True
    tail = f",码位清单 {list_total} 个" if list_total else ""
    for size in SIZES:
        name = f"tsxx_cjk_{size}"
        path = os.path.join(out_dir, name + ".c")
        if not os.path.exists(path):
            log(f"缺少字体文件: {path}(先跑生成模式)")
            return 1
        font = parse_font(path)
        log(f"  {name}: {os.path.getsize(path)} 字节, 字形 {len(font['dsc']) - 1} 个, "
            f"cmap {len(font['cmap'])} 码位, 行高 {font['line_height']}/基线 {font['base_line']}")
        missing = [cp for cp in codepoints if cp not in font["cmap"]]
        log(f"    覆盖 {len(codepoints) - len(missing)} / 缺失 {len(missing)}{tail}")
        if missing:
            log("    缺少码位: " + " ".join(f"U+{cp:04X}" for cp in missing[:10]))
            ok = False
        blank = []
        for cp in codepoints:
            if chr(cp).isspace():            # 空格类码位本来就没有墨迹,不是缺字
                continue
            got = decode_glyph(font, cp)
            if got and not any(got[5]):
                blank.append(cp)
        if blank:
            log(f"    {len(blank)} 个码位是空字形(会是方块): "
                + " ".join(f"U+{cp:04X}" for cp in blank[:10]))
            ok = False
        if not font_path:
            log("    未提供 --font:跳过逐像素比对")
            continue
        log(f"    用 {os.path.basename(font_path)} 重新栅格化 {size}px 并逐像素比对 ...")
        check_font_coverage(font_path, codepoints)
        ok &= verify(path, codepoints, rasterize(font_path, size, codepoints)) == 0
    if not ok:
        log("字体自检失败:重新运行生成命令(见本文件顶部说明)")
        return 1
    log(f"  字体自检通过({len(codepoints)} 个码位全部覆盖)")
    return 0


# ---------------------------------------------------------------- 大小估算
def estimate_size(font_path: str, codepoints: list, size: int) -> None:
    """报某个字号的占用估算 —— 只算,不写文件(分区预算紧,先看数再决定生成)。"""
    glyphs = rasterize(font_path, size, codepoints)
    font = ImageFont.truetype(font_path, size, layout_engine=ImageFont.Layout.BASIC)
    ascent, descent = font.getmetrics()
    line_height = size + EXTRA_LEADING
    baseline = ascent - ((ascent + descent) - line_height) // 2
    text = emit_font(f"tsxx_cjk_{size}", size, codepoints, glyphs, line_height,
                     line_height - baseline, "size estimate only")
    room = footprint(codepoints, glyphs)
    log(f"  {size}px 估算(未写文件):")
    log(f"    字形 {room['glyphs']} 个, 行高 {line_height}/基线 {line_height - baseline}")
    log(f"    位图 {room['bitmap']} 字节 ({room['bitmap'] / 1024:.1f} KB)")
    log(f"    unicode_list {room['unicode_list']} 字节, glyph_dsc {room['glyph_dsc']} 字节")
    log(f"    编译进固件的 rodata 约 {room['total']} 字节 ({room['total'] / 1024:.1f} KB)")
    log(f"    C 源码约 {len(text)} 字节 ({len(text) / 1024:.1f} KB,仓库里的大小)")


# ---------------------------------------------------------------- 主流程
def generate(args, codepoints: list, list_total: int, source_note: str) -> int:
    check_font_coverage(args.font, codepoints)
    os.makedirs(args.out_dir, exist_ok=True)
    rc = 0
    for size in SIZES:
        log(f"  生成 {size}px/{BPP}bpp ...")
        glyphs = rasterize(args.font, size, codepoints)
        font = ImageFont.truetype(args.font, size, layout_engine=ImageFont.Layout.BASIC)
        ascent, descent = font.getmetrics()
        natural = ascent + descent
        line_height = size + EXTRA_LEADING
        # 基线在行框内的位置:把自然行框居中裁到 line_height
        baseline = ascent - (natural - line_height) // 2
        base_line = line_height - baseline
        name = f"tsxx_cjk_{size}"
        text = emit_font(name, size, codepoints, glyphs, line_height, base_line,
                         source_note)
        path = os.path.join(args.out_dir, name + ".c")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        empty = sum(1 for g in glyphs.values() if g[1] == 0 or g[2] == 0)
        room = footprint(codepoints, glyphs)
        log(f"    {path}: {os.path.getsize(path)} 字节,字形 {len(glyphs)} 个"
            f"(其中空白字形 {empty} 个:空格/换行),位图 {room['bitmap']} 字节"
            f",行高 {line_height}/基线 {base_line}")
        log(f"    编译进固件的 rodata 约 {room['total']} 字节 ({room['total'] / 1024:.1f} KB)"
            f" = 位图 {room['bitmap']} + unicode_list {room['unicode_list']}"
            f" + glyph_dsc {room['glyph_dsc']}")
        rc |= verify(path, codepoints, glyphs)
    rc |= check(args.out_dir, codepoints, args.font, list_total=list_total)
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(
        description="生成《天使☆騒々 RE-BOOT!》阅读器的 LVGL 9 中文字形子集")
    ap.add_argument("--font", help="源字体 TTF/OTF(生成/自检/估算都需要)")
    ap.add_argument("--symbols", default=DEFAULT_SYMBOLS, help="码位清单(词频序字符表)")
    ap.add_argument("--out-dir", default=DEFAULT_OUT_DIR, help="生成的 .c 输出目录")
    ap.add_argument("--check", action="store_true", help="只自检,不写文件(需 --font 才能逐像素比对)")
    ap.add_argument("--estimate-size", type=int, action="append", metavar="PX",
                    help="只估算某字号的占用,不写文件(可重复)")
    args = ap.parse_args()

    if not args.font:
        ap.error("需要 --font <NotoSansSC-Regular.ttf>(生成、自检、估算都要)")
    if args.check and args.estimate_size:
        ap.error("--check 与 --estimate-size 不能同时用")

    codepoints = read_symbols(args.symbols)
    total = len(codepoints)
    log(f"  码位清单 {args.symbols}: {total} 个码位")
    codepoints = raster_codepoints(codepoints)
    note = (f"码位表共 {total} 个码位,其中 {total - len(codepoints)} 个控制码位不生成字形"
            if total != len(codepoints) else "")
    if args.check:
        return check(args.out_dir, codepoints, args.font, list_total=total)
    if args.estimate_size:
        for size in args.estimate_size:
            estimate_size(args.font, codepoints, size)
        return 0
    return generate(args, codepoints, total, note)


if __name__ == "__main__":
    raise SystemExit(main())
