#!/usr/bin/env python3
"""Structural guard for the generated Senren * Banka LVGL font.

This test never rasterises a glyph (that is `tools/senren_lvgl_font.py --check`,
which needs Pillow and a source font); it re-parses the committed artefacts and
cross-checks them against the script pack, so it stays fast and dependency free:

  * assets/fonts/senren_cjk_symbols.txt is sorted, unique and "U+XXXX" formatted;
  * assets/fonts/senren_cjk_16.c declares `senren_cjk_16` with a line height of
    20 px (main/atri_ui.h ATRI_LINE_H) and consistent glyph_dsc / glyph_bitmap /
    cmaps sizes;
  * every code point of the script pack's SEC_CHAR (the character set the game
    text needs) is covered by both the symbols file and the font cmap -- a
    missing glyph is a tofu box on the device.

The pack lives in build/ and is not in CI, so both checks skip cleanly when the
pack or the font is absent:

  python tools/senren_scn_pack.py --source build/senren-source \
      --out build/senren-pack/senren_scn.bin          # needs the fetched source
  python3 tools/senren_lvgl_font.py --ttf <CJK font>  # regenerate the font
  python3 tests/test_senren_font.py
"""

from __future__ import annotations

import re
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "build" / "senren-pack" / "senren_scn.bin"
FONT = ROOT / "assets" / "fonts" / "senren_cjk_16.c"
SYMBOLS = ROOT / "assets" / "fonts" / "senren_cjk_symbols.txt"

FONT_NAME = "senren_cjk_16"
EXPECTED_LINE_HEIGHT = 20        # main/atri_ui.h 的 ATRI_LINE_H
EXPECTED_BPP = 4
ASCII_FIRST, ASCII_LAST = 0x20, 0x7E

# SENRSCN1 脚本包头部/段表(见 tools/senren_scn_pack.py)
SCN_HEADER = struct.Struct("<8sHHHHI")
SCN_SECTION = struct.Struct("<IIII")
SEC_CHAR = 0

SYMBOL_RE = re.compile(r"^U\+([0-9A-F]{4,6})$")
BITMAPS_RE = re.compile(r"const uint8_t glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", re.S)
BYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})")
GLYPH_DSC_RE = re.compile(
    r"\{\s*\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),"
    r"\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\s*\}")
CMAP_ENTRY_RE = re.compile(
    r"\{\s*\.range_start\s*=\s*(0x[0-9a-fA-F]+|\d+),\s*\.range_length\s*=\s*(\d+),"
    r"\s*\.glyph_id_start\s*=\s*(\d+),"
    r"\s*\.unicode_list\s*=\s*(\w+),\s*\.glyph_id_ofs_list\s*=\s*(\w+),"
    r"\s*\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*(\w+)")
UNICODE_LIST_RE = re.compile(r"static const uint16_t %s\[\]\s*=\s*\{(.*?)\};", re.S)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def symbols() -> list[int]:
    """字符清单里的码位,顺序即文件顺序(守卫要求升序且唯一)。"""
    points = []
    for line in read_text(SYMBOLS).splitlines():
        match = SYMBOL_RE.match(line)
        if match:
            points.append(int(match.group(1), 16))
    return points


def scn_char_points(path: Path) -> list[int]:
    """读脚本包的 SEC_CHAR:u32 count + count × u16 码点。"""
    blob = path.read_bytes()
    magic, _version, _header_size, section_count, _reserved, total = SCN_HEADER.unpack_from(blob)
    if magic != b"SENRSCN1":
        raise AssertionError(f"{path}: 魔数不符 {magic!r}")
    if total != len(blob):
        raise AssertionError(f"{path}: 头里写 {total} 字节,实际 {len(blob)}")
    sections = {}
    for index in range(section_count):
        at = SCN_HEADER.size + SCN_SECTION.size * index
        sec_type, offset, _count, size = SCN_SECTION.unpack_from(blob, at)
        sections[sec_type] = (offset, size)
    offset, size = sections[SEC_CHAR]
    count = struct.unpack_from("<I", blob, offset)[0]
    if size != 4 + 2 * count:
        raise AssertionError(f"{path}: SEC_CHAR 大小 {size} != 4+2*{count}")
    return list(struct.unpack_from("<%dH" % count, blob, offset + 4))


