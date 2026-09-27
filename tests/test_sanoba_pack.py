#!/usr/bin/env python3
"""Contract for the Sanoba Witch image pack (SANOBPK1).

CI has no third-party source material, so the pack tests skip unless the art has
been fetched:

  python tools/sanoba_fetch_source.py --dest build/sanoba-source
  python3 tests/test_sanoba_pack.py
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

SPEC = importlib.util.spec_from_file_location("sanoba_pack", ROOT / "tools" / "sanoba_pack.py")
assert SPEC and SPEC.loader
PACK = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PACK
SPEC.loader.exec_module(PACK)

EXPECTED_BG = 108          # 107 背景 + 标题画面
EXPECTED_SD = 292
EXPECTED_ENTRIES = EXPECTED_BG + EXPECTED_SD
JPEG_MAGIC = b"\xff\xd8"


@unittest.skipUnless(
    (SOURCE / "bg").is_dir() and (SOURCE / "sd").is_dir(),
    "missing build/sanoba-source; run tools/sanoba_fetch_source.py --dest build/sanoba-source",
)
class SanobaImagePackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="sanoba-pack-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.out = Path(cls.temporary.name) / "sanoba_pack.bin"
        cls.listing = Path(cls.temporary.name) / "sanoba_pack.json"
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            cls.build_code = PACK.main([
                "--source", str(SOURCE),
                "--out", str(cls.out),
                "--json", str(cls.listing),
            ])
        cls.pack = PACK.read_pack(cls.out)

    # ---- 构建与结构 ----
    def test_build_and_self_check_pass(self) -> None:
        self.assertEqual(self.build_code, 0, "打包器返回非零")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(PACK.check_pack(self.out), 0, "只读自检未通过")
        self.assertEqual(self.pack.validate_structure(), [])

    def test_entry_mix(self) -> None:
        self.assertEqual(len(self.pack.entries), EXPECTED_ENTRIES)
        kinds: dict[int, int] = {}
        pools: dict[int, int] = {}
        for entry in self.pack.entries:
            kinds[entry["kind"]] = kinds.get(entry["kind"], 0) + 1
            pools[entry["pool"]] = pools.get(entry["pool"], 0) + 1
        self.assertEqual(kinds, {PACK.KIND_BG: EXPECTED_BG, PACK.KIND_SD: EXPECTED_SD})
        self.assertEqual(pools, {PACK.POOL_BG: EXPECTED_BG, PACK.POOL_EV: EXPECTED_SD})

    def test_layout_matches_the_sibling_pack(self) -> None:
        """头/段/条目三张结构与 SENRNPK2 字节兼容,所以那层解析可以直接照搬。"""
        self.assertEqual(PACK.HEADER.size, 20)
        self.assertEqual(PACK.SECTION.size, 16)
        self.assertEqual(PACK.ENTRY.size, 36)
        self.assertEqual(PACK.MAGIC, b"SANOBPK1")
        self.assertEqual(PACK.VERSION, 1)
        # 段顺序与 SENRNPK2 相同:NAME / ASSET / BLOB / META
        offsets = [self.pack.sections[key][0] for key in (PACK.SEC_NAME, PACK.SEC_ASSET, PACK.SEC_BLOB, PACK.SEC_META)]
        self.assertEqual(offsets, sorted(offsets))

    def test_entries_are_sorted_for_binary_search(self) -> None:
        keys = [(entry["pool"], entry["name"]) for entry in self.pack.entries]
        self.assertEqual(keys, sorted(keys))
        self.assertEqual(len(set(keys)), len(keys))
        # 二分查找与线性查找一致
        for entry in self.pack.entries:
            found = self.pack.find(entry["pool"], entry["name"])
            self.assertIsNotNone(found)
            self.assertEqual(found["index"], entry["index"])
        self.assertIsNone(self.pack.find(PACK.POOL_BG, "没有这张背景"))

    def test_names_are_nul_terminated_and_in_bounds(self) -> None:
        for entry in self.pack.entries:
            encoded = entry["name"].encode("utf-8")
            offset = entry["name_off"]
            length = entry["name_len"]
            self.assertEqual(self.pack.names[offset:offset + length], encoded)
            self.assertEqual(length, len(encoded))
            self.assertEqual(self.pack.names[offset + length], 0, f"{entry['name']} 后面没有 NUL")

    def test_blob_is_the_concatenation_of_payloads(self) -> None:
        cursor = 0
        for entry in self.pack.entries:
            if entry["kind"] == PACK.KIND_MISSING:
                continue
            self.assertEqual(entry["data_off"], cursor, f"{entry['name']} 的载荷不是紧排的")
            cursor += entry["data_len"]
        self.assertEqual(cursor, len(self.pack.payload))

    # ---- 几何与载荷 ----
    def test_backgrounds_are_native_geometry_jpeg(self) -> None:
        from PIL import Image

        for entry in self.pack.entries:
            if entry["kind"] != PACK.KIND_BG:
                continue
            self.assertEqual((entry["w"], entry["h"]), (PACK.SCREEN_W, PACK.SCREEN_H))
            self.assertEqual((entry["dx"], entry["dy"]), (0, 0))
            payload = self.pack.data(entry)
            self.assertTrue(payload.startswith(JPEG_MAGIC), f"{entry['name']} 不是 JPEG")
            self.assertEqual(Image.open(io.BytesIO(payload)).size, (PACK.SCREEN_W, PACK.SCREEN_H))

    def test_sd_entries_are_verbatim_and_full_width(self) -> None:
        from PIL import Image

        sizes: dict[tuple[int, int], int] = {}
        for entry in self.pack.entries:
            if entry["kind"] != PACK.KIND_SD:
                continue
            self.assertEqual(entry["w"], PACK.SCREEN_W)
            self.assertLessEqual(entry["h"], PACK.SD_H)
            self.assertEqual(entry["dx"], 0)
            self.assertEqual(entry["dy"], PACK.SD_Y)
            payload = self.pack.data(entry)
            # 约定:SD 原样入库,字节必须与源文件一致
            self.assertEqual(payload, (SOURCE / "sd" / f"{entry['name']}.jpg").read_bytes())
            self.assertEqual(Image.open(io.BytesIO(payload)).size, (entry["w"], entry["h"]))
            sizes[(entry["w"], entry["h"])] = sizes.get((entry["w"], entry["h"]), 0) + 1
        self.assertEqual(sizes, {(240, 144): 291, (240, 136): 1})

    def test_title_entry_is_in_the_background_pool(self) -> None:
        title = self.pack.find(PACK.POOL_BG, PACK.TITLE_ART_NAME)
        self.assertIsNotNone(title, "标题图条目缺失")
        self.assertEqual(title["kind"], PACK.KIND_BG)
        self.assertEqual((title["w"], title["h"]), (PACK.SCREEN_W, PACK.SCREEN_H))

    # ---- 与剧本的交叉契约 ----
    def test_every_referenced_background_exists(self) -> None:
        scn_dir = SOURCE / "scn"
        if not scn_dir.is_dir():
            self.skipTest("没有剧本,跳过交叉检查")
        referenced = set()
        for path in sorted(scn_dir.glob("*.ks.txt")):
            for node in json.loads(path.read_text(encoding="utf-8")):
                if node and node[0] == 2 and len(node) > 1 and node[1]:
                    referenced.add(str(node[1]))
        missing = sorted(name for name in referenced if self.pack.find(PACK.POOL_BG, name) is None)
        self.assertEqual(missing, [], f"剧本引用了 {len(missing)} 张包里没有的背景")

    def test_every_referenced_sd_exists(self) -> None:
        scn_dir = SOURCE / "scn"
        if not scn_dir.is_dir():
            self.skipTest("没有剧本,跳过交叉检查")
        referenced = set()
        for path in sorted(scn_dir.glob("*.ks.txt")):
            for node in json.loads(path.read_text(encoding="utf-8")):
                if node and node[0] == 5 and len(node) > 1 and node[1] and str(node[1]).startswith("sd"):
                    referenced.add(str(node[1]))
        missing = sorted(name for name in referenced if self.pack.find(PACK.POOL_EV, name) is None)
        self.assertEqual(missing, [], f"剧本引用了 {len(missing)} 张包里没有的 SD")
        self.assertEqual(len(referenced), EXPECTED_SD)

    def test_missing_placeholders_are_opt_in(self) -> None:
        refs = PACK.read_script_refs(SOURCE)
        absent = [name for name in refs["ev"] if self.pack.find(PACK.POOL_EV, name) is None]
        self.assertGreater(len(absent), 1000, "源工程缺的事件图应超过 1000 个")
        self.assertIn("ev501ag", absent)
        self.assertIn("画面_黒", absent)
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            code = PACK.main(["--source", str(SOURCE), "--out", str(Path(self.temporary.name) / "ph.bin"),
                              "--missing-placeholders"])
        self.assertEqual(code, 0)
        pack = PACK.read_pack(Path(self.temporary.name) / "ph.bin")
        placeholders = [entry for entry in pack.entries if entry["kind"] == PACK.KIND_MISSING]
        self.assertEqual(len(placeholders), len(absent))
        for entry in placeholders:
            self.assertEqual(entry["data_len"], 0)
        self.assertEqual(pack.meta["missing_placeholders"], "yes")

    # ---- 质量与产物特性 ----
    def test_compare_verifies_transcodes_and_sd_bytes(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()) as buffer:
            code = PACK.compare_pack(self.out, SOURCE)
        self.assertEqual(code, 0)
        text = buffer.getvalue()
        self.assertIn("108 张转码图", text)
        self.assertIn("292 张 SD", text)

    def test_output_is_deterministic_and_within_budget(self) -> None:
        again = Path(self.temporary.name) / "again.bin"
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            PACK.main(["--source", str(SOURCE), "--out", str(again)])
        self.assertEqual(again.read_bytes(), self.out.read_bytes(), "两次打包结果不一致")
        size = self.out.stat().st_size
        self.assertLess(size, int(3.40 * 1048576), f"图片包 {size} 字节超出 3.40 MB 预算")
        # SD 原样入库,不应该比源素材更大
        sd_bytes = sum(entry["data_len"] for entry in self.pack.entries if entry["kind"] == PACK.KIND_SD)
        source_sd = sum(path.stat().st_size for path in (SOURCE / "sd").glob("*.jpg"))
        self.assertEqual(sd_bytes, source_sd)

    def test_metadata_records_the_provenance(self) -> None:
        meta = self.pack.meta
        self.assertEqual(meta["format"], "SANOBPK1")
        self.assertEqual(meta["source_repo"], "https://github.com/hrk666666/Sanoba-Witch-MiBand-10")
        self.assertEqual(meta["entries"], str(EXPECTED_ENTRIES))
        self.assertEqual(meta["bg_quality"], str(PACK.DEFAULT_BG_QUALITY))
        self.assertEqual(meta["sd_encoding"], "jpeg-verbatim")
        self.assertEqual(meta["sd_rect"], "0,24,240,144")
        self.assertEqual(meta["sd_sizes"], "240x136:1,240x144:291")
        self.assertEqual(meta["kind_counts"], "BG=108,SD=292")

    def test_json_listing_is_written(self) -> None:
        listing = json.loads(self.listing.read_text(encoding="utf-8"))
        self.assertEqual(len(listing["entries"]), EXPECTED_ENTRIES)
        self.assertEqual(listing["meta"]["format"], "SANOBPK1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
