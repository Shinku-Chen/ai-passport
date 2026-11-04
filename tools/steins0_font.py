#!/usr/bin/env python3
"""Generate the Steins;Gate 0 reader's 16 px font (LVGL 9 bitmap font).

The story text is stored in the source's private-use encoding
(U+E000 + SC3 glyph id).  This generator keeps that encoding and draws each
PUA code point from the licensed-for-local-testing game font `sg-original.ttf`;
UI labels, ASCII and the Unicode punctuation aliases come from a normal CJK
font (Noto Sans SC by default).  Widths for the PUA Latin/punctuation glyphs
follow the converted watch port's `glyph-widths.js`, so the C layout engine and
the font advances agree exactly.

Why not lv_font_conv: the last release (1.5.3, 2021) emits corrupt 4bpp
bitmaps under the Node version used here.  This tool rasterizes with Pillow,
writes the LVGL `fmt_txt` structures itself, and then re-reads the generated C
file using LVGL's cmap semantics and compares every glyph pixel by pixel.

Usage:
  python tools/steins0_font.py --pack assets/steins0/steins0_pack.bin \
      --pua-font <steins0-rw5>/src/common/sg-original.ttf \
      --ui-font /path/to/NotoSansSC-Regular.otf \
      --widths <steins0-rw5>/src/common/glyph-widths.js \
      --out-dir assets/fonts
  python tools/steins0_font.py --pack ... --check --out-dir assets/fonts

The generated `steins0_font16.c` and `steins0_symbols.txt` are committed.  The
source fonts are not.
"""

from __future__ import annotations

import argparse
import ast
import io
import json
import os
import re
import struct
import sys
import unicodedata
import zlib

SIZE = 16
BPP = 4
EXTRA_LEADING = 3          # 16 px glyphs on a 19 px line
FONT_NAME = "steins0_font16"
INVENTORY = "steins0_symbols.txt"
CMAP_TINY = "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY"
CMAP_SPARSE = "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY"
PUA_BASE = 0xE000
PUA_END = 0xF8FF

Image = ImageDraw = ImageFont = None


def require_pillow():
    global Image, ImageDraw, ImageFont
    if Image is not None:
        return
    try:
        from PIL import Image as _I, ImageDraw as _D, ImageFont as _F
    except ImportError:  # pragma: no cover
        sys.exit("生成字体需要 Pillow: python -m pip install pillow")
    Image, ImageDraw, ImageFont = _I, _D, _F


def log(msg):
    print(msg, file=sys.stderr)


