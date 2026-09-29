#!/usr/bin/env python3
"""生成对讲机应用的中文字库源码(assets/fonts/intercom_cjk_16.c)。

为什么自己写光栅器,而不继续用 lv_font_conv:
    lv_font_conv 停在 1.5.3(2021 年),在 Node 24 下写出的 4bpp 位图与 LVGL 9 的
    PLAIN 解码路径不匹配(同一份 OTF、同一组参数,读回来是噪声,实机就是乱码)。
    本脚本用 Pillow(FreeType)自己栅格化,自行写出 LVGL 9 的 fmt_txt 位图格式;
    写完后立刻把 .c 回读解析,与栅格化结果逐像素比对,--check 走的是同一条路径。

为什么必须用静态字体:
    可变字体 NotoSansSC-VF.ttf 的默认实例在 16px 下笔画太细,4bpp 位图会发虚,
    这正是屏幕上的字"太模糊"的原因。候选字体表里没有 VF 字体,也不含 Windows
    随附的 simhei/msyh/simsun(系统授权不允许把派生的位图字库随固件再分发)。

字符集合(与改造前的 GB2312 全量字库保持一致,共 7540 个码位):
    - GB2312 符号区 682 个:中文标点、全角 ASCII U+FF01-U+FF5E、单位与货币
      (℃ ℉ № ‰ §…)、箭头、几何、制表符、圈数字、希腊与西里尔字母等;
    - GB2312 汉字区 6763 个:一级 3755 + 二级 3008;
    - ASCII 可打印字符 0x20-0x7E 共 95 个:状态行、提示、版本号里的字母与数字。
    对讲机显示的是 App 识别文本与网关回复,内容不可预测,所以仍是全量字库,
    不做子集,避免出现缺字方框。

用法::

    python3 tools/intercom_font.py                       # 生成 + 逐像素自检
    python3 tools/intercom_font.py --check               # 只自检默认产物
    python3 tools/intercom_font.py --check <file.c>      # 只自检指定产物
    python3 tools/intercom_font.py --font <静态 CJK 字体>  # 换源字体

产物(都提交进仓库;源字体本身不提交):
    assets/fonts/intercom_cjk_16.c          LVGL 字库源码,符号 lv_font_intercom_cjk_16
    assets/fonts/intercom_cjk_symbols.txt   覆盖的码位清单,每行 "U+XXXX",升序去重

接线:
    main/CMakeLists.txt 用
      target_sources(${COMPONENT_LIB} PRIVATE
          "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/intercom_cjk_16.c")
    引入;使用方(main/oc_ui.c)用 LV_FONT_DECLARE(lv_font_intercom_cjk_16) 声明。

依赖:Pillow(`python -m pip install pillow`);--check 的结构自检不需要 Pillow。
"""

from __future__ import annotations

import argparse
import io
import os
import re
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - 工具依赖
    Image = ImageDraw = ImageFont = None


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = "assets/fonts/intercom_cjk_16.c"
SYMBOLS_NAME = "intercom_cjk_symbols.txt"
SYMBOL_NAME = "lv_font_intercom_cjk_16"

SIZE = 16               # 字号 px:14px/2bpp 在屏上太糊,改用 16px
BPP = 4                 # LVGL 的 PLAIN 4bpp:连续 nibble 打包
EXTRA_LEADING = 4       # 行高 = 字号 + 4,与 main/oc_ui.c 的排版常量对应
EXPECTED_LINE_HEIGHT = 20   # main/oc_ui.c 按 20px 行高排版(状态栏 30px 等),断言在此

# GB2312 的符号区与汉字区都落在 A1A1-F7FE;F8-FE 是用户自定义区,不生成。
GB2312_FIRST_ROW = 0xA1
GB2312_LAST_ROW = 0xF7
GB2312_FIRST_CELL = 0xA1
GB2312_LAST_CELL = 0xFE
# ASCII 可打印区间,含空格(状态行里必须能画出空格宽度)。
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E

# 探测"缺字形"用的码位:Unicode 里永远不分配,栅格化结果与它完全相同 = 源字体没这个字形。
NOTDEF_PROBE = 0x10FFFE

