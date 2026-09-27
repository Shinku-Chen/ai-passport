#!/usr/bin/env python3
"""Generate the LVGL CJK bitmap font subset for the Senren * Banka port.

Why a hand-written generator instead of lv_font_conv: that tool stopped at 1.5.3
(2021) and under Node v24 writes broken glyph bitmaps -- the same OTF with the
same parameters decodes into noise when LVGL reads it back as PLAIN 4bpp, and
the device shows garbled text.  This tool rasterises with Pillow (FreeType) and
writes the LVGL 4bpp bitmap-font format itself, then parses the generated C file
back and compares every glyph pixel by pixel against a fresh rasterisation.

Character set (the union becomes the font; the generated .c is committed while
the source font is never committed):

  * ``--scn`` script pack: every code point of ``SEC_CHAR`` (the exact character
    set the game text needs, ordered there by global frequency) plus the literal
    text of the pack itself (``SEC_NAME`` name tables and the UTF-8 ``SEC_META``
    block).
  * ``--sources``: every non-ASCII character of the C string literals (the UI
    copy lives in the sources) plus the ASCII letters and digits found there.
    UI copy is not final yet, so a regeneration stays possible at any time.
  * ``--extra-text``: newline separated extra strings, for copy that lives
    outside the C sources.
  * the complete printable ASCII range 0x20..0x7E is always added.  The UI copy
    uses ASCII punctuation ":/;+%=", and a contiguous range lets the ASCII part
    use LVGL's compact FORMAT0_TINY cmap (same layout as the reference port).

Control code points (0x0A, 0x0E appear in the script character table) get an
empty glyph so that the coverage guard and the requested set stay identical:
LVGL handles '\\n' itself and never looks those up.

Line height: the UI hard-codes ATRI_LINE_H == 20 px (main/atri_ui.h) for a 16 px
face, so EXTRA_LEADING == 4 and the tool asserts the result is 20.  The baseline
is the natural FreeType baseline, centred by the natural line box.

Usage:
  python3 tools/senren_lvgl_font.py                                # generate + verify
  python3 tools/senren_lvgl_font.py --ttf <CJK font> --out <file.c>
  python3 tools/senren_lvgl_font.py --check assets/fonts/senren_cjk_16.c

Outputs (both committed):
  assets/fonts/senren_cjk_16.c          LVGL font (glyph_bitmap + glyph_dsc + cmaps)
  assets/fonts/senren_cjk_symbols.txt   covered code points, one "U+XXXX" per line,
                                        sorted ascending, unique -- used by the
                                        UI-copy coverage guard.
"""

from __future__ import annotations

import argparse
import glob
import io
import os
import re
import struct
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - tool dependency
    sys.exit("Pillow is required: python -m pip install pillow")

# 脚本包(SENRSCN1)格式常量,见 tools/senren_scn_pack.py
SCN_MAGIC = b"SENRSCN1"
SCN_HEADER = struct.Struct("<8sHHHHI")     # magic, version, header_size, sections, rsvd, total
SCN_SECTION = struct.Struct("<IIII")       # type, offset, count, size
SEC_CHAR, SEC_NAME, SEC_META = 0, 1, 5

BPP = 4                       # LVGL 的 PLAIN 4bpp:连续 nibble 打包
EXTRA_LEADING = 4             # 行高 = 字号 + 4(见 main/atri_ui.h 的 ATRI_LINE_H)
EXPECTED_LINE_HEIGHT = 20     # ATRI_LINE_H;16px 字面 + 4px 行距,断言在此
ASCII_FIRST, ASCII_LAST = 0x20, 0x7E
SYMBOLS_NAME = "senren_cjk_symbols.txt"
# 探测 .notdef 的码位(Unicode 里永远不分配):栅格化结果与它完全相同 = 源字体没有这个字形
NOTDEF_PROBE = 0x10FFFE

# 行内字符的自动探测顺序:优先思源黑体/思源黑体同族(Noto Sans CJK),与参考移植
# 用的字体一致,且覆盖剧本里的 ♪(U+266A)—— Windows 的微软雅黑缺这个字。
# 不使用可变字体(NotoSansSC-VF.ttf):它的默认实例在 16px 下笔画太细,4bpp 位图会糊。
TTF_CANDIDATES = (
    "assets/fonts/SourceHanSansSC-Normal.otf",
    "assets/fonts/NotoSansSC-Regular.otf",
    # LVGL 组件自带的思源黑体,第一次 idf.py build 之后才存在
    "managed_components/lvgl__lvgl/scripts/generators/built_in_font/SourceHanSansSC-Normal.otf",
    "managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/simsun.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/source-han-sans/SourceHanSansSC-Regular.otf",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
)


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def is_control(cp: int) -> bool:
    """控制码:不栅格化(LVGL 自己处理换行),只留一个空字形占位。"""
    return cp < 0x20 or 0x7F <= cp <= 0x9F


