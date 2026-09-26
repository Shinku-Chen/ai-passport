#!/usr/bin/env python3
"""资源包结构守卫:校验 tools/starry_pack.py 产出的二进制包自洽。

不需要编译器,也不需要源仓库 —— 只读仓库里已提交的包。检查项:
  1. 头部/段表/魔数/长度自洽,每段的 (count, size) 与条目宽度一致;
  2. 章节图:编号 1..40 缺 7、next 链顺序、场景区间连续且不重叠;
  3. 每条对白的文本偏移落在文本段内且按 UTF-8 可解码;名字/立绘/背景下标合法;
  4. 选项目标是合法场景下标,且目标场景与选项所在场景同章(本作数据如此);
  5. 立绘颜色是无损 RGB565(每像素 2 字节)、遮罩是 4bpp(每行字节对齐);背景尺寸等于屏幕尺寸;
  6. 记录字数/字节数,便于人工对照(main/starry_data/README 里也要写)。
"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "main" / "starry_data" / "starry_pack.bin"

MAGIC = b"SSRPK001"
VERSION = 2
NONE = 0xFFFF
BG_KEEP = 0xFFFE
SCREEN_W, SCREEN_H = 240, 320
SPRITE_MAX_W = 168            # 与 main/starry_render.h / tools/starry_pack.py 一致
SPRITE_MAX_H = 252
SPRITE_RIGHT_EDGE_MIN = 232   # 贴右边(240 - 右边距 4,留 4px 容差)
SPRITE_BOTTOM_EDGE_MIN = 300  # 贴屏幕底(320 - 底边距 6,留 8px 容差)

SEC_TEXT, SEC_NAME, SEC_CHAPTER, SEC_SCENE, SEC_DLG, SEC_BG, SEC_FG, SEC_META = range(8)
NAME_ENTRY, CHAPTER_ENTRY, SCENE_ENTRY, DLG_ENTRY = 8, 12, 16, 14
BG_ENTRY, FG_ENTRY = 14, 26


def parse(blob: bytes) -> dict:
    assert blob[:8] == MAGIC, "魔数不对,先跑 tools/starry_pack.py"
    version, total, count = struct.unpack_from("<III", blob, 8)
    assert version == VERSION, f"版本 {version} != {VERSION}"
    assert total == len(blob), f"头部长度 {total} != 实际 {len(blob)}"
    sections = {}
    for i in range(count):
        sec_type, off, n, size = struct.unpack_from("<IIII", blob, 20 + 16 * i)
        assert off + size <= len(blob), f"段 {sec_type} 越界"
        sections[sec_type] = (off, n, size)
    assert set(sections) == set(range(8)), f"段集合不完整: {sorted(sections)}"
    return {"blob": blob, "sections": sections}


class PackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not PACK.exists():
            raise unittest.SkipTest(f"缺少资源包 {PACK}(先运行 tools/starry_pack.py)")
        cls.pack = parse(PACK.read_bytes())
        cls.blob = cls.pack["blob"]
        cls.sec = cls.pack["sections"]

    def section(self, sec_type: int) -> tuple:
        off, count, size = self.sec[sec_type]
        return off, count, size

    def text(self, off: int, length: int) -> str:
        text_off, _count, text_size = self.section(SEC_TEXT)
        self.assertLessEqual(off, text_size)
        self.assertLessEqual(off + length, text_size)
        return self.blob[text_off + off : text_off + off + length].decode("utf-8")

    def name(self, index: int) -> str:
        if index == NONE:
            return ""
        off, count, size = self.section(SEC_NAME)
        self.assertLess(index, count)
        assert size >= count * NAME_ENTRY
        text_off, length, _pad = struct.unpack_from("<IHH", self.blob, off + index * NAME_ENTRY)
        return self.text(text_off, length)

    def chapter(self, index: int) -> tuple:
        off, count, size = self.section(SEC_CHAPTER)
        self.assertLess(index, count)
        assert size >= count * CHAPTER_ENTRY
        return struct.unpack_from("<HHHHHH", self.blob, off + index * CHAPTER_ENTRY)

    def scene(self, index: int) -> tuple:
        off, count, size = self.section(SEC_SCENE)
        self.assertLess(index, count)
        assert size >= count * SCENE_ENTRY
        return struct.unpack_from("<HBBHHHHHH", self.blob, off + index * SCENE_ENTRY)

    def dialogue(self, index: int) -> tuple:
        off, count, size = self.section(SEC_DLG)
        self.assertLess(index, count)
        assert size >= count * DLG_ENTRY
        return struct.unpack_from("<IHHHBBH", self.blob, off + index * DLG_ENTRY)

    def test_section_sizes_match_entry_widths(self) -> None:
        for sec_type, entry in ((SEC_NAME, NAME_ENTRY), (SEC_CHAPTER, CHAPTER_ENTRY),
                                (SEC_SCENE, SCENE_ENTRY), (SEC_DLG, DLG_ENTRY)):
            _off, count, size = self.section(sec_type)
            self.assertGreaterEqual(size, count * entry, f"段 {sec_type} 尺寸不足")

    def test_chapter_graph(self) -> None:
        _off, chapters, _size = self.section(SEC_CHAPTER)
        self.assertEqual(chapters, 39, "章节数应为 39(源脚本 1..40 缺 7)")
        ids = []
        first_scene = 0
        for i in range(chapters):
            cid, first, scene_count, first_dlg, dlg_count, nxt = self.chapter(i)
            ids.append(cid)
            self.assertEqual(first, first_scene, f"章节 {i} 场景区间不连续")
            if i + 1 < chapters:
                self.assertEqual(nxt, i + 1, f"章节 {i} 的 next 应为 {i + 1}")
            else:
                self.assertEqual(nxt, NONE, "最后一章不应有 next")
            self.assertGreater(scene_count, 0)
            first_scene += scene_count
            # 对白区间连续
            if i > 0:
                prev = self.chapter(i - 1)
                self.assertEqual(first_dlg, prev[3] + prev[4], f"章节 {i} 对白区间不连续")
            self.assertGreater(dlg_count, 0)
        self.assertEqual(ids, [i for i in range(1, 41) if i != 7])
        _off, scenes, _size = self.section(SEC_SCENE)
        self.assertEqual(first_scene, scenes, "章节场景数之和应等于场景总数")

    def test_dialogues_reference_valid_text_and_names(self) -> None:
        _off, dialogues, _size = self.section(SEC_DLG)
        _toff, names, _t2 = self.section(SEC_NAME)
        _bg_off, bgs, _b2 = self.section(SEC_BG)
        _fg_off, fgs, _f2 = self.section(SEC_FG)
        empty = 0
        for i in range(dialogues):
            text_off, text_len, name, sprite, flags, _jump, arg = self.dialogue(i)
            text = self.text(text_off, text_len)
            if not text:
                empty += 1
            if name != NONE:
                self.assertLess(name, names)
            if sprite not in (NONE,):
                self.assertLess(sprite, fgs)
            if flags & 1:
                self.assertLess(arg, names)
        # 源脚本里有极少数空台词(纯场景切换),但不该大面积出现。
        self.assertLess(empty, 50, f"空台词过多: {empty}")

    def test_scenes_reference_valid_backgrounds(self) -> None:
        _off, scenes, _size = self.section(SEC_SCENE)
        _bg_off, bgs, _b2 = self.section(SEC_BG)
        _fg_off, fgs, _f2 = self.section(SEC_FG)
        _doff, dialogues, _d2 = self.section(SEC_DLG)
        for i in range(scenes):
            bg, choice_count, _pad, first_dlg, dlg_count, n0, n1, t0, t1 = self.scene(i)
            self.assertTrue(bg == NONE or bg == BG_KEEP or bg < bgs, f"场景 {i} 背景下标越界")
            self.assertLessEqual(first_dlg + dlg_count, dialogues)
            self.assertLessEqual(choice_count, 2)
            if choice_count == 0:
                self.assertEqual((n0, n1), (NONE, NONE))
            else:
                for n in (n0, n1)[:choice_count]:
                    self.assertLess(n, self.section(SEC_NAME)[1])
                for t in (t0, t1)[:choice_count]:
                    self.assertLess(t, scenes, f"场景 {i} 选项目标越界")

    def test_choice_targets_stay_in_chapter(self) -> None:
        _off, scenes, _size = self.section(SEC_SCENE)
        _coff, chapters, _c2 = self.section(SEC_CHAPTER)
        for i in range(scenes):
            _bg, choice_count, _pad, _fd, _dc, _n0, _n1, t0, t1 = self.scene(i)
            for target in (t0, t1)[:choice_count]:
                if target == NONE:
                    continue
                chapter = None
                for ci in range(chapters):
                    _cid, first, count, _fd, _dc, _nx = self.chapter(ci)
                    if first <= target < first + count:
                        chapter = ci
                        break
                self.assertIsNotNone(chapter, f"选项目标 {target} 不在任何章节内")
                # 选项所在场景也必须落在同一章
                current = None
                for ci in range(chapters):
                    _cid, first, count, _fd, _dc, _nx = self.chapter(ci)
                    if first <= i < first + count:
                        current = ci
                        break
                self.assertEqual(chapter, current, "选项跳出了本章")

    def test_backgrounds_and_sprites(self) -> None:
        off, count, _size = self.section(SEC_NAME)
        name_count = count
        for sec_type, entry, is_sprite in ((SEC_BG, BG_ENTRY, False), (SEC_FG, FG_ENTRY, True)):
            off, count, size = self.section(sec_type)
            self.assertGreater(count, 0)
            data_off = off + count * entry
            data_size = size - count * entry
            for i in range(count):
                rec = self.blob[off + i * entry : off + (i + 1) * entry]
                if not is_sprite:
                    img_off, img_len, w, h, flags = struct.unpack("<IIHHH", rec)
                    self.assertEqual((w, h), (SCREEN_W, SCREEN_H), "背景尺寸必须是整屏")
                    self.assertLessEqual(img_off + img_len, data_size)
                    self.assertEqual(flags & ~1, 0, "背景 flags 只定义了 bit0")
                else:
                    c_off, c_len, m_off, m_len, w, h, x, y, owner =                         struct.unpack("<IIIIHHHHH", rec)
                    self.assertTrue(owner == NONE or owner < name_count,
                                    f"立绘归属 {owner} 不是有效的名字下标")
                    self.assertLessEqual(w, SPRITE_MAX_W)
                    self.assertLessEqual(h, SPRITE_MAX_H)
                    self.assertLessEqual(c_off + c_len, data_size)
                    self.assertLessEqual(m_off + m_len, data_size)
                    # 立绘颜色是无损 RGB565(每像素 2 字节),遮罩是 4bpp(每行字节对齐)。
                    self.assertEqual(c_len, w * h * 2, "RGB565 颜色长度与 w/h 不匹配")
                    self.assertEqual(m_len, ((w + 1) // 2) * h, "4bpp 遮罩长度与 w/h 不匹配")
                    self.assertLess(x + w, SCREEN_W + 1)
                    # 立绘贴右边、底边贴屏幕底:下半身落在半透明文本框下面,允许越过 BOX_Y。
                    self.assertGreaterEqual(x + w, SPRITE_RIGHT_EDGE_MIN, "立绘没贴右边")
                    self.assertGreaterEqual(y + h, SPRITE_BOTTOM_EDGE_MIN, "立绘没贴到屏幕底部")
                    self.assertLessEqual(y + h, SCREEN_H)
        # 0 号背景固定是标题画面
        off, count, _size = self.section(SEC_BG)
        self.assertGreater(struct.unpack_from("<I", self.blob, off + 4)[0], 0)
        # 事件 CG 与纯色幕必须打上"不叠立绘"标志(否则立绘会盖在插图或黑幕上)。
        no_sprite = sum(1 for i in range(count)
                        if struct.unpack_from("<IIHHH", self.blob, off + i * BG_ENTRY)[4] & 1)
        self.assertGreater(no_sprite, 50, f"CG/纯色幕只有 {no_sprite} 张,标志可能没生效")
        # 每张立绘都要能追到一个说话人(或明确标成 NONE),否则运行时会被整片收起。
        fg_off, fg_count, _fg_size = self.section(SEC_FG)
        ownerless = sum(1 for i in range(fg_count)
                        if struct.unpack_from("<IIIIHHHHH", self.blob,
                                              fg_off + i * FG_ENTRY)[8] == NONE)
        self.assertLessEqual(ownerless, 2, f"有 {ownerless} 张立绘追不到归属角色")

    def test_meta_reports_expected_counts(self) -> None:
        off, _count, size = self.section(SEC_META)
        meta = {}
        for line in self.blob[off : off + size].decode("utf-8").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                meta[key] = value
        self.assertEqual(meta.get("chapters"), str(self.section(SEC_CHAPTER)[1]))
        self.assertEqual(meta.get("scenes"), str(self.section(SEC_SCENE)[1]))
        self.assertEqual(meta.get("dialogues"), str(self.section(SEC_DLG)[1]))
        self.assertIn("github.com/liuyuze61/", meta.get("source_repo", ""))
        self.assertEqual(meta.get("screen"), f"{SCREEN_W}x{SCREEN_H}")
        self.assertEqual(meta.get("fg_format"), "rgb565+4bpp-mask",
                         "立绘编码必须是无损 RGB565 + 4bpp 遮罩")


if __name__ == "__main__":
    unittest.main(verbosity=2)