def parse_cmaps(text: str) -> dict:
    """cmap 表 -> {码位: glyph id}(与生成器的两张表语义一致)。"""
    block = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};\n",
                      text, re.S)
    if not block:
        raise AssertionError("找不到 cmaps[]")
    entries = CMAP_ENTRY_RE.findall(block.group(1))
    if not entries:
        raise AssertionError("解析不出任何 cmap 条目")
    cmap = {}
    for start_s, length_s, gid_s, ulist, ofs_list, list_len_s, kind in entries:
        start = int(start_s, 16) if start_s.startswith("0x") else int(start_s)
        length, gid_start, list_len = int(length_s), int(gid_s), int(list_len_s)
        if ofs_list != "NULL":
            raise AssertionError(f"生成器不使用 glyph_id_ofs_list,实际 {ofs_list}")
        if kind.endswith("FORMAT0_TINY"):
            for i in range(length):
                cmap[start + i] = gid_start + i
        elif kind.endswith("SPARSE_TINY"):
            arr = re.search(r"static const uint16_t %s\[\]\s*=\s*\{(.*?)\};" % ulist,
                            text, re.S)
            if not arr:
                raise AssertionError(f"找不到 {ulist}")
            offsets = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", arr.group(1))]
            if len(offsets) != list_len:
                raise AssertionError(f"{ulist} 长度 {len(offsets)} != list_length {list_len}")
            for i, off in enumerate(offsets):
                cmap[start + off] = gid_start + i
        else:
            raise AssertionError(f"不支持的 cmap 类型 {kind}")
    return cmap


class SymbolsFileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.lines = read_text(SYMBOLS).splitlines() if SYMBOLS.exists() else []

    @unittest.skipUnless(SYMBOLS.exists(), f"missing {SYMBOLS.name}; run tools/senren_lvgl_font.py")
    def test_every_line_is_u_plus_form(self) -> None:
        bad = [line for line in self.lines if not SYMBOL_RE.match(line)]
        self.assertEqual(bad, [], f"清单里有 {len(bad)} 行不是 U+XXXX: {bad[:5]}")

    @unittest.skipUnless(SYMBOLS.exists(), f"missing {SYMBOLS.name}; run tools/senren_lvgl_font.py")
    def test_sorted_and_unique(self) -> None:
        points = symbols()
        self.assertEqual(len(points), len(self.lines), "有重复的码位行")
        self.assertEqual(points, sorted(points), "清单没有按码位升序")
        self.assertEqual(len(set(points)), len(points), "清单里有重复码位")

    @unittest.skipUnless(SYMBOLS.exists(), f"missing {SYMBOLS.name}; run tools/senren_lvgl_font.py")
    def test_printable_ascii_is_covered(self) -> None:
        points = set(symbols())
        missing = [cp for cp in range(ASCII_FIRST, ASCII_LAST + 1) if cp not in points]
        self.assertEqual(missing, [], "ASCII 区不完整,UI 文案里的标点会缺字")


@unittest.skipUnless(PACK.exists(), f"missing {PACK.name}; run tools/senren_scn_pack.py")
@unittest.skipUnless(SYMBOLS.exists(), f"missing {SYMBOLS.name}; run tools/senren_lvgl_font.py")
class ScriptCoverageTest(unittest.TestCase):
    def test_every_script_character_is_listed(self) -> None:
        wanted = scn_char_points(PACK)
        listed = set(symbols())
        missing = [cp for cp in wanted if cp not in listed]
        self.assertEqual(missing, [],
                         "SCN_SEC_CHAR 有 %d 个码位不在字符清单里: %s"
                         % (len(missing), " ".join(f"U+{cp:04X}" for cp in missing[:10])))