# ---------------------------------------------------------------- 字符收集
def pack_chars(path: str) -> tuple[list[int], set[int]]:
    """读脚本包:返回 (SEC_CHAR 码位, 包里字面文本的码位)。

    字面文本只取 SEC_NAME(名字表的 u16 码点)与 SEC_META(UTF-8 的 key=value),
    不去扫整个二进制 —— 段表/压缩块里的随机字节会被误认成 UTF-8,产生几千个垃圾码位。
    """
    blob = open(path, "rb").read()
    if len(blob) < SCN_HEADER.size:
        raise SystemExit(f"{path}: 文件太短,不是 SENRSCN1 脚本包")
    magic, version, header_size, section_count, _reserved, total = SCN_HEADER.unpack_from(blob)
    if magic != SCN_MAGIC:
        raise SystemExit(f"{path}: 魔数不符 {magic!r},不是 SENRSCN1 脚本包")
    if total != len(blob):
        raise SystemExit(f"{path}: 头里写 {total} 字节,实际 {len(blob)}")
    sections: dict[int, tuple[int, int]] = {}
    for index in range(section_count):
        at = SCN_HEADER.size + SCN_SECTION.size * index
        if at + SCN_SECTION.size > len(blob) or at + SCN_SECTION.size > header_size:
            raise SystemExit(f"{path}: 段表越界(section {index})")
        sec_type, offset, _count, size = SCN_SECTION.unpack_from(blob, at)
        if offset + size > len(blob):
            raise SystemExit(f"{path}: 段 {sec_type} 越界 {offset}+{size}")
        sections[sec_type] = (offset, size)
    if SEC_CHAR not in sections:
        raise SystemExit(f"{path}: 缺少 SEC_CHAR 字符表")

    offset, size = sections[SEC_CHAR]
    if size < 4:
        raise SystemExit(f"{path}: SEC_CHAR 大小异常 {size}")
    count = struct.unpack_from("<I", blob, offset)[0]
    if 4 + 2 * count > size:
        raise SystemExit(f"{path}: SEC_CHAR 声明 {count} 项,段里放不下")
    codepoints = list(struct.unpack_from("<%dH" % count, blob, offset + 4))

    literals: set[int] = set()
    if SEC_NAME in sections:
        # 名字表:每个 block 是 u16 条目数,每条 u16 长度 + 长度个 u16 码表下标;
        # 下标要经过 SEC_CHAR 映射才是真正的码位。读到段尾为止。
        name_off, name_size = sections[SEC_NAME]
        end = name_off + name_size
        cursor = name_off
        while cursor + 2 <= end:
            entries = struct.unpack_from("<H", blob, cursor)[0]
            cursor += 2
            for _ in range(entries):
                if cursor + 2 > end:
                    cursor = end
                    break
                length = struct.unpack_from("<H", blob, cursor)[0]
                cursor += 2
                if cursor + 2 * length > end:
                    cursor = end
                    break
                for code in struct.unpack_from("<%dH" % length, blob, cursor):
                    if code < len(codepoints):
                        literals.add(codepoints[code])
                cursor += 2 * length
    if SEC_META in sections:
        meta_off, meta_size = sections[SEC_META]
        text = blob[meta_off : meta_off + meta_size].decode("utf-8", "replace")
        literals |= {ord(c) for c in text if ord(c) >= ASCII_FIRST and ord(c) != 0xFFFD}
    return codepoints, literals


def source_chars(patterns: str) -> tuple[set[int], set[int]]:
    """从 C 源文件里取字符:(字面量里的非 ASCII, 全文件里的 ASCII 字母数字)。

    只认字符串字面量里的非 ASCII 字符,注释里的说明文字不算 UI 文案。
    """
    quoted = re.compile('"(?:[^"\\\n]|\\.)*"')
    non_ascii: set[int] = set()
    ascii_alnum: set[int] = set()
    files: list[str] = []
    for pattern in (p.strip() for p in patterns.split(",")):
        if not pattern:
            continue
        files.extend(sorted(glob.glob(pattern)))
    for path in dict.fromkeys(files):          # 去掉重复(如 "main/*.c,main/*.c")
        try:
            text = open(path, "r", encoding="utf-8").read()
        except (OSError, UnicodeDecodeError) as exc:
            log(f"  跳过 {path}: {exc}")
            continue
        for literal in quoted.findall(text):
            non_ascii |= {ord(c) for c in literal if ord(c) > 0x7F}
        ascii_alnum |= {ord(c) for c in text if c.isascii() and c.isalnum()}
    return non_ascii, ascii_alnum