# --------------------------------------------------------------- char sources
def source_chars(root: str) -> set[str]:
    chars: set[str] = set()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in
                       (".git", "build", "managed_components", "assets")]
        for name in filenames:
            if not name.endswith((".c", ".h", ".cpp", ".hpp")):
                continue
            try:
                data = open(os.path.join(dirpath, name), "rb").read().decode("utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            for literal in re.findall(r'"(?:[^"\\]|\\.)*"', data):
                chars |= {c for c in literal if ord(c) > 0x7F}
    return chars


def pack_chars(pack_path: str) -> set[str]:
    """Read every displayed code point from the per-chunk zlib text blobs."""
    blob = open(pack_path, "rb").read()
    if blob[:8] != b"SG0PK001":
        raise SystemExit(f"{pack_path}: bad magic")
    version, header_size, section_count = struct.unpack_from("<HHH", blob, 8)
    total = struct.unpack_from("<I", blob, 16)[0]
    if version != 1 or total != len(blob):
        raise SystemExit(f"{pack_path}: bad header")
    sections = {}
    for i in range(section_count):
        sec_type, off, count, size = struct.unpack_from("<IIII", blob, 20 + i * 16)
        sections[sec_type] = (off, count, size)
    text_chars: set[str] = set()
    chunk_off, chunk_count, _ = sections[3]
    frame_bytes = 12
    for i in range(chunk_count):
        zoff, zlen, raw_len, frames = struct.unpack_from("<IIHB", blob, chunk_off + i * 12)
        base = sections[17][0]
        raw = zlib.decompress(blob[base + zoff: base + zoff + zlen], bufsize=raw_len)
        text_off = frame_bytes * frames
        for f in range(frames):
            off, length = struct.unpack_from("<HH", raw, f * frame_bytes)
            text_chars |= set(raw[text_off + off: text_off + off + length].decode("utf-8"))
    # strings table (names, labels, choices, ...)
    strt_off, strt_count, _ = sections[1]
    text_off, _text_count, _text_size = sections[0]
    for i in range(strt_count):
        off, length = struct.unpack_from("<IH", blob, strt_off + i * 8)
        text_chars |= set(blob[text_off + off: text_off + off + length].decode("utf-8"))
    return {c for c in text_chars if c not in "\r\n\t"}


def inventory_chars(out_dir: str) -> set[str]:
    path = os.path.join(out_dir, INVENTORY)
    if not os.path.exists(path):
        return set()
    chars = set()
    for line in open(path, encoding="utf-8"):
        if line.startswith("#"):
            continue
        chars |= {c for c in line.rstrip("\n") if c not in "\r\n\t"}
    return chars


def required_chars(pack: str, root: str, out_dir: str) -> set[str]:
    chars = source_chars(root) | pack_chars(pack) | inventory_chars(out_dir)
    chars |= {chr(cp) for cp in range(0x20, 0x7F)}
    return {c for c in chars if c not in "\r\n\t"}


# ------------------------------------------------------------------- widths
def parse_js_object(text: str, name: str) -> dict:
    marker = f"export const {name}="
    start = text.index(marker) + len(marker)
    end = text.index("};", start) + 1
    return json.loads(text[start:end])


def width_table(path: str):
    text = open(path, encoding="utf-8").read()
    return (parse_js_object(text, "glyphWidths"),
            parse_js_object(text, "asciiWidths"),
            parse_js_object(text, "unicodeWidths"))


def advance_width(cp: int, widths) -> int:
    """Advance in 1/16 px, matching main/steins0_text.c."""
    glyph, ascii_w, unicode_w = widths
    if PUA_BASE <= cp <= PUA_END:
        frac = glyph.get(str(cp - PUA_BASE), 1.0)
        return max(0, int(round(frac * 16 * 16)))
    ch = chr(cp)
    if cp < 0x80:
        frac = ascii_w.get(ch, 0.6)
        return max(0, int(round(frac * 16 * 16)))
    frac = unicode_w.get(ch, 1.0)
    return max(0, int(round(frac * 16 * 16)))


# ---------------------------------------------------------------- rasterize
def rasterize(pua_font: str, ui_font: str, codepoints: list[int], widths) -> dict:
    require_pillow()
    pua = ImageFont.truetype(pua_font, SIZE, layout_engine=ImageFont.Layout.BASIC)
    ui = ImageFont.truetype(ui_font, SIZE, layout_engine=ImageFont.Layout.BASIC)
    ui_ascent, ui_descent = ui.getmetrics()
    natural = ui_ascent + ui_descent
    line_height = SIZE + EXTRA_LEADING
    baseline_from_top = ui_ascent - (natural - line_height) // 2
    base_line = line_height - baseline_from_top
    pad = 4
    span = SIZE * 2 + pad * 2
    glyphs = {}
    for cp in codepoints:
        ch = chr(cp)
        font = pua if PUA_BASE <= cp <= PUA_END else ui
        ascent, _descent = font.getmetrics()
        adv_w = advance_width(cp, widths)
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
                nibbles.append(min(15, (px[x, y] * 15 + 127) // 255))
        glyphs[cp] = (adv_w, bw, bh, x0 - pad, ascent - (y1 - pad), nibbles)
    return glyphs, line_height, base_line


def emit_widths_header(path: str, widths) -> None:
    glyph, ascii_w, _unicode_w = widths
    pua = [advance_width(PUA_BASE + i, widths) for i in range(232)]
    ascii_adv = [advance_width(cp, widths) for cp in range(32, 127)]

    def fmt(values):
        return chr(10).join("    " + ", ".join(str(v) for v in values[i:i + 12]) + ","
                             for i in range(0, len(values), 12))

    text = ("// main/steins0_widths.h —— 字形推进宽度(单位 1/16 px),由 tools/steins0_font.py 生成。" + chr(10) +
            "// 数据来自转换源的 src/common/glyph-widths.js;未列出的码位按全角 256 处理。" + chr(10) +
            "#pragma once" + chr(10) + chr(10) + "#include <stdint.h>" + chr(10) + chr(10) +
            "#define SG0_ADV_FULL 256u" + chr(10) + "#define SG0_ADV_HALF 128u" + chr(10) + chr(10) +
            "static const uint16_t sg0_pua_adv[232] = {" + chr(10) + fmt(pua) + chr(10) + "};" +
            chr(10) + chr(10) + "static const uint16_t sg0_ascii_adv[95] = {" + chr(10) +
            fmt(ascii_adv) + chr(10) + "};" + chr(10))
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline=chr(10)) as fh:
        fh.write(text)


def pack_nibbles(nibbles: list[int]) -> bytes:
    out = bytearray()
    for i in range(0, len(nibbles), 2):
        hi = nibbles[i] & 0xF
        lo = nibbles[i + 1] & 0xF if i + 1 < len(nibbles) else 0
        out.append((hi << 4) | lo)
    return bytes(out)


def build_cmaps(codepoints: list[int]):
    ascii_cps = list(range(0x20, 0x7F))
    entries = []
    gid = 1
    if all(cp in codepoints for cp in ascii_cps):
        entries.append({"range_start": 0x20, "range_length": len(ascii_cps),
                        "glyph_id_start": gid, "offsets": None,
                        "list_length": 0, "type": CMAP_TINY})
        gid += len(ascii_cps)
        rest = [cp for cp in codepoints if cp > 0x7E]
    else:
        rest = list(codepoints)
    if rest:
        span = rest[-1] - rest[0] + 1
        if rest[-1] - rest[0] > 0xFFFF:
            raise SystemExit("稀疏码位跨度超过 uint16")
        entries.append({"range_start": rest[0], "range_length": span,
                        "glyph_id_start": gid,
                        "offsets": [cp - rest[0] for cp in rest],
                        "list_length": len(rest), "type": CMAP_SPARSE})
    return entries


def emit_font(codepoints, glyphs, line_height, base_line) -> str:
    cmaps = build_cmaps(codepoints)
    out = io.StringIO()
    w = out.write
    w("/*******************************************************************************\n")
    w(f" * Size: {SIZE} px  Bpp: {BPP}\n")
    w(" * Generated by tools/steins0_font.py (Pillow rasterizer, no lv_font_conv).\n")
    w(" * PUA U+E000..U+F8FF comes from sg-original.ttf (source game font, local test);\n")
    w(" * ASCII/UI glyphs come from the configured CJK font. Widths follow the source\n")
    w(" * glyph-widths.js. Generated file; do not edit by hand.\n")
    w(" *****************************************************************************/\n\n")
    w('#include "lvgl.h"\n\n')
    w(f"#ifndef {FONT_NAME.upper()}\n#define {FONT_NAME.upper()} 1\n\n")
    w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")
    index = 0
    dsc = []
    for i, cp in enumerate(codepoints):
        adv_w, bw, bh, ofs_x, ofs_y, nibbles = glyphs[cp]
        gid = i + 1
        if bw == 0 or bh == 0:
            dsc.append((gid, index, adv_w, 0, 0, ofs_x, ofs_y))
            continue
        data = pack_nibbles(nibbles)
        dsc.append((gid, index, adv_w, bw, bh, ofs_x, ofs_y))
        w(f"    /* U+{cp:04X} */\n")
        for j in range(0, len(data), 12):
            w("    " + ", ".join(f"0x{b:02x}" for b in data[j:j + 12]) + ",\n")
        index += len(data)
    w("};\n\n")
    w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
    w("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},\n")
    for gid, bi, adv_w, bw, bh, ofs_x, ofs_y in dsc:
        w(f"    {{.bitmap_index = {bi}, .adv_w = {adv_w}, .box_w = {bw}, .box_h = {bh},"
          f" .ofs_x = {ofs_x}, .ofs_y = {ofs_y}}},\n")
    w("};\n\n")
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
    w("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
    w("    .glyph_bitmap = glyph_bitmap,\n")
    w("    .glyph_dsc = glyph_dsc,\n")
    w("    .cmaps = cmaps,\n")
    w("    .kern_dsc = NULL,\n")
    w("    .kern_scale = 0,\n")
    w(f"    .cmap_num = {len(cmaps)},\n")
    w(f"    .bpp = {BPP},\n")
    w("    .kern_classes = 0,\n")
    w("    .bitmap_format = 0,\n")
    w("};\n\n")
    w(f"const lv_font_t {FONT_NAME} = {{\n")
    w("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
    w("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
    w(f"    .line_height = {line_height},\n")
    w(f"    .base_line = {base_line},\n")
    w("#if LV_VERSION_CHECK(9, 6, 0)\n")
    w(f"    .cap_height = {SIZE},\n")
    w(f"    .x_height = {SIZE * 9 // 16},\n")
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
    w(f"#endif /* {FONT_NAME.upper()} */\n")
    return out.getvalue()


# ------------------------------------------------------------------ readback
def parse_font(path: str) -> dict:
    text = open(path, encoding="utf-8").read()
    bitmap_text = re.search(r"const uint8_t glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not bitmap_text:
        raise SystemExit(f"{path}: glyph_bitmap missing")
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", bitmap_text.group(1)))
    dsc = [tuple(int(x) for x in m) for m in re.findall(
        r"\{\s*\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),"
        r"\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\s*\}",
        text)]
    ulists = {name: [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)]
              for name, body in re.findall(
                  r"const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};", text, re.S)}
    cmap_body = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};",
                          text, re.S)
    if not cmap_body:
        raise SystemExit(f"{path}: cmaps missing")
    cmaps = []
    for entry in re.findall(r"\{(.*?)\}", cmap_body.group(1), re.S):
        get = lambda key: re.search(rf"\.{key}\s*=\s*([\w\d]+)", entry)
        cmaps.append({
            "range_start": int(get("range_start").group(1)),
            "range_length": int(get("range_length").group(1)),
            "glyph_id_start": int(get("glyph_id_start").group(1)),
            "unicode_list": ulists.get(get("unicode_list").group(1)),
            "list_length": int(get("list_length").group(1)),
            "type": get("type").group(1),
        })
    line_height = int(re.search(r"\.line_height\s*=\s*(\d+)", text).group(1))
    base_line = int(re.search(r"\.base_line\s*=\s*(\d+)", text).group(1))
    return {"bitmap": data, "dsc": dsc, "cmaps": cmaps, "line_height": line_height,
            "base_line": base_line}


def lookup_glyph_id(font, cp: int) -> int:
    for cmap in font["cmaps"]:
        rcp = cp - cmap["range_start"]
        if rcp < 0 or rcp >= cmap["range_length"]:
            continue
        if cmap["type"] == CMAP_TINY:
            return cmap["glyph_id_start"] + rcp
        if cmap["type"] == CMAP_SPARSE:
            if cmap["unicode_list"] and rcp in cmap["unicode_list"]:
                return cmap["glyph_id_start"] + cmap["unicode_list"].index(rcp)
            continue
        raise SystemExit(f"unsupported cmap {cmap['type']}")
    return 0


def decode_glyph(font, cp: int):
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


def verify(path: str, codepoints, glyphs) -> int:
    font = parse_font(path)
    errors = []
    for cp in codepoints:
        want = glyphs[cp]
        got = decode_glyph(font, cp)
        if got is None:
            errors.append(f"U+{cp:04X}: glyph not found")
            continue
        if (want[0], want[1], want[2], want[3], want[4]) != got[:5]:
            errors.append(f"U+{cp:04X}: metrics mismatch {want[:5]} != {got[:5]}")
            continue
        if want[5] != got[5]:
            diff = sum(1 for a, b in zip(want[5], got[5]) if a != b)
            errors.append(f"U+{cp:04X}: bitmap mismatch {diff}/{len(want[5])}")
    if errors:
        for e in errors[:20]:
            log("  ERROR: " + e)
        log(f"  verify FAIL ({len(errors)})")
        return 1
    log(f"  verify PASS: {len(codepoints)} glyphs match rasterization")
    return 0


def check(out_dir: str, codepoints) -> int:
    path = os.path.join(out_dir, FONT_NAME + ".c")
    if not os.path.exists(path):
        log(f"missing font: {path}")
        return 1
    font = parse_font(path)
    missing = [cp for cp in codepoints if not lookup_glyph_id(font, cp)]
    blank = []
    for cp in codepoints:
        # E000/E03F 是源编码里的空格,正常字形就是空的。
        if cp <= 0x20 or cp in (0xE000, 0xE03F) or unicodedata.category(chr(cp)).startswith("Z"):
            continue
        got = decode_glyph(font, cp)
        if got and not any(got[5]):
            blank.append(cp)
    log(f"  {FONT_NAME}: {len(codepoints)} code points, missing {len(missing)}, "
        f"blank {len(blank)}, line height {font['line_height']}, base {font['base_line']}")
    if missing:
        log("    missing: " + " ".join(f"U+{cp:04X}" for cp in missing[:12]))
    if blank:
        log("    blank: " + " ".join(f"U+{cp:04X}" for cp in blank[:12]))
    return 0 if not missing and not blank else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pack", required=True)
    ap.add_argument("--pua-font", default="")
    ap.add_argument("--ui-font", default="")
    ap.add_argument("--widths", default="")
    ap.add_argument("--source-root", default="main")
    ap.add_argument("--out-dir", default="assets/fonts")
    ap.add_argument("--widths-header", default="main/steins0_widths.h")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    chars = required_chars(args.pack, args.source_root, args.out_dir)
    codepoints = sorted(ord(c) for c in chars)
    if args.check:
        return check(args.out_dir, codepoints)
    if not (args.pua_font and args.ui_font and args.widths):
        ap.error("generation needs --pua-font --ui-font --widths")
    widths = width_table(args.widths)
    emit_widths_header(args.widths_header, widths)
    log(f"  widths header: {args.widths_header}")
    log(f"  code points: {len(codepoints)}")
    glyphs, line_height, base_line = rasterize(args.pua_font, args.ui_font,
                                               codepoints, widths)
    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, INVENTORY), "w", encoding="utf-8",
              newline="\n") as fh:
        fh.write("# Generated by tools/steins0_font.py: pack text + UI literals.\n")
        fh.write("".join(chr(cp) for cp in codepoints) + "\n")
    text = emit_font(codepoints, glyphs, line_height, base_line)
    path = os.path.join(args.out_dir, FONT_NAME + ".c")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    log(f"  {path} ({os.path.getsize(path) / 1024:.0f} KB source)")
    return verify(path, codepoints, glyphs) | check(args.out_dir, codepoints)


if __name__ == "__main__":
    raise SystemExit(main())
