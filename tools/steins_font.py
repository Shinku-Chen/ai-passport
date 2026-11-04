#!/usr/bin/env python3
r"""生成《命运石之门》阅读器用的两张 LVGL 9 位图字体 + 排版度量表。

为什么要两张字体:
  * 剧本正文与说话人名在源快照里是 PUA 私有区字形编码(每个字一个 \uE1xx/\uF1xx
    码点),只有源快照自带的 sg-original.ttf 有对应字形。这些字必须用原字形渲染,
    否则就是把正文换成别的字体。-> steins_story_16
  * 章节名、日期、结局名、界面文案在源里是普通 Unicode 中文,原字形表里没有这些
    码位;按 OFL 许可用 Noto Sans SC 做一张很小的界面子集。-> steins_ui_16 / 20
  * 正文标签用 story 字体,它的 fallback 指向 ui 字体,所以一行里混排 PUA 与普通
    Unicode(例如「日期：08.07」)也能完整渲染。

另外输出 steins_text_metrics.c:每个码位在 16px 下的横向量度(1/16 px)。排版层
(不依赖 LVGL,可在宿主机测试)用它算折行,和 LVGL 实际取到的 adv_w 同源,避免
"算出来 19 字一行、渲染时溢出"。

用法:
  python tools/steins_font.py --pack assets/steins/steins_pack.bin \
      --story-font <sg-original.ttf> --ui-font <NotoSansSC-Regular.otf> \
      --out-dir assets/fonts
  python tools/steins_font.py --check --pack assets/steins/steins_pack.bin \
      --out-dir assets/fonts

生成物(assets/fonts/steins_*.c)进仓库;两张源字体都不入库,只在本地生成时使用。
"""

from __future__ import annotations

import argparse
import io
import os
import re
import struct
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gal_font  # noqa: E402  复用已在本仓库验证过的 LVGL 位图字体生成/回读逻辑

Image = ImageDraw = ImageFont = None

BPP = 4
STORY_EXTRA_LEADING = 3     # 16px 字号 -> 行高 19
UI_EXTRA_LEADING = 3

STORY_FAMILY = "steins_story"
UI_FAMILY = "steins_ui"
METRICS_FILE = "steins_text_metrics.c"
STORY_INVENTORY = "steins_story_symbols.txt"
UI_INVENTORY = "steins_ui_symbols.txt"


def log(msg):
    print(msg, file=sys.stderr)


def _require_pillow():
    global Image, ImageDraw, ImageFont
    if Image is not None:
        return
    from PIL import Image as _Image, ImageDraw as _ImageDraw, ImageFont as _ImageFont
    Image, ImageDraw, ImageFont = _Image, _ImageDraw, _ImageFont


# ---------------------------------------------------------------- 资源包读取

SEC = {
    "STRT": 1, "TXT": 2, "NODE": 3, "CHUNKREF": 4, "CHUNK": 5, "FRAME": 6,
    "VIEW": 7, "LAYER": 8, "ASSET": 9, "BLOB": 10, "CHOICE": 11, "CITEM": 12,
    "CHAPTER": 13, "GAL": 14, "GENTRY": 15, "WLINE": 16, "WCHANGE": 17,
    "BREAK": 18, "UI": 19, "META": 20,
}