def extra_text_chars(paths: list[str] | None) -> set[int]:
    """--extra-text:逐行额外文案(UI 文案还没定稿时用)。"""
    chars: set[int] = set()
    for path in paths or []:
        try:
            text = open(path, "r", encoding="utf-8").read()
        except OSError as exc:
            raise SystemExit(f"--extra-text {path}: {exc}")
        chars |= {ord(c) for c in text if ord(c) >= ASCII_FIRST}
    return chars


def requested_codepoints(args: argparse.Namespace) -> list[int]:
    """字体要覆盖的全部码位(升序);这就是 symbols.txt 的内容。"""
    if not os.path.exists(args.scn):
        raise SystemExit(
            f"缺少脚本包 {args.scn};先用 tools/senren_scn_pack.py 生成,"
            "或显式指定 --scn <pack>")
    scn_points, scn_literals = pack_chars(args.scn)
    src_non_ascii, src_alnum = source_chars(args.sources)
    wanted = set(scn_points) | scn_literals | src_non_ascii | src_alnum
    wanted |= extra_text_chars(args.extra_text)
    wanted |= set(range(ASCII_FIRST, ASCII_LAST + 1))
    log(f"  码位: 剧本 {len(scn_points)} + 包头字面 {len(scn_literals)} + "
        f"源码 {len(src_non_ascii) + len(src_alnum)} + 额外文案 "
        f"{len(extra_text_chars(args.extra_text))} -> 合计 {len(wanted)}")
    return sorted(wanted)


# ---------------------------------------------------------------- 字号 / 字体
def line_height_for(size: int) -> int:
    """UI 按 16px 字、20px 行高排版,所以行高必须正好是 ATRI_LINE_H。"""
    line_height = size + EXTRA_LEADING
    if line_height != EXPECTED_LINE_HEIGHT:
        raise SystemExit(
            f"--size {size} 得到行高 {line_height},但界面写死 ATRI_LINE_H == "
            f"{EXPECTED_LINE_HEIGHT}(main/atri_ui.h);请用 "
            f"--size {EXPECTED_LINE_HEIGHT - EXTRA_LEADING}")
    assert line_height == EXPECTED_LINE_HEIGHT, line_height
    return line_height


def find_ttf(explicit: str | None) -> str:
    """返回源字体路径;没给就按候选表探测,失败时把找过的路径全列出来。"""
    if explicit:
        if not os.path.exists(explicit):
            raise SystemExit(f"--ttf {explicit}: 文件不存在")
        return explicit
    env = os.environ.get("SENREN_FONT")
    candidates = ([env] if env else []) + list(TTF_CANDIDATES)
    for path in candidates:
        if os.path.exists(path):
            log(f"  源字体: {path}(候选表第一个存在的;可用 --ttf 覆盖)")
            return path
    listing = "\n".join(f"    {path}" for path in candidates)
    raise SystemExit("找不到可用的 CJK 字体,请用 --ttf <path> 指定;已查找:\n"
                     f"{listing}\n"
                     "  也可设环境变量 SENREN_FONT。首次 idf.py build 后 LVGL 组件目录里"
                     "会带思源黑体(managed_components/...)。")


# ---------------------------------------------------------------- 字形栅格化
def rasterize(ttf_path: str, size: int, codepoints: list[int]) -> tuple[dict, set[int]]:
    """返回 ({码位: (adv_w, box_w, box_h, ofs_x, ofs_y, nibbles)}, 缺字形码位)。

    ofs_y 的约定来自 LVGL 的绘制公式:
      letter_y = line_top + (line_height - base_line) - box_h - ofs_y
    即 ofs_y = baseline - 字形盒底(基线以下的部分为负)。adv_w 是 4.4 定点宽度
    (LVGL 的 advance 单位),所以乘 16。
    """
    font = ImageFont.truetype(ttf_path, size, layout_engine=ImageFont.Layout.BASIC)
    ascent, _descent = font.getmetrics()
    pad = 4                       # 画布留白:原点固定在 (pad, pad) = 行顶(上文线)
    span = size * 2 + pad * 2
    glyphs: dict[int, tuple] = {}
    missing: set[int] = set()
    # .notdef 参考位图:缺字形的字在屏幕上就是方块,生成时要点名
    notdef = Image.new("L", (span, span), 0)
    ImageDraw.Draw(notdef).text((pad, pad), chr(NOTDEF_PROBE), font=font, fill=255)
    notdef_bytes = notdef.tobytes()
    for cp in codepoints:
        if is_control(cp):
            glyphs[cp] = (0, 0, 0, 0, 0, [])
            continue
        ch = chr(cp)
        adv_w = int(round(font.getlength(ch) * 16))
        canvas = Image.new("L", (span, span), 0)
        ImageDraw.Draw(canvas).text((pad, pad), ch, font=font, fill=255)
        if canvas.tobytes() == notdef_bytes:
            missing.add(cp)
        bbox = canvas.getbbox()
        if bbox is None:
            # 空白字符(空格/全角空格/U+FEFF):有字形但没墨迹
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
        glyphs[cp] = (adv_w, bw, bh, x0 - pad, ascent - (y1 - pad), nibbles)
    return glyphs, missing


