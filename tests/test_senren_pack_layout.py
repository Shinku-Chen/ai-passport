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
import re
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
CHUNK = struct.Struct("<IIIHHHH")
BLOCK = struct.Struct("<IIII")

SEC_NAME, SEC_ASSET, SEC_BLOB, SEC_META = range(4)
SCN_SEC_CHAR, SCN_SEC_NAME, SCN_SEC_FLAG, SCN_SEC_CHUNK, SCN_SEC_BLOCK, SCN_SEC_BLOB, SCN_SEC_META = range(7)

KIND_BG, KIND_SPRITE, KIND_SD, KIND_CG, KIND_CG_DIFF, KIND_MISSING, KIND_EFFECT = range(7)
BLOCKED_KINDS = (KIND_SPRITE, KIND_SD, KIND_EFFECT)
# SPRITE/SD/EFFECT 的载荷是分块调色板格式(不是 PNG):见 tools/senren_pack.py 文件头
BLOCK_HEADER = struct.Struct("<6HI")   # w, h, palette_count, block_rows, block_count, reserved, raw_block_bytes
FILTER_PAETH = 4

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
                self.assertGreaterEqual(entry["data_len"], 12)
                # 补丁 = u32 掩码长度 + u32 块数 + 每块 u32 长度 + 掩码流 + 各像素块
                start = entry["data_off"]
                mask_len, block_count = struct.unpack_from("<II", blob, start)
                self.assertGreater(block_count, 0, "补丁至少要有一块像素")
                lengths = [struct.unpack_from("<I", blob, start + 8 + 4 * index)[0]
                           for index in range(block_count)]
                self.assertEqual(8 + 4 * block_count + mask_len + sum(lengths), entry["data_len"],
                                 "补丁长度与块账对不上")
                expected_mask = (entry["dw"] * entry["dh"] + 7) // 8
                offset = start + 8 + 4 * block_count
                mask = zlib.decompress(blob[offset:offset + mask_len])
                self.assertEqual(len(mask), expected_mask, "掩码字节数不对")
                offset += mask_len
                pixels = b""
                for length in lengths:
                    pixels += zlib.decompress(blob[offset:offset + length])
                    offset += length
                marked = sum(bin(byte).count("1") for byte in mask)
                self.assertEqual(len(pixels), marked * 2, "像素数不等于掩码置位数")
            if entry["kind"] in (KIND_BG, KIND_CG):
                head = blob[entry["data_off"]] if entry["data_len"] else 0
                self.assertEqual(head, 0xFF, f"{entry['name']} 不是 JPEG")

    def test_blocked_payloads_decode(self) -> None:
        """SPRITE/SD/EFFECT 是分块调色板载荷(**不是 PNG**):头部 + 逐块解压都要对得上。

        固件按 block_rows 一块块解压:块表错一位、滤波器类型越界或行数对不上,
        真机是画面错位/越界读,这里先把整包逐块走一遍。
        """
        names_off, _, names_size = self.sections[SEC_NAME]
        asset_off, entry_count, _ = self.sections[SEC_ASSET]
        blob_off, _, blob_size = self.sections[SEC_BLOB]
        names = self.raw[names_off:names_off + names_size]
        blob = self.raw[blob_off:blob_off + blob_size]
        checked = 0
        for index in range(entry_count):
            record = ASSET.unpack_from(self.raw, asset_off + index * ASSET.size)
            kind = record[2]
            if kind not in BLOCKED_KINDS:
                continue
            name = names[record[0]:record[0] + record[1]].decode("utf-8")
            if record[13] & 1:      # ALIAS:没有载荷,直接指向基准条目
                self.assertEqual(record[12], 0, f"{name} 别名条目不该有载荷")
                continue
            payload = blob[record[11]:record[11] + record[12]]
            # 载荷以 u16 宽/高开头 —— 不可能再命中 PNG 魔数
            self.assertNotEqual(payload[:4], b"\x89PNG", f"{name} 不该是 PNG")
            (width, height, palette_count, block_rows, block_count,
             reserved, raw_block_bytes) = BLOCK_HEADER.unpack_from(payload)
            self.assertEqual((width, height), (record[5], record[6]), f"{name} 载荷尺寸与条目不符")
            self.assertTrue(1 <= palette_count <= 256, f"{name} palette_count 越界")
            self.assertGreater(block_rows, 0, f"{name} block_rows=0")
            self.assertEqual(block_count, (height + block_rows - 1) // block_rows,
                             f"{name} block_count 不等于 ceil(height / block_rows)")
            self.assertEqual(reserved, 0, f"{name} reserved 应该是 0")
            self.assertEqual(raw_block_bytes, block_rows * (width + 1),
                             f"{name} raw_block_bytes 不等于 block_rows * (width + 1)")
            table = BLOCK_HEADER.size + palette_count * 3 + block_count * 4
            self.assertLessEqual(table, len(payload), f"{name} 调色板/块表越界")
            lengths = struct.unpack_from(f"<{block_count}I", payload,
                                         BLOCK_HEADER.size + palette_count * 3)
            position = table
            rows = 0
            first_filter = None
            row_bytes = width + 1
            for block_index, length in enumerate(lengths):
                self.assertLessEqual(position + length, len(payload),
                                     f"{name} 第 {block_index} 块越界")
                plain = zlib.decompress(payload[position:position + length])
                position += length
                self.assertLessEqual(len(plain), raw_block_bytes,
                                     f"{name} 第 {block_index} 块解压后超过 raw_block_bytes")
                self.assertEqual(len(plain) % row_bytes, 0, f"{name} 第 {block_index} 块不是整行")
                for offset in range(0, len(plain), row_bytes):
                    if first_filter is None:
                        first_filter = plain[offset]
                    self.assertLessEqual(plain[offset], FILTER_PAETH,
                                         f"{name} 第 {block_index} 块滤波器非法")
                rows += len(plain) // row_bytes
            self.assertEqual(position, len(payload), f"{name} 块流后还有多余字节")
            self.assertEqual(rows, height, f"{name} 解压行数不等于 height")
            self.assertIsNotNone(first_filter, f"{name} 没有解压出任何行")
            self.assertLessEqual(first_filter, FILTER_PAETH, f"{name} 首行滤波器非法")
            checked += 1
        self.assertGreater(checked, 0, "包里没有分块调色板条目")

    def test_lookup_finds_every_entry(self) -> None:
        """按 main/senren_pack.c 的senren_pack_find 同逻辑二分查找:每条都应该查到自己。

        池比较的方向写反时,池 0/1 的条目会永远收敛不到(真机表现为标题图/立绘查不到)。
        """
        names_off, _, names_size = self.sections[SEC_NAME]
        asset_off, entry_count, _ = self.sections[SEC_ASSET]
        names = self.raw[names_off:names_off + names_size]

        def lookup(pool: int, wanted: bytes) -> int:
            low, high = 0, entry_count
            while low < high:
                middle = low + (high - low) // 2
                record = ASSET.unpack_from(self.raw, asset_off + middle * ASSET.size)
                entry_name = names[record[0]:record[0] + record[1]]
                if pool != record[3]:
                    # 与 C 一致:cmp<0 表示「条目排在目标之前」-> 往右半区找
                    cmp = -1 if record[3] < pool else 1
                elif entry_name < wanted:
                    cmp = -1
                elif entry_name > wanted:
                    cmp = 1
                else:
                    cmp = 0
                if cmp == 0:
                    return middle
                if cmp < 0:
                    low = middle + 1
                else:
                    high = middle
            return -1

        checked = 0
        for index in range(entry_count):
            record = ASSET.unpack_from(self.raw, asset_off + index * ASSET.size)
            entry_name = names[record[0]:record[0] + record[1]]
            self.assertEqual(lookup(record[3], entry_name), index,
                             f"{entry_name!r}(pool {record[3]}) 二分查找落到了别的条目")
            checked += 1
        self.assertEqual(checked, entry_count)
        self.assertGreater(checked, 700, "条目数太少了,不像完整包")

    def test_title_art_is_present(self) -> None:
        """标题图必须按 main/senren_pack.h 里的宏观名打进背景名字空间。"""
        header = (ROOT / "main" / "senren_pack.h").read_text(encoding="utf-8")
        match = re.search(r'#define\s+SENREN_TITLE_ART_NAME\s+"([^"]+)"', header)
        self.assertIsNotNone(match, "main/senren_pack.h 里没有 SENREN_TITLE_ART_NAME")
        wanted = match.group(1).encode("utf-8")
        names_off, _, names_size = self.sections[SEC_NAME]
        asset_off, entry_count, _ = self.sections[SEC_ASSET]
        names = self.raw[names_off:names_off + names_size]
        found = False
        for index in range(entry_count):
            record = ASSET.unpack_from(self.raw, asset_off + index * ASSET.size)
            if names[record[0]:record[0] + record[1]] == wanted:
                found = True
                self.assertEqual(record[2], KIND_BG, "标题图应该是背景类型")
                self.assertEqual((record[5], record[6]), (240, 320), "标题图必须是整屏")
        self.assertTrue(found, f"包里找不到标题图 {wanted!r}(先跑 tools/senren_pack.py)")

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
        # SEC_CHUNK: u16 count + count × 20
        chunk_off, chunk_count, chunk_size = self.sections[SCN_SEC_CHUNK]
        self.assertEqual(struct.unpack_from("<H", self.raw, chunk_off)[0], chunk_count)
        self.assertEqual(chunk_size, 2 + CHUNK.size * chunk_count)
        # SEC_BLOCK: u16 count + count × 16(每个 chunk 的节点被切成 <=3 KB 的小块)
        block_off, block_count, block_size = self.sections[SCN_SEC_BLOCK]
        self.assertEqual(struct.unpack_from("<H", self.raw, block_off)[0], block_count)
        self.assertEqual(block_size, 2 + BLOCK.size * block_count)

    def test_char_table_is_plausible(self) -> None:
        """固件把 SEC_CHAR 当作「u32 计数之后的码位数组」;错位会读出垃圾码位。"""
        char_off, char_count, _ = self.sections[SCN_SEC_CHAR]
        codes = struct.unpack_from(f"<{char_count}H", self.raw, char_off + 4)
        self.assertEqual(len(set(codes)), len(codes), "字符表有重复码位")
        self.assertNotIn(0, codes, "字符表里有 0 码位(疑似段前缀错位)")
        for code in codes[:32]:
            self.assertTrue(0x20 <= code <= 0xFFFF, f"码位不像字符: U+{code:04X}")

    def test_chunks_are_split_into_small_blocks(self) -> None:
        """每个 chunk 由若干小块组成;固件每次只解一块(空闲堆只有十几 KB)。

        这里按 main/senren_model.c 的读法逐块走:块首节点连续、每块节点数合得上、
        每块解压长度 == raw_len、块内记录走完不留尾巴。
        """
        chunk_off, chunk_count, _ = self.sections[SCN_SEC_CHUNK]
        block_off, block_count, _ = self.sections[SCN_SEC_BLOCK]
        blob_off, _, blob_size = self.sections[SCN_SEC_BLOB]
        blocks = [BLOCK.unpack_from(self.raw, block_off + 2 + index * BLOCK.size)
                  for index in range(block_count)]
        max_raw = 0
        for index in range(chunk_count):
            data_off, data_len, _raw_total, node_count, first_block, count, _ = CHUNK.unpack_from(
                self.raw, chunk_off + 2 + index * CHUNK.size)
            self.assertEqual(data_off + data_len <= blob_size, True, f"块 {index} 越界")
            seen = 0
            for step in range(count):
                entry = blocks[first_block + step]
                block_data_off, block_len, raw_len, first_node = entry
                self.assertEqual(first_node, seen, f"块 {index} 第 {step} 个小块 first_node 不连续")
                self.assertLessEqual(block_data_off + block_len, blob_size, "小块越界")
                plain = zlib.decompress(
                    self.raw[blob_off + block_data_off:blob_off + block_data_off + block_len])
                self.assertEqual(len(plain), raw_len, f"小块 {first_block + step} 解压长度不符")
                nodes_here = struct.unpack_from("<H", plain)[0]
                self.assertEqual(self.walk_nodes(plain), nodes_here, "小块内记录走不完")
                seen += nodes_here
                max_raw = max(max_raw, raw_len)
            self.assertEqual(seen, node_count, f"块 {index} 节点总数不符: {seen} != {node_count}")
        self.assertLessEqual(max_raw, 4096,
                             f"最大小块 {max_raw} 字节超过固件缓冲(SENREN_BLOCK_RAW_MAX=4096)")

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
