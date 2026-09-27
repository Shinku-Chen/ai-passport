#!/usr/bin/env python3
"""limelight 素材打包器的守卫测试。

分三层,前两层不需要 Pillow(CI 默认没装),第三层在本地有 Pillow 时跑完整管线:

  1. 容器层:build_container/parse_container 的索引、对齐、边界、kind 语义;
  2. 规则层:family_of / filter_enabled / is_referenced 的裁剪判定 —— 尤其
     "缺脚本时绝不能拿鉴赏列表去裁背景和立绘" 这条;
  3. 管线层(需要 Pillow):在临时目录造一套合成素材跑完整打包,验证尺寸、
     去重、裁剪、遮罩长度、meta 名表。

合成的都是纯色/渐变小图,不涉及任何真实素材。
"""

from __future__ import annotations

import io
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "limelight_material_pack.py"
sys.path.insert(0, str(ROOT / "tools"))

import limelight_material_pack as pack  # noqa: E402

try:
    from PIL import Image
except ImportError:  # pragma: no cover - CI 没有 Pillow
    Image = None


class ContainerTest(unittest.TestCase):
    """容器层:纯字节,不需要 Pillow。"""

    def build(self, entries, blobs):
        return pack.build_container(entries, blobs, quality=30, bg_rows=214, flags=0)

    def test_index_layout_and_alignment(self) -> None:
        blobs = [b"A" * 5, b"B" * 300, b"C" * 7]
        entries = [
            (pack.KIND_IMAGE, "a.jpg", 240, 214, 0, None),
            (pack.KIND_SPRITE, "b.png", 10, 20, 1, 123),      # 一个 blob = JPEG + 遮罩
            (pack.KIND_META, "meta", 0, 0, 2, None),
        ]
        blob = self.build(entries, blobs)
        head, parsed = pack.parse_container(blob)

        self.assertEqual(head["n_entries"], 3)
        self.assertEqual(head["n_blobs"], 3)
        self.assertEqual(head["screen"], (pack.SCREEN_W, pack.SCREEN_H))
        self.assertEqual(head["bg_rows"], 214)

        for entry in parsed:
            self.assertLessEqual(entry["off"] + entry["len"], len(blob))
            self.assertEqual(entry["off"] % 4, 0, "每段 4 字节对齐")
        # sprite 条目带 jpeg_len,image 不带
        self.assertEqual(parsed[0]["jpeg_len"], None)
        self.assertEqual(parsed[1]["jpeg_len"], 123)
        self.assertEqual(parsed[1]["len"], 300)
        # 段内容确实按索引落位
        self.assertEqual(blob[parsed[0]["off"]:parsed[0]["off"] + 5], b"A" * 5)
        self.assertEqual(blob[parsed[2]["off"]:parsed[2]["off"] + 7], b"C" * 7)

    def test_rejects_bad_magic_version_and_bounds(self) -> None:
        blob = self.build([(pack.KIND_IMAGE, "a", 1, 1, 0, None)], [b"X"])
        with self.assertRaises(ValueError):
            pack.parse_container(b"XXXXXXXX" + blob[8:])
        with self.assertRaises(ValueError):
            pack.parse_container(blob[:4] + blob[4:8] + struct.pack("<I", 99) + blob[12:])
        broken = bytearray(blob)
        struct.pack_into("<I", broken, 32 + 4, 10 ** 6)     # len 越界
        with self.assertRaises(ValueError):
            pack.parse_container(bytes(broken))

    def test_dedup_shares_one_blob(self) -> None:
        store = pack.BlobStore()
        first = store.put("k", b"same-bytes", "bg")
        second = store.put("k", b"same-bytes", "bg")
        self.assertEqual(first, second)
        self.assertEqual(store.dedup_hits, 1)
        self.assertEqual(store.dedup_saved, len(b"same-bytes"))
        self.assertEqual(len(store.blobs), 1)

        # 同一段被两个条目引用时,数据段只写一次,两条目 offset 必须相同
        entries = [
            (pack.KIND_IMAGE, "a1.jpg", 240, 214, 0, None),
            (pack.KIND_IMAGE, "a2.jpg", 240, 214, 0, None),
            (pack.KIND_IMAGE, "b.jpg", 240, 214, 1, None),
        ]
        blob = self.build(entries, [b"same-bytes", b"other"])
        _head, parsed = pack.parse_container(blob)
        self.assertEqual(parsed[0]["off"], parsed[1]["off"])
        self.assertNotEqual(parsed[0]["off"], parsed[2]["off"])
        # 最后一个段紧贴文件尾,不再补对齐
        self.assertEqual(len(blob), parsed[2]["off"] + parsed[2]["len"])
        self.assertEqual(blob[parsed[2]["off"]:parsed[2]["off"] + 5], b"other")


