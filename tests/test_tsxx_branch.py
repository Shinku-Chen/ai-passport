#!/usr/bin/env python3
"""分支系统的宿主机验证:拿真实资源包把整条游戏线走一遍,断言路径与结局名。

这一层故意把 main/tsxx_pack.c 的判定语义**重写一遍**,因为"能编译"完全不能说明
分支是对的:移植版原先把结局点当普通页翻过去、5 个 flag 闸门永远走默认分支,
结果 6 条女主线一条都进不去。所以这里要真的按源工程的规则走完路线:

  推进:no_next[p] → 它;gates[p] → 按 accumulated choices 第一条命中的规则;
        都不命中 → else(0 = 顺序 p+1);否则 p+1。
  结局:当前页在 ends 里 → 触发结局序列(名字显示在覆盖层上)。
  选项:把 (页, 选项号) 记进历史;历史是下一次闸门判定的输入。

覆盖:
  1. 包里的 7 个分支段与权威 branch.json 逐字节一致;
  2. 6 条女主线各自用一份指定选择序列走通,断言闸门命中、结局页与结局名;
  3. 闸门判定细节:条件必须全中、多出来的选择不影响、else、no_next 优先;
  4. no_next / no_back 的抽查与范围自检;
  5. 结局名里的每个字符都在字体子集里 —— 否则覆盖层会画出缺字方框。

不需要 Pillow / numpy,也不需要源仓库:只要 main/tsxx_data/tsxx_pack.bin
(缺包时整组跳过)。构建命令见 tools/validate.sh。
"""

from __future__ import annotations

import importlib.util
import struct
import sys
import unittest
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "main" / "tsxx_data" / "tsxx_pack.bin"
BRANCH = ROOT / "assets" / "tsxx-source" / "branch.json"
SYMBOLS = ROOT / "assets" / "fonts" / "tsxx_symbols.txt"

_spec = importlib.util.spec_from_file_location("tsxx_pack", ROOT / "tools" / "tsxx_pack.py")
tsxx = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tsxx)

# 保护值:正常路线最长约 61,436 页,留一倍余量。
MAX_STEPS = 200000


