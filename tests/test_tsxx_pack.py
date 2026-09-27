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
import json
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "main" / "tsxx_data" / "tsxx_pack.bin"

_spec = importlib.util.spec_from_file_location("tsxx_pack", ROOT / "tools" / "tsxx_pack.py")
tsxx = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tsxx)


def build_min_pack(pages_text, choices=None, branch=None) -> bytes:
    """只用纯逻辑拼一个最小但完整的包(不含任何图片)。

    choices: [(页号(0-based), [(文案, 目标页), ...])] —— 闸门的条件要引用真选择点,
        所以测分支时得给它一个。
    branch:  与 assets/tsxx-source/branch.json 同形的分支配置(1-based 页号),
        过 tsxx.parse_branch() 后再编成段。
    """
    choices = list(choices or [])
    option_texts = [text for _, options in choices for text, _ in options]
    symbols, symbol_id = tsxx.build_symbol_table(list(pages_text) + option_texts)
    blob = bytearray()
    lengths = bytearray()
    checkpoints = [0]
    for text in pages_text:
        blob += tsxx.encode_text(text, symbol_id)
        lengths.append(len(text))
    checkpoints.append(len(blob))
    count = len(pages_text)

    choice_blob = bytearray()
    option_blob = bytearray()
    first_option = 0
    option_counts = {}
    for page, options in choices:
        choice_blob += struct.pack("<IBBHI", page, len(options), 0, 0, first_option)
        option_counts[page] = len(options)
        for text, target in options:
            option_blob += struct.pack("<IBBHI", len(blob), len(text), 0, 0, target)
            blob += tsxx.encode_text(text, symbol_id)
        first_option += len(options)

    parts = tsxx.branch_blob(tsxx.parse_branch(branch or {}), count, option_counts)
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
        tsxx.plain_section(tsxx.SEC_CHOICE, bytes(choice_blob), len(choices)),
        tsxx.plain_section(tsxx.SEC_CHOICEOPT, bytes(option_blob), first_option),
        tsxx.Section(tsxx.SEC_BG, tsxx.BG_ENTRY),
        tsxx.Section(tsxx.SEC_FG, tsxx.FG_ENTRY),
        tsxx.Section(tsxx.SEC_EVB, tsxx.EV_ENTRY),
        tsxx.Section(tsxx.SEC_EVC, tsxx.EV_ENTRY),
        tsxx.Section(tsxx.SEC_CGDIR, tsxx.CGD_ENTRY),
        *[tsxx.plain_section(kind, payload, items) for kind, (payload, items) in parts.items()],
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