def pack_strings(path):
    """读出包里全部字符串(PUA 正文、界面文案、章节名……),用于取字符集。"""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"STEINPK3":
        raise SystemExit("%s: 不是 STEINPK3 资源包" % path)
    version, total, count = struct.unpack_from("<III", data, 8)
    if total != len(data):
        raise SystemExit("%s: 头部长度 %d 与文件长度 %d 不符" % (path, total, len(data)))
    sections = {}
    off = 32
    for _ in range(count):
        typ, soff, scount, ssize = struct.unpack_from("<IIII", data, off)
        off += 16
        sections[typ] = (soff, scount, ssize)
    if SEC["STRT"] not in sections or SEC["TXT"] not in sections:
        raise SystemExit("%s: 缺少 STRT/TXT 段" % path)
    strt_off, strt_count, _ = sections[SEC["STRT"]]
    txt_off, _, txt_size = sections[SEC["TXT"]]
    if version >= 5:
        import zlib
        blk = sections.get(40)      # TXTBLK
        raw = sections.get(41)      # TXTRAW
        if not blk or not raw:
            raise SystemExit("%s: v5 包缺少 TXTBLK/TXTRAW 段" % path)
        blk_off, blk_count, _ = blk
        raw_len = struct.unpack_from("<I", data, raw[0])[0]
        out_txt = bytearray()
        for i in range(blk_count):
            coff, clen, crc = struct.unpack_from("<IHH", data, blk_off + i * 8)
            comp = data[txt_off + coff:txt_off + coff + clen]
            d = zlib.decompressobj(-15)
            chunk = d.decompress(comp)
            if not d.eof or d.unused_data:
                raise SystemExit("%s: 第 %d 块未完整结束" % (path, i))
            want = min(2048, raw_len - i * 2048)
            if len(chunk) != want:
                raise SystemExit("%s: 第 %d 块长度 %d != %d" % (path, i, len(chunk), want))
            if zlib.crc32(chunk) & 0xFFFF != crc and _crc16(chunk) != crc:
                raise SystemExit("%s: 第 %d 块校验不符" % (path, i))
            out_txt += chunk
        if len(out_txt) != raw_len:
            raise SystemExit("%s: TXT 解压长度不符" % path)
        txt = bytes(out_txt)
    else:
        txt = data[txt_off:txt_off + txt_size]
    out = []
    for i in range(strt_count):
        soff, slen = struct.unpack_from("<IH", data, strt_off + i * 6)
        out.append(txt[soff:soff + slen * 2].decode("utf-16-le"))
    return out


DEFAULT_UI_SOURCES = ("main/steins_app.c", "main/steins_ui.c",
                      "main/main.c", "main/steins_image.c")


def c_literals(path):
    """从 C 文件抽字符串字面量:跳过 // 与 /* */ 注释,处理转义与相邻拼接。"""
    src = io.open(path, encoding="utf-8").read()
    out = []
    i, n = 0, len(src)
    cur = None
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != chr(10):
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c == '"':
            i += 1
            buf = []
            while i < n and src[i] != '"':
                if src[i] == chr(92) and i + 1 < n:
                    nxt = src[i + 1]
                    simple = {"n": chr(10), "t": chr(9), "r": chr(13),
                              "0": chr(0), '"': '"', chr(92): chr(92)}
                    buf.append(simple.get(nxt, chr(92) + nxt))
                    i += 2
                    continue
                buf.append(src[i])
                i += 1
            i += 1
            piece = "".join(buf)
            cur = piece if cur is None else cur + piece
            continue
        if cur is not None:
            out.append(cur)
            cur = None
        i += 1
    if cur is not None:
        out.append(cur)
    return out


def ui_strings(sources):
    got = []
    for path in sources:
        if os.path.exists(path):
            got.extend(c_literals(path))
    return got


def _dir_sources(args):
    out = []
    for d in getattr(args, "ui_source_dir", []) or []:
        for name in sorted(os.listdir(d)):
            if name.endswith((".c", ".h")):
                out.append(os.path.join(d, name))
    return out


def _crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def inventory(pack_path, extra_sources=()):
    """生成与 --check 共用的完整字符集:pack 全部字符串 + 生产 UI 源字面量。"""
    strings = list(pack_strings(pack_path))
    sources = list(DEFAULT_UI_SOURCES) + list(extra_sources)
    strings.extend(ui_strings(sources))
    return strings, sources


def classify(strings):
    """PUA -> 原字形字体;其余 -> 界面字体。空白不参与。"""
    story = set()
    ui = set()
    for s in strings:
        for ch in s:
            cp = ord(ch)
            if cp in (0x09, 0x0A, 0x0D):
                continue
            if 0xE000 <= cp <= 0xF8FF:
                story.add(cp)
            else:
                ui.add(cp)
    return sorted(story), sorted(ui)


# ---------------------------------------------------------------- 生成