def pack_nibbles(nibbles: list[int]) -> bytes:
    """4bpp 连续打包:高半字节在前,字形之间按字节对齐(LVGL 的 PLAIN 读法)。"""
    out = bytearray()
    for i in range(0, len(nibbles), 2):
        hi = nibbles[i]
        lo = nibbles[i + 1] if i + 1 < len(nibbles) else 0
        out.append(((hi & 0xF) << 4) | (lo & 0xF))
    return bytes(out)


# ---------------------------------------------------------------- 生成 C 文件
def emit_font(name: str, size: int, codepoints: list[int], glyphs: dict, line_height: int,
              base_line: int, ttf_name: str) -> str:
    # 字符映射分两张表,与 LVGL 的 fmt_txt 语义对应(见 lv_font_fmt_txt.h):
    #   cmap 0: 连续的 ASCII 区(0x20..0x7E)用 FORMAT0_TINY(不需要额外表)
    #   cmap 1: 剩下所有码位用 SPARSE_TINY(排好序的相对偏移表 + 二分查找)
    ascii_cps = [cp for cp in codepoints if ASCII_FIRST <= cp <= ASCII_LAST]
    rest = [cp for cp in codepoints if not (ASCII_FIRST <= cp <= ASCII_LAST)]
    if ascii_cps != list(range(ASCII_FIRST, ASCII_LAST + 1)):
        raise SystemExit("ASCII 区必须完整连续(0x20..0x7E),否则 FORMAT0_TINY 不适用")
    ordered = ascii_cps + rest
    cmap_num = 2 if rest else 1
    out = io.StringIO()
    w = out.write
    w("/*******************************************************************************\n")
    w(f" * Size: {size} px\n")
    w(f" * Bpp: {BPP}\n")
    w(" * 由 tools/senren_lvgl_font.py 生成(自研生成器,不依赖 lv_font_conv);格式:\n")
    w(" *   - 4bpp,连续 nibble 打包,每个字形按字节对齐\n")
    w(" *   - cmap 分两张:ASCII(0x20..0x7E)用 FORMAT0_TINY,其余用 SPARSE_TINY\n")
    w(f" *   - 源字体: {ttf_name}(只用于生成,不随仓库分发)\n")
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
    sparse_start = 0
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
    bpp = int(re.search(r"\.bpp\s*=\s*(\d+)", text).group(1))
    return {
        "text": text,
        "bitmap": data,
        "dsc": dsc,
        "cmap": cmap,
        "line_height": line_height,
        "base_line": base_line,
        "bpp": bpp,
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


def verify(path: str, codepoints: list[int], glyphs: dict) -> int:
    """回读 C 文件,与栅格化结果逐像素比对;缺字形/度量不符都算失败。"""
    font = parse_font(path)
    errors: list[str] = []
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
    if font["line_height"] != EXPECTED_LINE_HEIGHT:
        errors.append(f"行高 {font['line_height']} != ATRI_LINE_H {EXPECTED_LINE_HEIGHT}")
    if font["bpp"] != BPP:
        errors.append(f"bpp {font['bpp']} != {BPP}")
    if errors:
        for line in errors[:20]:
            log(f"    ERROR: {line}")
        log(f"  校验失败: {len(errors)} 项")
        return 1
    log(f"  校验通过: {len(codepoints)} 个字形逐像素与栅格化结果一致,"
        f"行高 {font['line_height']}/基线 {font['base_line']}")
    return 0


# ---------------------------------------------------------------- 主流程
def symbols_path_for(out_path: str) -> str:
    """字符清单固定与 .c 同目录(tests/test_senren_font.py 依赖这个规则)。"""
    return os.path.join(os.path.dirname(out_path), SYMBOLS_NAME)


def write_symbols(path: str, codepoints: list[int]) -> None:
    """一行一个 "U+XXXX",升序、去重;纯数据,方便守卫直接当字符集合读。"""
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for cp in codepoints:
            fh.write(f"U+{cp:04X}\n")


def generate(args: argparse.Namespace) -> int:
    line_height = line_height_for(args.size)
    ttf = find_ttf(args.ttf)
    codepoints = requested_codepoints(args)
    glyphs, missing = rasterize(ttf, args.size, codepoints)
    font = ImageFont.truetype(ttf, args.size, layout_engine=ImageFont.Layout.BASIC)
    ascent, descent = font.getmetrics()
    # 基线在行框内的位置:把 FreeType 的自然行框居中裁到 line_height
    baseline = ascent - (ascent + descent - line_height) // 2
    base_line = line_height - baseline
    name = os.path.splitext(os.path.basename(args.out))[0]
    text = emit_font(name, args.size, codepoints, glyphs, line_height, base_line,
                     os.path.basename(ttf))
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    symbols = symbols_path_for(args.out)
    write_symbols(symbols, codepoints)
    log(f"  字形 {len(codepoints)} 个,源字体自然行高 {ascent + descent},"
        f"基线 {baseline} -> base_line {base_line},行高 {line_height}(ATRI_LINE_H)")
    log(f"  写出 {args.out} ({os.path.getsize(args.out) / 1048576:.2f} MB 源码)")
    log(f"  写出 {symbols} ({os.path.getsize(symbols) / 1024:.0f} KB 清单)")
    if missing:
        log(f"  警告: 源字体缺 {len(missing)} 个字形,设备上会显示成方块: "
            + " ".join(f"U+{cp:04X}" for cp in sorted(missing)[:12]))
    return verify(args.out, codepoints, glyphs)


def check(args: argparse.Namespace) -> int:
    """只校验:重新栅格化并与已生成的 C 文件逐像素比对。"""
    if not os.path.exists(args.check):
        raise SystemExit(f"--check {args.check}: 文件不存在")
    ttf = find_ttf(args.ttf)
    codepoints = requested_codepoints(args)
    glyphs, missing = rasterize(ttf, args.size, codepoints)
    code = verify(args.check, codepoints, glyphs)
    symbols = symbols_path_for(args.check)
    if not os.path.exists(symbols):
        log(f"  缺少字符清单 {symbols}")
        return 1
    wanted = [int(line[2:], 16) for line in open(symbols, encoding="utf-8").read().split()]
    expected = {f"U+{cp:04X}" for cp in codepoints}
    actual = set(open(symbols, encoding="utf-8").read().split())
    if actual != expected:
        log(f"  {symbols} 与当前请求集不一致: 多 {len(actual - expected)}, 少 {len(expected - actual)}")
        return 1
    if missing:
        log(f"  警告: 源字体缺 {len(missing)} 个字形: "
            + " ".join(f"U+{cp:04X}" for cp in sorted(missing)[:12]))
    log(f"  清单 {symbols}: {len(wanted)} 个码位一致")
    return code


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scn", default="build/senren-pack/senren_scn.bin",
                        help="SENRSCN1 脚本包(字符集来源: SEC_CHAR + 包内字面文本)")
    parser.add_argument("--extra-text", action="append", metavar="FILE",
                        help="逐行的额外文案(可重复)")
    parser.add_argument("--sources", default="main/*.c,main/*.h",
                        help="逗号分隔的源码通配符,提取字面量里的非 ASCII 字符与 ASCII 字母数字")
    parser.add_argument("--size", type=int, default=16, help="字号 px(行高必须等于 ATRI_LINE_H)")
    parser.add_argument("--bpp", type=int, default=BPP, help="位深(只支持 4)")
    parser.add_argument("--ttf", help="CJK 源字体 TTF/OTF;省略时按候选表探测")
    parser.add_argument("--out", default="assets/fonts/senren_cjk_16.c",
                        help="输出的 LVGL 字体 C 文件(清单写在其同目录)")
    parser.add_argument("--check", metavar="FONT_C",
                        help="只校验已生成的字体 C 文件(重新栅格化逐像素比对)")
    args = parser.parse_args(argv)
    if args.bpp != BPP:
        raise SystemExit(f"只支持 --bpp {BPP}(LVGL PLAIN 4bpp);收到 {args.bpp}")
    if args.check:
        return check(args)
    return generate(args)


if __name__ == "__main__":
    raise SystemExit(main())
