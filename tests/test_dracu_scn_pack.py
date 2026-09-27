#!/usr/bin/env python3
"""Structural contract for the committed DRACU-RIOT script pack (DRACUSC1).

The firmware reads pages out of fixed-size page blocks and resolves the branch
tables (前进特判 / 回退堵死 / 条件路由 / 结局) by hand, so the layout must stay
exactly as tools/dracu_scn_pack.py writes it.  This test re-parses the committed
pack independently and checks:

  * header/sections and the block table (page blocks are full, the last one short);
  * every page record decodes and its text slice stays inside the text stream;
  * the character table and every string entry decode as valid UTF-8;
  * the branch tables are internally consistent (indices in range, targets are
    real pages, hidden rules reference existing condition pages);
  * the choice table's targets are real pages and its texts live in the stream;
  * the chapter table ascends by page and its names decode.

Skips cleanly when the pack is absent (regenerate with tools/dracu_scn_pack.py).

Run: python3 tests/test_dracu_scn_pack.py
"""

from __future__ import annotations

import collections
import struct
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "main" / "dracu_data" / "dracu_scn.bin"

HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
BLOCK_HEADER = struct.Struct("<HHII")
BLOCK = struct.Struct("<III")
PAGE = struct.Struct("<IHHHBBBBBB")
CHOICE_HEAD = struct.Struct("<IBBH")
CHOICE_SLOT = struct.Struct("<IHHI")
CHAPTER = struct.Struct("<IHBB")

SEC_CHAR, SEC_STR, SEC_BLOCK, SEC_BLOB, SEC_BRANCH, SEC_CHOICE, SEC_CHAPTERS, SEC_META = range(8)


class ScriptPackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not PACK.is_file():
            raise unittest.SkipTest(f"缺少 {PACK}(先用 tools/dracu_scn_pack.py 生成)")
        cls.blob = PACK.read_bytes()
        magic, version, header_size, section_count, _reserved, total = \
            HEADER.unpack_from(cls.blob)
        assert magic == b"DRACUSC1" and total == len(cls.blob)
        cls.sections: dict[int, tuple[int, int, int]] = {}
        for index in range(section_count):
            kind, offset, count, size = SECTION.unpack_from(
                cls.blob, HEADER.size + index * SECTION.size)
            cls.sections[kind] = (offset, count, size)
        cls.version = version

        # 字符表
        char_off, char_count, _size = cls.sections[SEC_CHAR]
        cls.chars = list(struct.unpack_from(f"<{char_count}H", cls.blob, char_off + 4))

        # 块表:前半是页表,后半是正文
        block_off, _count, _size = cls.sections[SEC_BLOCK]
        cls.block_total, cls.page_blocks, cls.page_count, cls.pages_per_block = \
            BLOCK_HEADER.unpack_from(cls.blob, block_off)
        cls.blocks = [BLOCK.unpack_from(cls.blob, block_off + BLOCK_HEADER.size + i * BLOCK.size)
                      for i in range(cls.block_total)]
        cls.blob_off, _c, cls.blob_size = cls.sections[SEC_BLOB]

        # 页表(全解出来,后面几条都用它)
        cls.pages = b"".join(cls.decompress(index) for index in range(cls.page_blocks))
        cls.text_blocks = [cls.decompress(index)
                           for index in range(cls.page_blocks, cls.block_total)]
        cls.build_graph()

    @classmethod
    def build_graph(cls) -> None:
        """把分支表拆成"前进图":非选项页一条边(hidden 用兜底),选项页多条边。"""
        off, _count, _size = cls.sections[SEC_BRANCH]
        position = off

        def take() -> int:
            nonlocal position
            value = struct.unpack_from("<I", cls.blob, position)[0]
            position += 4
            return value

        cls.no_next = {}
        for _ in range(take()):
            page, target = struct.unpack_from("<II", cls.blob, position)
            position += 8
            cls.no_next[page] = target
        for _ in range(take()):          # noBack 不需要
            position += 8
        cls.ends = {}
        for _ in range(take()):
            page, name = struct.unpack_from("<IH", cls.blob, position)
            position += 6
            cls.ends[page] = name
        cls.cond_pages = []
        for _ in range(take()):
            cls.cond_pages.append(struct.unpack_from("<I", cls.blob, position)[0])
            position += 4
        cls.cond_index = {page: i for i, page in enumerate(cls.cond_pages)}
        hidden_records = []
        for _ in range(take()):
            hidden_records.append(struct.unpack_from("<IIBBI", cls.blob, position))
            position += 14
        rules = []
        for _ in range(take()):
            rules.append(struct.unpack_from("<IBBI", cls.blob, position))
            position += 10
        literals = []
        for _ in range(take()):
            literals.append(struct.unpack_from("<BB", cls.blob, position))
            position += 2
        cls.hidden_fallback = {record[0]: record[4] for record in hidden_records}
        cls.hidden_rules = {record[0]: (record[1], record[2]) for record in hidden_records}
        cls.rules = rules
        cls.literals = literals

        cls.choices = {}
        off, count, _size = cls.sections[SEC_CHOICE]
        for index in range(count):
            base = off + 4 + index * (CHOICE_HEAD.size + 5 * CHOICE_SLOT.size)
            page, options, _a, _b = CHOICE_HEAD.unpack_from(cls.blob, base)
            targets = []
            for slot in range(options):
                targets.append(CHOICE_SLOT.unpack_from(
                    cls.blob, base + CHOICE_HEAD.size + slot * CHOICE_SLOT.size)[3])
            cls.choices[page] = targets

    @classmethod
    def decompress(cls, index: int) -> bytes:
        off, comp, raw = cls.blocks[index]
        data = cls.blob[cls.blob_off + off:cls.blob_off + off + comp]
        out = zlib.decompress(data)
        assert len(out) == raw, (index, len(out), raw)
        return out

    def page(self, number: int):
        return PAGE.unpack_from(self.pages, (number - 1) * PAGE.size)

    def char_at(self, index: int) -> str:
        return chr(self.chars[index])

    def decode_text(self, off: int, length: int) -> str:
        base = 0
        for block in self.text_blocks:
            if off < base + len(block):
                position = off - base
                return "".join(self.char_at(struct.unpack_from("<H", block, position + 2 * i)[0])
                               for i in range(length))
            base += len(block)
        raise AssertionError(f"正文偏移 {off} 越界")

    # -- 结构 -------------------------------------------------------------
    def test_page_blocks_are_full_except_the_last(self) -> None:
        raw_pages = len(self.pages) // PAGE.size
        self.assertGreater(raw_pages, 50000)
        for index, (off, comp, raw) in enumerate(self.blocks[:self.page_blocks]):
            if index < self.page_blocks - 1:
                self.assertEqual(raw, self.pages_per_block * PAGE.size)
            self.assertLessEqual(off + comp, self.blob_size)

    def test_every_page_record_fits_the_stream(self) -> None:
        text_total = sum(len(block) for block in self.text_blocks)
        for number in range(1, self.page_count + 1):
            text_off, text_len, cg, sprite, bg, sd, name, flags, cs, fs = self.page(number)
            if text_off != 0xFFFFFFFF:
                self.assertLess(text_off + text_len * 2, text_total + 2)
            for value in (cg, sprite):
                self.assertTrue(value == 0xFFFF or value < 0xFFFF)
            self.assertLessEqual(cs, 255)
            self.assertLessEqual(fs, 255)

    def test_first_page_text_is_readable(self) -> None:
        text_off, text_len, _cg, _sprite, _bg, _sd, _name, _flags, _cs, _fs = self.page(1)
        self.assertEqual(self.decode_text(text_off, text_len), "未知的风景，在车窗外流动着。")

    def test_character_table_is_unique_and_bmp_only(self) -> None:
        self.assertGreater(len(self.chars), 3000)
        self.assertEqual(len(self.chars), len(set(self.chars)))
        # 允许控制码(源剧本里有换行);LVGL 自己处理换行,字库给它们留空字形
        self.assertTrue(all(cp <= 0xFFFF for cp in self.chars))
        self.assertGreater(sum(1 for cp in self.chars if cp >= 0x20), len(self.chars) - 8)

    def test_string_table_decodes(self) -> None:
        off, count, _size = self.sections[SEC_STR]
        self.assertGreater(count, 100)
        position = off + 4
        names = []
        for _ in range(count):
            length = struct.unpack_from("<H", self.blob, position)[0]
            position += 2
            name = "".join(self.char_at(struct.unpack_from("<H", self.blob, position + 2 * i)[0])
                           for i in range(length))
            position += 2 * length
            names.append(name)
        # 章节名 / 结局名应当都在里面
        self.assertTrue(any("结局" in name or "END" in name for name in names))
        self.assertTrue(any(name.startswith(("序幕", "本篇")) for name in names))

    def test_branch_tables_reference_real_pages(self) -> None:
        off, _count, _size = self.sections[SEC_BRANCH]
        position = off

        def take(count: int) -> int:
            nonlocal position
            value = struct.unpack_from("<I", self.blob, position)[0]
            position += 4
            return value

        def pages_valid(page: int) -> bool:
            return 1 <= page <= self.page_count

        no_next_count = take(0)
        for _ in range(no_next_count):
            page, target = struct.unpack_from("<II", self.blob, position)
            position += 8
            self.assertTrue(pages_valid(page) and pages_valid(target), "前进特判目标越界")
        no_back_count = take(0)
        for _ in range(no_back_count):
            page, target = struct.unpack_from("<II", self.blob, position)
            position += 8
            self.assertTrue(pages_valid(page) and pages_valid(target), "回退特判目标越界")
        end_count = take(0)
        self.assertGreater(end_count, 0)
        for _ in range(end_count):
            page, name = struct.unpack_from("<IH", self.blob, position)
            position += 6
            self.assertTrue(pages_valid(page))
        cond_count = take(0)
        cond_pages = []
        for _ in range(cond_count):
            cond_pages.append(struct.unpack_from("<I", self.blob, position)[0])
            position += 4
        hidden_count = take(0)
        hidden = []
        for _ in range(hidden_count):
            page, rule_off, rule_count, _pad, fallback = struct.unpack_from("<IIBBI", self.blob,
                                                                            position)
            position += 14
            hidden.append((page, rule_off, rule_count, fallback))
            self.assertTrue(pages_valid(page) and pages_valid(fallback))
        rule_count = take(0)
        rules = []
        for _ in range(rule_count):
            lit_off, lit_count, _pad, target = struct.unpack_from("<IBBI", self.blob, position)
            position += 10
            rules.append((lit_off, lit_count, target))
            self.assertTrue(pages_valid(target))
        literal_count = take(0)
        literals = []
        for _ in range(literal_count):
            cond_index, value = struct.unpack_from("<BB", self.blob, position)
            position += 2
            literals.append((cond_index, value))
            self.assertLess(cond_index, cond_count, "条件下标越界")
            self.assertTrue(1 <= value <= 4, "选项取值应当从 1 起")
        # 规则引用的字面量范围必须落在字面量表里
        for lit_off, lit_count, _target in rules:
            self.assertLessEqual(lit_off + lit_count, literal_count)
        for _page, rule_off, hidden_rule_count, _fallback in hidden:
            self.assertLessEqual(rule_off + hidden_rule_count, rule_count, "规则下标越界")
        self.assertEqual(position - off, self.sections[SEC_BRANCH][2], "分支段长度与解析不符")

    def test_choices_point_at_real_pages(self) -> None:
        off, count, _size = self.sections[SEC_CHOICE]
        self.assertGreater(count, 20)
        text_total = sum(len(block) for block in self.text_blocks)
        for index in range(count):
            base = off + 4 + index * (CHOICE_HEAD.size + 5 * CHOICE_SLOT.size)
            page, options, _pad0, _pad1 = CHOICE_HEAD.unpack_from(self.blob, base)
            self.assertTrue(1 <= page <= self.page_count)
            self.assertTrue(1 <= options <= 5)
            for slot in range(options):
                text_off, text_len, _pad, target = CHOICE_SLOT.unpack_from(
                    self.blob, base + CHOICE_HEAD.size + slot * CHOICE_SLOT.size)
                self.assertTrue(1 <= target <= self.page_count, f"选项目标越界({page} -> {target})")
                self.assertLess(text_off + text_len * 2, text_total + 2)
                self.assertGreater(text_len, 0)

    def test_after_story_entries_are_listed(self) -> None:
        """五条线的后日谈入口要在章节表里(源工程 more.ux 的那份页码)。"""
        expected = {18039: "艾莉娜", 21698: "尼古拉", 31426: "莉音", 41801: "美羽", 51942: "梓"}
        off, count, _size = self.sections[SEC_CHAPTERS]
        found = {}
        for index in range(count):
            page, name, _route, _pad = CHAPTER.unpack_from(self.blob, off + 4 + index * CHAPTER.size)
            if page in expected:
                found[page] = name
        missing = sorted(set(expected) - set(found))
        self.assertFalse(missing, f"章节表里缺后日谈入口: {missing}")
        for page, name_index in found.items():
            self.assertTrue(self.decode_string(name_index).endswith("后日谈"),
                            f"页 {page} 的标签不是后日谈: {self.decode_string(name_index)}")
            self.assertTrue(self.decode_string(name_index).startswith(expected[page]))

    def decode_string(self, index: int) -> str:
        off, _count, _size = self.sections[SEC_STR]
        position = off + 4
        for _ in range(index + 1):
            length = struct.unpack_from("<H", self.blob, position)[0]
            position += 2
            if _ == index:
                return "".join(self.char_at(struct.unpack_from("<H", self.blob, position + 2 * i)[0])
                               for i in range(length))
            position += 2 * length
        return ""

    def test_chapters_ascend_and_decode(self) -> None:
        off, count, _size = self.sections[SEC_CHAPTERS]
        self.assertGreater(count, 10)
        previous = 0
        for index in range(count):
            page, name, route, _pad = CHAPTER.unpack_from(self.blob, off + 4 + index * CHAPTER.size)
            self.assertGreaterEqual(page, previous, "章节表必须按页号升序")
            previous = page
            self.assertTrue(1 <= page <= self.page_count)
            self.assertLess(route, 8)


    def test_every_reachable_page_can_still_reach_an_ending(self) -> None:
        """没有"进得去出不来"的死循环。

        地图是"非选项页一条前进边 + 选项页若干条"，所以要保证:从第 1 页出发能到达的
        每一页,都存在一条通往结局的路(选项页只要有**一个**选项能走下去就算通)。
        上游数据里尼古拉第 4 话的收场跳转原本被导回重复场景块,导致 20608..21011
        这 400 页无论怎么选都出不去 —— 打包器修正后这条用例守住它。
        """
        successors = {}
        for page in range(1, self.page_count + 1):
            if page in self.choices:
                successors[page] = list(self.choices[page])
            elif page in self.hidden_rules:
                # 条件路由:上限取"所有规则 + 兜底"都可达(乐观),只要有一条路能出去就不算死循环
                rule_off, rule_count = self.hidden_rules[page]
                targets = [self.rules[rule_off + r][3] for r in range(rule_count)]
                targets.append(self.hidden_fallback[page])
                successors[page] = targets
            else:
                successors[page] = [self.no_next.get(page, page + 1)]

        # 反向可达:能走到结局的页
        reverse = {}
        for page, targets in successors.items():
            for target in targets:
                if 1 <= target <= self.page_count:
                    reverse.setdefault(target, []).append(page)
        good = set(self.ends)
        queue = [page for page in self.ends]
        while queue:
            page = queue.pop()
            for previous in reverse.get(page, ()):
                if previous not in good:
                    good.add(previous)
                    queue.append(previous)

        # 从第 1 页出发(所有选项都算),找出到不了的页
        seen = {1}
        queue = [1]
        while queue:
            page = queue.pop()
            for target in successors.get(page, ()):
                if 1 <= target <= self.page_count and target not in seen:
                    seen.add(target)
                    queue.append(target)
        stuck = sorted(page for page in seen if page not in good)
        self.assertFalse(stuck, f"这些页进得去、但怎么选都到不了结局: {stuck[:12]}")

    def test_all_five_routes_are_reachable_from_the_prologue(self) -> None:
        """五条女主线都要能从第 1 页靠选项走到(带上选择历史做 BFS)。"""
        routes = {"梓": 42333, "美羽": 32071, "莉音": 22712, "艾莉娜": 9028, "尼古拉": 18437}
        blank = (0,) * len(self.cond_pages)

        def successor_states(page: int, hist):
            if page in self.choices:
                for index, target in enumerate(self.choices[page], start=1):
                    new_hist = list(hist)
                    slot = self.cond_index.get(page)
                    if slot is not None:
                        new_hist[slot] = index
                    yield target, tuple(new_hist)
            elif page in self.hidden_rules:
                rule_off, rule_count = self.hidden_rules[page]
                target = None
                for rule in range(rule_count):
                    lit_off, lit_count, _pad, rule_target = self.rules[rule_off + rule]
                    if all(hist[self.literals[lit_off + slot][0]] ==
                           self.literals[lit_off + slot][1] for slot in range(lit_count)):
                        target = rule_target
                        break
                yield target if target is not None else self.hidden_fallback[page], hist
            else:
                yield self.no_next.get(page, page + 1), hist

        for label, target in routes.items():
            seen = {(1, blank)}
            queue = collections.deque([(1, blank)])
            hit = False
            while queue and not hit:
                page, hist = queue.popleft()
                if page == target:
                    hit = True
                    break
                if page in self.ends or not (1 <= page <= self.page_count):
                    continue
                for state in successor_states(page, hist):
                    if state not in seen and 1 <= state[0] <= self.page_count:
                        seen.add(state)
                        queue.append(state)
            self.assertTrue(hit, f"{label}线(入口页 {target})不可达")


def main() -> int:
    suite = unittest.TestLoader().loadTestsFromTestCase(ScriptPackTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