class BranchTest(unittest.TestCase):
    """分支段:编码、自检与拒绝。合成包的页号与 branch.json 一样是 1-based。"""

    PAGES = ["第一页", "第二页", "第三页", "第四页", "第五页", "第六页"]
    # 第 2 页是选择点(两个选项);闸门的条件就引用它。
    CHOICES = [(1, [("选项一", 2), ("选项二", 4)])]
    BRANCH = {
        "ends": {"6": "END"},
        "no_next": {"1": 3},
        "no_back": {"3": 1},
        "gates": {"4": {"rules": [{"when": [[2, 2]], "to": 5}], "else": 2}},
    }

    def setUp(self):
        self.blob = build_min_pack(self.PAGES, self.CHOICES, self.BRANCH)
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name, "pack.bin")
        self.path.write_bytes(self.blob)
        self.pack = tsxx.parse_pack(self.blob)

    def tearDown(self):
        self.tmp.cleanup()

    def section_bytes(self, kind: int) -> bytes:
        return tsxx.section_bytes(self.pack, kind)

    def write_broken(self, mutate) -> None:
        broken = bytearray(self.blob)
        mutate(broken)
        self.path.write_bytes(bytes(broken))
        with self.assertRaises(ValueError):
            tsxx.verify(str(self.path))

    def test_branch_pack_passes_self_check(self):
        stats = tsxx.verify(str(self.path))
        self.assertEqual(stats["endings"], 1)
        self.assertEqual(stats["end_names"], 1)
        self.assertEqual(stats["branch_next"], 1)
        self.assertEqual(stats["branch_back"], 1)
        self.assertEqual(stats["branch_gates"], 1)
        self.assertEqual(stats["branch_rules"], 1)
        self.assertEqual(stats["branch_conds"], 1)

    def test_pages_are_stored_zero_based(self):
        """1-based 的 6 / 1 / 3 / 4 / 2 在包里必须是 5 / 0 / 2 / 3 / 1。"""
        page, name = struct.unpack("<IH", self.section_bytes(tsxx.SEC_BEND)[:6])
        self.assertEqual((page, name), (5, 0))
        self.assertEqual(struct.unpack("<II", self.section_bytes(tsxx.SEC_BNEXT)[:8]), (0, 2))
        self.assertEqual(struct.unpack("<II", self.section_bytes(tsxx.SEC_BBACK)[:8]), (2, 0))
        gate = struct.unpack("<IIHHI", self.section_bytes(tsxx.SEC_BGATE)[:16])
        self.assertEqual(gate, (3, 0, 1, 0, 1))
        rule = struct.unpack("<IIHH", self.section_bytes(tsxx.SEC_BRULE)[:12])
        self.assertEqual(rule, (4, 0, 1, 0))
        cond = struct.unpack("<IBBH", self.section_bytes(tsxx.SEC_BCOND)[:8])
        self.assertEqual(cond, (1, 2, 0, 0))

    def test_sequential_else_is_stored_as_zero(self):
        branch = {"gates": {"4": {"rules": [{"when": [[2, 1]], "to": 5}], "else": 0}}}
        pack = tsxx.parse_pack(build_min_pack(self.PAGES, self.CHOICES, branch))
        self.assertEqual(struct.unpack_from("<I", tsxx.section_bytes(pack, tsxx.SEC_BGATE), 12)[0],
                         0)

    def test_empty_branch_is_allowed(self):
        stats = tsxx.verify(str(self.write_empty_pack()))
        self.assertEqual(stats["endings"], 0)
        self.assertEqual(stats["branch_gates"], 0)

    def write_empty_pack(self) -> Path:
        path = Path(self.tmp.name, "empty.bin")
        path.write_bytes(build_min_pack(self.PAGES, self.CHOICES))
        return path

    def test_condition_page_must_be_a_choice_point(self):
        branch = {"gates": {"4": {"rules": [{"when": [[3, 1]], "to": 5}], "else": 0}}}
        with self.assertRaises(ValueError):
            build_min_pack(self.PAGES, self.CHOICES, branch)

    def test_condition_option_must_exist(self):
        branch = {"gates": {"4": {"rules": [{"when": [[2, 3]], "to": 5}], "else": 0}}}
        with self.assertRaises(ValueError):
            build_min_pack(self.PAGES, self.CHOICES, branch)

    def test_gate_target_must_be_in_range(self):
        branch = {"gates": {"4": {"rules": [{"when": [[2, 1]], "to": 99}], "else": 0}}}
        with self.assertRaises(ValueError):
            build_min_pack(self.PAGES, self.CHOICES, branch)

    def test_jump_page_must_be_in_range(self):
        with self.assertRaises(ValueError):
            build_min_pack(self.PAGES, self.CHOICES, {"no_next": {"99": 1}})
        with self.assertRaises(ValueError):
            build_min_pack(self.PAGES, self.CHOICES, {"no_back": {"1": 99}})
        with self.assertRaises(ValueError):
            build_min_pack(self.PAGES, self.CHOICES, {"ends": {"0": "END"}})

    def test_else_page_one_is_rejected(self):
        """else=1 与 0-based 的"没有规则命中就 p+1"哨兵撞车,只能构建失败。"""
        branch = {"gates": {"4": {"rules": [{"when": [[2, 1]], "to": 5}], "else": 1}}}
        with self.assertRaises(ValueError):
            build_min_pack(self.PAGES, self.CHOICES, branch)

    def test_non_integer_page_is_rejected(self):
        with self.assertRaises(ValueError):
            tsxx.parse_branch({"no_next": {"2": "3"}})
        with self.assertRaises(ValueError):
            tsxx.parse_branch({"unknown": {}})
        with self.assertRaises(ValueError):
            tsxx.parse_branch({"gates": {"4": {"rules": [{"when": [[2]], "to": 5}]}}})

    def test_end_name_with_newline_is_rejected(self):
        with self.assertRaises(ValueError):
            build_min_pack(self.PAGES, self.CHOICES, {"ends": {"6": "BAD\nEND"}})

    def test_broken_branch_sections_are_rejected(self):
        base = self.pack["sections"]
        cond_off = base[tsxx.SEC_BCOND][0]
        # 条件引用的选项号超出现有选项数。
        self.write_broken(lambda blob: blob.__setitem__(cond_off + 4, 9))
        # 条件页不是选择点。
        self.write_broken(lambda blob: struct.pack_into("<I", blob, cond_off, 4))
        # 规则的条件区间越界。
        self.write_broken(lambda blob: struct.pack_into(
            "<H", blob, base[tsxx.SEC_BRULE][0] + 8, 2))
        # 闸门的规则区间越界。
        self.write_broken(lambda blob: struct.pack_into(
            "<H", blob, base[tsxx.SEC_BGATE][0] + 8, 3))
        # 结局名下标越界。
        self.write_broken(lambda blob: struct.pack_into(
            "<H", blob, base[tsxx.SEC_BEND][0] + 4, 7))
        # no_next 的目标越界。
        self.write_broken(lambda blob: struct.pack_into(
            "<I", blob, base[tsxx.SEC_BNEXT][0] + 4, 99))
        # 结局名表的条数与 \n 分出来的条数不符。
        self.write_broken(lambda blob: struct.pack_into(
            "<I", blob, self.table_offset(tsxx.SEC_BENDNAME) + 8, 3))
        # 步长 × 条数 != 段长:把 BCOND 的条数改大。
        self.write_broken(lambda blob: struct.pack_into(
            "<I", blob, self.table_offset(tsxx.SEC_BCOND) + 8, 2))

    def test_branch_page_order_is_enforced(self):
        """表必须按页号递增(固件按序查找)。把两个结局点的页号搅乱。"""
        blob = bytearray(build_min_pack(
            self.PAGES, self.CHOICES,
            {"ends": {"6": "END", "1": "OTHER"}, "no_next": {}, "no_back": {},
             "gates": {}}))
        pack = tsxx.parse_pack(bytes(blob))
        offset = pack["sections"][tsxx.SEC_BEND][0]
        first, second = blob[offset:offset + 8], blob[offset + 8:offset + 16]
        blob[offset:offset + 8], blob[offset + 8:offset + 16] = second, first
        self.path.write_bytes(bytes(blob))
        with self.assertRaises(ValueError):
            tsxx.verify(str(self.path))

    def table_offset(self, kind: int) -> int:
        count = struct.unpack_from("<I", self.blob, 16)[0]
        for index in range(count):
            if struct.unpack_from("<I", self.blob, 20 + 16 * index)[0] == kind:
                return 20 + 16 * index
        raise AssertionError(f"段表里没有 {tsxx.SEC_NAMES[kind]}")

    def test_comparison_with_branch_json_survives_a_round_trip(self):
        """--verify-source 的那条比对:重建出的字节必须与包里的段一致。"""
        branch_path = Path(self.tmp.name, "branch.json")
        branch_path.write_text(json.dumps(self.BRANCH), encoding="utf-8")
        stats = tsxx.verify(str(self.path), None, str(branch_path))
        self.assertEqual(stats["source_ends"], 1)
        self.assertEqual(stats["source_gates"], 1)