class ReferenceRuleTest(unittest.TestCase):
    """规则层:裁剪判定不能误伤整个家族。"""

    def refs(self, **overrides):
        base = {"bg": set(), "char": set(), "cg": set(), "gallery": set(), "scripts": 0}
        base.update(overrides)
        return base

    def test_family_of(self) -> None:
        self.assertEqual(pack.family_of("bcgi", "学園_教室a_夏.jpg"), "bg")
        self.assertEqual(pack.family_of("cimg", "hz01_1.png"), "sprite")
        self.assertEqual(pack.family_of("evig", "ev001a.jpg"), "cg")
        self.assertEqual(pack.family_of("evig", "sd002_ea01_ta01.png"), "sprite")
        self.assertEqual(pack.family_of("", "title_bg0.jpg"), "misc")

    def test_gallery_alone_never_prunes_bg_or_sprite(self) -> None:
        refs = self.refs(gallery={"ev001a.jpg"}, scripts=0)
        self.assertFalse(pack.filter_enabled("bg", refs))
        self.assertFalse(pack.filter_enabled("sprite", refs))
        self.assertTrue(pack.filter_enabled("cg", refs))
        self.assertFalse(pack.filter_enabled("misc", refs))

    def test_scripts_enable_all_material_pruning(self) -> None:
        refs = self.refs(scripts=137, bg={"bgA"}, char={"hz01_1"})
        for family in ("bg", "cg", "sprite"):
            self.assertTrue(pack.filter_enabled(family, refs))
        self.assertTrue(pack.is_referenced("bg", "bgA", "bgA.jpg", refs))
        self.assertFalse(pack.is_referenced("bg", "bgB", "bgB.jpg", refs))
        self.assertTrue(pack.is_referenced("sprite", "hz01_1", "hz01_1.png", refs))
        self.assertFalse(pack.is_referenced("sprite", "hz99_9", "hz99_9.png", refs))
        # CG 可以被鉴赏列表或脚本任一引用
        self.assertTrue(pack.is_referenced("cg", "ev1a", "ev1a.jpg", self.refs(scripts=1, gallery={"ev1a.jpg"})))
        self.assertFalse(pack.is_referenced("cg", "ev1a", "ev1a.jpg", self.refs(scripts=1, cg={"ev2a.jpg"})))
        # 标题图不参与裁剪
        self.assertTrue(pack.is_referenced("misc", "title_bg0", "title_bg0.jpg", self.refs(scripts=1)))


