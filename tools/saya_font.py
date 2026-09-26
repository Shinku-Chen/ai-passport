#!/usr/bin/env python3
"""生成 AI Passport《沙耶之歌》阅读器的中文字体子集(LVGL 9 位图字体)。

为什么不用 lv_font_conv:这个工具最后一版是 1.5.3(2021),在 Node v24 下写出的
字形位图是坏的 —— 同一个 OTF、同一套参数,`--no-compress` 与
`--no-compress --no-prefilter` 产出的字节完全相同,按 LVGL 的 PLAIN 4bpp 读法解码
全是噪点(LVGL 自带的 Montserrat 字体用同一解码器完全正常)。表现到设备上就是
"文字全部乱码"。这里改成直接用 Pillow(FreeType)栅格化 + 自己写 LVGL 字体格式,
不依赖 Node;生成后会按 LVGL 的查找语义回读 C 文件,与栅格化结果逐像素比对。

cmap 结构必须与 LVGL 的 get_glyph_dsc_id() 严格对上(见
managed_components/lvgl__lvgl/src/font/fmt_txt/lv_font_fmt_txt.c):
  FORMAT0_TINY  连续码位,glyph_id = glyph_id_start + (cp - range_start)
  SPARSE_TINY   稀疏码位,二分查找 unicode_list(相对 range_start 的偏移),
                glyph_id = glyph_id_start + 命中下标
  FORMAT0_FULL  用 glyph_id_ofs_list(uint8 逐码位表),稀疏大字集不可用(且给 NULL 会崩)
所以这里跟 LVGL 自带的 Montserrat 一样:ASCII 一段 TINY + 其余一段 SPARSE_TINY。

字符集来自两处,保证界面文字与剧本正文都不缺字:
  1. 资源包(main/saya_data/saya_pack.bin)里的全部剧本文字与名字;
  2. main/ 下所有 .c/.h 里出现的非 ASCII 字面量(界面文案)。

用法:
  python tools/saya_font.py --font <NotoSansSC-Regular.otf> \
      --pack main/saya_data/saya_pack.bin --out-dir assets/fonts     # 生成 + 校验
  python tools/saya_font.py --check --pack main/saya_data/saya_pack.bin \
      --out-dir assets/fonts                                         # 只做覆盖/结构自检

生成物(assets/fonts/saya_cjk_*.c 与 saya_cjk_symbols.txt)提交进仓库;源字体不上传。
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

SIZES = (16, 20)      # 生成的像素字号
BPP = 4
# 行高固定为 字号+4:界面按"16px 4 行 / 20px 3 行"排版(见 main/saya_app.c 的 layout_for),
# 若用 Noto Sans SC 的自然行高(16px 字号约 24px),一屏就放不下 4 行。
# 多出来的行距在自然行框里上下各裁一半,基线随之平移,字形本身不受影响。
EXTRA_LEADING = 4

CMAP_TINY = "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY"
CMAP_SPARSE = "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY"


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


# ---------------------------------------------------------------- 字符收集
def pack_chars(pack_path: str) -> set:
    blob = open(pack_path, "rb").read()
    if blob[:8] != b"SAYAPK01":
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


def required_chars(pack_path: str, source_root: str) -> set:
    chars = pack_chars(pack_path) | source_chars(source_root)
    chars.discard("\u3000")
    for start, end in ((0x20, 0x7E),):
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


def build_cmaps(codepoints: list) -> list:
    """按 LVGL 的语义切 cmap:ASCII 连续段用 TINY,其余用 SPARSE_TINY。"""
    ascii_cps = list(range(0x20, 0x7F))
    dense = all(cp in codepoints for cp in ascii_cps) and codepoints[: len(ascii_cps)] == ascii_cps
    entries = []
    if dense:
        entries.append({"range_start": 0x20, "range_length": len(ascii_cps),
                        "glyph_id_start": 1, "offsets": None, "list_length": 0,
                        "type": CMAP_TINY})
        rest = [cp for cp in codepoints if cp > 0x7E]
        gid_start = 1 + len(ascii_cps)
    else:
        rest = list(codepoints)
        gid_start = 1
    if rest:
        span = rest[-1] - rest[0] + 1
        if rest[-1] - rest[0] > 0xFFFF:
            raise SystemExit("稀疏码位跨度超过 uint16,需要拆成多段 cmap")
        entries.append({"range_start": rest[0], "range_length": span,
                        "glyph_id_start": gid_start,
                        "offsets": [cp - rest[0] for cp in rest],
                        "list_length": len(rest), "type": CMAP_SPARSE})
    return entries


# ---------------------------------------------------------------- 生成 C 文件
def emit_font(name: str, size: int, codepoints: list, glyphs: dict, line_height: int,
              base_line: int) -> str:
    cmaps = build_cmaps(codepoints)
    out = io.StringIO()
    w = out.write
    w("/*******************************************************************************\n")
    w(f" * Size: {size} px\n")
    w(f" * Bpp: {BPP}\n")
    w(" * 由 tools/saya_font.py 生成(自研生成器,不依赖 lv_font_conv);格式:\n")
    w(" *   - 4bpp,连续 nibble 打包,每个字形按字节对齐\n")
    w(" *   - cmap:ASCII 段 FORMAT0_TINY + 其余 SPARSE_TINY(与 LVGL 自带字体同构)\n")
    w(" * 生成时会按 LVGL 的查找语义回读本文件,与栅格化结果逐像素比对。\n")
    w(" *****************************************************************************/\n\n")
    w('#include "lvgl.h"\n\n')
    guard = name.upper()
    w(f"#ifndef {guard}\n#define {guard} 1\n\n")
    w("/*-----------------\n *    BITMAPS\n *----------------*/\n\n")
    w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")

    index = 0
    dsc_lines = []
    for i, cp in enumerate(codepoints):
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
    for idx, entry in enumerate(cmaps):
        if entry["offsets"] is None:
            continue
        w(f"static const uint16_t unicode_list_{idx}[] = {{\n    ")
        for i, off in enumerate(entry["offsets"]):
            w(f"0x{off:x}, ")
            if (i + 1) % 8 == 0 and i + 1 < len(entry["offsets"]):
                w("\n    ")
        w("\n};\n\n")

    w("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
    for idx, entry in enumerate(cmaps):
        ulist = f"unicode_list_{idx}" if entry["offsets"] is not None else "NULL"
        w("    {\n")
        w(f"        .range_start = {entry['range_start']},"
          f" .range_length = {entry['range_length']},\n")
        w(f"        .glyph_id_start = {entry['glyph_id_start']},\n")
        w(f"        .unicode_list = {ulist}, .glyph_id_ofs_list = NULL,\n")
        w(f"        .list_length = {entry['list_length']}, .type = {entry['type']}\n")
        w("    },\n")
    w("};\n\n")

    w("/*-----------------\n *  ALL CUSTOM DATA\n *----------------*/\n\n")
    w("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
    w("    .glyph_bitmap = glyph_bitmap,\n")
    w("    .glyph_dsc = glyph_dsc,\n")
    w("    .cmaps = cmaps,\n")
    w("    .kern_dsc = NULL,\n")
    w("    .kern_scale = 0,\n")
    w(f"    .cmap_num = {len(cmaps)},\n")
    w(f"    .bpp = {BPP},\n")
    w("    .kern_classes = 0,\n")
    w("    .bitmap_format = 0,   /* LV_FONT_FMT_TXT_PLAIN:位图未压缩、未做行 XOR */\n")
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
    w(f"#endif /* {guard} */\n")
    return out.getvalue()


# ---------------------------------------------------------------- 回读校验
def parse_font(path: str) -> dict:
    """把写出的 C 文件解析回结构与位图,并按 LVGL 的语义做 cmap 查找。"""
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
    ulists = {name: [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)]
              for name, body in re.findall(
                  r"const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};", text, re.S)}
    cmap_body = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};",
                          text, re.S)
    if not cmap_body:
        raise SystemExit(f"{path}: 找不到 cmaps")
    cmaps = []
    for entry in re.findall(r"\{(.*?)\}", cmap_body.group(1), re.S):
        get = lambda key: re.search(rf"\.{key}\s*=\s*([\w\d]+)", entry)
        rs = int(get("range_start").group(1))
        rl = int(get("range_length").group(1))
        gs = int(get("glyph_id_start").group(1))
        ulist = get("unicode_list").group(1)
        llen = int(get("list_length").group(1))
        typ = get("type").group(1)
        cmaps.append({"range_start": rs, "range_length": rl, "glyph_id_start": gs,
                      "unicode_list": ulists.get(ulist), "list_length": llen, "type": typ})
    line_height = int(re.search(r"\.line_height\s*=\s*(\d+)", text).group(1))
    base_line = int(re.search(r"\.base_line\s*=\s*(\d+)", text).group(1))
    cmap_num = int(re.search(r"\.cmap_num\s*=\s*(\d+)", text).group(1))
    if cmap_num != len(cmaps):
        raise SystemExit(f"{path}: cmap_num {cmap_num} != 实际 {len(cmaps)}")
    return {"bitmap": data, "dsc": dsc, "cmaps": cmaps, "line_height": line_height,
            "base_line": base_line}


def lookup_glyph_id(font: dict, cp: int) -> int:
    """完全照 LVGL 的 get_glyph_dsc_id() 走一遍。"""
    for cmap in font["cmaps"]:
        rcp = cp - cmap["range_start"]
        if rcp < 0 or rcp >= cmap["range_length"]:
            continue
        if cmap["type"] == "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY":
            return cmap["glyph_id_start"] + rcp
        if cmap["type"] == "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY":
            if cmap["unicode_list"] and rcp in cmap["unicode_list"]:
                return cmap["glyph_id_start"] + cmap["unicode_list"].index(rcp)
            continue
        raise SystemExit(f"未实现的 cmap 类型: {cmap['type']}")
    return 0


def decode_glyph(font: dict, cp: int):
    """按 LVGL 的 PLAIN 4bpp 读法取一个字形(连续 nibble,字形起点按字节对齐)。"""
    gid = lookup_glyph_id(font, cp)
    if not gid or gid >= len(font["dsc"]):
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
    for cp in codepoints:
        want = glyphs[cp]
        got = decode_glyph(font, cp)
        if got is None:
            errors.append(f"U+{cp:04X} 查不到字形")
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
    log(f"  校验通过: {len(codepoints)} 个字形经 LVGL 语义查找后与栅格化结果逐像素一致")
    return 0


# ---------------------------------------------------------------- 自检
def check(out_dir: str, codepoints: list) -> int:
    ok = True
    for size in SIZES:
        path = os.path.join(out_dir, f"saya_cjk_{size}.c")
        if not os.path.exists(path):
            log(f"缺少字体文件: {path}")
            ok = False
            continue
        font = parse_font(path)
        missing = [cp for cp in codepoints if not lookup_glyph_id(font, cp)]
        blank = []
        for cp in codepoints:
            if cp <= 0x20:
                continue
            got = decode_glyph(font, cp)
            if got and not any(got[5]):
                blank.append(cp)
        types = "+".join(c["type"].replace("LV_FONT_FMT_TXT_CMAP_", "") for c in font["cmaps"])
        log(f"  {size}px: cmap {types}, {len(codepoints)} 码位缺失 {len(missing)}, "
            f"空字形 {len(blank)}, 行高 {font['line_height']}/基线 {font['base_line']}")
        if missing:
            log("    缺字: " + " ".join(f"U+{cp:04X}" for cp in missing[:10]))
            ok = False
        if blank:
            log("    空字形: " + " ".join(f"U+{cp:04X}" for cp in blank[:10]))
            ok = False
    if not ok:
        log("字体自检失败:重新运行生成命令(见本文件顶部说明)")
        return 1
    log("  字体覆盖/结构自检通过")
    return 0


# ---------------------------------------------------------------- 主流程
def generate(args) -> int:
    chars = required_chars(args.pack, args.source_root)
    codepoints = sorted(ord(c) for c in chars)
    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "saya_cjk_symbols.txt"), "w",
              encoding="utf-8", newline="\n") as fh:
        fh.write("# 由 tools/saya_font.py 生成:界面文案 + 剧本正文出现过的全部字符。\n")
        fh.write("# 重新生成字体时必须与本文件一致。\n")
        fh.write("".join(chr(cp) for cp in codepoints) + "\n")
    log(f"  字符清单 {len(codepoints)} 个")

    try:
        from fontTools.ttLib import TTFont  # type: ignore

        covered = set()
        for table in TTFont(args.font)["cmap"].tables:
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
        baseline = ascent - (natural - line_height) // 2
        base_line = line_height - baseline
        name = f"saya_cjk_{size}"
        text = emit_font(name, size, codepoints, glyphs, line_height, base_line)
        path = os.path.join(args.out_dir, name + ".c")
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        log(f"    {path} ({os.path.getsize(path) / 1024:.0f} KB 源码)")
        rc |= verify(path, codepoints, glyphs)
    rc |= check(args.out_dir, codepoints)
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--font", help="源字体 TTF/OTF(生成时必填)")
    ap.add_argument("--pack", default="main/saya_data/saya_pack.bin")
    ap.add_argument("--source-root", default="main")
    ap.add_argument("--out-dir", default="assets/fonts")
    ap.add_argument("--check", action="store_true", help="只做覆盖/结构自检,不生成")
    args = ap.parse_args()

    codepoints = sorted(ord(c) for c in required_chars(args.pack, args.source_root))
    if args.check:
        return check(args.out_dir, codepoints)
    if not args.font:
        ap.error("生成模式需要 --font")
    return generate(args)


if __name__ == "__main__":
    raise SystemExit(main())