def emit_font_c(name, size, codepoints, glyphs, line_height, base_line, fallback, note):
    """在 gal_font.emit_font 的基础上改成可指定符号名/说明/fallback。"""
    cmaps = gal_font.build_cmaps(codepoints)
    out = io.StringIO()
    w = out.write
    w("/*******************************************************************************\n")
    w(" * %s —— %s\n" % (name, note))
    w(" * Size: %d px, Bpp: %d\n" % (size, BPP))
    w(" * 由 tools/steins_font.py 生成(复用 tools/gal_font.py 的生成器与回读校验);\n")
    w(" * 格式:4bpp 连续 nibble 打包,cmap = ASCII 段 TINY + 其余 SPARSE_TINY。\n")
    w(" * 不要手改本文件:重新运行 tools/steins_font.py 生成。\n")
    w(" *****************************************************************************/\n\n")
    w('#include "lvgl.h"\n\n')
    if fallback and fallback.startswith("&"):
        w("/* 同一字号的两张字体互相引用:story 字体缺字形时回落到界面字体。 */\n")
        w("extern const lv_font_t %s;\n\n" % fallback[1:])
    guard = name.upper()
    w("#ifndef %s\n#define %s 1\n\n" % (guard, guard))
    w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")
    index = 0
    dsc_lines = []
    for i, cp in enumerate(codepoints):
        adv_w, bw, bh, ofs_x, ofs_y, nibbles = glyphs[cp]
        gid = i + 1
        if bw == 0 or bh == 0:
            dsc_lines.append((gid, index, adv_w, 0, 0, ofs_x, ofs_y))
            continue
        data = gal_font.pack_nibbles(nibbles)
        dsc_lines.append((gid, index, adv_w, bw, bh, ofs_x, ofs_y))
        w("    /* U+%04X */\n" % cp)
        for j in range(0, len(data), 12):
            chunk = data[j:j + 12]
            w("    " + ", ".join("0x%02x" % b for b in chunk) + ",\n")
        index += len(data)
    w("};\n\n")

    w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
    w("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0,"
      " .ofs_y = 0} /* id = 0 reserved */,\n")
    for gid, bi, adv_w, bw, bh, ofs_x, ofs_y in dsc_lines:
        w("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d,"
          " .ofs_x = %d, .ofs_y = %d},\n" % (bi, adv_w, bw, bh, ofs_x, ofs_y))
    w("};\n\n")

    for idx, entry in enumerate(cmaps):
        if entry["offsets"] is None:
            continue
        w("static const uint16_t unicode_list_%d[] = {\n    " % idx)
        for i, off in enumerate(entry["offsets"]):
            w("0x%x, " % off)
            if (i + 1) % 8 == 0 and i + 1 < len(entry["offsets"]):
                w("\n    ")
        w("\n};\n\n")

    w("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
    for idx, entry in enumerate(cmaps):
        ulist = "unicode_list_%d" % idx if entry["offsets"] is not None else "NULL"
        w("    {\n")
        w("        .range_start = %d, .range_length = %d,\n"
          % (entry["range_start"], entry["range_length"]))
        w("        .glyph_id_start = %d,\n" % entry["glyph_id_start"])
        w("        .unicode_list = %s, .glyph_id_ofs_list = NULL,\n" % ulist)
        w("        .list_length = %d, .type = %s\n" % (entry["list_length"], entry["type"]))
        w("    },\n")
    w("};\n\n")

    w("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
    w("    .glyph_bitmap = glyph_bitmap,\n")
    w("    .glyph_dsc = glyph_dsc,\n")
    w("    .cmaps = cmaps,\n")
    w("    .kern_dsc = NULL,\n")
    w("    .kern_scale = 0,\n")
    w("    .cmap_num = %d,\n" % len(cmaps))
    w("    .bpp = %d,\n" % BPP)
    w("    .kern_classes = 0,\n")
    w("    .bitmap_format = 0,   /* LV_FONT_FMT_TXT_PLAIN */\n")
    w("};\n\n")

    w("const lv_font_t %s = {\n" % name)
    w("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
    w("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
    w("    .line_height = %d,\n" % line_height)
    w("    .base_line = %d,\n" % base_line)
    w("#if LV_VERSION_CHECK(9, 6, 0)\n")
    w("    .cap_height = %d,\n" % size)
    w("    .x_height = %d,\n" % (size * 9 // 16))
    w("#endif\n")
    w("    .subpx = LV_FONT_SUBPX_NONE,\n")
    w("    .underline_position = -1,\n")
    w("    .underline_thickness = 1,\n")
    w("#if LV_VERSION_CHECK(9, 3, 0)\n")
    w("    .static_bitmap = 1,\n")
    w("#endif\n")
    w("    .dsc = &font_dsc,\n")
    w("    .fallback = %s,\n" % (fallback if fallback else "NULL"))
    w("    .user_data = NULL,\n")
    w("};\n\n")
    w("#endif /* %s */\n" % guard)
    return out.getvalue()


def emit_metrics(codepoints, adv):
    out = io.StringIO()
    w = out.write
    w("/* 由 tools/steins_font.py 生成:全部码位在 16px 下的横向量度(1/16 px)。\n"
      " * 排版层用它折行,与 LVGL 的 adv_w 同源;不要手改。*/\n\n")
    w('#include "steins_text_metrics.h"\n\n')
    w("const steins_adv_t steins_adv_table[] = {\n")
    for cp in codepoints:
        w("    {0x%04X, %d},\n" % (cp, adv[cp]))
    w("};\n\n")
    w("const unsigned steins_adv_count = %d;\n" % len(codepoints))
    return out.getvalue()


def generate(args):
    strings, used_sources = inventory(args.pack, args.ui_source + _dir_sources(args))
    story_cps, ui_cps = classify(strings)
    print("inventory: %d 串(%d 个 UI 源文件), story %d / ui %d 码位" %
          (len(strings), len(used_sources), len(story_cps), len(ui_cps)))
    log("  包里 %d 条字串:原字形码位 %d 个,界面码位 %d 个"
        % (len(strings), len(story_cps), len(ui_cps)))
    os.makedirs(args.out_dir, exist_ok=True)

    with open(os.path.join(args.out_dir, STORY_INVENTORY), "w", encoding="utf-8",
              newline="\n") as fh:
        fh.write("# 由 tools/steins_font.py 生成:资源包里出现的 PUA 正文码位。\n")
        fh.write("".join(chr(cp) for cp in story_cps) + "\n")
    with open(os.path.join(args.out_dir, UI_INVENTORY), "w", encoding="utf-8",
              newline="\n") as fh:
        fh.write("# 由 tools/steins_font.py 生成:资源包里出现的普通 Unicode 码位。\n")
        fh.write("".join(chr(cp) for cp in ui_cps) + "\n")

    try:
        from fontTools.ttLib import TTFont  # type: ignore

        def covered(path):
            cov = set()
            for table in TTFont(path)["cmap"].tables:
                if table.isUnicode():
                    cov |= set(table.cmap.keys())
            return cov

        miss = [cp for cp in story_cps if cp not in covered(args.story_font)]
        log("  原字形字体缺 %d 个正文码位" % len(miss))
        if miss:
            log("    " + " ".join("U+%04X" % cp for cp in miss[:12]))
        missu = [cp for cp in ui_cps if cp not in covered(args.ui_font)]
        log("  界面字体缺 %d 个码位" % len(missu))
        if missu:
            log("    " + " ".join("U+%04X" % cp for cp in missu[:12]))
        if miss or missu:
            return 1
    except ImportError:
        log("  (未安装 fontTools,跳过源字体缺字诊断)")

    _require_pillow()
    adv = {}
    rc = 0
    jobs = [(STORY_FAMILY, args.sizes, args.story_font, story_cps, "原字形正文"),
            (UI_FAMILY, args.ui_sizes, args.ui_font, ui_cps, "界面文案")]
    for family, sizes, font_path, cps, note in jobs:
        for size in sizes:
            log("  生成 %s_%d ..." % (family, size))
            glyphs = gal_font.rasterize(font_path, size, cps)
            font = ImageFont.truetype(font_path, size,
                                      layout_engine=ImageFont.Layout.BASIC)
            ascent, _descent = font.getmetrics()
            natural = ascent + _descent
            extra = STORY_EXTRA_LEADING if family == STORY_FAMILY else UI_EXTRA_LEADING
            line_height = size + extra
            baseline = ascent - (natural - line_height) // 2
            base_line = line_height - baseline
            fallback = "&steins_ui_%d" % size if family == STORY_FAMILY else None
            name = "%s_%d" % (family, size)
            text = emit_font_c(name, size, cps, glyphs, line_height, base_line,
                               fallback, note)
            path = os.path.join(args.out_dir, name + ".c")
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
            log("    %s (%d KB 源码)" % (path, os.path.getsize(path) // 1024))
            rc |= gal_font.verify(path, cps, glyphs)
            if size == 16:
                for cp in cps:
                    adv[cp] = glyphs[cp][0]

    # 度量表:18px 的界面字形也给正文用不上的码位补上,统一按 16px 度量
    for cp in ui_cps:
        if cp not in adv:
            adv[cp] = 0
    merged = sorted(set(story_cps) | set(ui_cps))
    path = os.path.join(args.out_dir, METRICS_FILE)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(emit_metrics(merged, adv))
    log("    %s (%d KB 源码)" % (path, os.path.getsize(path) // 1024))
    rc |= check(args.out_dir, story_cps, ui_cps, args.sizes, args.ui_sizes)
    return rc


# ---------------------------------------------------------------- 自检

def check(out_dir, story_cps=None, ui_cps=None, story_sizes=(16,), ui_sizes=(16, 18)):
    ok = True
    if story_cps is not None:
        for size in story_sizes:
            path = os.path.join(out_dir, "%s_%d.c" % (STORY_FAMILY, size))
            if not os.path.exists(path):
                log("  缺少 %s" % path)
                ok = False
                continue
            font = gal_font.parse_font(path)
            missing = [cp for cp in story_cps if not gal_font.lookup_glyph_id(font, cp)]
            log("  %s_%d: %d 码位,缺字 %d,行高 %d/基线 %d"
                % (STORY_FAMILY, size, len(story_cps), len(missing),
                   font["line_height"], font["base_line"]))
            if missing:
                log("    缺字: " + " ".join("U+%04X" % cp for cp in missing[:10]))
                ok = False
        for size in ui_sizes:
            path = os.path.join(out_dir, "%s_%d.c" % (UI_FAMILY, size))
            if not os.path.exists(path):
                log("  缺少 %s" % path)
                ok = False
                continue
            font = gal_font.parse_font(path)
            missing = [cp for cp in ui_cps if not gal_font.lookup_glyph_id(font, cp)]
            blank = []
            for cp in ui_cps:
                if cp <= 0x20 or unicodedata.category(chr(cp)).startswith("Z"):
                    continue
                got = gal_font.decode_glyph(font, cp)
                if got and got[1] and not any(got[5]):
                    blank.append(cp)
            log("  %s_%d: %d 码位,缺字 %d,空字形 %d,行高 %d/基线 %d"
                % (UI_FAMILY, size, len(ui_cps), len(missing), len(blank),
                   font["line_height"], font["base_line"]))
            if missing:
                log("    缺字: " + " ".join("U+%04X" % cp for cp in missing[:10]))
                ok = False
            if blank:
                log("    空字形: " + " ".join("U+%04X" % cp for cp in blank[:10]))
                ok = False
    # 度量表覆盖自检
    path = os.path.join(out_dir, METRICS_FILE)
    if not os.path.exists(path):
        log("  缺少 %s" % path)
        return 1
    text = open(path, "r", encoding="utf-8").read()
    table = {int(a, 16): int(b) for a, b in
             re.findall(r"\{0x([0-9a-fA-F]+), (\d+)\}", text)}
    if story_cps is not None:
        missing = [cp for cp in list(story_cps) + list(ui_cps) if cp not in table]
        log("  度量表: %d 条,缺 %d" % (len(table), len(missing)))
        if missing:
            log("    缺: " + " ".join("U+%04X" % cp for cp in missing[:10]))
            ok = False
    if not ok:
        log("字体自检失败")
        return 1
    log("  字体覆盖/结构自检通过")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", default="assets/steins/steins_pack.bin")
    ap.add_argument("--story-font", help="源快照自带的 sg-original.ttf")
    ap.add_argument("--ui-font", help="界面用中文字体(Noto Sans SC,OFL)")
    ap.add_argument("--out-dir", default="assets/fonts")
    ap.add_argument("--sizes", default="16")
    ap.add_argument("--ui-sizes", default="16,18")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--ui-source", action="append", default=[],
                    help="额外 UI 源 C 文件(可多次);默认含 main/steins_app.c 等")
    ap.add_argument("--ui-source-dir", action="append", default=[])
    args = ap.parse_args()
    args.sizes = [int(x) for x in args.sizes.split(",") if x]
    args.ui_sizes = [int(x) for x in args.ui_sizes.split(",") if x]

    if args.check:
        story_cps, ui_cps = classify(inventory(args.pack, args.ui_source + _dir_sources(args))[0])
        return check(args.out_dir, story_cps, ui_cps, args.sizes, args.ui_sizes)
    if not args.story_font or not args.ui_font:
        ap.error("生成模式需要 --story-font 与 --ui-font")
    return generate(args)


if __name__ == "__main__":
    raise SystemExit(main())
