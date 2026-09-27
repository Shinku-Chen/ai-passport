#!/usr/bin/env python3
"""生成 AI Passport《limelight lemonade jam》阅读器的 LVGL 中文字体子集。

ATRI 阅读器版的界面用 LVGL 绘制文字,所以字体必须是 LVGL 的位图字体格式。
本工具是 tools/starry_lvgl_font.py 的 limelight 版:字体格式与自检逻辑沿用(不用
lv_font_conv —— 那个工具最后一版 2021,在 Node 24 下写出的字形位图是坏的),只把
字符集来源换成 limelight 剧本包(LLSPK001)。

字符集 = 剧本包里出现过的全部字符(正文 + 说话人/章节标签/选项文案)
       ∪ main/ 下所有 .c/.h 里的非 ASCII 字面量(界面文案)
       ∪ U+3000(全角空格,行首缩进用) ∪ ASCII 0x20..0x7E

用法:
  python tools/limelight_lvgl_font.py --font <NotoSansSC-Regular.otf|ttf> \\
      --pack main/limelight_data/limelight_script.bin --out-dir assets/fonts
  python tools/limelight_lvgl_font.py --check \\
      --pack main/limelight_data/limelight_script.bin --out-dir assets/fonts

生成物 assets/fonts/limelight_cjk_16.c 提交进仓库;源字体不上传。
"""

from __future__ import annotations

import argparse
import io
import os
import re
import struct
import sys
import zlib

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - tool dependency
    sys.exit("需要 Pillow: python -m pip install pillow")

SIZES = (16,)          # 界面统一 16px(正文/名字/菜单),与 atri_ui 的 ATRI_LINE_H=20 配套
BPP = 4
EXTRA_LEADING = 4      # 行高 = 字号 + 4:一屏 5 行 x 20px 才放得进 106px 文本框
ASCII_RANGES = ((0x20, 0x7E),)

SEC_TEXT, SEC_CHUNK, SEC_NAME, SEC_NAMETEXT = 0, 1, 2, 3
CHUNK_FLAG_STORED = 1 << 0


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


# ---------------------------------------------------------------- 字符集
def script_pack_chars(pack_path: str) -> set:
    """收集剧本包里所有会显示到屏幕上的字符。"""
    blob = open(pack_path, "rb").read()
    if blob[:8] != b"LLSPK001":
        raise SystemExit(f"{pack_path}: 不是 LLSPK001 剧本包")
    version, total, sections = struct.unpack_from("<III", blob, 8)
    if total != len(blob):
        raise SystemExit(f"{pack_path}: 头部长度 {total} != 实际 {len(blob)}")
    sec = {}
    for i in range(sections):
        sec_type, off, count, ln = struct.unpack_from("<IIII", blob, 32 + 16 * i)
        sec[sec_type] = (off, count, ln)

    chars: set[str] = set()
    text_off, chunk_count, _ = sec[SEC_TEXT]
    chunk_off, _, _ = sec[SEC_CHUNK]
    for i in range(chunk_count):
        off, size, first_id, count, flags = struct.unpack_from("<II I H H", blob, chunk_off + 16 * i)
        raw = blob[text_off + off: text_off + off + size]
        data = raw if flags & CHUNK_FLAG_STORED else zlib.decompress(raw, -15)
        pos = 0
        for _ in range(count):
            dlg_flags = data[pos]
            pos += 1
            if dlg_flags & 0x01:
                pos += 2
            if dlg_flags & 0x02:
                pos += 2
            if dlg_flags & 0x04:
                pos += 2
            if dlg_flags & 0x08:
                pos += 2
            if dlg_flags & 0x10:
                length = struct.unpack_from("<H", data, pos)[0]
                pos += 2
                chars.update(data[pos:pos + length].decode("utf-8"))
                pos += length
        del first_id
    name_off, name_len, _ = sec[SEC_NAMETEXT]
    chars.update(blob[name_off:name_off + name_len].decode("utf-8"))
    return chars