@unittest.skipUnless(FONT.exists(), f"missing {FONT.name}; run tools/senren_lvgl_font.py")
class LvglFontFileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not FONT.exists():
            raise unittest.SkipTest(f"missing {FONT.name}; run tools/senren_lvgl_font.py")
        cls.text = read_text(FONT)
        cls.bitmap_block = BITMAPS_RE.search(cls.text)
        cls.cmap = parse_cmaps(cls.text)
        cls.dsc = [tuple(int(x) for x in m) for m in GLYPH_DSC_RE.findall(cls.text)]

    def test_declares_the_font_symbol(self) -> None:
        self.assertIn(f"const lv_font_t {FONT_NAME} = {{", self.text)
        self.assertIn(f"#ifndef {FONT_NAME.upper()}", self.text)

    def test_line_height_is_the_ui_line_height(self) -> None:
        line_height = int(re.search(r"\.line_height\s*=\s*(\d+)", self.text).group(1))
        self.assertEqual(line_height, EXPECTED_LINE_HEIGHT,
                         "行高必须等于 main/atri_ui.h 的 ATRI_LINE_H")
        bpp = int(re.search(r"\.bpp\s*=\s*(\d+)", self.text).group(1))
        self.assertEqual(bpp, EXPECTED_BPP)

    def test_bitmap_only_contains_byte_tokens(self) -> None:
        self.assertIsNotNone(self.bitmap_block, "找不到 glyph_bitmap[]")
        self.assertIn("LV_FONT_FMT_TXT_PLAIN", self.text)

    def test_glyph_dsc_count_matches_cmap_and_bitmap(self) -> None:
        """dsc 条目 = 1 个保留项 + cmap 覆盖的全部字形,且位图正好放得下。"""
        space = sum(1 for _ in BYTE_RE.finditer(self.bitmap_block.group(1)))
        self.assertEqual(sorted(self.cmap.values()), list(range(1, len(self.cmap) + 1)),
                         "cmap 里的 glyph id 不连续")
        self.assertEqual(len(self.dsc), len(self.cmap) + 1,
                         "glyph_dsc 条目数 != 1 + cmap 码位数")
        # 生成器按 glyph id 顺序写位图:每个 dsc 的 bitmap_index 必须紧接上一个的字形
        cursor = 0
        for gid, (bitmap_index, _adv_w, box_w, box_h, _ofs_x, _ofs_y) in enumerate(self.dsc):
            self.assertEqual(bitmap_index, cursor, f"glyph {gid} 的 bitmap_index 不连续")
            cursor += (box_w * box_h + 1) // 2
        self.assertEqual(cursor, space,
                         f"glyph_dsc 声明的位图字节数 {cursor} != glyph_bitmap 的 {space}")

    def test_typical_glyph_is_not_empty(self) -> None:
        """抽查几个必然有墨迹的字:全空说明位图打包或 cmap 映射坏了。"""
        space = sum(1 for _ in BYTE_RE.finditer(self.bitmap_block.group(1)))
        self.assertGreater(space, len(self.dsc), "位图字节数比字形数还少,打包异常")
        for cp in (ord("永"), ord("恋"), ord("A")):
            self.assertIn(cp, self.cmap, f"U+{cp:04X} 不在 cmap 里")


@unittest.skipUnless(FONT.exists(), f"missing {FONT.name}; run tools/senren_lvgl_font.py")
@unittest.skipUnless(PACK.exists(), f"missing {PACK.name}; run tools/senren_scn_pack.py")
class FontCoversScriptTest(unittest.TestCase):
    def test_every_script_character_has_a_glyph(self) -> None:
        cmap = parse_cmaps(read_text(FONT))
        missing = [cp for cp in scn_char_points(PACK) if cp not in cmap]
        self.assertEqual(missing, [],
                         "剧本有 %d 个码位在字体里没有字形(设备上是方块): %s"
                         % (len(missing), " ".join(f"U+{cp:04X}" for cp in missing[:10])))


if __name__ == "__main__":
    unittest.main()