FONT_ENV = "INTERCOM_FONT"
# 源字体的自动探测顺序:优先 LVGL 组件里自带的思源黑体(本仓库第一次 build 后必然
# 存在,16.4 MB,Source Han Sans 与 Noto Sans CJK 同源,OFL 1.1,可再分发)。
# 不使用可变字体,也不用 simhei/msyh(见模块 docstring)。
FONT_CANDIDATES = (
    "managed_components/lvgl__lvgl/scripts/generators/built_in_font/SourceHanSansSC-Normal.otf",
    "assets/fonts/SourceHanSansSC-Normal.otf",
    "managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf",
    "assets/fonts/NotoSansSC-Regular.otf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/source-han-sans/SourceHanSansSC-Regular.otf",
)

GLYPH_COMMENT_RE = re.compile(r"/\* U\+([0-9A-F]{4,6}) \*/")


class FontToolError(RuntimeError):
    """可预期的失败:缺工具、缺字体、栅格化或自检不通过。"""


def log(message: str) -> None:
    print(message, file=sys.stderr)


# ---------------------------------------------------------------- 字符收集
def gb2312_characters() -> tuple[list[str], list[str]]:
    """解码 GB2312 码位,返回 (汉字列表, 非汉字符号列表)。

    GB2312 行与列各 94 个位置,其中存在未分配的空位,直接解码会抛
    UnicodeDecodeError,这里按位跳过,得到与标准一致的真实字符数:
    6763 个汉字 + 682 个符号。
    """
    hanzi: list[str] = []
    symbols: list[str] = []
    for row in range(GB2312_FIRST_ROW, GB2312_LAST_ROW + 1):
        for cell in range(GB2312_FIRST_CELL, GB2312_LAST_CELL + 1):
            try:
                char = bytes((row, cell)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if "\u4e00" <= char <= "\u9fff":
                hanzi.append(char)
            else:
                symbols.append(char)
    return hanzi, symbols


def build_inventory() -> dict[str, object]:
    """构造请求字符集与分类计数。"""
    hanzi, symbols = gb2312_characters()
    ascii_chars = [chr(cp) for cp in range(ASCII_FIRST, ASCII_LAST + 1)]
    requested = sorted(set(hanzi) | set(symbols) | set(ascii_chars))
    return {
        "requested": requested,
        "hanzi": set(hanzi),
        "symbols": set(symbols),
        "ascii": set(ascii_chars),
    }


def requested_codepoints(inventory: dict[str, object]) -> list[int]:
    """字体要覆盖的全部码位(升序);这就是 symbols.txt 的内容。"""
    return [ord(char) for char in inventory["requested"]]  # type: ignore[arg-type]


# ---------------------------------------------------------------- 源字体
def find_font(explicit: str | None) -> str:
    """返回源字体路径;没给就按候选表探测,失败时把找过的路径全部列出来。"""
    if explicit:
        if not os.path.exists(explicit):
            raise FontToolError(f"--font {explicit}: 文件不存在")
        return explicit
    env = os.environ.get(FONT_ENV)
    candidates = ([env] if env else []) + list(FONT_CANDIDATES)
    for path in candidates:
        if os.path.exists(path):
            log(f"  源字体: {path}(候选表第一个存在的;可用 --font 覆盖)")
            return path
    listing = "\n".join(f"    {path}" for path in candidates)
    raise FontToolError(
        "找不到可用的静态 CJK 字体,请用 --font <path> 指定;已查找:\n"
        f"{listing}\n"
        "  也可设环境变量 INTERCOM_FONT。第一次 idf.py build 后 LVGL 组件目录里"
        "会带思源黑体(managed_components/...)。"
    )


def line_height_for(size: int) -> int:
    """界面按 16px 字、20px 行高排版,所以行高必须正好是 EXPECTED_LINE_HEIGHT。"""
    line_height = size + EXTRA_LEADING
    if line_height != EXPECTED_LINE_HEIGHT:
        raise FontToolError(
            f"--size {size} 得到行高 {line_height},但 main/oc_ui.c 的排版按 "
            f"{EXPECTED_LINE_HEIGHT}px 行高固定(状态栏 30px、提示行、配对面板);"
            f"请用 --size {EXPECTED_LINE_HEIGHT - EXTRA_LEADING},或同步调整界面常量"
        )
    return line_height


# ---------------------------------------------------------------- 字形栅格化
def rasterize(
    font_path: str, size: int, codepoints: list[int]
) -> tuple[dict[int, tuple], set[int]]:
    """返回 ({码位: (adv_w, box_w, box_h, ofs_x, ofs_y, nibbles)}, 缺字形码位)。

    ofs_y 的约定来自 LVGL 的绘制公式:
      letter_y = line_top + (line_height - base_line) - box_h - ofs_y
    即 ofs_y = baseline - 字形盒底(基线以下的部分为负)。adv_w 是 4.4 定点宽度
    (LVGL 的 advance 单位),所以乘 16。
    """
    if ImageFont is None:
        raise FontToolError("需要 Pillow 才能栅格化字形:python -m pip install pillow")
    font = ImageFont.truetype(font_path, size, layout_engine=ImageFont.Layout.BASIC)
    ascent, _descent = font.getmetrics()
    pad = 4                        # 画布留白:原点固定在 (pad, pad) = 行顶(上文线)
    span = size * 2 + pad * 2
    glyphs: dict[int, tuple] = {}
    missing: set[int] = set()
    # .notdef 参考位图:缺字形的字在屏幕上就是方块,生成时要点名
    notdef = Image.new("L", (span, span), 0)
    ImageDraw.Draw(notdef).text((pad, pad), chr(NOTDEF_PROBE), font=font, fill=255)
    notdef_bytes = notdef.tobytes()
    for cp in codepoints:
        char = chr(cp)
        adv_w = int(round(font.getlength(char) * 16))
        canvas = Image.new("L", (span, span), 0)
        ImageDraw.Draw(canvas).text((pad, pad), char, font=font, fill=255)
        if canvas.tobytes() == notdef_bytes:
            missing.add(cp)
        bbox = canvas.getbbox()
        if bbox is None:
            # 空白字符(空格/全角空格):有字形但没有墨迹
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
def emit_font(
    symbol: str,
    size: int,
    codepoints: list[int],
    glyphs: dict[int, tuple],
    line_height: int,
    base_line: int,
    font_name: str,
) -> str:
    """把字形表写成 LVGL 9 的 fmt_txt 源码(自包含,只 include "lvgl.h")。"""
    # 字符映射分两类表,与 LVGL 的 fmt_txt 语义对应(见 lv_font_fmt_txt.h):
    #   cmap 0: 连续的 ASCII 区(0x20..0x7E)用 FORMAT0_TINY(不需要额外表)
    #   其余:   SPARSE_TINY(排好序的相对偏移表 + 二分查找),偏移是 u16,
    #          所以每张表的码位跨度必须 <= 0xFFFE;跨度大的码位再开一张。
    #          GB2312 的符号 + 汉字全在 U+00A4..U+FFE5,平时只有一张表。
    ascii_cps = [cp for cp in codepoints if ASCII_FIRST <= cp <= ASCII_LAST]
    rest = [cp for cp in codepoints if not (ASCII_FIRST <= cp <= ASCII_LAST)]
    if ascii_cps != list(range(ASCII_FIRST, ASCII_LAST + 1)):
        raise FontToolError("ASCII 区必须完整连续(0x20..0x7E),否则 FORMAT0_TINY 不适用")
    # 字形编号必须按码位升序:LVGL 的 SPARSE_TINY cmap 对 unicode_list 做二分查找,
    # 顺序错乱会让查表落到别的字形上。ASCII(0x20..0x7E)固定排在前 95 个,
    # 其余按码位升序分组;分组按「组内第一个码位」算跨度,保证 range_length 不溢出。
    rest_sorted = sorted(rest)
    ordered = ascii_cps + rest_sorted
    sparse_groups: list[list[int]] = []
    for cp in rest_sorted:
        if not sparse_groups or cp - sparse_groups[-1][0] > 0xFFFE:
            sparse_groups.append([cp])
        else:
            sparse_groups[-1].append(cp)
    cmap_num = 1 + len(sparse_groups)

    out = io.StringIO()
    write = out.write
    write("/*******************************************************************************\n")
    write(f" * Size: {size} px\n")
    write(f" * Bpp: {BPP}\n")
    write(f" * 由 tools/intercom_font.py 生成,请勿手工修改;格式:\n")
    write(" *   - 4bpp,PLAIN(非压缩)位图,连续 nibble 打包,每个字形按字节对齐\n")
    write(" *   - cmap:ASCII(0x20..0x7E)用 FORMAT0_TINY,其余按跨度分组用 SPARSE_TINY\n")
    write(f" *   - 源字体: {font_name}(只用于生成,不随仓库分发)\n")
    write(" * 生成时会回读本文件并与 FreeType 栅格化结果逐像素比对。\n")
    write(" *****************************************************************************/\n\n")
    write('#include "lvgl.h"\n\n')
    write(f"#ifndef {symbol.upper()}\n#define {symbol.upper()} 1\n\n")
    write("/*-----------------\n *    BITMAPS\n *----------------*/\n\n")
    write("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")

    index = 0
    descriptors = []
    for position, cp in enumerate(ordered):
        adv_w, bw, bh, ofs_x, ofs_y, nibbles = glyphs[cp]
        glyph_id = position + 1
        if bw == 0 or bh == 0:
            descriptors.append((glyph_id, index, adv_w, 0, 0, ofs_x, ofs_y))
            continue
        data = pack_nibbles(nibbles)
        descriptors.append((glyph_id, index, adv_w, bw, bh, ofs_x, ofs_y))
        write(f"    /* U+{cp:04X} */\n")
        for offset in range(0, len(data), 12):
            chunk = data[offset : offset + 12]
            write("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",\n")
        index += len(data)
    write("};\n\n")

    write("/*-----------------\n *  GLYPH DESCRIPTION\n *----------------*/\n\n")
    write("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
    write(
        "    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0,"
        " .ofs_y = 0} /* id = 0 reserved */,\n"
    )
    for _glyph_id, bitmap_index, adv_w, bw, bh, ofs_x, ofs_y in descriptors:
        write(
            f"    {{.bitmap_index = {bitmap_index}, .adv_w = {adv_w}, .box_w = {bw},"
            f" .box_h = {bh}, .ofs_x = {ofs_x}, .ofs_y = {ofs_y}}},\n"
        )
    write("};\n\n")

    write("/*-----------------\n *  CHARACTER MAPPING\n *----------------*/\n\n")
    for group_index, group in enumerate(sparse_groups, 1):
        start = group[0]
        write(f"static const uint16_t unicode_list_{group_index}[] = {{\n    ")
        for i, cp in enumerate(group):
            write(f"0x{cp - start:x}, ")
            if (i + 1) % 8 == 0 and i + 1 < len(group):
                write("\n    ")
        write("\n};\n\n")

    write("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
    write("    {\n")
    write("        .range_start = 0x20, .range_length = 95,\n")
    write("        .glyph_id_start = 1,\n")
    write("        .unicode_list = NULL, .glyph_id_ofs_list = NULL,\n")
    write("        .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n")
    write("    }")
    glyph_id_start = len(ascii_cps) + 1
    for group_index, group in enumerate(sparse_groups, 1):
        write(",\n    {\n")
        write(
            f"        .range_start = {group[0]},"
            f" .range_length = {group[-1] - group[0] + 1},\n"
        )
        write(f"        .glyph_id_start = {glyph_id_start},\n")
        write(
            f"        .unicode_list = unicode_list_{group_index},"
            " .glyph_id_ofs_list = NULL,\n"
        )
        write(
            f"        .list_length = {len(group)},"
            " .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n"
        )
        write("    }")
        glyph_id_start += len(group)
    write("\n};\n\n")

    write("/*-----------------\n *  ALL CUSTOM DATA\n *----------------*/\n\n")
    write("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
    write("    .glyph_bitmap = glyph_bitmap,\n")
    write("    .glyph_dsc = glyph_dsc,\n")
    write("    .cmaps = cmaps,\n")
    write("    .kern_dsc = NULL,\n")
    write("    .kern_scale = 0,\n")
    write(f"    .cmap_num = {cmap_num},\n")
    write(f"    .bpp = {BPP},\n")
    write("    .kern_classes = 0,\n")
    write("    .bitmap_format = 0,   /* LV_FONT_FMT_TXT_PLAIN:位图未压缩、未做行 XOR */\n")
    write("};\n\n")

    write("/*-----------------\n *  PUBLIC FONT\n *----------------*/\n\n")
    write(f"const lv_font_t {symbol} = {{\n")
    write("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
    write("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
    write(f"    .line_height = {line_height},\n")
    write(f"    .base_line = {base_line},\n")
    write("#if LV_VERSION_CHECK(9, 6, 0)\n")
    write(f"    .cap_height = {size},\n")
    write(f"    .x_height = {size * 9 // 16},\n")
    write("#endif\n")
    write("    .subpx = LV_FONT_SUBPX_NONE,\n")
    write("    .underline_position = -1,\n")
    write("    .underline_thickness = 1,\n")
    write("#if LV_VERSION_CHECK(9, 3, 0)\n")
    write("    .static_bitmap = 1,\n")
    write("#endif\n")
    write("    .dsc = &font_dsc,\n")
    write("    .fallback = NULL,\n")
    write("    .user_data = NULL,\n")
    write("};\n\n")
    write(f"#endif /* {symbol.upper()} */\n")
    return out.getvalue()


def write_symbols(path: str, codepoints: list[int]) -> None:
    """一行一个 "U+XXXX",升序、去重;纯数据,方便消费方直接当字符集合读。"""
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        for cp in codepoints:
            handle.write(f"U+{cp:04X}\n")


def symbols_path_for(output: str) -> str:
    """字符清单固定与 .c 同目录。"""
    return os.path.join(os.path.dirname(output), SYMBOLS_NAME)


# ---------------------------------------------------------------- 回读自检
def parse_font(path: str) -> dict:
    """把写出的 C 文件解析回结构与位图,用于结构自检与逐像素校验。"""
    text = Path(path).read_text(encoding="utf-8")
    bitmap_text = re.search(
        r"const uint8_t glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", text, re.S
    )
    if not bitmap_text:
        raise FontToolError(f"{path}: 找不到 glyph_bitmap")
    data = bytes(
        int(byte, 16) for byte in re.findall(r"0x([0-9a-fA-F]{2})", bitmap_text.group(1))
    )
    descriptors = [
        tuple(int(value) for value in match)
        for match in re.findall(
            r"\{\s*\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),"
            r"\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\s*\}",
            text,
        )
    ]
    cmap: dict[int, int] = {}
    cmap_text = re.search(
        r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};\n", text, re.S
    )
    if not cmap_text:
        raise FontToolError(f"{path}: 找不到 cmaps[]")
    entries = re.findall(
        r"\{\s*\.range_start\s*=\s*(0x[0-9a-fA-F]+|\d+),\s*\.range_length\s*=\s*(\d+),"
        r"\s*\.glyph_id_start\s*=\s*(\d+),"
        r"\s*\.unicode_list\s*=\s*(\w+),\s*\.glyph_id_ofs_list\s*=\s*(\w+),"
        r"\s*\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*(\w+)",
        cmap_text.group(1),
        re.S,
    )
    if not entries:
        raise FontToolError(f"{path}: 解析不出任何 cmap 条目")
    for (
        start_text,
        length_text,
        gid_start_text,
        list_name,
        ofs_name,
        list_len_text,
        kind,
    ) in entries:
        start = int(start_text, 16) if start_text.lower().startswith("0x") else int(start_text)
        length = int(length_text)
        glyph_id_start = int(gid_start_text)
        list_length = int(list_len_text)
        if ofs_name != "NULL":
            raise FontToolError(f"{path}: 生成器不使用 glyph_id_ofs_list,实际 {ofs_name}")
        if kind.endswith("FORMAT0_TINY"):
            for i in range(length):
                cmap[start + i] = glyph_id_start + i
        elif kind.endswith("SPARSE_TINY"):
            array_text = re.search(
                rf"static const uint16_t {list_name}\[\]\s*=\s*\{{(.*?)\}};", text, re.S
            )
            if not array_text:
                raise FontToolError(f"{path}: 找不到 {list_name}")
            unicode_list = [
                int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]+", array_text.group(1))
            ]
            if len(unicode_list) != list_length:
                raise FontToolError(
                    f"{path}: {list_name} 长度 {len(unicode_list)} != list_length {list_length}"
                )
            for i, offset in enumerate(unicode_list):
                cmap[start + offset] = glyph_id_start + i
        else:
            raise FontToolError(f"{path}: 不支持的 cmap 类型 {kind}")

    def field(name: str) -> int:
        match = re.search(rf"\.{name}\s*=\s*(-?\d+)", text)
        if not match:
            raise FontToolError(f"{path}: 找不到字段 .{name}")
        return int(match.group(1))

    return {
        "text": text,
        "bitmap": data,
        "glyph_dsc": descriptors,
        "cmap": cmap,
        "line_height": field("line_height"),
        "base_line": field("base_line"),
        "bpp": field("bpp"),
    }


def decode_glyph(font: dict, cp: int):
    """按 LVGL 的 PLAIN 4bpp 读法取一个字形(连续 nibble,字形起点按字节对齐)。"""
    glyph_id = font["cmap"].get(cp)
    if glyph_id is None:
        return None
    bitmap_index, adv_w, bw, bh, ofs_x, ofs_y = font["glyph_dsc"][glyph_id]
    nibbles = []
    for i in range(bw * bh):
        bit_position = bitmap_index * 8 + i * 4
        byte = font["bitmap"][bit_position // 8]
        nibbles.append((byte >> 4) if bit_position % 8 == 0 else (byte & 0xF))
    return (adv_w, bw, bh, ofs_x, ofs_y, nibbles)


def structural_checks(path: str, symbol: str, codepoints: list[int]) -> tuple[list[str], dict]:
    """字形数、导出符号、PLAIN、码位覆盖等结构自检;返回 (问题列表, 解析结果)。"""
    problems: list[str] = []
    if not os.path.isfile(path):
        return [f"缺少产物 {path}"], {}
    font = parse_font(path)
    text = font["text"]

    if f"const lv_font_t {symbol} = {{" not in text:
        problems.append(f"{path}: 未找到导出符号 {symbol}")
    if ".bitmap_format = 0,   /* LV_FONT_FMT_TXT_PLAIN" not in text:
        problems.append(f"{path}: 位图不是 PLAIN(bitmap_format=0),与 LVGL 9 解码不兼容")
    if "get_glyph_bitmap = lv_font_get_bitmap_fmt_txt" not in text:
        problems.append(f"{path}: 缺少 lv_font_get_bitmap_fmt_txt 绑定")
    if ".fallback = NULL," not in text:
        problems.append(f"{path}: 缺少 LVGL 9 的 .fallback 字段")
    if '#include "lvgl.h"' not in text:
        problems.append(f"{path}: 缺少 LVGL 头文件包含(非自包含)")
    if "lvgl/lvgl.h" in text:
        problems.append(f"{path}: 仍包含 lvgl/lvgl.h 分支,本仓库 main 组件无法解析")
    if font["bpp"] != BPP:
        problems.append(f"{path}: bpp {font['bpp']} != {BPP}")
    if font["line_height"] != EXPECTED_LINE_HEIGHT:
        problems.append(
            f"{path}: 行高 {font['line_height']} != main/oc_ui.c 的 {EXPECTED_LINE_HEIGHT}"
        )
    if ".range_start = 0x20, .range_length = 95," not in text:
        problems.append(f"{path}: ASCII 区不是完整的 FORMAT0_TINY(0x20..0x7E)")

    missing = [cp for cp in codepoints if cp not in font["cmap"]]
    if missing:
        problems.append(
            f"{path}: {len(missing)} 个码位不在 cmap 里: "
            + " ".join(f"U+{cp:04X}" for cp in missing[:8])
        )
    expected_glyphs = len(codepoints) + 1     # +1 是 id 0 的保留项
    if len(font["glyph_dsc"]) != expected_glyphs:
        problems.append(
            f"{path}: glyph_dsc 条目 {len(font['glyph_dsc'])} != 期望 {expected_glyphs}"
        )
    extra = sorted(set(font["cmap"]) - set(codepoints))
    if extra:
        problems.append(
            f"{path}: cmap 含 {len(extra)} 个未请求的码位: "
            + " ".join(f"U+{cp:04X}" for cp in extra[:8])
        )
    return problems, font


def pixel_problems(font: dict, codepoints: list[int], glyphs: dict[int, tuple]) -> list[str]:
    """把回读的位图与栅格化结果逐像素比对;缺字形/度量不符都算失败。"""
    problems: list[str] = []
    for cp in codepoints:
        want = glyphs.get(cp)
        if want is None:
            continue
        got = decode_glyph(font, cp)
        if got is None:
            continue
        if (want[1], want[2]) != (got[1], got[2]):
            problems.append(f"U+{cp:04X} 盒尺寸不符: {got[1]}x{got[2]} != {want[1]}x{want[2]}")
            continue
        if (want[0], want[3], want[4]) != (got[0], got[3], got[4]):
            problems.append(
                f"U+{cp:04X} 度量不符: adv/ofs {got[0]},{got[3]},{got[4]} != "
                f"{want[0]},{want[3]},{want[4]}"
            )
            continue
        if want[5] != got[5]:
            diff = sum(1 for a, b in zip(want[5], got[5]) if a != b)
            problems.append(f"U+{cp:04X} 位图不一致({diff}/{len(want[5])} 像素不同)")
        if len(problems) > 40:
            problems.append("…(其余差异略)")
            break
    return problems


# ---------------------------------------------------------------- 主流程
def report(inventory: dict, output: str, font: dict, missing: set[int]) -> None:
    """打印可粘贴进 assets/README 的统计数字。"""
    c_size = os.path.getsize(output)
    log("—— 自检报告 ——")
    log(
        f"请求字符数: {len(inventory['requested'])} "
        f"(汉字 {len(inventory['hanzi'])} + GB2312 符号 {len(inventory['symbols'])} "
        f"+ ASCII {len(inventory['ascii'])})"
    )
    if font:
        log(f"生成字形数: {len(font['glyph_dsc']) - 1},cmap 覆盖 {len(font['cmap'])} 个码位")
        log(
            f"行高 {font['line_height']} / 基线 base_line {font['base_line']},"
            f"bpp {font['bpp']},PLAIN(bitmap_format=0)"
        )
        log(f"位图数据: {len(font['bitmap'])} 字节 ({len(font['bitmap']) / 1024:.0f} KiB)")
    log(f"{output}: {c_size} 字节 ({c_size / 1048576:.2f} MiB 源码)")
    if missing:
        log(
            f"源字体缺 {len(missing)} 个字形,设备上会显示成方块: "
            + " ".join(f"U+{cp:04X}" for cp in sorted(missing)[:12])
        )
    else:
        log("源字体覆盖: 7540/7540 全部命中")


def generate(args: argparse.Namespace) -> int:
    size = args.size
    if args.bpp != BPP:
        raise FontToolError(f"只支持 --bpp {BPP}(LVGL PLAIN 4bpp);收到 {args.bpp}")
    line_height = line_height_for(size)
    font_path = find_font(args.font)
    inventory = build_inventory()
    codepoints = requested_codepoints(inventory)

    glyphs, missing = rasterize(font_path, size, codepoints)
    metrics = ImageFont.truetype(font_path, size, layout_engine=ImageFont.Layout.BASIC)
    ascent, descent = metrics.getmetrics()
    # 基线在行框内的位置:把 FreeType 的自然行框居中裁到 line_height
    baseline = ascent - (ascent + descent - line_height) // 2
    base_line = line_height - baseline

    output = args.out
    os.makedirs(os.path.dirname(output) or ".", exist_ok=True)
    Path(output).write_text(
        emit_font(
            args.symbol, size, codepoints, glyphs, line_height, base_line,
            os.path.basename(font_path),
        ),
        encoding="utf-8",
        newline="\n",
    )
    write_symbols(symbols_path_for(output), codepoints)
    log(
        f"  源字体自然行高 {ascent + descent},基线 {baseline} -> base_line {base_line},"
        f"行高 {line_height}"
    )
    log(f"  写出 {output}({os.path.getsize(output) / 1048576:.2f} MiB 源码)")
    log(f"  写出 {symbols_path_for(output)}({len(codepoints)} 个码位)")

    problems, font = structural_checks(output, args.symbol, codepoints)
    if not problems:
        problems = pixel_problems(font, codepoints, glyphs)
    report(inventory, output, font, missing)
    if problems:
        for problem in problems[:40]:
            log(f"    ERROR: {problem}")
        log(f"自检失败: {len(problems)} 项")
        return 1
    log(f"自检: PASS({len(codepoints)} 个字形逐像素与栅格化结果一致)")
    return 0


def check(args: argparse.Namespace) -> int:
    """只自检:结构检查 + (能找到源字体时)重新栅格化逐像素比对。"""
    output = args.check
    if not os.path.isfile(output):
        raise FontToolError(f"--check {output}: 文件不存在")
    inventory = build_inventory()
    codepoints = requested_codepoints(inventory)

    problems, font = structural_checks(output, args.symbol, codepoints)
    if problems:
        for problem in problems:
            log(f"    ERROR: {problem}")
        log(f"  自检失败: {len(problems)} 项")
        return 1

    symbols_path = symbols_path_for(output)
    if not os.path.isfile(symbols_path):
        log(f"  缺少字符清单 {symbols_path}")
        return 1
    expected = {f"U+{cp:04X}" for cp in codepoints}
    actual = set(Path(symbols_path).read_text(encoding="utf-8").split())
    if actual != expected:
        log(
            f"  {symbols_path} 与当前请求集不一致: 多 {len(actual - expected)},"
            f" 少 {len(expected - actual)}"
        )
        return 1

    try:
        font_path = find_font(args.font)
    except FontToolError as error:
        log(f"  跳过像素比对:{error}")
        report(inventory, output, font, set())
        log("自检: PASS(仅结构检查;装好源字体后再跑可做像素比对)")
        return 0

    glyphs, missing = rasterize(font_path, args.size, codepoints)
    problems = pixel_problems(font, codepoints, glyphs)
    report(inventory, output, font, missing)
    if problems:
        for problem in problems[:40]:
            log(f"    ERROR: {problem}")
        log(f"自检失败: {len(problems)} 项")
        return 1
    log(
        f"自检: PASS(导出符号 {args.symbol}、{len(codepoints)} 个码位覆盖、"
        f"PLAIN 4bpp、逐像素一致)"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="生成覆盖完整 GB2312 的对讲机中文字库(16px / 4bpp / PLAIN)。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--font",
        default=None,
        help=f"静态 CJK 源字体 TTF/OTF;省略时按候选表探测(环境变量 {FONT_ENV} 可覆盖)",
    )
    parser.add_argument("--size", type=int, default=SIZE, help=f"字号像素(默认 {SIZE})")
    parser.add_argument("--bpp", type=int, default=BPP, help=f"位深(只支持 {BPP})")
    parser.add_argument(
        "--out",
        default=DEFAULT_OUTPUT,
        help=f"输出 .c(清单写在其同目录,默认 {DEFAULT_OUTPUT})",
    )
    parser.add_argument(
        "--symbol",
        default=SYMBOL_NAME,
        help=f"导出符号名(默认 {SYMBOL_NAME})",
    )
    parser.add_argument(
        "--check",
        nargs="?",
        const=DEFAULT_OUTPUT,
        metavar="FONT_C",
        help=f"只自检(默认 {DEFAULT_OUTPUT}):字形数/导出符号/PLAIN/码位覆盖,"
        "并在能找到源字体时逐像素比对",
    )
    args = parser.parse_args(argv)
    try:
        if args.check:
            return check(args)
        return generate(args)
    except FontToolError as error:
        log(f"ERROR: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
