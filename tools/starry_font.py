#!/usr/bin/env python3
"""生成 AI Passport《星空列车与白的旅行》阅读器的中文字形包(自研二进制格式)。

为什么不用 lv_font_conv:这个工具最后一版是 1.5.3(2021),在 Node v24 下写出的
字形位图是坏的 —— 同一个 OTF、同一套参数,产出的字节按 LVGL 的 PLAIN 4bpp 读法
解码全是噪点,设备上表现为"文字全部乱码"(见 feature/saya-no-uta 的排查记录)。
这里用 Pillow(FreeType)栅格化 + 自己写格式,不依赖 Node;生成后会回读自己写出的
文件,与栅格化结果逐像素比对。

本应用不经过 LVGL 绘制文字(见 main/starry_render.c 的逐行合成器),所以格式按
"逐行取字形位图"设计,不套用 LVGL 的 cmap/bitmap 语义:

  header 40B : magic "SSRFONT1" | version | total | px | line_height | ascent
               | glyph_count | bitmap_size | reserved
  cmap       : glyph_count × { cp u32, index u16, pad u16 },按码位升序,二分查找
  glyph dsc  : glyph_count × { bitmap_off u32, w u8, h u8, ofs_x i8, ofs_y i8,
                               advance u8, pad u16 } = 12 字节
  bitmap     : 每字形 h 行 × ceil(w/2) 字节,行内 4bpp,高半字节在左

advance 只是记录值:排版与绘制都用"半角单位 × px/2"(与 main/starry_model.c 的
starry_char_units() 同一张宽度表),两边不会各算一套。

字符集来自两处,保证界面文字与剧本正文都不缺字:
  1. 资源包(main/starry_data/starry_pack.bin)里的全部剧本文字与名字;
  2. main/ 下所有 .c/.h 里出现的非 ASCII 字面量(界面文案)。

用法:
  python tools/starry_font.py --font <NotoSansSC-Regular.otf> \
      --pack main/starry_data/starry_pack.bin --out-dir assets/fonts   # 生成 + 自检
  python tools/starry_font.py --check --pack main/starry_data/starry_pack.bin \
      --out-dir assets/fonts                                            # 只做覆盖自检

生成物(assets/fonts/starry_font16.bin、starry_font20.bin、starry_symbols.txt)
提交进仓库;源字体不上传。
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - tool dependency
    sys.exit("需要 Pillow: python -m pip install pillow")

MAGIC = b"SSRFONT1"
VERSION = 1
# header: magic(8) + version(4) + total(4) + px(4) + line_height(4) + ascent(4)
#         + glyph_count(4) + bitmap_size(4) + reserved(4) = 40 字节
HEADER_SIZE = 40
BPP = 4
# 字号 -> 行高。16px 用 20px 行、20px 用 25px 行:文本框固定 3 行(见 starry_render.h)。
SIZES = ((16, 20), (20, 25))
CELL_MARGIN = 1          # 单元格左右各留 1px,避免相邻字贴着
PAD = 6                  # 栅格化画布留白


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def char_units(cp: int) -> int:
    """宽度单位:全角 2、半角 1。必须与 main/starry_model.c 的表一致。"""
    if cp == 0x00B7:      # 间隔号 ·:中文语境下按全角排
        return 2
    if cp < 0x1100:
        return 1
    wide = (
        (0x1100, 0x115F), (0x2E80, 0x303E), (0x3041, 0x33FF), (0x3400, 0x4DBF),
        (0x4E00, 0x9FFF), (0xA000, 0xA4CF), (0xAC00, 0xD7A3), (0xF900, 0xFAFF),
        (0xFE30, 0xFE6F), (0xFF00, 0xFF60), (0xFFE0, 0xFFE6), (0x20000, 0x3FFFD),
        # East Asian Width 的 "Ambiguous" 段:中文语境下按全角排(与 main/starry_model.c 同步)。
        (0x2010, 0x2027), (0x2030, 0x205E), (0x2100, 0x21FF), (0x2600, 0x27BF),
    )
    return 2 if any(lo <= cp <= hi for lo, hi in wide) else 1


# ---------------------------------------------------------------- 字符收集
def pack_chars(pack_path: str) -> set:
    blob = open(pack_path, "rb").read()
    if blob[:8] != b"SSRPK001":
        raise SystemExit(f"资源包魔数不对: {pack_path}")
    version, total, section_count = struct.unpack_from("<III", blob, 8)
    if version != 2 or total != len(blob):
        raise SystemExit("资源包版本/长度不匹配,先重新生成")
    sections = {}
    for i in range(section_count):
        sec_type, off, _count, size = struct.unpack_from("<IIII", blob, 20 + 16 * i)
        sections[sec_type] = (off, size)
    if 0 not in sections:
        raise SystemExit("资源包缺少文本段")
    text_off, text_size = sections[0]
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
    chars |= {chr(c) for c in range(0x20, 0x7F)}
    return {c for c in chars if c not in "\r\n\t"}


def raw_text_chars(pack_path: str, source_root: str) -> set:
    """运行时会显示的全部字符,不做任何过滤(用于“会不会出方块”的检查)。

    与 required_chars 的区别:这里不过滤 U+3000 之类“生成时会被丢掉”的字符,
    所以在字体里缺了任何一个都会被抓出来 —— 缺字在屏幕上就是方块。
    """
    chars = pack_chars(pack_path) | source_chars(source_root)
    return {c for c in chars if ord(c) >= 0x20}


# ---------------------------------------------------------------- 栅格化
def rasterize(otf_path: str, size: int, line_height: int, codepoints: list) -> tuple:
    """返回 (ascent, glyphs);glyphs[cp] = (w, h, ofs_x, ofs_y, advance, nibbles)。

    ofs_x / ofs_y 是字形位图相对"单元格左上角"的偏移;单元格宽度 = 单位 × px/2。
    基线放在 ascent 处,选值保证所有字形的墨迹都落在行框内。
    """
    font = ImageFont.truetype(otf_path, size, layout_engine=ImageFont.Layout.BASIC)
    natural_ascent, natural_descent = font.getmetrics()
    span = size * 4

    raw = {}
    min_top = 0
    max_bottom = 0
    for cp in codepoints:
        ch = chr(cp)
        canvas = Image.new("L", (span, span), 0)
        ImageDraw.Draw(canvas).text((PAD, PAD), ch, font=font, fill=255)
        bbox = canvas.getbbox()
        if bbox is None:
            raw[cp] = None
            continue
        x0, y0, x1, y1 = bbox
        ink = canvas.crop(bbox)
        px = ink.load()
        nibbles = []
        for y in range(ink.height):
            for x in range(ink.width):
                nibbles.append(min(15, (px[x, y] * 15 + 127) // 255))
        # 相对基线的墨迹范围(负 = 基线上方)
        top = (y0 - PAD) - natural_ascent
        bottom = (y1 - PAD) - natural_ascent
        min_top = min(min_top, top)
        max_bottom = max(max_bottom, bottom)
        raw[cp] = (x0 - PAD, top, ink.width, ink.height, nibbles)

    ascent = -min_top
    if ascent + max_bottom > line_height:
        raise SystemExit(
            f"{size}px 行高 {line_height} 放不下字形(需要 {ascent + max_bottom}px),"
            "请调大 line_height 或字号")
    if ascent > line_height:
        raise SystemExit(f"{size}px 基线 {ascent} 超出行高 {line_height}")

    half = max(1, size // 2)
    glyphs = {}
    full = ImageFont.truetype(otf_path, size, layout_engine=ImageFont.Layout.BASIC)
    for cp in codepoints:
        unit_w = half * char_units(cp)
        item = raw[cp]
        # advance 用源字体的自然宽度(四舍五入到 px):ASCII 的 W/M 比半角格子宽,
        # 按格子宽度画会互相重叠,所以设备端按 advance 推进(见 main/starry_gfx.c)。
        advance = max(1, min(255, int(round(full.getlength(chr(cp))))))
        if item is None:
            glyphs[cp] = (0, 0, 0, 0, advance, [])
            continue
        natural_x, top, w, h, nibbles = item
        if char_units(cp) == 2:
            # 全角字居中放进单元格:源字体的边距不必可信,统一居中更整齐。
            ofs_x = (unit_w - w) // 2
        else:
            ofs_x = natural_x
        ofs_y = ascent + top
        glyphs[cp] = (w, h, ofs_x, ofs_y, advance, nibbles)
    log(f"  {size}px: 基线 {ascent}(自然 {natural_ascent})+ 下行 {max_bottom},"
        f"行高 {line_height},字形 {len(glyphs)} 个")
    return ascent, glyphs


def pack_bitmap(w: int, h: int, nibbles: list) -> bytes:
    """4bpp 逐行打包,行内高半字节在左,行间不跨字节(读起来简单)。"""
    stride = (w + 1) // 2
    out = bytearray()
    for y in range(h):
        row = bytearray(stride)
        for x in range(w):
            v = nibbles[y * w + x] & 0xF
            if x % 2 == 0:
                row[x // 2] |= v << 4
            else:
                row[x // 2] |= v
        out += row
    return bytes(out)


def build_pack(size: int, line_height: int, ascent: int, glyphs: dict) -> bytes:
    codepoints = sorted(glyphs)
    cmap = bytearray()
    dscs = bytearray()
    bitmap = bytearray()
    for index, cp in enumerate(codepoints):
        cmap += struct.pack("<IHH", cp, index, 0)
        w, h, ofs_x, ofs_y, advance, nibbles = glyphs[cp]
        off = len(bitmap)
        if w and h:
            bitmap += pack_bitmap(w, h, nibbles)
        dscs += struct.pack("<IBBbbBBH", off, w, h, ofs_x, ofs_y, advance, 0, 0)
    head = bytearray(MAGIC)
    total = HEADER_SIZE + len(cmap) + len(dscs) + len(bitmap)
    head += struct.pack("<IIIIiIII", VERSION, total, size, line_height, ascent,
                        len(codepoints), len(bitmap), 0)
    assert len(head) == HEADER_SIZE, len(head)
    blob = bytes(head) + bytes(cmap) + bytes(dscs) + bytes(bitmap)
    assert len(blob) == total, (len(blob), total)
    return blob


def parse_pack(path: str) -> dict:
    blob = open(path, "rb").read()
    if blob[:8] != MAGIC:
        raise SystemExit(f"字形包魔数不对: {path}")
    version, total, size, line_height, ascent, count, bitmap_size, _ = struct.unpack_from(
        "<IIIIiIII", blob, 8)
    if version != VERSION or total != len(blob):
        raise SystemExit(f"字形包版本/长度不匹配: {path}")
    cmap_at = HEADER_SIZE
    dsc_at = cmap_at + count * 8
    bitmap_at = dsc_at + count * 12
    glyphs = {}
    for i in range(count):
        cp, index, _pad = struct.unpack_from("<IHH", blob, cmap_at + i * 8)
        off, w, h, ofs_x, ofs_y, advance, _pad, _pad2 = struct.unpack_from(
            "<IBBbbBBH", blob, dsc_at + index * 12)
        glyphs[cp] = (off, w, h, ofs_x, ofs_y, advance)
    return {"size": size, "line_height": line_height, "ascent": ascent,
            "count": count, "bitmap_size": bitmap_size, "glyphs": glyphs, "blob": blob,
            "bitmap_at": bitmap_at}


def verify_pack(path: str, size: int, line_height: int, glyphs: dict) -> None:
    """回读自己写出的文件,与栅格化结果逐像素比对。"""
    pack = parse_pack(path)
    for key, want in (("size", size), ("line_height", line_height), ("count", len(glyphs))):
        if pack[key] != want:
            raise SystemExit(f"{path}: {key} 不符 {pack[key]} != {want}")
    if sorted(pack["glyphs"]) != sorted(glyphs):
        raise SystemExit(f"{path}: 码位集合不符")
    for cp, (w, h, ofs_x, ofs_y, advance, nibbles) in glyphs.items():
        off, gw, gh, gx, gy, gadv = pack["glyphs"][cp]
        if (gw, gh, gx, gy, gadv) != (w, h, ofs_x, ofs_y, advance):
            raise SystemExit(f"{path}: U+{cp:04X} 度量不符")
        if not w or not h:
            continue
        expect = pack_bitmap(w, h, nibbles)
        got = pack["blob"][pack["bitmap_at"] + off : pack["bitmap_at"] + off + len(expect)]
        if got != expect:
            raise SystemExit(f"{path}: U+{cp:04X} 位图不符")
    log(f"  自检通过: {path}")


def check_coverage(pack_path: str, out_dir: str) -> int:
    """确认剧本/界面用到的每个字符都在生成好的字形包里。"""
    wanted = required_chars(pack_path, "main")
    covered = set()
    for size, _line_height in SIZES:
        path = os.path.join(out_dir, f"starry_font{size}.bin")
        if not os.path.exists(path):
            raise SystemExit(f"缺少字形包: {path}")
        pack = parse_pack(path)
        covered |= {chr(cp) for cp in pack["glyphs"]}
    missing = sorted(c for c in wanted if c not in covered)
    if missing:
        raise SystemExit("有 %d 个字符缺字形: %s" % (
            len(missing), " ".join(f"U+{ord(c):04X}" for c in missing[:40])))
    log(f"  覆盖自检通过: {len(wanted)} 个字符全部有字形")
    return len(wanted)


def write_symbols(path: str, chars: set) -> None:
    body = "".join(sorted(chars))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# 由 tools/starry_font.py 生成:界面文案 + 剧本正文出现过的全部字符。\n")
        fh.write("# 重新生成字体时必须与本文件一致。\n")
        for i in range(0, len(body), 120):
            fh.write(body[i:i + 120] + "\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="源字体 TTF/OTF(生成时必填)")
    ap.add_argument("--pack", default="main/starry_data/starry_pack.bin")
    ap.add_argument("--source-root", default="main")
    ap.add_argument("--out-dir", default="assets/fonts")
    ap.add_argument("--check", action="store_true", help="只做覆盖自检,不生成")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    if args.check:
        check_coverage(args.pack, args.out_dir)
        return 0

    if not args.font:
        raise SystemExit("生成时需要 --font <NotoSansSC-Regular.otf>")
    chars = required_chars(args.pack, args.source_root)
    codepoints = sorted(ord(c) for c in chars)
    log(f"字符集 {len(codepoints)} 个码位")

    for size, line_height in SIZES:
        ascent, glyphs = rasterize(args.font, size, line_height, codepoints)
        blob = build_pack(size, line_height, ascent, glyphs)
        path = os.path.join(args.out_dir, f"starry_font{size}.bin")
        with open(path, "wb") as fh:
            fh.write(blob)
        verify_pack(path, size, line_height, glyphs)
        log(f"  写出 {path}: {len(blob)} 字节({len(blob)/1024:.0f} KB)")

    write_symbols(os.path.join(args.out_dir, "starry_symbols.txt"), chars)
    check_coverage(args.pack, args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