class FirmwareContractTest(unittest.TestCase):
    """打包器与固件共享的常量必须在两边一致(否则错误只能等到设备上才暴露)。"""

    def header(self) -> str:
        return (ROOT / "main" / "tsxx_pack.h").read_text(encoding="utf-8")

    def test_section_count_matches_firmware(self):
        self.assertIn(f"#define TSXX_PACK_SECTIONS {tsxx.SECTION_COUNT}", self.header())

    def test_branch_buffer_limit_matches_firmware(self):
        self.assertIn(f"#define TSXX_PACK_BRANCH_MAX {tsxx.BRANCH_BLOB_MAX}u", self.header())

    def test_branch_blob_limit_is_enforced(self):
        """结束名表长到超过固件常驻拷贝上限时,打包必须直接失败。"""
        pages = [f"第 {i} 页" for i in range(64)]
        long_name = "A" * (tsxx.BRANCH_BLOB_MAX // 2)
        with self.assertRaises(ValueError):
            build_min_pack(pages, [], {"ends": {f"{i + 1}": long_name + str(i)
                                                 for i in range(4)}})


class CommittedPackTest(unittest.TestCase):
    """仓库里带了资源包时,顺手自检一次真实产物。"""

    def test_committed_pack_is_self_consistent(self):
        if not PACK.exists():
            raise unittest.SkipTest(f"缺少 {PACK.relative_to(ROOT)}(先跑 tools/tsxx_pack.py)")
        stats = tsxx.verify(str(PACK))
        self.assertGreater(stats["pages"], 0)
        self.assertGreater(stats["backgrounds"], 0)
        self.assertGreater(stats["sprites"], 0)
        # 分支段也必须真的进了包:没有它就是"结局被当普通页翻过去"的旧包。
        self.assertGreater(stats["endings"], 0)
        self.assertGreater(stats["branch_gates"], 0)
        self.assertGreaterEqual(stats["branch_rules"], stats["branch_gates"])
        self.assertGreaterEqual(stats["branch_conds"], stats["branch_rules"])
        # 每页最多 255 字,正文解码字符数必须与页数同量级
        self.assertLessEqual(stats["text_chars"], stats["pages"] * tsxx.MAX_PAGE_CHARS)


if __name__ == "__main__":
    unittest.main()