class Story:
    """资源包里的分支数据 + 与固件同一套语义的推进器(页号一律 0-based)。"""

    def __init__(self, pack: dict):
        self.pack = pack
        blob = pack["blob"]
        self.pages = pack["sections"][tsxx.SEC_TLEN][1]
        self.ends = self._ends()
        self.no_next = self._pairs(tsxx.SEC_BNEXT)
        self.no_back = self._pairs(tsxx.SEC_BBACK)
        self.gates = self._gates()
        self.option_counts = tsxx.choice_option_counts(pack)
        self._options: dict[int, list[tuple[str, int]]] = {}
        choice_off = pack["sections"][tsxx.SEC_CHOICE][0]
        opt_off = pack["sections"][tsxx.SEC_CHOICEOPT][0]
        symbols = tsxx.unpack_symbols(pack)
        text = tsxx.section_bytes(pack, tsxx.SEC_TEXT)
        for index in range(pack["sections"][tsxx.SEC_CHOICE][1]):
            page, count, _, _, first = struct.unpack_from(
                "<IBBHI", blob, choice_off + index * tsxx.CH_ENTRY)
            options = []
            for slot in range(count):
                off, length, _, _, target = struct.unpack_from(
                    "<IBBHI", blob, opt_off + (first + slot) * tsxx.CHOPT_ENTRY)
                options.append((tsxx.decode_text(text, off, length, symbols)[0], target))
            self._options[page] = options

    def _pairs(self, kind: int) -> dict[int, int]:
        blob = tsxx.section_bytes(self.pack, kind)
        count = self.pack["sections"][kind][1]
        return {struct.unpack_from("<II", blob, i * 8)[0]:
                struct.unpack_from("<II", blob, i * 8)[1] for i in range(count)}

    def _ends(self) -> dict[int, str]:
        names = tsxx.decode_name_table(tsxx.section_bytes(self.pack, tsxx.SEC_BENDNAME))
        blob = tsxx.section_bytes(self.pack, tsxx.SEC_BEND)
        count = self.pack["sections"][tsxx.SEC_BEND][1]
        out = {}
        for index in range(count):
            page, name = struct.unpack_from("<IH", blob, index * tsxx.BEND_ENTRY)
            out[page] = names[name]
        return out

    def _gates(self) -> dict[int, tuple[int, list[tuple[int, list[tuple[int, int]]]]]]:
        gblob = tsxx.section_bytes(self.pack, tsxx.SEC_BGATE)
        rblob = tsxx.section_bytes(self.pack, tsxx.SEC_BRULE)
        cblob = tsxx.section_bytes(self.pack, tsxx.SEC_BCOND)
        out = {}
        for index in range(self.pack["sections"][tsxx.SEC_BGATE][1]):
            page, first_rule, rule_count, _, else_target = struct.unpack_from(
                "<IIHHI", gblob, index * tsxx.BGATE_ENTRY)
            rules = []
            for rule in range(rule_count):
                target, first_cond, cond_count, _ = struct.unpack_from(
                    "<IIHH", rblob, (first_rule + rule) * tsxx.BRULE_ENTRY)
                conditions = []
                for cond in range(cond_count):
                    cond_page, value, _, _ = struct.unpack_from(
                        "<IBBH", cblob, (first_cond + cond) * tsxx.BCOND_ENTRY)
                    conditions.append((cond_page, value))
                rules.append((target, conditions))
            out[page] = (else_target, rules)
        return out

    def option_target(self, page: int, index: int) -> int:
        return self._options[page][index][1]

    def next_page(self, page: int, history: set) -> tuple[int, str]:
        """返回 (下一页, 走了哪条规则)。'seq' = 没有特殊规则,按顺序推进。"""
        if page in self.no_next:
            return self.no_next[page], "no_next"
        if page in self.gates:
            else_target, rules = self.gates[page]
            for target, conditions in rules:
                if all(condition in history for condition in conditions):
                    return target, "rule"
            if else_target != 0:
                return else_target, "else"
            return page + 1, "seq"
        return page + 1, "seq"

    def walk(self, plan: dict[int, int], start: int = 0):
        """plan: {页号(0-based): 选项下标};没写的选项页按第 1 个选项走。

        返回 (结局名, 结局页, 做过的选择(1-based 页号, 1-based 选项号), 跳转记录)。
        结局名是 None 表示走进了死循环(测试会据此报错)。
        """
        page = start
        history: set = set()
        choices = []
        jumps = []
        for _ in range(MAX_STEPS):
            if page in self.ends:
                return self.ends[page], page, choices, jumps
            if page in self.option_counts:
                index = plan.get(page, 0)
                if index >= self.option_counts[page]:
                    raise AssertionError(f"第 {page + 1} 页没有第 {index + 1} 个选项")
                history.add((page, index + 1))
                choices.append((page + 1, index + 1))
                page = self.option_target(page, index)
                continue
            target, why = self.next_page(page, history)
            if why != "seq":
                jumps.append((page + 1, why, target + 1))
            page = target
        return None, page, choices, jumps


def open_story() -> Story:
    if not PACK.exists():
        raise unittest.SkipTest(f"缺少 {PACK.relative_to(ROOT)}(先跑 tools/tsxx_pack.py)")
    with open(PACK, "rb") as handle:
        return Story(tsxx.parse_pack(handle.read(), str(PACK)))


# 6 条女主线各一份走通的选择序列(1-based 页号 / 1-based 选项号)。这些序列是从
# 闸门条件反推出来的:每条都恰好命中通向该结局的那条规则。
HEROINE_ROUTES = {
    "Noa end": [(2029, 2), (2963, 1), (3612, 1), (5248, 1), (6002, 1), (7930, 1),
                (8391, 2), (9096, 1), (9206, 1), (17695, 2)],
    "Kaguya end": [(2029, 2), (2963, 1), (3612, 1), (5248, 2), (6002, 1), (7930, 1),
                   (8391, 2), (9096, 3), (9206, 1), (9441, 1)],
    "Kurumi end": [(2029, 2), (2963, 1), (3612, 2), (5248, 2), (6002, 1), (7930, 1),
                   (8391, 2), (9096, 2), (9205, 1), (9441, 2)],
    "Amane end": [(2029, 2), (2963, 1), (3612, 2), (5248, 2), (6002, 1), (7930, 2),
                  (8391, 2), (9096, 5), (9205, 2), (9441, 1)],
    "Fumika end": [(2029, 2), (2963, 3), (3612, 1), (5248, 2), (6002, 1), (7930, 1),
                   (8391, 1), (9096, 1), (9206, 1), (9441, 1)],
    "Orie end": [(2029, 2), (2963, 1), (3612, 1), (5248, 2), (6002, 1), (7930, 1),
                 (8391, 2), (9096, 4), (9206, 1), (9441, 3), (9884, 1)],
}


