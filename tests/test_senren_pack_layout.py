#!/usr/bin/env python3
"""Pin down the on-flash layout that the firmware readers assume.

main/senren_pack.c and main/senren_model.c walk the two packs with hand-written
offsets.  A silent off-by-N there produces garbage text or failed decompression
only on the device, so this test re-reads the shipped packs and checks every
offset the C code relies on against the documented layout:

  * section table position/size, magic and total-size fields;
  * per-section prefixes (SEC_CHAR/SEC_FLAG carry a u32 count, SEC_CHUNK a u16
    count, SEC_NAME a bare block list);
  * the image pack's 36-byte entry records: the fields the firmware reads are
    inside the asset section, the name blob is NUL-terminated, and every
    payload range stays inside SEC_BLOB;
  * the script pack's 16-byte chunk records: each block inflates to exactly its
    raw_len, its leading u16 equals the recorded node_count, and walking the
    records by the documented sizes consumes the whole block.

Run: python3 tests/test_senren_pack_layout.py
Skips cleanly when the packs are absent (they are generated artifacts).
"""

from __future__ import annotations

import struct
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE_PACK = ROOT / "main" / "senren_data" / "senren_pack.bin"
SCN_PACK = ROOT / "main" / "senren_data" / "senren_scn.bin"

IMAGE_MAGIC = b"SENRNPK2"
SCN_MAGIC = b"SENRSCN1"
HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
ASSET = struct.Struct("<IHBBHHHHHHHIIIH")
CHUNK = struct.Struct("<IIII")

SEC_NAME, SEC_ASSET, SEC_BLOB, SEC_META = range(4)
SCN_SEC_CHAR, SCN_SEC_NAME, SCN_SEC_FLAG, SCN_SEC_CHUNK, SCN_SEC_BLOB, SCN_SEC_META = range(6)

KIND_BG, KIND_SPRITE, KIND_SD, KIND_CG, KIND_CG_DIFF, KIND_MISSING, KIND_EFFECT = range(7)

NODE_LABEL, NODE_CHAPTER, NODE_BG, NODE_DIALOGUE = 0, 1, 2, 3
NODE_SELECT, NODE_EV, NODE_NEXT, NODE_SPRITE_OFF = 4, 5, 6, 7


def read_pack(path: Path, magic: bytes) -> tuple[bytes, dict[int, tuple[int, int, int]]]:
    raw = path.read_bytes()
    header = HEADER.unpack_from(raw)
    assert header[0] == magic, (path.name, header[0])
    _, _, header_size, section_count, _, total = header
    assert total == len(raw), (path.name, total, len(raw))
    sections: dict[int, tuple[int, int, int]] = {}
    for index in range(section_count):
        section_type, offset, count, size = SECTION.unpack_from(raw, HEADER.size + SECTION.size * index)
        assert offset + size <= len(raw), (path.name, section_type)
        sections[section_type] = (offset, count, size)
    return raw, sections


