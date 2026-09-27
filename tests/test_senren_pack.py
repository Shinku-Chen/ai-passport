#!/usr/bin/env python3
"""Round-trip tests for the blocked palette payload codec in tools/senren_pack.py.

The device has ~11 KB of free heap and a 7.7 KB largest block, so SPRITE / SD /
EFFECT payloads are no longer PNG: they are palettized, row-filtered and cut
into `block_rows`-high zlib blocks so the firmware never needs a 32 KB inflate
dictionary.  The firmware reader replays the frozen layout by hand, so the
encoder must produce exactly that layout and the PNG row filters must be
invertible across block boundaries.

These tests build small synthetic RGBA images, encode them, and decode them
with the same reader `--compare` uses, so they do not need the multi-megabyte
source assets (run tools/senren_fetch_source.py for those).

Run: python3 tests/test_senren_pack.py
Skips cleanly when Pillow/numpy are unavailable (they are only needed to pack).
"""

from __future__ import annotations

import importlib.util
import struct
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:  # pragma: no cover - 环境问题
    np = None
    Image = None
    SKIP_REASON = f"需要 Pillow/numpy 才能打包: {exc}"
else:
    SKIP_REASON = ""


def load_packer():
    """按路径加载 tools/senren_pack.py(它不在 sys.path 上)。"""
    spec = importlib.util.spec_from_file_location("senren_pack", ROOT / "tools" / "senren_pack.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["senren_pack"] = module   # dataclass 需要模块已在 sys.modules 里
    spec.loader.exec_module(module)
    return module


def constant_color_image(width: int, height: int, color=(200, 60, 30, 255)):
    array = np.empty((height, width, 4), dtype=np.uint8)
    array[:, :] = color
    return Image.fromarray(array, "RGBA")


def few_color_image(width: int, height: int):
    """十几色的宽动态图(含透明):调色板量化应当能把颜色和 alpha 分开。"""
    array = np.empty((height, width, 4), dtype=np.uint8)
    array[:, :, 0] = (np.arange(width) % 4).astype(np.uint8) * 64
    array[:, :, 1] = (np.arange(height) % 3).astype(np.uint8)[:, None] * 96
    array[:, :, 2] = 200
    transparent = ((np.arange(width)[None, :] + np.arange(height)[:, None]) % 7) == 0
    array[:, :, 3] = np.where(transparent, 0, 255).astype(np.uint8)
    return Image.fromarray(array, "RGBA"), transparent


def repeated_row_image(width: int, height: int):
    """每一行都与上一行逐像素相同(横向 5 种颜色):Up 的代价因此是 0。"""
    stripes = np.array([[10, 10, 10], [200, 40, 40], [40, 200, 60],
                        [50, 60, 220], [230, 220, 30]], dtype=np.uint8)
    pattern = (np.arange(width) // 3) % len(stripes)
    array = np.empty((height, width, 4), dtype=np.uint8)
    array[:, :, :3] = stripes[pattern][None, :, :]
    array[:, :, 3] = 255
    return Image.fromarray(array, "RGBA")


@unittest.skipIf(SKIP_REASON, SKIP_REASON)
class BlockedPayloadTest(unittest.TestCase):
    """分块调色板载荷:头部字段、逐块解压、滤波器选择与还原。"""

    @classmethod
    def setUpClass(cls) -> None:
        cls.pack = load_packer()
        cls.header = cls.pack.BLOCK_HEADER

    def read_blocks(self, payload: bytes):
        """按固件的方式走一遍块表,返回 (头部字段字典, 每块解压后的字节)。"""
        (width, height, palette_count, block_rows, block_count,
         reserved, raw_block_bytes) = self.header.unpack_from(payload)
        table = self.header.size + palette_count * 3 + block_count * 4
        lengths = struct.unpack_from(f"<{block_count}I", payload, self.header.size + palette_count * 3)
        self.assertEqual(table + sum(lengths), len(payload), "块流之后不该有多余字节")
        position = table
        blocks = []
        for length in lengths:
            blocks.append(zlib.decompress(payload[position:position + length]))
            position += length
        fields = dict(width=width, height=height, palette_count=palette_count, block_rows=block_rows,
                      block_count=block_count, reserved=reserved, raw_block_bytes=raw_block_bytes)
        return fields, blocks

    def test_header_matches_the_frozen_layout(self) -> None:
        image = constant_color_image(120, 37)
        payload = self.pack.encode_blocked(image, 255)
        fields, blocks = self.read_blocks(payload)
        self.assertEqual((fields["width"], fields["height"]), (120, 37))
        self.assertEqual(fields["block_rows"], 8)
        self.assertEqual(fields["block_count"], (37 + 7) // 8)          # 最后一块只有 5 行
        self.assertEqual(fields["reserved"], 0)
        self.assertEqual(fields["raw_block_bytes"], 8 * (120 + 1))
        self.assertTrue(1 <= fields["palette_count"] <= 255)
        self.assertEqual(len(blocks), fields["block_count"])
        # 最后一块更短,其余都满;每块都 <= raw_block_bytes(固件只备一块缓冲)
        for block in blocks[:-1]:
            self.assertEqual(len(block), fields["raw_block_bytes"])
        self.assertLessEqual(len(blocks[-1]), fields["raw_block_bytes"])

    def test_every_row_is_one_filter_byte_plus_indices(self) -> None:
        image, _ = few_color_image(53, 29)
        fields, blocks = self.read_blocks(self.pack.encode_blocked(image, 255))
        row_bytes = fields["width"] + 1
        rows = 0
        filters = set()
        for block in blocks:
            self.assertEqual(len(block) % row_bytes, 0)
            for offset in range(0, len(block), row_bytes):
                filters.add(block[offset])
                self.assertLessEqual(block[offset], self.pack.FILTER_PAETH)
            rows += len(block) // row_bytes
        self.assertEqual(rows, fields["height"])
        self.assertLessEqual(max(filters), 4)

    def test_decode_reproduces_the_quantized_palette(self) -> None:
        """解码结果必须等于量化结果(颜色只余 RGB565 的 5/6/5 位舍入),索引与 alpha 逐像素一致。"""
        image, transparent = few_color_image(53, 29)
        quantized = image.quantize(colors=255, method=Image.FASTOCTREE).convert("RGBA")
        payload = self.pack.encode_blocked(image, 255)
        fields, _ = self.read_blocks(payload)
        self.assertTrue(1 <= fields["palette_count"] <= 255)
        self.assertGreaterEqual(fields["palette_count"], 4, "样例图应该量化出多个调色板项")
        decoded = self.pack.decode_blocked(payload)
        self.assertEqual(decoded.size, image.size)
        # 调色板按 RGB565 存,只剩下 5/6/5 位舍入;不能有明显的串色
        self.assertLess(self.pack.mean_abs_error(decoded, quantized), 4.0)
        self.assertTrue(np.array_equal(np.asarray(decoded.getchannel("A")),
                                       np.asarray(quantized.getchannel("A"))),
                        "每个调色板项的 alpha 必须原样保留")
        alpha = np.asarray(decoded.getchannel("A"))
        self.assertEqual(int(alpha[transparent].max()), 0, "透明像素必须保持全透明")
        self.assertEqual(int(alpha[~transparent].min()), 255, "不透明像素必须保持不透明")

    def test_up_filter_is_kept_across_block_boundaries(self) -> None:
        """每一行都和上一行相同,Up(2)的代价是 0:非全零行必须是 Up,跨块也一样。"""
        image = repeated_row_image(40, 20)
        fields, blocks = self.read_blocks(self.pack.encode_blocked(image, 255))
        row_bytes = fields["width"] + 1
        previous = np.zeros(fields["width"], dtype=np.uint8)
        checked = 0
        block_start = 0
        for block_index, block in enumerate(blocks):
            for offset in range(0, len(block), row_bytes):
                row_index = block_index * fields["block_rows"] + offset // row_bytes
                filter_type = block[offset]
                row = np.frombuffer(block, dtype=np.uint8, count=fields["width"], offset=offset + 1)
                previous = self.pack.unfilter_row(filter_type, row, previous)
                if row_index == 0 or not previous.any():
                    continue    # 全零行的 None/Sub 代价也是 0,不要求是 Up
                self.assertEqual(filter_type, self.pack.FILTER_UP,
                                 f"第 {row_index} 行与上一行相同,Up 的代价是 0,应该被选中")
                checked += 1
                if row_index % fields["block_rows"] == 0:
                    block_start += 1
        self.assertGreater(checked, 0)
        self.assertGreater(block_start, 0, "没有覆盖到新块的第一行(上一行要跨块保留)")

    def test_decoder_rejects_bad_block_lengths(self) -> None:
        """自检必须能发现块表越界和解压后超长,不然真机读到的是垃圾。"""
        image = constant_color_image(24, 16)
        payload = bytearray(self.pack.encode_blocked(image, 255))
        fields, _ = self.read_blocks(bytes(payload))
        table = self.header.size + fields["palette_count"] * 3
        struct.pack_into("<I", payload, table, len(payload))   # 第一块长度指向载荷之外
        entry = {"w": 24, "h": 16}
        errors: list[str] = []
        self.pack.check_blocked("测试", bytes(payload), entry, errors)
        self.assertTrue(errors, "越界的块长度没有被自检发现")


def main() -> int:
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(BlockedPayloadTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