class BranchDataTest(unittest.TestCase):
    """包里的分支段必须就是权威 branch.json。"""

    def setUp(self):
        self.story = open_story()

    def test_pack_matches_authoritative_branch_json(self):
        expected = tsxx.load_branch(str(BRANCH))
        blob = tsxx.branch_blob(expected, self.story.pages, self.story.option_counts)
        for kind, (payload, count) in blob.items():
            offset, actual_count, size = self.story.pack["sections"][kind]
            self.assertEqual(self.story.pack["blob"][offset:offset + size], payload,
                             f"{tsxx.SEC_NAMES[kind]} 与 branch.json 不一致")
            self.assertEqual(actual_count, count, f"{tsxx.SEC_NAMES[kind]} 条数不一致")

    def test_counts_and_tables_are_sane(self):
        self.assertEqual(len(self.story.ends), 15)
        self.assertEqual(len(self.story.no_next), 18)
        self.assertEqual(len(self.story.no_back), 38)
        self.assertEqual(len(self.story.gates), 5)
        self.assertEqual(len(self.story.option_counts), 13)
        for table in (self.story.ends, self.story.no_next, self.story.no_back,
                      self.story.gates):
            for page in table:
                self.assertLess(page, self.story.pages)
            self.assertEqual(list(table), sorted(table), "分支表的页号必须递增")
        for page, (else_target, rules) in self.story.gates.items():
            self.assertTrue(rules, f"第 {page + 1} 页的闸门没有规则")
            for target, conditions in rules:
                self.assertLess(target, self.story.pages)
                self.assertTrue(conditions)
                for cond_page, value in conditions:
                    self.assertIn(cond_page, self.story.option_counts,
                                  f"条件页 {cond_page + 1} 不是选择点")
                    self.assertLessEqual(value, self.story.option_counts[cond_page])
            self.assertLess(else_target, self.story.pages)

    def test_no_next_and_no_back_spot_checks(self):
        # 源工程第 3143 页"推进"跳到 3386(不走向下一页),6762 页"回退"到 2029。
        self.assertEqual(self.story.no_next[3142], 3385)
        self.assertEqual(self.story.no_back[10071], 2028)
        self.assertEqual(self.story.next_page(3142, set()), (3385, "no_next"))
        self.assertEqual(self.story.ends[61435], "回到主页")


class GateRuleTest(unittest.TestCase):
    """闸门判定:全条件命中、部分命中、多出来的选择、else、no_next 优先。"""

    def setUp(self):
        self.story = open_story()

    def conditions(self, gate: int, rule: int) -> set:
        return set(self.story.gates[gate][1][rule][1])

    def test_all_conditions_must_match(self):
        conditions = self.conditions(9340, 0)
        self.assertEqual(len(conditions), 7)
        self.assertEqual(self.story.next_page(9340, conditions), (9353, "rule"))
        # 少一个条件就不命中:落到 else 9392(1-based 9393)。
        for missing in conditions:
            self.assertEqual(self.story.next_page(9340, conditions - {missing}),
                             (9392, "else"), f"少 {missing} 仍然命中了规则")
        # 空历史也一样走 else。
        self.assertEqual(self.story.next_page(9340, set()), (9392, "else"))

    def test_extra_choices_do_not_break_a_match(self):
        conditions = self.conditions(9340, 0)
        noisy = conditions | {(2028, 1), (6001, 1), (9883, 1)}
        self.assertEqual(self.story.next_page(9340, noisy), (9353, "rule"))

    def test_two_rules_are_picked_by_page_value(self):
        # 闸门 9204 的两条规则只差选项号:选 2 → 9205,选 1 → 9206,没选 → else 是 0。
        self.assertEqual(self.story.next_page(9203, {(3611, 2)}), (9204, "rule"))
        self.assertEqual(self.story.next_page(9203, {(3611, 1)}), (9205, "rule"))
        self.assertEqual(self.story.next_page(9203, set()), (9204, "seq"))

    def test_no_next_wins_over_gate(self):
        # 语义顺序:推进先看 no_next,再看闸门。数据里两者不重叠,这里用真实表确认。
        for page in self.story.no_next:
            self.assertNotIn(page, self.story.gates)

    def test_gate_conditions_reference_real_choice_points(self):
        referenced = set()
        for _, rules in self.story.gates.values():
            for _, conditions in rules:
                referenced.update(page for page, _ in conditions)
        self.assertEqual(len(referenced), 11)
        for page in referenced:
            self.assertIn(page, self.story.option_counts)
            self.assertGreaterEqual(self.story.option_counts[page], 1)


