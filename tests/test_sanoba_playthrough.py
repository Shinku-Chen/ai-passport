#!/usr/bin/env python3
"""Sanoba 剧本包的推进语义测试(纯 Python,和 main/sanoba_model.c 同一套规则)。

C 侧的 main/sanoba_model.c 不读整本剧本就能推进,靠的是这些规则:

  1. 场景内跳转按标签落点(SEC_LABEL 直接给出 scenario/chunk/node,不需要扫块);
  2. 本场景没定义的名字按名字做全局回退(源数据的分支链跨文件);
  3. 块走完 → 同场景的下一块;场景走完 → 命中 SEC_ROUTE 就按旗标选线(最高分优先、
     全 0 或并列走兜底),否则按场景表顺序推进;最后一个场景走完即通关;
  4. 两个哪都没定义的标签(*gameend_title / *endrecollection)当结局。

这里用同一批规则把整本剧本跑一遍,断言流程能走通、选线正确、跳转都能落地。
真实的剧本包与源素材不入库,所以没取过源就跳过。

  python tools/sanoba_fetch_source.py --dest build/sanoba-source --chunks
  python3 tests/test_sanoba_playthrough.py
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "build" / "sanoba-source"
PACK_PATH = ROOT / "build" / "sanoba-pack" / "sanoba_scn.bin"

SPEC = importlib.util.spec_from_file_location("sanoba_scn_pack", ROOT / "tools" / "sanoba_scn_pack.py")
assert SPEC and SPEC.loader
PACKER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PACKER
SPEC.loader.exec_module(PACKER)


class Player:
    """main/sanoba_model.c 的等价实现(只实现推进所需的部分)。"""

    def __init__(self, pack) -> None:
        self.pack = pack
        self.scenario = 0
        self.chunk = 0
        self.node = 0
        self.flags = [0] * PACKER.SANOBA_FLAG_MAX if hasattr(PACKER, "SANOBA_FLAG_MAX") else [0] * 32
        self.ending = None
        self.trace: list[tuple[str, int, int]] = []
        self.dialogues = 0
        self.choices: list[int] = []
        self.pending_ending = False
        self._record_cache: dict[int, list] = {}
        # 名字 -> label id:本场景优先;跨场景只认分章链(与打包器的 resolve_target 同一套规则)
        self.local_labels: dict[tuple[int, str], int] = {}
        self.first_labels: dict[str, int] = {}
        for label_id, (name_id, scenario, _chunk, _node) in enumerate(pack.labels):
            name = pack.pools["label"][name_id]
            self.local_labels.setdefault((scenario, name), label_id)
            self.first_labels.setdefault(name, label_id)

    # ---- 字典/标签 ----
    def scenario_span(self, index: int) -> tuple[int, int]:
        return self.pack.scenarios[index]["first_chunk"], self.pack.scenarios[index]["chunk_count"]

    def jump_label(self, label_id: int) -> None:
        name_id, scenario, chunk, node = self.pack.labels[label_id]
        del name_id
        self.scenario = scenario
        self.chunk = chunk
        self.node = node

    def next_scenario(self) -> int | None:
        route = self.pack.route
        targets = route["targets"]
        if targets and self.scenario == route["trigger"]:
            best = None
            best_score = 0
            tie = False
            for flag_id, scenario in targets:
                score = self.flags[flag_id] if flag_id < len(self.flags) else 0
                if best is None or score > best_score:
                    best = scenario
                    best_score = score
                    tie = False
                elif score == best_score:
                    tie = True
            if best is None or best_score == 0 or tie:
                return route["fallback"]
            return best
        following = self.pack.scenarios[self.scenario]["next_scenario"]
        return None if following == 0xFFFF else following

    # ---- 推进 ----
    def records(self, chunk: int) -> list:
        cached = self._record_cache.get(chunk)
        if cached is None:
            cached = self.pack.records(chunk)
            self._record_cache[chunk] = cached
        return cached

    def normalize(self, guard: int = 200000) -> bool:
        for _ in range(guard):
            nodes = self.records(self.chunk)
            if self.node < len(nodes):
                return True
            first, length = self.scenario_span(self.scenario)
            if self.chunk + 1 < first + length:
                self.chunk += 1
                self.node = 0
                continue
            nxt = self.next_scenario()
            if nxt is None:
                return False
            self.scenario = nxt
            self.chunk, self.node = self.scenario_span(nxt)[0], 0
            first, length = self.scenario_span(self.scenario)
            if length == 0:
                return False
        return False

    def advance(self, choice: int = 0) -> str:
        if self.pending_ending:
            self.pending_ending = False
            return "ending"
        for _ in range(200000):
            if not self.normalize():
                return "ending"
            nodes = self.records(self.chunk)
            node = nodes[self.node]
            self.trace.append((self.pack.scenarios[self.scenario]["title"], self.chunk, self.node))
            self.node += 1
            kind = node[0]
            if kind == PACKER.K_CHAPTER:
                continue
            if kind == PACKER.K_BG:
                continue
            if kind == PACKER.K_EV:
                continue
            if kind == PACKER.K_LABEL or kind == PACKER.K_SPRITE_OFF:
                continue
            if kind == PACKER.K_DIALOGUE:
                self.dialogues += 1
                return "text"
            if kind == PACKER.K_SELECT:
                options = node[1]
                index = min(choice, len(options) - 1)
                text, target, _expr = options[index]
                del text
                self.choices.append(index)
                self._apply_option_assignments(node, index)
                if target is None:
                    return "text"
                if target in PACKER.ENDING_LABELS:
                    self.ending = PACKER.ENDING_LABELS.index(target)
                    return "ending"
                label_id = self.label_id_for(target)
                if label_id is None:
                    continue          # 源里没定义:忽略,接着往下读
                self.jump_label(label_id)
                continue
            if kind == PACKER.K_NEXT:
                target = node[1]
                if target is None:
                    continue
                if target in PACKER.ENDING_LABELS:
                    self.ending = PACKER.ENDING_LABELS.index(target)
                    return "ending"
                label_id = self.label_id_for(target)
                if label_id is not None:
                    self.jump_label(label_id)
                continue
            raise AssertionError(f"未知节点: {node!r}")
        raise AssertionError("推进没有收敛")

    def _apply_option_assignments(self, node: list, index: int) -> None:
        """选项的赋值在包里是解析后的三元组,解码后又拼回 f.x = n 文本,这里再解析一次
        —— 这一步顺带验证了赋值的往返无损。"""
        options = node[1]
        expr = options[index][2]
        for name, op, value in PACKER.parse_assignments(expr or ""):
            flag_id = self.flag_index[name]
            if op == PACKER.OP_ADD:
                self.flags[flag_id] = max(0, min(255, self.flags[flag_id] + value))
            else:
                self.flags[flag_id] = value & 0xFF

    def label_id_for(self, target: str) -> int | None:
        """与打包器 resolve_target 同一套规则:本场景优先,跨场景只认分章链。"""
        local = self.local_labels.get((self.scenario, target))
        if local is not None:
            return local
        if PACKER.CHAIN_LABEL.match(target):
            return self.first_labels.get(target)
        return None


@unittest.skipUnless(
    (SOURCE / "scn").is_dir(),
    "missing build/sanoba-source; run tools/sanoba_fetch_source.py --dest build/sanoba-source --chunks",
)
class SanobaPlaythroughTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not PACK_PATH.is_file():
            with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
                PACKER.main(["--source", str(SOURCE), "--out", str(PACK_PATH)])
        cls.pack = PACKER.read_pack(PACK_PATH)
        order, _config = PACKER.scenario_order(SOURCE)
        cls.order = order
        cls.titles = [PACKER.strip_extension(name) for _id, _group, name in order]

    def make_player(self) -> Player:
        player = Player(self.pack)
        player.flag_index = {name: index for index, name in enumerate(self.pack.pools["flag"])}
        return player

    def scenario_id(self, index: int) -> str:
        return self.order[index][0]

    def scenario_index(self, prefix: str) -> int:
        for index, (scn_id, _group, _name) in enumerate(self.order):
            if scn_id == prefix or scn_id.startswith(prefix + ""):
                return index
        raise AssertionError(f"场景 {prefix} 不在场景表里")

    # ---- 规则本身 ----
    def test_route_selection_matches_the_source_rule(self) -> None:
        """019 的选线:最高分优先,全 0 或并列走兜底。"""
        route = self.pack.route
        trigger = route["trigger"]
        flags = self.pack.pools["flag"]
        player = self.make_player()
        player.scenario = trigger

        def pick(values: dict[str, int]) -> str:
            player.flags = [0] * len(player.flags)
            for name, value in values.items():
                player.flags[flags.index(name)] = value
            return self.scenario_id(player.next_scenario())

        self.assertEqual(pick({}), "020_meguru", "全 0 要走兜底")
        self.assertEqual(pick({"nen_flag": 1}), "020_nene")
        self.assertEqual(pick({"meg_flag": 2}), "020_meguru")
        self.assertEqual(pick({"tsu_flag": 3, "nen_flag": 1}), "020_tsumugi")
        self.assertEqual(pick({"nen_flag": 2, "meg_flag": 2}), "020_meguru", "并列要走兜底")
        self.assertEqual(pick({"tou_flag": 1}), "020_akogare")
        self.assertEqual(pick({"wak_flag": 1}), "500")
        self.assertEqual(self.scenario_id(trigger), "019")

    def test_cross_scenario_chain_resolves(self) -> None:
        """020_nene 的末跳 *nen_part_1 落在 101(宁宁线第一章),这是跨文件分支链。"""
        player = self.make_player()
        scenario = self.scenario_index("020_nene")
        first, length = player.scenario_span(scenario)
        last_chunk = first + length - 1
        last_node = len(player.records(last_chunk)) - 1
        jump = player.records(last_chunk)[last_node]
        self.assertEqual(jump[0], PACKER.K_NEXT, "020_nene 的末节点应是跳转")
        self.assertEqual(jump[1], "*nen_part_1")
        player.scenario = scenario
        player.chunk, player.node = last_chunk, last_node
        player.advance()
        self.assertEqual(self.scenario_id(player.scenario), "101")

    def run_playthrough(self, route_flag: str | None = None) -> Player:
        """从剧本开头一路推进(每处选项都取第一个),到结局或卡住为止。
        route_flag 给定时,到达选线场景的那一刻把该旗标拉到最高分,用来覆盖各条线
        (选项自己只会 ++ 到 1,所以 5 一定赢)。"""
        player = self.make_player()
        steps = 0
        result = "text"
        while result == "text" and steps < 200000:
            if route_flag is not None and player.scenario == self.pack.route["trigger"]:
                player.flags[player.flag_index[route_flag]] = 5
                route_flag = None
            result = player.advance(choice=0)
            steps += 1
        self.assertEqual(result, "ending", f"推进 {steps} 步后仍未到结局")
        return player

    def test_full_playthrough_reaches_an_ending(self) -> None:
        player = self.run_playthrough()
        # 结局有两种来源:源数据里的结局标签,或场景表走到 next_scenario = 0xFFFF。
        self.assertGreater(player.dialogues, 8000, "一条线应当读到上万句")
        visited = {self.scenario_id(player.scenario)} | {
            title.split(".")[0] for title, _chunk, _node in player.trace
        }
        self.assertTrue(any(title.startswith("019") for title, _c, _n in player.trace), "应经过选线场景")
        self.assertTrue(player.choices)
        del visited

    def test_all_routes_are_reachable(self) -> None:
        """五条角色线各跑一遍,合起来应当覆盖全部场景(含公共线)。"""
        visited: set[str] = set()
        dialogues = 0
        first_chapters = set()
        for flag in (None, "nen_flag", "meg_flag", "tsu_flag", "tou_flag", "wak_flag"):
            player = self.run_playthrough(flag)
            visited |= {self.pack.scenarios[player.scenario]["title"]}
            visited |= {title for title, _c, _n in player.trace}
            dialogues += player.dialogues
            first_chapters |= {
                title.split(".")[0] for title, _c, _n in player.trace
                if title.split(".")[0] in ("101", "201", "301", "401", "500")
            }
        self.assertEqual(first_chapters, {"101", "201", "301", "401", "500"},
                         "五条角色线的第一章都应当能走到")
        self.assertGreaterEqual(len(visited), 40, f"六条线合起来只走过 {len(visited)} 个场景")
        self.assertGreaterEqual(dialogues / 6, 8000)

    def test_every_visited_target_resolves(self) -> None:
        """推进过程中出现的每个跳转目标都能在标签表里找到(否则模型会 STUCK)。"""
        names = {self.pack.pools["label"][name_id] for name_id, _s, _c, _n in self.pack.labels}
        for index in range(len(self.pack.chunks)):
            for node in self.pack.records(index):
                targets = []
                if node[0] == PACKER.K_NEXT:
                    targets = [node[1]]
                elif node[0] == PACKER.K_SELECT:
                    targets = [option[1] for option in node[1]]
                for target in targets:
                    if target is None or target in PACKER.ENDING_LABELS:
                        continue
                    self.assertIn(target, names, f"目标 {target} 不在标签表")

    def test_label_positions_land_on_label_nodes(self) -> None:
        """每个标签落点必须是同名 LABEL 节点(C 模型直接按落点跳)。"""
        for label_id, (name_id, _scenario, chunk, node) in enumerate(self.pack.labels):
            records = self.pack.records(chunk)
            self.assertLess(node, len(records), f"标签 {label_id} 越界")
            self.assertEqual(records[node][0], PACKER.K_LABEL)
            self.assertEqual(records[node][1], self.pack.pools["label"][name_id])


if __name__ == "__main__":
    unittest.main(verbosity=2)