def source_chars(root: str) -> set:
    """main/ 下所有 C 源码里的非 ASCII 字面量(界面文案)。"""
    chars: set[str] = set()
    for base, _dirs, files in os.walk(root):
        for name in files:
            if not name.endswith((".c", ".h")):
                continue
            try:
                text = open(os.path.join(base, name), encoding="utf-8").read()
            except (OSError, UnicodeDecodeError):
                continue
            chars.update(ch for ch in text if ord(ch) > 0x7F)
    return chars


def runtime_chars(pack_path: str, source_root: str) -> set:
    """运行时真正会显示的字符(剧本文本 + 界面文案);控制字符不算。"""
    chars = script_pack_chars(pack_path) | source_chars(source_root)
    return {ch for ch in chars if ord(ch) >= 0x20}


def font_codepoints(pack_path: str, source_root: str) -> list:
    chars = runtime_chars(pack_path, source_root)
    chars.discard("\r")
    chars.discard("\n")
    chars.discard("\t")
    chars.add("\u3000")                     # 全角空格:行首缩进要用
    chars |= {chr(c) for c in range(0x20, 0x7F)}
    return sorted(ord(ch) for ch in chars)


# ---------------------------------------------------------------- 栅格化
def rasterize(font_path: str, size: int, codepoints: list) -> dict:
    """返回 {码位: (adv_w, box_w, box_h, ofs_x, ofs_y, 4bpp nibble 列表)}。

    ofs_y 的约定来自 LVGL 绘制公式:
      letter_y = line_top + (line_height - base_line) - box_h - ofs_y
    """
    font = ImageFont.truetype(font_path, size, layout_engine=ImageFont.Layout.BASIC)
    ascent, _descent = font.getmetrics()
    pad = 4
    span = size * 2 + pad * 2
    glyphs = {}
    for cp in codepoints:
        ch = chr(cp)
        adv_w = int(round(font.getlength(ch) * 16))
        canvas = Image.new("L", (span, span), 0)
        ImageDraw.Draw(canvas).text((pad, pad), ch, font=font, fill=255)
        bbox = canvas.getbbox()
        if bbox is None:
            glyphs[cp] = (adv_w, 0, 0, 0, 0, [])
            continue
        x0, y0, x1, y1 = bbox
        ink = canvas.crop(bbox)
        bw, bh = ink.size
        px = ink.load()
        nibbles = []
        for y in range(bh):
            for x in range(bw):
                value = px[x, y]
                nibbles.append(min(15, (value * 15 + 127) // 255))
        ofs_x = x0 - pad
        ofs_y = ascent - (y1 - pad)
        glyphs[cp] = (adv_w, bw, bh, ofs_x, ofs_y, nibbles)
    return glyphs


def pack_nibbles(nibbles: list) -> bytes:
    """4bpp 连续打包:高半字节在前,字形之间按字节对齐(LVGL PLAIN 读法)。"""
    out = bytearray()
    for i in range(0, len(nibbles), 2):
        hi = nibbles[i]
        lo = nibbles[i + 1] if i + 1 < len(nibbles) else 0
        out.append(((hi & 0xF) << 4) | (lo & 0xF))
    return bytes(out)


# ---------------------------------------------------------------- 生成 C 文件
def emit_font(name: str, size: int, codepoints: list, glyphs: dict, line_height: int,
              base_line: int) -> str:
    ascii_cps = [cp for cp in codepoints if 0x20 <= cp <= 0x7E]
    rest = [cp for cp in codepoints if not (0x20 <= cp <= 0x7E)]
    # 稀疏表分组:同一张表内首尾码位跨度不超过 0x8000(偏移量存 u16)。
    groups: list[list[int]] = []
    for cp in rest:
        if groups and cp - groups[-1][0] <= 0x8000:
            groups[-1].append(cp)
        else:
            groups.append([cp])
    if ascii_cps != list(range(0x20, 0x7F)):
        raise SystemExit("ASCII 区必须完整连续(0x20..0x7E),否则 FORMAT0_TINY 不适用")
    ordered = ascii_cps + rest
    cmap_num = len(groups) + 1
    out = io.StringIO()
    w = out.write
    w("/*******************************************************************************\n")
    w(f" * Size: {size} px\n")
    w(f" * Bpp: {BPP}\n")
    w(" * 由 tools/limelight_lvgl_font.py 生成(自研生成器,不依赖 lv_font_conv):\n")
    w(" *   - 4bpp,连续 nibble 打包,每个字形按字节对齐\n")
    w(" *   - cmap 两张:ASCII(0x20..0x7E)用 FORMAT0_TINY,其余用 SPARSE_TINY\n")
    w(" * 生成时会回读本文件,与 FreeType 栅格化结果逐像素比对。\n")
    w(" * 字形来源:Noto Sans SC Regular(SIL Open Font License 1.1,可自由再分发),"
      "只取剧本用到的子集。\n")
    w(" *****************************************************************************/\n\n")
    w('#include "lvgl.h"\n\n')
    w(f"#ifndef {name.upper()}\n#define {name.upper()} 1\n\n")
    w("/*-----------------\n *    BITMAPS\n *----------------*/\n\n")
    w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")

    index = 0
    dsc_lines = []
    for i, cp in enumerate(ordered):
        adv_w, bw, bh, ofs_x, ofs_y, nibbles = glyphs[cp]
        gid = i + 1
        if bw == 0 or bh == 0:
            dsc_lines.append((gid, index, adv_w, 0, 0, ofs_x, ofs_y))
            continue
        data = pack_nibbles(nibbles)
        dsc_lines.append((gid, index, adv_w, bw, bh, ofs_x, ofs_y))
        w(f"    /* U+{cp:04X} */\n")
        for j in range(0, len(data), 12):
            chunk = data[j:j + 12]
            w("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        index += len(data)
    w("};\n\n")

    w("/*-----------------\n *  GLYPH DESCRIPTION\n *----------------*/\n\n")
    w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
    w("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}"
      " /* id = 0 reserved */,\n")
    for gid, bi, adv_w, bw, bh, ofs_x, ofs_y in dsc_lines:
        w(f"    {{.bitmap_index = {bi}, .adv_w = {adv_w}, .box_w = {bw}, .box_h = {bh},"
          f" .ofs_x = {ofs_x}, .ofs_y = {ofs_y}}},\n")
    w("};\n\n")

    w("/*-----------------\n *  CHARACTER MAPPING\n *----------------*/\n\n")
    # 稀疏表按“跨度”切成多张:LVGL 的 range_length 是 u16,而本作剧本里既有
    # U+00B7、U+FF01 这类标点,也有 U+2B695(CJK 扩展 B)这种高码位,放进一张表里
    # 跨度会超过 65535。每张表内部的码位偏移量都是 u16。
    for gi, group in enumerate(groups):
        start = group[0]
        w(f"static const uint16_t unicode_list_{gi + 1}[] = {{\n    ")
        for i, cp in enumerate(group):
            w(f"0x{cp - start:x}, ")
            if (i + 1) % 8 == 0 and i + 1 < len(group):
                w("\n    ")
        w("\n};\n\n")
    w("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
    w("    {\n")
    w("        .range_start = 0x20, .range_length = 95,\n")
    w("        .glyph_id_start = 1,\n")
    w("        .unicode_list = NULL, .glyph_id_ofs_list = NULL,\n")
    w("        .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n")
    w("    }")
    gid_start = len(ascii_cps) + 1
    for gi, group in enumerate(groups):
        start = group[0]
        w(",\n    {\n")
        w(f"        .range_start = {start}, .range_length = {group[-1] - start + 1},\n")
        w(f"        .glyph_id_start = {gid_start},\n")
        w(f"        .unicode_list = unicode_list_{gi + 1}, .glyph_id_ofs_list = NULL,\n")
        w(f"        .list_length = {len(group)},"
          " .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n")
        w("    }")
        gid_start += len(group)
    w("\n};\n\n")

    w("/*-----------------\n *  ALL CUSTOM DATA\n *----------------*/\n\n")
    w("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
    w("    .glyph_bitmap = glyph_bitmap,\n")
    w("    .glyph_dsc = glyph_dsc,\n")
    w("    .cmaps = cmaps,\n")
    w("    .kern_dsc = NULL,\n")
    w("    .kern_scale = 0,\n")
    w(f"    .cmap_num = {cmap_num},\n")
    w(f"    .bpp = {BPP},\n")
    w("    .kern_classes = 0,\n")
    w("    .bitmap_format = 0,   /* LV_FONT_FMT_TXT_PLAIN */\n")
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


# ---------------------------------------------------------------- 回读校验
def parse_font(path: str) -> dict:
    text = open(path, "r", encoding="utf-8").read()
    bitmap_text = re.search(r"const uint8_t glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not bitmap_text:
        raise SystemExit(f"{path}: 找不到 glyph_bitmap")
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", bitmap_text.group(1)))
    dsc = [tuple(int(x) for x in m) for m in re.findall(
        r"\{\s*\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),"
        r"\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\s*\}",
        text)]
    cmap = {}
    cmap_text = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};\n",
                          text, re.S)
    if not cmap_text:
        raise SystemExit(f"{path}: 找不到 cmaps[]")
    entries = re.findall(
        r"\{\s*\.range_start\s*=\s*(0x[0-9a-fA-F]+|\d+),\s*\.range_length\s*=\s*(\d+),"
        r"\s*\.glyph_id_start\s*=\s*(\d+),"
        r"\s*\.unicode_list\s*=\s*(\w+),\s*\.glyph_id_ofs_list\s*=\s*(\w+),"
        r"\s*\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*(\w+)", cmap_text.group(1), re.S)
    if not entries:
        raise SystemExit(f"{path}: 解析不出 cmap 条目")
    for start_s, length_s, gid_start_s, ulist, ofs_name, list_len_s, ctype in entries:
        start = int(start_s, 16) if start_s.lower().startswith("0x") else int(start_s)
        length = int(length_s)
        gid_start = int(gid_start_s)
        list_len = int(list_len_s)
        if ofs_name != "NULL":
            raise SystemExit(f"{path}: 生成器不使用 glyph_id_ofs_list,实际 {ofs_name}")
        if ctype.endswith("FORMAT0_TINY"):
            for i in range(length):
                cmap[start + i] = gid_start + i
        elif ctype.endswith("SPARSE_TINY"):
            arr = re.search(rf"static const uint16_t {ulist}\[\]\s*=\s*\{{(.*?)\}};", text, re.S)
            if not arr:
                raise SystemExit(f"{path}: 找不到 {ulist}")
            values = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", arr.group(1))]
            if len(values) != list_len:
                raise SystemExit(f"{path}: {ulist} 长度 {len(values)} != {list_len}")
            for i, off in enumerate(values):
                cmap[start + off] = gid_start + i
        else:
            raise SystemExit(f"{path}: 不支持的 cmap 类型 {ctype}")
    line_height = int(re.search(r"\.line_height\s*=\s*(\d+)", text).group(1))
    base_line = int(re.search(r"\.base_line\s*=\s*(\d+)", text).group(1))
    return {"bitmap": data, "dsc": dsc, "cmap": cmap,
            "line_height": line_height, "base_line": base_line}


def decode_glyph(font: dict, cp: int):
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
    font = parse_font(path)
    errors = []
    missing = [cp for cp in codepoints if cp not in font["cmap"]]
    if missing:
        errors.append(f"{len(missing)} 个码位不在 cmap 里: "
                      + " ".join(f"U+{cp:04X}" for cp in missing[:8]))
    for cp in codepoints:
        want = glyphs[cp]
        got = decode_glyph(font, cp)
        if got is None:
            continue
        if (want[1], want[2]) != (got[1], got[2]):
            errors.append(f"U+{cp:04X} 盒尺寸不符: {got[1]}x{got[2]} != {want[1]}x{want[2]}")
            continue
        if (want[0], want[3], want[4]) != (got[0], got[3], got[4]):
            errors.append(f"U+{cp:04X} 度量不符: {got[0]},{got[3]},{got[4]} != "
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


def check(out_dir: str, codepoints: list, runtime: set) -> int:
    ok = True
    for size in SIZES:
        path = os.path.join(out_dir, f"limelight_cjk_{size}.c")
        if not os.path.exists(path):
            log(f"缺少字体文件: {path}")
            return 1
        font = parse_font(path)
        missing = [cp for cp in codepoints if cp not in font["cmap"]]
        text_missing = [ord(ch) for ch in runtime if ord(ch) not in font["cmap"]]
        blank = [cp for cp in codepoints
                 if cp > 0x20 and not chr(cp).isspace() and decode_glyph(font, cp)
                 and not any(decode_glyph(font, cp)[5])]
        log(f"  {size}px: cmap {len(font['cmap'])} 码位, 缺失 {len(missing)}, "
            f"空字形 {len(blank)}, 行高 {font['line_height']}/基线 {font['base_line']}")
        if missing:
            log("    缺字: " + " ".join(f"U+{cp:04X}" for cp in missing[:10]))
            ok = False
        if text_missing:
            log("    会显示成方块的字符: "
                + " ".join(f"{chr(cp)!r} U+{cp:04X}" for cp in text_missing[:10]))
            ok = False
        if blank:
            log("    空字形: " + " ".join(f"U+{cp:04X}" for cp in blank[:10]))
            ok = False
    if not ok:
        log("字体自检失败:重新运行生成命令")
        return 1
    log(f"  字体覆盖/结构自检通过(运行时字符 {len(runtime)} 个全部覆盖)")
    return 0


# ---------------------------------------------------------------- 主流程
def generate(args) -> int:
    codepoints = font_codepoints(args.pack, args.source_root)
    os.makedirs(args.out_dir, exist_ok=True)
    symbols = "".join(chr(cp) for cp in codepoints)
    with open(os.path.join(args.out_dir, "limelight_cjk_symbols.txt"), "w",
              encoding="utf-8", newline="\n") as fh:
        fh.write("# 由 tools/limelight_lvgl_font.py 生成:剧本 + 界面文案出现过的全部字符。\n")
        fh.write("# 重新生成字体时必须与本文件一致。\n")
        fh.write(symbols + "\n")
    log(f"  字符清单 {len(codepoints)} 个")

    rc = 0
    for size in SIZES:
        log(f"  生成 {size}px/{BPP}bpp ...")
        glyphs = rasterize(args.font, size, codepoints)
        font = ImageFont.truetype(args.font, size, layout_engine=ImageFont.Layout.BASIC)
        ascent, descent = font.getmetrics()
        natural = ascent + descent
        line_height = size + EXTRA_LEADING
        baseline = ascent - (natural - line_height) // 2
        base_line = line_height - baseline
        name = f"limelight_cjk_{size}"
        text = emit_font(name, size, codepoints, glyphs, line_height, base_line)
        path = os.path.join(args.out_dir, name + ".c")
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        bitmap_bytes = sum(len(pack_nibbles(glyphs[cp][5])) for cp in codepoints)
        log(f"    {path} ({os.path.getsize(path) / 1024:.0f} KB 源码, "
            f"位图 {bitmap_bytes / 1024:.0f} KB, 行高 {line_height} 基线 {base_line})")
        rc |= verify(path, codepoints, glyphs)
    rc |= check(args.out_dir, codepoints, runtime_chars(args.pack, args.source_root))
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--font", help="源字体 TTF/OTF(生成时必填)")
    ap.add_argument("--pack", default="main/limelight_data/limelight_script.bin")
    ap.add_argument("--source-root", default="main")
    ap.add_argument("--out-dir", default="assets/fonts")
    ap.add_argument("--check", action="store_true", help="只做覆盖/结构自检,不生成")
    args = ap.parse_args()

    codepoints = font_codepoints(args.pack, args.source_root)
    if args.check:
        return check(args.out_dir, codepoints, runtime_chars(args.pack, args.source_root))
    if not args.font:
        ap.error("生成模式需要 --font")
    return generate(args)


if __name__ == "__main__":
    raise SystemExit(main())
