#!/usr/bin/env python3
"""Round-trip contract for the Senren * Banka script pack (SENRSCN1).

CI has no third-party source material, so the pack tests skip unless the
script chunks have been fetched:

  python tools/senren_fetch_source.py --dest build/senren-source --chunks
  python3 tests/test_senren_scn_pack.py
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
SOURCE = ROOT / "build" / "senren-source"
SCN_DIR = SOURCE / "scn"

SPEC = importlib.util.spec_from_file_location(
    "senren_scn_pack", ROOT / "tools" / "senren_scn_pack.py"
)
assert SPEC and SPEC.loader
PACK = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PACK
SPEC.loader.exec_module(PACK)

EXPECTED_NODES = 74504


def chunks() -> list[list]:
    """源剧本按 chunk001…chunkNNN 顺序读出。"""
    paths = sorted(SCN_DIR.glob("chunk*.txt"))
    return [json.loads(path.read_text(encoding="utf-8")) for path in paths]


@unittest.skipUnless(
    SCN_DIR.is_dir(),
    "missing build/senren-source/scn; run tools/senren_fetch_source.py --dest build/senren-source --chunks",
)
class SenrenScriptPackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="senren-scn-pack-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.out = Path(cls.temporary.name) / "senren_scn.bin"
        cls.listing = Path(cls.temporary.name) / "senren_scn.json"
        # CLI 会往 stderr 打报告;这里吞掉,保持测试输出干净
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            cls.build_code = PACK.main([
                "--source", str(SOURCE),
                "--out", str(cls.out),
                "--json", str(cls.listing),
            ])
        cls.pack = PACK.read_pack(cls.out)
        cls.source = chunks()

    def test_build_and_self_check_pass(self) -> None:
        self.assertEqual(self.build_code, 0)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(PACK.check_pack(self.out), 0)
            self.assertEqual(PACK.main(["--check", str(self.out)]), 0)

    def test_chunk_count_matches_source(self) -> None:
        self.assertEqual(len(self.pack.chunks), len(self.source))
        self.assertEqual(len(self.pack.chunks), 112)

    def test_node_count_is_exact(self) -> None:
        self.assertEqual(sum(info.node_count for info in self.pack.chunks), EXPECTED_NODES)
        # 每块表里的 node_count 必须与解出来的记录数一致
        for index, info in enumerate(self.pack.chunks):
            self.assertEqual(len(self.pack.records(index)), info.node_count)

    def test_round_trip_is_node_by_node_identical(self) -> None:
        mismatches: list[str] = []
        for index, nodes in enumerate(self.source):
            decoded = self.pack.records(index)
            self.assertEqual(len(decoded), len(nodes), f"chunk{index + 1:03d} 节点数不符")
            for position, (wanted, actual) in enumerate(zip(nodes, decoded)):
                if wanted != actual:
                    mismatches.append(
                        f"chunk{index + 1:03d}[{position}] {wanted!r} != {actual!r}"
                    )
                    if len(mismatches) >= 10:
                        break
            if len(mismatches) >= 10:
                break
        self.assertEqual(mismatches, [], "节点无法逐节点还原")

    def test_dialogue_text_and_sprite_fields_are_exact(self) -> None:
        dialogue = 0
        exact_text = 0
        exact_sprite = 0
        with_sprite = 0
        for index, nodes in enumerate(self.source):
            decoded = self.pack.records(index)
            for wanted, actual in zip(nodes, decoded):
                if wanted[0] != 3:
                    continue
                dialogue += 1
                if wanted[2].encode("utf-8") == actual[2].encode("utf-8"):
                    exact_text += 1
                if len(wanted) > 3:
                    with_sprite += 1
                    if wanted[3] == actual[3]:
                        exact_sprite += 1
        self.assertEqual(dialogue, 55726)
        self.assertEqual(exact_text, dialogue, "对白文本不是逐字节一致")
        self.assertEqual(exact_sprite, with_sprite, "立绘键/动作不能还原")

    def test_labels_chapters_events_and_flags_are_preserved(self) -> None:
        labels = 0
        chapters = set()
        events = 0
        event_clears = 0
        for nodes in self.source:
            labels += sum(1 for node in nodes if node[0] == 0)
            chapters.update(node[1] for node in nodes if node[0] == 1)
            for node in nodes:
                if node[0] == 5:
                    events += 1
                    event_clears += node[1] is None
        # 源里量出的冻结基数
        self.assertEqual(labels, 149)
        self.assertEqual(len(chapters), 45)
        self.assertEqual(events, 14569)
        self.assertEqual(event_clears, 597)
        # 解出来的标签/章节集合必须与源完全一致
        decoded_labels: set[int] = set()
        decoded_chapters: set[str] = set()
        for index in range(len(self.pack.chunks)):
            for node in self.pack.records(index):
                if node[0] == 0:
                    decoded_labels.add(int(node[1][2:]))
                elif node[0] == 1:
                    decoded_chapters.add(node[1])
        source_labels = {int(node[1][2:]) for nodes in self.source for node in nodes if node[0] == 0}
        self.assertEqual(decoded_labels, source_labels)
        self.assertEqual(decoded_chapters, chapters)
        self.assertEqual(len(self.pack.pools["speaker"]), 158)
        self.assertEqual(len(self.pack.pools["sprite"]), 123)
        self.assertEqual(len(self.pack.pools["event"]), 570)
        self.assertEqual(len(self.pack.pools["background"]), 92)
        self.assertEqual(len(self.pack.pools["ending"]), 8)
        self.assertEqual(len(self.pack.flags), 44)

    def test_char_table_is_a_bijection(self) -> None:
        pack = self.pack
        self.assertEqual(len(pack.char_table), 3455)
        index = {point: position for position, point in enumerate(pack.char_table)}
        for position, point in enumerate(pack.char_table):
            self.assertEqual(PACK.decode_text([position], pack.char_table), chr(point))
        # 名字表与对白原串编码再解码必须回到原串(不做任何归一化)
        samples = [name for block in PACK.NAME_BLOCKS for name in pack.pools[block]]
        samples += [
            node[2]
            for nodes in self.source[:3]
            for node in nodes
            if node[0] == 3
        ]
        for text in samples:
            codes = PACK.encode_text(text, index)
            self.assertEqual(PACK.decode_text(codes, pack.char_table), text)

    def test_output_is_deterministic_and_within_budget(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            second = Path(self.temporary.name) / "again.bin"
            PACK.main(["--source", str(SOURCE), "--out", str(second)])
        self.assertEqual(self.out.read_bytes(), second.read_bytes(), "两次构建结果不同")
        size = self.out.stat().st_size
        self.assertGreaterEqual(size, 1_150_000)
        self.assertLessEqual(size, 1_300_000)

    def test_metadata_records_the_provenance(self) -> None:
        meta = self.pack.meta
        self.assertEqual(meta["format"], "SENRSCN1")
        self.assertEqual(meta["version"], "1")
        self.assertEqual(meta["chunks"], "112")
        self.assertEqual(meta["nodes"], str(EXPECTED_NODES))
        self.assertEqual(meta["flags"], "44")
        self.assertEqual(meta["labels"], "149")
        self.assertEqual(meta["name_blocks"], "speaker,sprite,event,background,ending")
        self.assertIn("Senren-Banka-MiBand-10", meta["source_repo"])
        listing = json.loads(self.listing.read_text(encoding="utf-8"))
        self.assertEqual(listing["counts"]["nodes"], EXPECTED_NODES)
        self.assertEqual(listing["counts"]["nodes_by_kind"]["DIALOGUE"], 55726)
        self.assertEqual(listing["counts"]["name_blocks"]["ending"], 8)


if __name__ == "__main__":
    unittest.main()