class RouteTest(unittest.TestCase):
    """真正走一遍:每条女主线都要能走到,而且落在正确的结局名上。"""

    def setUp(self):
        self.story = open_story()

    def test_route_reaches_expected_ending(self):
        for name, log in HEROINE_ROUTES.items():
            plan = {page - 1: value - 1 for page, value in log}
            ending, page, choices, _ = self.story.walk(plan)
            self.assertEqual(choices, log, f"{name} 路线做过的选择与预期不同")
            self.assertEqual(ending, name, f"{name} 路线走到了 {ending!r}")
            self.assertEqual(self.story.ends[page], name)

    def test_rule_9341_first_rule_reaches_amane(self):
        """任务里点名的路线:选 choices[2029]=2, [3612]=2, … 命中 9341 → 9354。

        9341 第一条规则的条件集与 10043 唯一那条规则完全一样,所以这条线在
        10043 再次右转 → 20228,最后落在 Amane end(源工程的路线归属就是这样)。
        """
        log = [(2029, 2), (2963, 1), (3612, 2), (5248, 2), (6002, 1), (7930, 2),
               (8391, 2), (9096, 5), (9205, 2), (9441, 1)]
        plan = {page - 1: value - 1 for page, value in log}
        ending, page, choices, jumps = self.story.walk(plan)
        self.assertEqual(choices, log)
        self.assertIn((9341, "rule", 9354), jumps, "没有命中 9341 → 9354 那条规则")
        self.assertIn((10043, "rule", 20228), jumps)
        self.assertEqual(ending, "Amane end")
        self.assertEqual((page + 1), 29606)

    def test_route_9341_second_rule_reaches_fumika(self):
        log = HEROINE_ROUTES["Fumika end"]
        plan = {page - 1: value - 1 for page, value in log}
        ending, _, _, jumps = self.story.walk(plan)
        self.assertIn((9341, "rule", 56458), jumps)
        # 56458 一路走到 56474,再从那里跳回 9393(no_next),然后再一次进闸门。
        self.assertIn((56474, "no_next", 9393), jumps)
        self.assertIn((10053, "rule", 56475), jumps)
        self.assertEqual(ending, "Fumika end")

    def test_plain_walk_without_matching_choices_falls_through_all_gates(self):
        """全选第 1 项:先在第 2029 页选 1,然后一道闸门都命不中,落在 BAD END。"""
        ending, page, choices, jumps = self.story.walk({})
        self.assertEqual(ending, "BAD END")
        self.assertEqual(choices, [(2029, 1)])
        self.assertEqual(page + 1, 10559)
        self.assertEqual([why for _, why, _ in jumps if why != "no_next"], [])

    def test_every_line_ends_on_an_ending_page(self):
        """随便挑几份历史,终点一定是 ends 里的页,不能跑到页表末尾。"""
        for name in HEROINE_ROUTES:
            plan = {page - 1: value - 1 for page, value in HEROINE_ROUTES[name]}
            _, page, _, _ = self.story.walk(plan)
            self.assertIn(page, self.story.ends)
        # 每个选择页都选第 N 个选项(不足 N 个就选最后一个),终点仍必须是结局页。
        for value in range(1, 6):
            plan = {page: min(value, count) - 1
                    for page, count in self.story.option_counts.items()}
            ending, page, _, _ = self.story.walk(plan)
            self.assertIsNotNone(ending, f"选第 {value} 项时走进了死循环")
            self.assertIn(page, self.story.ends)

    def test_first_choice_second_option_stays_on_the_main_line(self):
        """第 2029 页选第 2 项 = 继续主线的同一个选择点,最后走闸门 10053 的 else 到 "END"。"""
        ending, page, choices, jumps = self.story.walk({2028: 1})
        self.assertEqual(ending, "END")
        self.assertEqual(page + 1, 10070)
        self.assertEqual(choices[0], (2029, 2))
        self.assertIn((10053, "else", 10054), jumps)
        self.assertNotIn("rule", [why for gate, why, _ in jumps if gate == 10053])


class EndingFontTest(unittest.TestCase):
    """结局覆盖层画的是包里的结局名,每个字符都必须有字形。"""

    def test_every_ending_name_is_in_the_font_subset(self):
        story = open_story()
        symbols = set(SYMBOLS.read_text(encoding="utf-8"))
        missing = {}
        for page, name in story.ends.items():
            gaps = sorted({ch for ch in name if ch not in symbols and ch not in "\n\r\t "})
            if gaps:
                missing[name] = "".join(gaps)
        self.assertEqual(missing, {}, f"结局名缺字形: {missing}")


if __name__ == "__main__":
    unittest.main()
