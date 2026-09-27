#!/usr/bin/env python3
"""Round-trip contract for the Sanoba Witch script pack (SANOSCN1).

CI has no third-party source material, so the pack tests skip unless the
script has been fetched:

  python tools/sanoba_fetch_source.py --dest build/sanoba-source --chunks
  python3 tests/test_sanoba_scn_pack.py
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "build" / "sanoba-source"
SCN_DIR = SOURCE / "scn"

SPEC = importlib.util.spec_from_file_location("sanoba_scn_pack", ROOT / "tools" / "sanoba_scn_pack.py")
assert SPEC and SPEC.loader
PACK = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PACK
SPEC.loader.exec_module(PACK)

EXPECTED_SCENARIOS = 101
EXPECTED_NODES = 61382
EXPECTED_DIALOGUE = 53190
EXPECTED_LABELS = 555
EXPECTED_FLAGS = 8


def source_scenarios() -> list[list]:
    """按 game.txt 顺序读出全部剧本节点。"""
    order, _config = PACK.scenario_order(SOURCE)
    out = []
    for _scn_id, _group, file_name in order:
        out.append(json.loads((SCN_DIR / file_name).read_text(encoding="utf-8")))
    return out


def normalize_source_node(node: list, scenario_index: int, is_last: bool, story: "PACK.Story",
                          keep_sprites: bool) -> list:
    """把源节点规整成解码后的形状:选项表达式比较解析结果,未定义目标按 None。"""
    kind = node[0]
    allow_cross = is_last

    def target_of(name: str):
        kind_, _label = PACK.resolve_target(story, scenario_index, name, allow_cross)
        return None if kind_ == PACK.TARGET_IGNORED else name

    if kind == PACK.K_DIALOGUE:
        out = [kind, node[1], node[2]]
        if len(node) > 3 and node[3] and keep_sprites:
            out.append([[str(f) for f in sprite] for sprite in node[3]])
        return out
    if kind == PACK.K_SELECT:
        options = []
        for option in node[1]:
            assignments = PACK.parse_assignments(str(option[2])) if len(option) > 2 and option[2] else []
            options.append([str(option[0]), target_of(str(option[1])), PACK.format_assignments(assignments)])
        return [kind, options]
    if kind == PACK.K_NEXT:
        out = [kind, target_of(str(node[1]))]
        if len(node) > 2 and node[2]:
            out.append(PACK.format_conditions(PACK.parse_condition(str(node[2]))))
        return out
    return list(node)


def normalize_decoded_node(node: list) -> list:
    """解码侧:忽略型目标在源里就是"未找到标签",两边都视作 None。"""
    return node


@unittest.skipUnless(
    SCN_DIR.is_dir() and (SOURCE / "game.txt").is_file(),
    "missing build/sanoba-source; run tools/sanoba_fetch_source.py --dest build/sanoba-source --chunks",
)
class SanobaScriptPackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="sanoba-scn-pack-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.out = Path(cls.temporary.name) / "sanoba_scn.bin"
        cls.listing = Path(cls.temporary.name) / "sanoba_scn.json"
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            cls.build_code = PACK.main([
                "--source", str(SOURCE),
                "--out", str(cls.out),
                "--json", str(cls.listing),
            ])
        cls.pack = PACK.read_pack(cls.out)
        cls.scenarios = source_scenarios()
        cls.story, _notes = PACK.scan_source(SOURCE)

    # ---- 构建与结构 ----
    def test_build_and_self_check_pass(self) -> None:
        self.assertEqual(self.build_code, 0, "打包器返回非零")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(PACK.check_pack(self.out), 0, "只读自检未通过")

    def test_structure_has_no_problems(self) -> None:
        self.assertEqual(self.pack.validate_structure(), [])

    def test_scenario_table_matches_source_order(self) -> None:
        order, _ = PACK.scenario_order(SOURCE)
        self.assertEqual(len(self.pack.scenarios), EXPECTED_SCENARIOS)
        self.assertEqual([PACK.strip_extension(name) for _id, _g, name in order],
                         [scenario["title"] for scenario in self.pack.scenarios])
        self.assertEqual(sum(scenario["chunk_count"] for scenario in self.pack.scenarios), len(self.pack.chunks))

    def test_node_count_is_exact(self) -> None:
        self.assertEqual(sum(len(nodes) for nodes in self.scenarios), EXPECTED_NODES)
        decoded = sum(len(self.pack.scenario_nodes(scenario)) for scenario in self.pack.scenarios)
        self.assertEqual(decoded, EXPECTED_NODES)

    # ---- 回环 ----
    def test_round_trip_is_node_by_node_identical(self) -> None:
        problems = []
        for scenario_index, scenario in enumerate(self.pack.scenarios):
            source = self.scenarios[scenario_index]
            decoded = self.pack.scenario_nodes(scenario)
            if len(source) != len(decoded):
                problems.append(f"{scenario['title']}: 节点数 {len(decoded)} != {len(source)}")
                continue
            for number, (before, after) in enumerate(zip(source, decoded)):
                expected = normalize_source_node(before, scenario_index, number == len(source) - 1,
                                                self.story, keep_sprites=False)
                got = normalize_decoded_node(after)
                if expected != got:
                    problems.append(f"{scenario['title']} 第 {number} 个节点: {expected!r} != {got!r}")
                    break
        self.assertEqual(problems, [])

    def test_dialogue_text_is_byte_identical(self) -> None:
        source_text = []
        decoded_text = []
        for scenario in self.scenarios:
            source_text.extend(node[2] for node in scenario if node[0] == PACK.K_DIALOGUE)
        for entry in self.pack.scenarios:
            decoded_text.extend(node[2] for node in self.pack.scenario_nodes(entry) if node[0] == PACK.K_DIALOGUE)
        self.assertEqual(len(source_text), EXPECTED_DIALOGUE)
        self.assertEqual(decoded_text, source_text)
        self.assertEqual(sum(len(text) for text in source_text), 1123521)

    def test_keep_sprites_round_trip(self) -> None:
        out = Path(self.temporary.name) / "with_sprites.bin"
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            code = PACK.main(["--source", str(SOURCE), "--out", str(out), "--keep-sprites"])
        self.assertEqual(code, 0)
        pack = PACK.read_pack(out)
        self.assertEqual(pack.meta["sprites"], "kept")
        sprites = 0
        for scenario_index, scenario in enumerate(pack.scenarios):
            source = self.scenarios[scenario_index]
            decoded = pack.scenario_nodes(scenario)
            for before, after in zip(source, decoded):
                if before[0] == PACK.K_DIALOGUE and len(before) > 3 and before[3]:
                    self.assertEqual([list(sprite) for sprite in before[3]], after[3])
                    sprites += len(before[3])
        self.assertEqual(sprites, 23594)
        self.assertGreater(out.stat().st_size, 0)

    # ---- 资源与标签 ----
    def test_labels_resolve_to_their_own_node(self) -> None:
        self.assertEqual(len(self.pack.labels), EXPECTED_LABELS)
        for label_id, (name_id, _scenario, chunk, node) in enumerate(self.pack.labels):
            records = self.pack.records(chunk)
            self.assertLess(node, len(records), f"标签 {label_id} 落点越界")
            landed = records[node]
            self.assertEqual(landed[0], PACK.K_LABEL, f"标签 {label_id} 落点不是 LABEL 节点")
            self.assertEqual(landed[1], self.pack.pools["label"][name_id])

    def test_label_targets_point_at_real_label_nodes(self) -> None:
        """每个被引用的标签目标都能真的跳过去:落点必须是同名 LABEL 节点。"""
        names = {self.pack.pools["label"][name_id] for name_id, _s, _c, _n in self.pack.labels}
        for scenario in self.pack.scenarios:
            decoded = self.pack.scenario_nodes(scenario)
            for node in decoded:
                targets = []
                if node[0] == PACK.K_SELECT:
                    targets = [option[1] for option in node[1]]
                elif node[0] == PACK.K_NEXT:
                    targets = [node[1]]
                for target in targets:
                    if target is None or target in PACK.ENDING_LABELS:
                        continue
                    self.assertIn(target, names, f"跳转目标 {target} 不在标签表里")
        self.assertTrue(names)

    def test_events_and_backgrounds_are_preserved(self) -> None:
        events = self.pack.pools["event"]
        self.assertEqual(len(events), 1501)
        self.assertEqual(self.pack.pools["background"], sorted({
            str(node[1]) for scenario in self.scenarios for node in scenario if node[0] == PACK.K_BG
        }))
        decoded_events = []
        for scenario in self.pack.scenarios:
            decoded_events.extend(
                node[1] for node in self.pack.scenario_nodes(scenario) if node[0] == PACK.K_EV
            )
        self.assertEqual(len(decoded_events), 3581)
        self.assertEqual(sum(1 for name in decoded_events if name is None), 245)

    def test_flags_and_route_are_packed(self) -> None:
        self.assertEqual(len(self.pack.pools["flag"]), EXPECTED_FLAGS)
        self.assertEqual(sorted(self.pack.pools["flag"]),
                         ["eye_flag", "glass_flag", "meg_flag", "nen_flag", "sel_flag", "tou_flag", "tsu_flag", "wak_flag"])
        route = self.pack.route
        self.assertEqual(len(route["targets"]), 5)
        order, _config = PACK.scenario_order(SOURCE)
        self.assertEqual(order[route["trigger"]][0], "019")
        self.assertEqual(order[route["fallback"]][0], "020_meguru")
        flags = {self.pack.pools["flag"][flag_id] for flag_id, _scenario in route["targets"]}
        self.assertEqual(flags, {"nen_flag", "meg_flag", "tsu_flag", "tou_flag", "wak_flag"})
        self.assertEqual(order[route["targets"][0][1]][0], "020_nene" if flags else "")

    def test_char_table_is_a_bijection(self) -> None:
        table = self.pack.char_table
        self.assertEqual(len(table), 3445)
        self.assertEqual(len(set(table)), len(table))
        self.assertTrue(all(0 < code <= 0x10FFFF for code in table))
        self.assertEqual(table[:5], [ord("…"), ord("，"), ord("「"), ord("」"), ord("的")])

    # ---- 产物特性 ----
    def test_output_is_deterministic_and_within_budget(self) -> None:
        again = Path(self.temporary.name) / "again.bin"
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            PACK.main(["--source", str(SOURCE), "--out", str(again)])
        self.assertEqual(again.read_bytes(), self.out.read_bytes(), "两次打包结果不一致")
        size = self.out.stat().st_size
        self.assertLess(size, int(1.60 * 1048576), f"剧本包 {size} 字节超出 1.60 MB 预算")
        raw = sum(entry["raw_len"] for entry in self.pack.chunks)
        packed = sum(entry["data_len"] for entry in self.pack.chunks)
        self.assertGreater(raw / packed, 1.5, "压缩率低于预期")
        for entry in self.pack.chunks:
            self.assertLessEqual(entry["raw_len"], 2 + 20000, "单块超过 raw_limit")
            self.assertLessEqual(entry["raw_len"], 32768, "单块超过固件 32 KB 解压缓冲")

    def test_metadata_records_the_provenance(self) -> None:
        meta = self.pack.meta
        self.assertEqual(meta["format"], "SANOSCN1")
        self.assertEqual(meta["source_repo"], "https://github.com/hrk666666/Sanoba-Witch-MiBand-10")
        self.assertEqual(meta["nodes"], str(EXPECTED_NODES))
        self.assertEqual(meta["dialogue"], str(EXPECTED_DIALOGUE))
        self.assertEqual(meta["characters"], str(len(self.pack.char_table)))
        self.assertEqual(meta["labels"], str(EXPECTED_LABELS))
        self.assertEqual(meta["flags"], str(EXPECTED_FLAGS))
        self.assertEqual(meta["sprites"], "dropped")
        self.assertTrue(meta["target_enum"].startswith("0=label,1=ignored,2=ending"))
        self.assertEqual(meta["event_clear"], "245")

    def test_json_listing_is_written(self) -> None:
        listing = json.loads(self.listing.read_text(encoding="utf-8"))
        self.assertEqual(len(listing["scenarios"]), EXPECTED_SCENARIOS)
        self.assertEqual(len(listing["chunks"]), len(self.pack.chunks))
        self.assertEqual(len(listing["labels"]), EXPECTED_LABELS)
        self.assertEqual(listing["scenarios"][0]["id"], "001")


if __name__ == "__main__":
    unittest.main(verbosity=2)