class ImagePackLayoutTest(unittest.TestCase):
    """图片包(SENRNPK2):固件只读 SEC_NAME / SEC_ASSET / SEC_BLOB。"""

    @classmethod
    def setUpClass(cls) -> None:
        if not IMAGE_PACK.exists():
            raise unittest.SkipTest(f"没有 {IMAGE_PACK.relative_to(ROOT)}(先跑素材打包器)")
        cls.raw, cls.sections = read_pack(IMAGE_PACK, IMAGE_MAGIC)

    def test_sections_and_names(self) -> None:
        for required in (SEC_NAME, SEC_ASSET, SEC_BLOB, SEC_META):
            self.assertIn(required, self.sections)
        names_off, name_count, names_size = self.sections[SEC_NAME]
        asset_off, entry_count, asset_size = self.sections[SEC_ASSET]
        self.assertEqual(name_count, entry_count)
        self.assertGreaterEqual(asset_size, entry_count * ASSET.size)
        names = self.raw[names_off:names_off + names_size]
        self.assertTrue(names.endswith(b"\x00"), "名称表必须以 NUL 结尾")

    def test_entries_stay_inside_the_sections(self) -> None:
        names_off, _, names_size = self.sections[SEC_NAME]
        asset_off, entry_count, _ = self.sections[SEC_ASSET]
        blob_off, _, blob_size = self.sections[SEC_BLOB]
        names = self.raw[names_off:names_off + names_size]
        blob = self.raw[blob_off:blob_off + blob_size]
        by_index: list[dict] = []
        for index in range(entry_count):
            (name_off, name_len, kind, pool, base, width, height,
             dx, dy, dw, dh, data_off, data_len, flags, _) = ASSET.unpack_from(
                self.raw, asset_off + index * ASSET.size)
            self.assertLessEqual(name_off + name_len, len(names), "名字越界")
            if kind != KIND_MISSING:
                self.assertLessEqual(data_off + data_len, len(blob), "载荷越界")
                self.assertTrue(blob[data_off:data_off + 4], "载荷为空")
            by_index.append(dict(kind=kind, pool=pool, base=base, width=width, height=height,
                                 dx=dx, dy=dy, dw=dw, dh=dh, flags=flags, data_len=data_len,
                                 data_off=data_off,
                                 name=names[name_off:name_off + name_len]))

        # 固件按 (pool, name) 二分查找,所以表必须真的按这个顺序排好
        keys = [(entry["pool"], entry["name"]) for entry in by_index]
        self.assertEqual(keys, sorted(keys), "条目没有按 (pool, name) 升序排列")

        for index, entry in enumerate(by_index):
            if entry["flags"] & 1:   # ALIAS
                self.assertEqual(entry["data_len"], 0, "别名条目不该有载荷")
                base = by_index[entry["base"]]
                self.assertNotEqual(base["flags"] & 1, 1, "别名不能指向别名")
            if entry["kind"] == KIND_CG_DIFF:
                base = by_index[entry["base"]]
                self.assertEqual(base["kind"], KIND_CG, "差分基准必须是完整 CG")
                self.assertLessEqual(entry["dx"] + entry["dw"], base["width"])
                self.assertLessEqual(entry["dy"] + entry["dh"], base["height"])
                self.assertGreaterEqual(entry["data_len"], 8)
            if entry["kind"] in (KIND_BG, KIND_CG):
                head = blob[entry["data_off"]] if entry["data_len"] else 0
                self.assertEqual(head, 0xFF, f"{entry['name']} 不是 JPEG")
            if entry["kind"] == KIND_CG_DIFF:
                # 补丁载荷 = u32 掩码长度 + u32 像素长度 + 两段 zlib 流
                mask_len, pixels_len = struct.unpack_from("<II", blob, entry["data_off"])
                self.assertEqual(mask_len + pixels_len + 8, entry["data_len"],
                                 f"{entry['name']} 补丁长度对不上")
                expected_mask = (entry["dw"] * entry["dh"] + 7) // 8
                mask = zlib.decompress(blob[entry["data_off"] + 8:
                                            entry["data_off"] + 8 + mask_len])
                pixels = zlib.decompress(blob[entry["data_off"] + 8 + mask_len:])
                self.assertEqual(len(mask), expected_mask, f"{entry['name']} 掩码字节数不对")
                marked = sum(bin(byte).count("1") for byte in mask)
                self.assertEqual(len(pixels), marked * 2, f"{entry['name']} 像素数不等于掩码置位数")

    def test_geometry_matches_the_screen(self) -> None:
        _, _, _ = self.sections[SEC_ASSET]
        asset_off, entry_count, _ = self.sections[SEC_ASSET]
        seen = {}
        for index in range(entry_count):
            record = ASSET.unpack_from(self.raw, asset_off + index * ASSET.size)
            seen.setdefault(record[2], set()).add((record[5], record[6]))
        # 背景 / CG / 特效 都是 240x320;立绘高度 320;SD 是 240x144
        self.assertEqual(seen[KIND_BG], {(240, 320)})
        self.assertEqual(seen[KIND_CG], {(240, 320)})
        self.assertEqual(seen[KIND_EFFECT], {(240, 320)})
        self.assertEqual(seen[KIND_SD], {(240, 144)})
        for width, height in seen[KIND_SPRITE]:
            self.assertEqual(height, 320, "立绘高度必须是屏幕高")
            self.assertLessEqual(width, 240)


