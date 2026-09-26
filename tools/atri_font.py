#!/usr/bin/env python3
"""生成 AI Passport《ATRI -My Dear Moments-》阅读器的中文字体子集(LVGL 9 位图字体)。

为什么不用 lv_font_conv:这个工具最后一版是 1.5.3(2021),在 Node v24 下写出的
字形位图是坏的 —— 同一个 OTF、同一套参数,`--no-compress` 与
`--no-compress --no-prefilter` 产出的字节完全相同,按 LVGL 的 PLAIN 4bpp 读法解码
全是噪点(LVGL 自带的 Montserrat 字体用同一解码器完全正常)。表现到设备上就是
"文字全部乱码"。这里改成直接用 Pillow(FreeType)栅格化 + 自己写 LVGL 字体格式,
不依赖 Node,并且生成后会把写出的 C 文件重新解析回来与栅格化结果逐像素比对。

字符集来自两处,保证界面文字与剧本正文都不缺字:
  1. 资源包(main/atri_data/atri_pack.bin)里的全部剧本文字与名字;
  2. main/ 下所有 .c/.h 里出现的非 ASCII 字面量(界面文案)。

用法:
  python tools/atri_font.py --font <NotoSansSC-Regular.ttf>       --pack main/atri_data/atri_pack.bin --out-dir assets/fonts     # 生成 + 校验
  python tools/atri_font.py --check --pack main/atri_data/atri_pack.bin       --out-dir assets/fonts                                         # 只做覆盖/结构自检

生成物(assets/fonts/atri_cjk_16.c 与 atri_cjk_symbols.txt)提交进仓库;源字体不上传。
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

SIZES = (16,)         # 生成的像素字号(正文/名字/菜单统一用 16px)
BPP = 4
# 行高固定为 字号+4:界面按"16px 字、20px 行距"排版(见 main/atri_ui.c 的 ATRI_LINE_H),
# 若用 Noto Sans SC 的自然行高(16px 字号约 22px),一屏就放不下 5 行。
# 多出来的行距在自然行框里上下各裁一半,基线随之平移,字形本身不受影响。
EXTRA_LEADING = 4
ASCII_RANGES = ((0x20, 0x7E),)


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


# ---------------------------------------------------------------- 字符收集
def pack_chars(pack_path: str) -> set:
    blob = open(pack_path, "rb").read()
    if blob[:8] != b"ATRIPK01":
        raise SystemExit(f"资源包魔数不对: {pack_path}")
    version, total, section_count = struct.unpack_from("<III", blob, 8)
    if version != 1 or total != len(blob):
        raise SystemExit("资源包版本/长度不匹配,先重新生成")
    sections = {}
    for i in range(section_count):
        sec_type, off, count, size = struct.unpack_from("<IIII", blob, 20 + 16 * i)
        sections[sec_type] = (off, count, size)
    text_off, _, text_size = sections[0]
    text = blob[text_off : text_off + text_size].decode("utf-8")
    return {c for c in text if c not in "\r\n\t"}


def source_chars(root: str) -> set:
    chars = set()
    for dirpath, _dirnames, filenames in os.walk(root):
        if os.path.basename(dirpath) in {"managed_components", "build"}:
            continue
        for name in filenames:
            if not name.endswith((".c", ".h", ".hpp", ".cpp")):
                continue
            try:
                data = open(os.path.join(dirpath, name), "rb").read().decode("utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            # 只取字符串字面量里的非 ASCII 字符,避免把注释里的说明也算进去。
            for literal in re.findall(r'"(?:[^"\\]|\\.)*"', data):
                chars |= {c for c in literal if ord(c) > 0x7F}
    return chars


def raw_text_chars(pack_path: str, source_root: str) -> set:
    """运行时会显示的全部字符,不做任何过滤(用于“会不会出方块”的检查)。

    与 required_chars 的区别:这里不过滤 U+3000 之类“生成时会被丢掉”的字符,
    所以在字体里缺了任何一个都会被抓出来 —— 缺字在屏幕上就是方块。
    """
    chars = pack_chars(pack_path) | source_chars(source_root)
    return {c for c in chars if ord(c) >= 0x20}


def required_chars(pack_path: str, source_root: str) -> set:
    chars = pack_chars(pack_path) | source_chars(source_root)
    chars.discard("\u3000")
    for start, end in ASCII_RANGES:
        chars |= {chr(c) for c in range(start, end + 1)}
    return {c for c in chars if c not in "\r\n\t"}


# ---------------------------------------------------------------- 字形栅格化
def rasterize(otf_path: str, size: int, codepoints: list) -> dict:
    """返回 {码位: (adv_w, box_w, box_h, ofs_x, ofs_y, 4bpp 像素列表)}。

    ofs_y 的约定来自 LVGL 的绘制公式:
      letter_y = line_top + (line_height - base_line) - box_h - ofs_y
    即 ofs_y = baseline - 字形盒底(基线以下的部分为负)。
    """
    font = ImageFont.truetype(otf_path, size, layout_engine=ImageFont.Layout.BASIC)
    ascent, descent = font.getmetrics()
    pad = 4                       # 画布留白:原点固定在 (pad, pad) = 行顶(上文线)
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
                v = px[x, y]
                nibbles.append(min(15, (v * 15 + 127) // 255))
        ofs_x = x0 - pad
        ofs_y = ascent - (y1 - pad)
        glyphs[cp] = (adv_w, bw, bh, ofs_x, ofs_y, nibbles)
    return glyphs


def pack_nibbles(nibbles: list) -> bytes:
    """4bpp 连续打包:高半字节在前,字形之间按字节对齐(LVGL 的 PLAIN 读法)。"""
    out = bytearray()
    for i in range(0, len(nibbles), 2):
        hi = nibbles[i]
        lo = nibbles[i + 1] if i + 1 < len(nibbles) else 0
        out.append(((hi & 0xF) << 4) | (lo & 0xF))
    return bytes(out)


# ---------------------------------------------------------------- 生成 C 文件
def emit_font(name: str, size: int, codepoints: list, glyphs: dict, line_height: int,
              base_line: int) -> str:
    # 字符映射分两张表,与 LVGL 9.6 的 fmt_txt 语义对应(见 lv_font_fmt_txt.h 的格式说明):
    #   cmap 0: 连续的 ASCII 区(0x20..0x7E)用 FORMAT0_TINY(不需要额外表)
    #   cmap 1: 剩下所有码位用 SPARSE_TINY(排好序的相对偏移表 + 二分查找)
    # 注意 FORMAT0_FULL 在 9.6 里要的是“每个码位一个 uint8 偏移”,超过 256 个字形就表达
    # 不了,而且漏填 glyph_id_ofs_list 会直接空指针崩溃 —— 别用。
    ascii_cps = [cp for cp in codepoints if 0x20 <= cp <= 0x7E]
    rest = [cp for cp in codepoints if not (0x20 <= cp <= 0x7E)]
    if ascii_cps != list(range(0x20, 0x7F)):
        raise SystemExit("ASCII 区必须完整连续(0x20..0x7E),否则 FORMAT0_TINY 不适用")
    ordered = ascii_cps + rest
    cmap_num = 2 if rest else 1
    out = io.StringIO()
    w = out.write
    w("/*******************************************************************************\n")
    w(f" * Size: {size} px\n")
    w(f" * Bpp: {BPP}\n")
    w(" * 由 tools/atri_font.py 生成(自研生成器,不依赖 lv_font_conv);格式:\n")
    w(" *   - 4bpp,连续 nibble 打包,每个字形按字节对齐\n")
    w(" *   - cmap 分两张:ASCII(0x20..0x7E)用 FORMAT0_TINY,其余用 SPARSE_TINY\n")
    w(" * 生成时会回读本文件并与 FreeType 栅格化结果逐像素比对。\n")
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
            chunk = data[j : j + 12]
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
    if rest:
        sparse_start = rest[0]
        sparse_len = rest[-1] - sparse_start + 1
        if sparse_len > 0xFFFF:
            raise SystemExit("码位跨度超过 u16,需要拆成多张 cmap")
        w("static const uint16_t unicode_list_1[] = {\n    ")
        for i, cp in enumerate(rest):
            w(f"0x{cp - sparse_start:x}, ")
            if (i + 1) % 8 == 0 and i + 1 < len(rest):
                w("\n    ")
        w("\n};\n\n")
    w("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
    w("    {\n")
    w("        .range_start = 0x20, .range_length = 95,\n")
    w("        .glyph_id_start = 1,\n")
    w("        .unicode_list = NULL, .glyph_id_ofs_list = NULL,\n")
    w("        .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n")
    w("    }")
    if rest:
        w(",\n    {\n")
        w(f"        .range_start = {sparse_start}, .range_length = {sparse_len},\n")
        w(f"        .glyph_id_start = {len(ascii_cps) + 1},\n")
        w("        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL,\n")
        w(f"        .list_length = {len(rest)},"
          " .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n")
        w("    }")
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
    w("    .bitmap_format = 0,   /* LV_FONT_FMT_TXT_PLAIN:位图未压缩、未做行 XOR */\n")
    w("};\n\n")

    w("/*-----------------\n *  PUBLIC FONT\n *----------------*/\n\n")
    w("const lv_font_t " + name + " = {\n")
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
    """把写出的 C 文件解析回结构与位图,用于逐像素校验。"""
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
    for start_s, length_s, gid_start_s, ulist_name, ofs_name, list_len_s, ctype in entries:
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
            arr = re.search(rf"static const uint16_t {ulist_name}\[\]\s*=\s*\{{(.*?)\}};",
                            text, re.S)
            if not arr:
                raise SystemExit(f"{path}: 找不到 {ulist_name}")
            ulist = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", arr.group(1))]
            if len(ulist) != list_len:
                raise SystemExit(
                    f"{path}: {ulist_name} 长度 {len(ulist)} != list_length {list_len}")
            for i, off in enumerate(ulist):
                cmap[start + off] = gid_start + i
        else:
            raise SystemExit(f"{path}: 不支持的 cmap 类型 {ctype}")

    line_height = int(re.search(r"\.line_height\s*=\s*(\d+)", text).group(1))
    base_line = int(re.search(r"\.base_line\s*=\s*(\d+)", text).group(1))
    return {
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


# ---------------------------------------------------------------- 自检
def check(out_dir: str, codepoints: list, runtime_chars: set) -> int:
    ok = True
    for size in SIZES:
        path = os.path.join(out_dir, f"atri_cjk_{size}.c")
        if not os.path.exists(path):
            log(f"缺少字体文件: {path}")
            ok = False
            continue
        font = parse_font(path)
        missing = [cp for cp in codepoints if cp not in font["cmap"]]
        # 严格检查:剧本与界面文案里真正会显示的每个字都必须在字体里(缺字 = 方块)。
        text_missing = [ord(c) for c in runtime_chars if ord(c) not in font["cmap"]]
        blank = [cp for cp in codepoints
                 if cp > 0x20 and decode_glyph(font, cp) and
                 not any(decode_glyph(font, cp)[5])]
        log(f"  {size}px: cmap {len(font['cmap'])} 码位, 缺失 {len(missing)}, 空字形 {len(blank)}, "
            f"行高 {font['line_height']}/基线 {font['base_line']}")
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
        log("字体自检失败:重新运行生成命令(见本文件顶部说明)")
        return 1
    log(f"  字体覆盖/结构自检通过(运行时字符 {len(runtime_chars)} 个全部覆盖)")
    return 0


# ---------------------------------------------------------------- 主流程
def generate(args) -> int:
    chars = required_chars(args.pack, args.source_root)
    codepoints = sorted(ord(c) for c in chars)
    os.makedirs(args.out_dir, exist_ok=True)
    symbols = "".join(chr(cp) for cp in codepoints)
    with open(os.path.join(args.out_dir, "atri_cjk_symbols.txt"), "w",
              encoding="utf-8", newline="\n") as fh:
        fh.write("# 由 tools/atri_font.py 生成:界面文案 + 剧本正文出现过的全部字符。\n")
        fh.write("# 重新生成字体时必须与本文件一致。\n")
        fh.write(symbols + "\n")
    log(f"  字符清单 {len(codepoints)} 个")

    # 缺字诊断(可选,依赖 fontTools)
    try:
        from fontTools.ttLib import TTFont  # type: ignore

        covered = set()
        tt = TTFont(args.font)
        for table in tt["cmap"].tables:
            if table.isUnicode():
                covered |= set(table.cmap.keys())
        missing = [cp for cp in codepoints if cp not in covered]
        if missing:
            log(f"  警告: 源字体缺 {len(missing)} 个码位: "
                + " ".join(f"U+{cp:04X}" for cp in missing[:10]))
        else:
            log("  源字体覆盖全部所需码位")
    except ImportError:
        log("  (未安装 fontTools,跳过源字体缺字诊断)")

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
        name = f"atri_cjk_{size}"
        text = emit_font(name, size, codepoints, glyphs, line_height, base_line)
        path = os.path.join(args.out_dir, name + ".c")
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        log(f"    {path} ({os.path.getsize(path) / 1024:.0f} KB 源码)")
        rc |= verify(path, codepoints, glyphs)
    rc |= check(args.out_dir, codepoints,
                raw_text_chars(args.pack, args.source_root))
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--font", help="源字体 TTF/OTF(生成时必填)")
    ap.add_argument("--pack", default="main/atri_data/atri_pack.bin")
    ap.add_argument("--source-root", default="main")
    ap.add_argument("--out-dir", default="assets/fonts")
    ap.add_argument("--check", action="store_true", help="只做覆盖/结构自检,不生成")
    args = ap.parse_args()

    codepoints = sorted(ord(c) for c in required_chars(args.pack, args.source_root))
    if args.check:
        return check(args.out_dir, codepoints,
                     raw_text_chars(args.pack, args.source_root))
    if not args.font:
        ap.error("生成模式需要 --font")
    return generate(args)


if __name__ == "__main__":
    raise SystemExit(main())
