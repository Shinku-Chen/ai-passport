#!/usr/bin/env python3
"""tsxx_pack 结构与纯逻辑守卫。

不需要 Pillow / numpy，也不需要源仓库:
  1. 正文变长码编解码往返(含 1 字节码与 2 字节转义两条路径);
  2. 检查点 + 每页字符数还原的正文偏移,与线性解码一致;
  3. 差分组识别、资源名表编解码、引用过滤;
  4. 上游缺陷归一规则(空 cg / 立绘名缺前缀);
  5. 用纯逻辑造一个最小资源包,过一遍 tools/tsxx_pack.py 的自检;
     再逐项破坏,确认自检会拒绝(段越界 / 下标越界 / 正文越界 / 魔数)。
  6. 仓库里存在 main/tsxx_data/tsxx_pack.bin 时,额外做一次结构自检。
"""

from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "main" / "tsxx_data" / "tsxx_pack.bin"

_spec = importlib.util.spec_from_file_location("tsxx_pack", ROOT / "tools" / "tsxx_pack.py")
tsxx = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tsxx)


def build_min_pack(pages_text) -> bytes:
    """只用纯逻辑拼一个最小但完整的包(不含任何图片)。"""
    symbols, symbol_id = tsxx.build_symbol_table(pages_text)
    blob = bytearray()
    lengths = bytearray()
    checkpoints = [0]
    for text in pages_text:
        blob += tsxx.encode_text(text, symbol_id)
        lengths.append(len(text))
    checkpoints.append(len(blob))
    count = len(pages_text)

    sections = [
        tsxx.plain_section(tsxx.SEC_SYM,
                           b"".join(struct.pack("<I", ord(c)) for c in symbols), len(symbols)),
        tsxx.plain_section(tsxx.SEC_TEXT, bytes(blob), len(blob)),
        tsxx.plain_section(tsxx.SEC_TOFF,
                           b"".join(struct.pack("<I", v) for v in checkpoints), len(checkpoints)),
        tsxx.plain_section(tsxx.SEC_TLEN, bytes(lengths), count),
        tsxx.plain_section(tsxx.SEC_PBG, bytes([0]) * count, count),
        tsxx.plain_section(tsxx.SEC_PSPK, bytes([tsxx.NONE8]) * count, count),
        tsxx.plain_section(tsxx.SEC_PSPR, bytes([tsxx.NONE8]) * count, count),
        tsxx.plain_section(tsxx.SEC_PFLAG, bytes(count), count),
        tsxx.plain_section(tsxx.SEC_PCGB, bytes((count + 7) // 8), (count + 7) // 8),
        tsxx.plain_section(tsxx.SEC_PCG, b"", 0),
        tsxx.plain_section(tsxx.SEC_BGNAME, tsxx.encode_name_table(["bg/one"]), 1),
        tsxx.plain_section(tsxx.SEC_SPKNAME, b"", 0),
        tsxx.plain_section(tsxx.SEC_SPRNAME, b"", 0),
        tsxx.plain_section(tsxx.SEC_CGNAME, b"", 0),
        tsxx.plain_section(tsxx.SEC_CHOICE, b"", 0),
        tsxx.plain_section(tsxx.SEC_CHOICEOPT, b"", 0),
        tsxx.Section(tsxx.SEC_BG, tsxx.BG_ENTRY),
        tsxx.Section(tsxx.SEC_FG, tsxx.FG_ENTRY),
        tsxx.Section(tsxx.SEC_EVB, tsxx.EV_ENTRY),
        tsxx.Section(tsxx.SEC_EVC, tsxx.EV_ENTRY),
        tsxx.Section(tsxx.SEC_CGDIR, tsxx.CGD_ENTRY),
        tsxx.plain_section(tsxx.SEC_META,
                           f"generator=test\nart={tsxx.ART_W}x{tsxx.ART_H}"
                           f"\nbg={tsxx.ART_W}x{tsxx.ART_H}"
                           f"\nevent={tsxx.ART_W}x{tsxx.ART_H}\n".encode("utf-8"), 4),
    ]
    background = tsxx.Section(tsxx.SEC_BG, tsxx.BG_ENTRY)
    background.add(struct.pack("<IIHH", 0, 2, tsxx.ART_W, tsxx.ART_H), b"\xff\xd8")
    sections.append(background)
    sections.sort(key=lambda section: section.kind)
    return tsxx.write_pack(sections)


class TextCodecTest(unittest.TestCase):
    def test_round_trip_covers_both_code_widths(self):
        # 造 300 个不同字符,保证有 >255 个码位,转义分支一定会被走到
        texts = ["".join(chr(0x4E00 + i) for i in range(300))]
        symbols, symbol_id = tsxx.build_symbol_table(texts)
        self.assertEqual(len(symbols), 300)
        encoded = tsxx.encode_text(texts[0], symbol_id)
        # 前 255 个码位 1 字节,剩下 45 个各 3 字节
        self.assertEqual(len(encoded), 255 + 45 * 3)
        decoded, end = tsxx.decode_text(encoded, 0, len(texts[0]), symbols)
        self.assertEqual(decoded, texts[0])
        self.assertEqual(end, len(encoded))

    def test_frequency_order_puts_common_glyphs_first(self):
        symbols, _ = tsxx.build_symbol_table(["我我我你", "你我"])
        self.assertEqual(symbols[0], "我")
        self.assertEqual(symbols[1], "你")

    def test_ties_are_broken_by_codepoint_for_reproducibility(self):
        first, _ = tsxx.build_symbol_table(["甲乙"])
        second, _ = tsxx.build_symbol_table(["乙甲"])
        self.assertEqual(first, second)

    def test_checkpoint_offset_matches_linear_scan(self):
        pages = ["第一行", "second line", "第三行更长一些", "x" * 40]
        symbols, symbol_id = tsxx.build_symbol_table(pages)
        blob = bytearray()
        lengths = bytearray()
        checkpoints = [0]
        for text in pages:
            blob += tsxx.encode_text(text, symbol_id)
            lengths.append(len(text))
        checkpoints.append(len(blob))
        walk = 0
        for index, text in enumerate(pages):
            offset = tsxx.text_byte_offset(checkpoints, lengths, bytes(blob), index)
            self.assertEqual(offset, walk, f"第 {index} 页偏移不对")
            decoded, walk = tsxx.decode_text(bytes(blob), offset, len(text), symbols)
            self.assertEqual(decoded, text)

    def test_checkpoint_count_matches_block_boundaries(self):
        self.assertEqual(tsxx.checkpoint_count(0), 1)
        self.assertEqual(tsxx.checkpoint_count(1), 2)
        self.assertEqual(tsxx.checkpoint_count(tsxx.CHECKPOINT_PAGES), 2)
        self.assertEqual(tsxx.checkpoint_count(tsxx.CHECKPOINT_PAGES + 1), 3)


class GroupingTest(unittest.TestCase):
    def test_event_variants_group_by_number(self):
        self.assertEqual(tsxx.event_group("ev111a.jpg"), "ev111")
        self.assertEqual(tsxx.event_group("ev111k.png"), "ev111")
        self.assertIsNone(tsxx.event_group("ev111.jpg"))
        self.assertIsNone(tsxx.event_group("sd_気配.png"))
        self.assertIsNone(tsxx.event_group("ev111ab.jpg"))

    def test_name_table_round_trip(self):
        names = ["学院_教室a", "画面_黒", "sd_気配.png"]
        self.assertEqual(tsxx.decode_name_table(tsxx.encode_name_table(names)), names)
        self.assertEqual(tsxx.decode_name_table(b""), [])
        with self.assertRaises(ValueError):
            tsxx.encode_name_table(["bad\nname"])


class ReferenceTest(unittest.TestCase):
    def test_resolve_reports_missing_without_dropping_order(self):
        index = {"a.jpg": "a.jpg", "a": "a.jpg", "b.jpg": "b.jpg", "b": "b.jpg"}
        names, missing = tsxx.resolve_assets(["a", "b", "_1", "画面_黒"], index)
        self.assertEqual(names, ["a", "b"])
        self.assertEqual(missing, ["_1", "画面_黒"])

    def test_index_assets_accepts_both_stem_and_filename(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "学院_教室a.jpg").write_bytes(b"x")
            index = tsxx.index_assets(tmp)
            self.assertIn("学院_教室a.jpg", index)
            self.assertIn("学院_教室a", index)

    def test_normalize_drops_upstream_defects_only(self):
        known = {"b": {"bg"}, "s": {"【甲】"}, "c": {"lh01_1"}, "cg": {"ev101a.jpg"}}
        page = {"b": "bg", "s": "【甲】", "c": "lh01_1", "cg": "ev101a.jpg", "t": "台词"}
        normalized, touched = tsxx.normalize_source_page(page, known)
        self.assertEqual(normalized, page)
        self.assertIsNone(touched)

        empty = {"b": "bg", "c": "_3", "cg": "", "t": ""}
        normalized, touched = tsxx.normalize_source_page(empty, known)
        self.assertEqual(normalized, {"b": "bg"})
        self.assertEqual(touched, "c+cg+t")

        kept = {"b": "bg", "cg": "unknown.png"}
        normalized, touched = tsxx.normalize_source_page(kept, known)
        self.assertEqual(normalized, {"b": "bg"})
        self.assertEqual(touched, "cg")


class ContainerTest(unittest.TestCase):
    def setUp(self):
        self.pages = ["你好", "世界啊", "第三行"]
        self.blob = build_min_pack(self.pages)
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name, "pack.bin")
        self.path.write_bytes(self.blob)

    def tearDown(self):
        self.tmp.cleanup()

    def table_offset(self, kind: int) -> int:
        """段类型在段表里的位置(不假设段表按类型排序)。"""
        count = struct.unpack_from("<I", self.blob, 16)[0]
        for index in range(count):
            if struct.unpack_from("<I", self.blob, 20 + 16 * index)[0] == kind:
                return 20 + 16 * index
        raise AssertionError(f"段表里没有 {tsxx.SEC_NAMES[kind]}")

    def write_broken(self, mutate) -> None:
        broken = bytearray(self.blob)
        mutate(broken)
        self.path.write_bytes(bytes(broken))
        with self.assertRaises(ValueError):
            tsxx.verify(str(self.path))

    def test_min_pack_passes_self_check(self):
        stats = tsxx.verify(str(self.path))
        self.assertEqual(stats["pages"], len(self.pages))
        self.assertEqual(stats["text_chars"], sum(len(t) for t in self.pages))

    def test_wrong_magic_is_rejected(self):
        self.write_broken(lambda blob: blob.__setitem__(slice(0, 8), b"XXXXXXXX"))

    def test_truncated_file_is_rejected(self):
        self.path.write_bytes(self.blob[:12])
        with self.assertRaises(ValueError):
            tsxx.verify(str(self.path))

    def test_total_size_mismatch_is_rejected(self):
        self.write_broken(lambda blob: struct.pack_into("<I", blob, 12, len(self.blob) + 8))

    def test_section_overflow_is_rejected(self):
        self.write_broken(lambda blob: struct.pack_into(
            "<I", blob, self.table_offset(tsxx.SEC_TEXT) + 4, len(self.blob) + 1))

    def test_background_index_out_of_range_is_rejected(self):
        offset = tsxx.parse_pack(self.blob)["sections"][tsxx.SEC_PBG][0]
        self.write_broken(lambda blob: blob.__setitem__(offset + 1, 7))

    def test_text_length_overflow_is_rejected(self):
        offset = tsxx.parse_pack(self.blob)["sections"][tsxx.SEC_TLEN][0]
        self.write_broken(lambda blob: blob.__setitem__(offset, tsxx.MAX_PAGE_CHARS))

    def test_checkpoint_count_is_enforced(self):
        self.write_broken(lambda blob: struct.pack_into(
            "<I", blob, self.table_offset(tsxx.SEC_TOFF) + 12,
            tsxx.parse_pack(self.blob)["sections"][tsxx.SEC_TOFF][2] - 4))

    def test_name_table_count_mismatch_is_rejected(self):
        self.write_broken(lambda blob: struct.pack_into(
            "<I", blob, self.table_offset(tsxx.SEC_BGNAME) + 8, 2))

    def test_background_size_mismatch_is_rejected(self):
        """BG 目录项的尺寸必须与 META 的 bg= 一致(固件按它决定放大比例)。"""
        offset = tsxx.parse_pack(self.blob)["sections"][tsxx.SEC_BG][0]
        self.write_broken(lambda blob: struct.pack_into("<H", blob, offset + 8,
                                                        tsxx.ART_W - 1))

    def test_meta_without_bg_size_is_rejected(self):
        """META 里没有 bg=<宽>x<高> 就无法合成,一律拒绝。"""
        meta_offset, _, meta_size = tsxx.parse_pack(self.blob)["sections"][tsxx.SEC_META]
        index = bytes(self.blob[meta_offset:meta_offset + meta_size]).find(b"\nbg=")
        self.assertGreaterEqual(index, 0)
        self.write_broken(lambda blob: blob.__setitem__(meta_offset + index + 2, ord("q")))


class CommittedPackTest(unittest.TestCase):
    """仓库里带了资源包时,顺手自检一次真实产物。"""

    def test_committed_pack_is_self_consistent(self):
        if not PACK.exists():
            raise unittest.SkipTest(f"缺少 {PACK.relative_to(ROOT)}(先跑 tools/tsxx_pack.py)")
        stats = tsxx.verify(str(PACK))
        self.assertGreater(stats["pages"], 0)
        self.assertGreater(stats["backgrounds"], 0)
        self.assertGreater(stats["sprites"], 0)
        # 每页最多 255 字,正文解码字符数必须与页数同量级
        self.assertLessEqual(stats["text_chars"], stats["pages"] * tsxx.MAX_PAGE_CHARS)


if __name__ == "__main__":
    unittest.main()