class ScnPackLayoutTest(unittest.TestCase):
    """剧本包(SENRSCN1):固件按段前缀 + 16 字节块记录读。"""

    @classmethod
    def setUpClass(cls) -> None:
        if not SCN_PACK.exists():
            raise unittest.SkipTest(f"没有 {SCN_PACK.relative_to(ROOT)}(先跑剧本打包器)")
        cls.raw, cls.sections = read_pack(SCN_PACK, SCN_MAGIC)

    def test_section_prefixes(self) -> None:
        # SEC_CHAR: u32 count + count × u16,count 与段头一致
        char_off, char_count, char_size = self.sections[SCN_SEC_CHAR]
        self.assertEqual(struct.unpack_from("<I", self.raw, char_off)[0], char_count)
        self.assertEqual(char_size, 4 + 2 * char_count)
        # SEC_FLAG: u32 count + count × u32
        flag_off, flag_count, flag_size = self.sections[SCN_SEC_FLAG]
        self.assertEqual(struct.unpack_from("<I", self.raw, flag_off)[0], flag_count)
        self.assertEqual(flag_size, 4 + 4 * flag_count)
        # SEC_CHUNK: u16 count + count × 16
        chunk_off, chunk_count, chunk_size = self.sections[SCN_SEC_CHUNK]
        self.assertEqual(struct.unpack_from("<H", self.raw, chunk_off)[0], chunk_count)
        self.assertEqual(chunk_size, 2 + CHUNK.size * chunk_count)

    def test_char_table_is_plausible(self) -> None:
        """固件把 SEC_CHAR 当作「u32 计数之后的码位数组」;错位会读出垃圾码位。"""
        char_off, char_count, _ = self.sections[SCN_SEC_CHAR]
        codes = struct.unpack_from(f"<{char_count}H", self.raw, char_off + 4)
        self.assertEqual(len(set(codes)), len(codes), "字符表有重复码位")
        self.assertNotIn(0, codes, "字符表里有 0 码位(疑似段前缀错位)")
        for code in codes[:32]:
            self.assertTrue(0x20 <= code <= 0xFFFF, f"码位不像字符: U+{code:04X}")

    def test_chunks_decompress_to_their_declared_size(self) -> None:
        chunk_off, chunk_count, _ = self.sections[SCN_SEC_CHUNK]
        blob_off, _, blob_size = self.sections[SCN_SEC_BLOB]
        for index in range(chunk_count):
            data_off, data_len, raw_len, node_count = CHUNK.unpack_from(
                self.raw, chunk_off + 2 + index * CHUNK.size)
            self.assertLessEqual(data_off + data_len, blob_size, f"块 {index} 越界")
            block = self.raw[blob_off + data_off:blob_off + data_off + data_len]
            plain = zlib.decompress(block)
            self.assertEqual(len(plain), raw_len, f"块 {index} 解压长度不符")
            self.assertEqual(struct.unpack_from("<H", plain)[0], node_count,
                             f"块 {index} 的节点数与记录不符")
            self.assertEqual(self.walk_nodes(plain), node_count, f"块 {index} 记录走不完")

    def walk_nodes(self, plain: bytes) -> int:
        """按 main/senren_model.c 的 node_skip 逐条走,返回节点数(走不完就抛错)。"""
        count = struct.unpack_from("<H", plain)[0]
        position = 2
        for _ in range(count):
            kind = plain[position]
            position += 1
            if kind == NODE_LABEL:
                position += 4
            elif kind == NODE_CHAPTER:
                position = self.skip_text(plain, position)
            elif kind == NODE_BG:
                position += 1
            elif kind == NODE_DIALOGUE:
                position += 1
                position = self.skip_text(plain, position)
                position += 3
            elif kind == NODE_EV:
                position += 2
            elif kind == NODE_SPRITE_OFF:
                pass
            elif kind == NODE_SELECT:
                options = plain[position]
                position += 1
                for _ in range(options):
                    # 选项 = text + u8 目标类型 + u16 块号 + u32 页号 + u8 标志 + u8 值
                    position = self.skip_text(plain, position)
                    position += 9
            elif kind == NODE_NEXT:
                position += 7
                conditions = plain[position]
                position += 1 + conditions * 2
            else:
                raise AssertionError(f"未知节点类型 {kind}")
            if position > len(plain):
                raise AssertionError("记录越过块尾")
        if position != len(plain):
            raise AssertionError(f"块里还有没走完的字节({len(plain) - position})")
        return count

    @staticmethod
    def skip_text(plain: bytes, position: int) -> int:
        length = struct.unpack_from("<H", plain, position)[0]
        return position + 2 + 2 * length


def main() -> int:
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    for case in (ImagePackLayoutTest, ScnPackLayoutTest):
        suite.addTests(loader.loadTestsFromTestCase(case))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