@unittest.skipIf(Image is None, "需要 Pillow 才能跑完整打包管线")
class PipelineTest(unittest.TestCase):
    """管线层:合成素材跑一次完整打包。"""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="limelight-pack-test.")
        root = Path(cls.tmp.name)
        common = root / "src" / "common"
        for folder in ("bcgi", "cimg", "evig", "script"):
            (common / folder).mkdir(parents=True, exist_ok=True)

        def gradient(size, seed):
            im = Image.new("RGB", size)
            px = im.load()
            for y in range(size[1]):
                for x in range(size[0]):
                    px[x, y] = ((x * 3 + seed) % 256, (y * 5 + seed) % 256, (x + y + seed) % 256)
            return im

        # 背景:bgA 被脚本引用, bgB 与 bgA 字节完全相同(顺带验去重), bgC 没人引用
        bg_a = gradient((336, 480), 1)
        bg_a.save(common / "bcgi" / "bgA.jpg", quality=90)
        bg_a.save(common / "bcgi" / "bgB.jpg", quality=90)
        gradient((336, 480), 7).save(common / "bcgi" / "bgC.jpg", quality=90)
        # CG:鉴赏表与脚本都引用
        gradient((336, 480), 3).save(common / "evig" / "ev001a.jpg", quality=90)
        # 立绘:一张 cimg(脚本 c 字段引用) + 一张 sd*(鉴赏表引用)
        for name, size, seed in (("hz01_1.png", (200, 900), 11), ("sd002_ea01.png", (336, 480), 13)):
            im = Image.new("RGBA", size, (0, 0, 0, 0))
            draw = im.load()
            for y in range(size[1] // 4, size[1] * 3 // 4):
                for x in range(size[0] // 4, size[0] * 3 // 4):
                    draw[x, y] = ((x + seed) % 256, (y * 2) % 256, 64, 255)
            im.save(common / ("cimg" if name.startswith("hz") else "evig") / name)
        # 标题图:永远保留
        gradient((336, 189), 5).save(common / "title_bg0.jpg", quality=90)
        # 引用源
        (common / "script" / "scriptData1.txt").write_text(
            '{"1": {"b": "bgA", "c": "hz01_1", "cg": "ev001a.jpg", "t": "x"},'
            ' "2": {"b": "bgB", "t": "y"}}',
            encoding="utf-8")
        (root / "src" / "pages" / "cgs").mkdir(parents=True, exist_ok=True)
        (root / "src" / "pages" / "cgs" / "cgs.ux").write_text(
            'cgList: ["/common/evig/ev001a.jpg","/common/evig/sd002_ea01.png"]',
            encoding="utf-8")
        cls.source = root
        cls.out = root / "pack.bin"
        result = subprocess.run(
            [sys.executable, str(TOOL), "--source", str(root), "--out", str(cls.out)],
            capture_output=True, text=True, cwd=ROOT)
        assert result.returncode == 0, result.stderr
        cls.blob = cls.out.read_bytes()
        cls.head, cls.entries = pack.parse_container(cls.blob)
        cls.meta = cls.blob[cls.entries[-1]["off"]:cls.entries[-1]["off"] + cls.entries[-1]["len"]].decode("utf-8")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_defaults_and_pruning(self) -> None:
        self.assertEqual(self.head["quality"], pack.DEFAULT_QUALITY)
        self.assertEqual(self.head["bg_rows"], pack.DEFAULT_BG_ROWS)
        self.assertTrue(self.head["flags"] & pack.FLAG_FILTERED)
        self.assertTrue(self.head["flags"] & pack.FLAG_BG_CROPPED)
        # bgA + bgB(内容相同) + ev001a + hz01_1 + sd002 + title_bg0 + meta
        self.assertEqual(self.head["n_entries"], 7)
        self.assertNotIn("bgC", self.meta, "没被引用的背景应被裁掉")
        self.assertIn("[names]", self.meta)
        self.assertIn("quality=", self.meta)

    def test_geometry_and_kinds(self) -> None:
        kinds = [e["kind"] for e in self.entries]
        self.assertEqual(kinds.count(pack.KIND_IMAGE), 4)      # bgA bgB ev001a(cg) title_bg0
        self.assertEqual(kinds.count(pack.KIND_SPRITE), 2)
        self.assertEqual(kinds.count(pack.KIND_META), 1)
        for entry in self.entries:
            if entry["kind"] == pack.KIND_IMAGE and entry["w"] == pack.SCREEN_W and entry["h"] == pack.DEFAULT_BG_ROWS:
                continue
            if entry["kind"] == pack.KIND_SPRITE:
                self.assertLessEqual(entry["w"], pack.SPRITE_MAX_W)
                self.assertLessEqual(entry["h"], pack.SPRITE_MAX_H)
                self.assertIsNotNone(entry["jpeg_len"])
        # 不变量:所有"图片"类条目都必须是画布尺寸 240x214(固件按这个尺寸解码,
        # 多一行/少一行都会拒绝),标题图也一样(打包器把它居中裁进画布)。
        images = [e for e in self.entries if e["kind"] == pack.KIND_IMAGE]
        self.assertEqual(len(images), 4, "2 张背景 + 1 张 CG + 1 张标题图")
        for entry in images:
            self.assertEqual((entry["w"], entry["h"]),
                             (pack.SCREEN_W, pack.DEFAULT_CG_ROWS))
        # 立绘:夹在 168x252 与 SPRITE_MAX_PIXELS 之内(固件按上限申请静态缓冲)
        sprites = [e for e in self.entries if e["kind"] == pack.KIND_SPRITE]
        self.assertEqual(len(sprites), 2)
        for entry in sprites:
            self.assertLessEqual(entry["w"], pack.SPRITE_MAX_W)
            self.assertLessEqual(entry["h"], pack.SPRITE_MAX_H)
            self.assertLessEqual(entry["w"] * entry["h"], pack.SPRITE_MAX_PIXELS)

    def test_dedup_and_mask(self) -> None:
        bgs = [e for e in self.entries if e["h"] == pack.DEFAULT_BG_ROWS]
        self.assertEqual(bgs[0]["off"], bgs[1]["off"], "字节相同的背景应共用同一段")
        sprite = [e for e in self.entries if e["kind"] == pack.KIND_SPRITE][0]
        mask = pack.decode_mask_1bpp_rle(
            self.blob[sprite["off"] + sprite["jpeg_len"]:sprite["off"] + sprite["len"]])
        self.assertEqual(len(mask), ((sprite["w"] + 7) // 8) * sprite["h"])

    def test_keep_all_and_bg_rows_knob(self) -> None:
        out = self.out.with_name("pack_keepall.bin")
        result = subprocess.run(
            [sys.executable, str(TOOL), "--source", str(self.source), "--out", str(out),
             "--keep-all", "--quality", "60", "--bg-rows", "320"],
            capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stderr)
        blob = out.read_bytes()
        head, entries = pack.parse_container(blob)
        self.assertEqual(head["quality"], 60)
        self.assertEqual(head["bg_rows"], 320)
        self.assertFalse(head["flags"] & pack.FLAG_FILTERED)
        self.assertEqual(head["n_entries"], 8, "--keep-all 应保留 bgC")


class CliTest(unittest.TestCase):
    """CLI 参数与前置校验(不需要 Pillow:参数错误在转换之前就退出)。"""

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *args],
                              capture_output=True, text=True, cwd=ROOT)

    def test_bad_arguments(self) -> None:
        result = self.run_tool("--source", str(ROOT), "--out", "x.bin", "--quality", "0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("quality", result.stderr)
        result = self.run_tool("--source", str(ROOT), "--out", "x.bin", "--bg-rows", "999")
        self.assertNotEqual(result.returncode, 0)
        result = self.run_tool("--source", str(ROOT / "tools"), "--out", "x.bin")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("src/common", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
