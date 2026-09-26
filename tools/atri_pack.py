#!/usr/bin/env python3
"""生成 AI Passport《ATRI -My Dear Moments-》阅读器的资源包。

来源项目:https://github.com/liuyuze61/ATRI-miband
  - 小米手环 9(Xiaomi Vela / aiot quick app)上的《ATRI -My Dear Moments-》同人移植。
  - 素材与译文版权归原作品与移植者所有;本仓库只保存转换产物,不再分发源素材。
  - 源仓库自带免责声明:请支持正版。

资源包是一个小端二进制,固件直接从 Flash 读,没有解压、没有运行时 JSON 解析。布局:

  header  : magic "ATRIPK01", version, total size, section count, section table
  section : { type u32, offset u32, count u32, size u32 }
    SEC_TEXT    UTF-8 blob;每个字符串按 (offset, length) 寻址
    SEC_NAME    角色名 / 选项文案 / 结局名: { off u32, len u16, pad u16 }
    SEC_CHAPTER { id u16, flags u8, branch_count u8,
                  first_scene u16, scene_count u16,
                  first_dlg u16, dlg_count u16,
                  next u16, branch_bad u16, branch_pick u16, pad u16 } = 20 字节
    SEC_SCENE   { bg u16, ovl u16, ovl_x i16, ovl_y i16,
                  first_dlg u16, dlg_count u16, choice_count u8, pad u8,
                  choice_name[2] u16, choice_jump[2] u16 }
    SEC_DLG     { text_off u32, text_len u16, name u16, flags u8, jump u8, arg u16 }
    SEC_BG      { off u32, len u32, w u16, h u16 }(off 相对 SEC_BG 数据区)
    SEC_OVL     { color_off u32, color_len u32, mask_off u32, mask_len u32, w u16, h u16 }

叠加图为什么存原始 RGB565 而不是 JPEG:设备端只有几十 KB 堆,全屏叠加的 JPEG 解码
缓冲(最坏 100KB)根本拿不出来。背景解码成 240x210 画布是“解码到画布本身”,
没有额外缓冲,所以背景继续用 JPEG;叠加改成无损 RGB565 + 4bpp 遮罩,合成时直接从
Flash 逐行读进画布 —— 零解码、零缓冲、字面量无损,代价只是多占几百 KB Flash(本
产品 8MB Flash、app 分区还有 5MB 空闲,划算)。
    SEC_META    UTF-8 key=value 文本(来源仓库 / 转换参数)

画面换算(固定屏幕 240x320,画面区 240x210,下方 110px 是文本框):
  源素材是手环的 336x480 全屏图 -> 整体等比缩放到 240x343,再取顶部 210 行。
  源 App 的文本框盖住 315..480 行,换算过来正好是 210..320,所以画面区与源版一致。
  背景 336x480    -> 上述裁剪 -> JPEG(q80);透明背景先合成到黑底
  叠加 336x480 等  -> 同样的缩放与裁剪;带 alpha 的素材额外存 4bpp 遮罩,
                     绘制位置 = (ImgLeft, ImgTop) * 240/336
  标题画面(bg.png)固定放在背景表 0 号;TRUE END 标题叠加固定放在叠加表 0 号。

用法:
  python tools/atri_pack.py --source <ATRI-miband checkout> \\
      --out main/atri_data/atri_pack.bin

生成物提交进仓库,普通 checkout 就能编译;只有需要重新生成时才要源素材 checkout。
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import struct
import sys
import unicodedata
from typing import Dict, List, Optional, Tuple

try:
    from PIL import Image
except ImportError:  # pragma: no cover - tool dependency
    sys.exit("需要 Pillow: python -m pip install pillow")

MAGIC = b"ATRIPK01"
VERSION = 1
GEN_VERSION = "atri_pack/2"

# 屏幕换算:源素材是 336x480,画面区是 240x210(见文件头说明)。
SRC_W, SRC_H = 336, 480
ART_W, ART_H = 240, 320
SCALE = ART_W / SRC_W
BG_QUALITY = 88
# 叠加图不再用 JPEG,这个常量保留给文档说明(见 atri_pack 文件头)。
OVL_QUALITY = 82

SEC_TEXT, SEC_NAME, SEC_CHAPTER, SEC_SCENE, SEC_DLG, SEC_BG, SEC_OVL, SEC_META = range(8)

CH_HAS_BRANCH = 1 << 0
CH_IS_BAD_END = 1 << 1
CH_IS_TRUE_END = 1 << 2
CH_IS_HAPPY_END = 1 << 3

DLG_END = 1 << 0        # arg = 结局名(SEC_NAME 下标)
DLG_TO_SCENE = 1 << 1   # 本句是场景末句,之后跳转到 scene + jump
DLG_BRANCH = 1 << 2     # 本章末句,按选择历史决定 next / branch_bad

NONE = 0xFFFF
# 背景表 0 号固定是标题画面,TITLE_SOURCE 是源素材里的文件名。
TITLE_SOURCE = "bg"
# 叠加表 0 号固定是 TRUE END 标题叠加(源 App 里达成两个结局后标题页显示的图)。
TRUE_END_SOURCE = "ATRI_TrueEnding"

# 源脚本里引用但素材仓库缺失的图:按最接近的同类素材重定向。
# 依据是同一段剧情的上下文(见每条注释),不是随便挑一张。
BG_ALIASES = {
    "bg0015": "bg015",      # b122 场景 14:明显是 bg015 的手滑多打一个 0
    "bg011": "bg011a",      # b401 场景 3:bg011 家族只有 a/e 两个变体
    "bg015n2": "bg015n",    # b405/b406:夜版 bg015 只有 bg015n
    "bg002n": "bg002n2",    # b406 场景 8:夜版 bg002 只有 bg002n2
    "ev102b": "ev102a",     # b206 场景 10:紧邻的上下两幕都是 ev102a
    "ev012a": "ev012c",     # b304 场景 7:ev012 家族只有 c
}

# 章节顺序 = 源 App detail.ux 里 chapterList 的顺序(b999 是序章)。
CHAPTER_ORDER = [
    "b999", "b101", "b102", "b103", "b111", "b112", "b113", "b114",
    "b121", "b122", "b123", "b124", "b200", "b201", "b202", "b203", "b204",
    "b205", "b206", "b207", "b301", "b302", "b303", "b304", "b401", "b402",
    "b403", "b404", "b405", "b406", "b407", "b501", "b601", "b701",
]
# 三个结局章(源 App 里 loadData('BE') / loadData('TE') 的目标,HE 是主线末章)
HAPPY_END_CHAPTER = "b501"
BAD_END_CHAPTER = "b601"
TRUE_END_CHAPTER = "b701"


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


class Strings:
    """UTF-8 blob,按字节精确去重。"""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._index: Dict[bytes, int] = {}

    def add(self, text: str) -> Tuple[int, int]:
        raw = text.encode("utf-8")
        if not raw:
            return 0, 0
        off = self._index.get(raw)
        if off is None:
            off = len(self._buf)
            self._buf.extend(raw)
            self._index[raw] = off
        return off, len(raw)

    @property
    def data(self) -> bytes:
        return bytes(self._buf)


def clean_text(text: str) -> str:
    """源数据里混了不可显示的控制字符与 CJK 兼容汉字,转换时清理掉。

    - C0 控制字符不是可排版字符,直接删除;
    - CJK 兼容汉字区(U+F900-U+FAFF)按 NFKC 归一到标准汉字,字形等价但字体一定覆盖。
    """
    out = []
    for ch in text:
        if ord(ch) < 0x20:
            continue
        if 0xF900 <= ord(ch) <= 0xFAFF:
            ch = unicodedata.normalize("NFKC", ch)
        out.append(ch)
    return "".join(out)


def chapter_key(name: str) -> int:
    m = re.match(r"[a-z]*(\d+)", name)
    return int(m.group(1)) if m else 1 << 30


def scale_size(w: int, h: int) -> Tuple[int, int]:
    return max(1, round(w * SCALE)), max(1, round(h * SCALE))


def crop_top(im: Image.Image) -> Image.Image:
    """缩放后取顶部画面区高度(模型:文本区盖住下方,再往下的内容看不到)。"""
    if im.height > ART_H:
        im = im.crop((0, 0, im.width, ART_H))
    return im


def scale_art(im: Image.Image) -> Image.Image:
    w, h = scale_size(im.width, im.height)
    resized = im.resize((w, h), Image.LANCZOS)
    return crop_top(resized)


def pack_alpha_4bpp(alpha: Image.Image) -> bytes:
    """4bpp alpha 遮罩,每行字节对齐,高半字节是左侧像素。"""
    w, h = alpha.size
    stride = (w + 1) // 2
    px = alpha.load()
    out = bytearray(stride * h)
    for y in range(h):
        row = y * stride
        for x in range(w):
            v = px[x, y]
            nib = min(15, (v * 15 + 127) // 255)
            if x % 2 == 0:
                out[row + x // 2] |= nib << 4
            else:
                out[row + x // 2] |= nib
    return bytes(out)


class PackBuilder:
    def __init__(self, source: str) -> None:
        self.src = source
        self.common = self._find_common(source)
        self.strings = Strings()
        self.names: List[Tuple[int, int]] = []
        self._name_index: Dict[str, int] = {}
        self.bg_list: List[str] = []
        self.bg_blobs: List[bytes] = []
        self._bg_index: Dict[str, int] = {}
        self.ovl_list: List[str] = []
        self.ovl_blobs: List[Tuple[bytes, bytes, int, int]] = []
        self._ovl_index: Dict[str, Tuple[int, int, int]] = {}
        self.counts: Dict[str, int] = {}
        self.warnings: List[str] = []

    @staticmethod
    def _find_common(source: str) -> str:
        for rel in (os.path.join("src", "common"), os.path.join("mb9", "common"), "common"):
            path = os.path.join(source, rel)
            if os.path.isdir(path):
                return path
        raise SystemExit(f"在 {source} 下找不到 common/ 素材目录")

    def asset(self, stem: str, suffix: str = ".png") -> Optional[str]:
        path = os.path.join(self.common, stem + suffix)
        return path if os.path.exists(path) else None

    def name_id(self, text: str) -> int:
        if not text:
            return NONE
        got = self._name_index.get(text)
        if got is not None:
            return got
        off, ln = self.strings.add(text)
        idx = len(self.names)
        self.names.append((off, ln))
        self._name_index[text] = idx
        return idx

    # ---------------------------------------------------------------- 图像
    def _resolve_bg(self, name: str) -> Optional[str]:
        """把脚本里的背景名解析成仓库里真实存在的素材名(含别名与去扩展名)。"""
        if not name:
            return None
        stem = os.path.splitext(os.path.basename(name))[0].lower()
        for candidate in (stem, BG_ALIASES.get(stem, "")):
            if candidate and self.asset(candidate):
                if candidate != stem:
                    self.warnings.append(f"背景 {name} 缺失,改用 {candidate}.png")
                return candidate
        return None

    def bg_id(self, name: str) -> int:
        stem = self._resolve_bg(name)
        if stem is None:
            if name:
                self.warnings.append(f"背景 {name} 缺失且无别名,按黑屏处理")
            return NONE
        got = self._bg_index.get(stem)
        if got is not None:
            return got
        im = Image.open(os.path.join(self.common, stem + ".png"))
        # 源 App 的底层是黑底 div,透明背景透出黑色;JPEG 没有 alpha,先合成到黑底。
        if im.mode in ("P", "RGBA", "LA") and im.info.get("transparency") is not None:
            rgba = im.convert("RGBA")
            flat = Image.new("RGB", rgba.size, (0, 0, 0))
            flat.paste(rgba, mask=rgba.getchannel("A"))
            im = flat
        art = scale_art(im.convert("RGB"))
        buf = io.BytesIO()
        art.save(buf, "JPEG", quality=BG_QUALITY, optimize=True, subsampling=2)
        idx = len(self.bg_list)
        self.bg_list.append(stem)
        self.bg_blobs.append(buf.getvalue())
        self._bg_index[stem] = idx
        return idx

    def ovl_id(self, name: str) -> Tuple[int, int, int]:
        """叠加图:返回 (下标, 裁剪偏移 x, 裁剪偏移 y)。

        带 alpha 的叠加会按 alpha 包围盒裁掉四周全透明的部分 —— 既省 Flash,也让设备端
        解码缓冲小一大截(设备只有 70KB 左右堆,全屏叠加的解码缓冲拿不出来时会退化)。
        调用方要把裁剪偏移加到绘制位置上。
        """
        if not name:
            return NONE, 0, 0
        stem = os.path.splitext(os.path.basename(name))[0].lower()
        path = self.asset(stem)
        if path is None:
            self.warnings.append(f"叠加图 {name} 缺失,跳过")
            return NONE, 0, 0
        got = self._ovl_index.get(stem)
        if got is not None:
            return got

        im = Image.open(path)
        has_alpha = im.mode in ("P", "RGBA", "LA") and im.info.get("transparency") is not None
        rgba = im.convert("RGBA")
        art = scale_art(rgba)
        dx = dy = 0
        if has_alpha:
            bbox = art.getchannel("A").getbbox()
            if bbox is None:
                self.warnings.append(f"叠加图 {name} 全透明,跳过")
                return NONE, 0, 0
            dx, dy = bbox[0], bbox[1]
            art = art.crop(bbox)

        # 颜色存 RGB565(小端两字节),遮罩存 4bpp(每行字节对齐)。
        rgb = art.convert("RGB")
        px = rgb.load()
        color = bytearray(art.width * art.height * 2)
        for y in range(art.height):
            for x in range(art.width):
                r, g, b = px[x, y]
                value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                off = (y * art.width + x) * 2
                color[off] = value & 0xFF
                color[off + 1] = (value >> 8) & 0xFF
        mask = pack_alpha_4bpp(art.getchannel("A")) if has_alpha else b""

        idx = len(self.ovl_list)
        self.ovl_list.append(stem)
        self.ovl_blobs.append((bytes(color), mask, art.width, art.height))
        self._ovl_index[stem] = (idx, dx, dy)
        log(f"  叠加 #{idx} {stem} {art.width}x{art.height} "
            f"color {len(color) / 1024:.1f} KB mask {len(mask) / 1024:.1f} KB")
        return idx, dx, dy

    # ---------------------------------------------------------------- 剧本
    def load_chapters(self) -> List[dict]:
        chapters = []
        for stem in CHAPTER_ORDER:
            path = os.path.join(self.common, stem + ".txt")
            if not os.path.exists(path):
                raise SystemExit(f"缺少章节脚本: {path}")
            data = json.loads(open(path, "rb").read().decode("utf-8"))
            chapters.append({"stem": stem, "scenes": data})
        return chapters

    def build(self, meta: Dict[str, str]) -> bytes:
        # 0 号背景 = 标题画面;0 号叠加 = TRUE END 标题叠加。
        if self.bg_id(TITLE_SOURCE) != 0:
            raise SystemExit(f"标题画面缺失: {TITLE_SOURCE}.png")
        if self.ovl_id(TRUE_END_SOURCE)[0] != 0:
            raise SystemExit(f"TRUE END 叠加缺失: {TRUE_END_SOURCE}.png")

        chapters = self.load_chapters()
        chapter_index = {c["stem"]: i for i, c in enumerate(chapters)}

        scene_recs: List[tuple] = []
        dlg_recs: List[tuple] = []
        chapter_recs: List[tuple] = []
        stats = {"scenes": 0, "dlg": 0, "choices": 0, "ovl": 0, "chars": 0}
        branch = None

        for ci, chapter in enumerate(chapters):
            first_scene = len(scene_recs)
            first_dlg = len(dlg_recs)
            scenes = chapter["scenes"]
            flags = 0
            branch = None
            pick = 0
            pick_count = 0
            branch_bad = NONE

            for si, scene in enumerate(scenes):
                scene_first_dlg = len(dlg_recs)
                bg = self.bg_id(scene.get("background", "") or "")
                ovl = NONE
                ovl_x = ovl_y = 0
                ovl_name = scene.get("Img")
                if ovl_name:
                    ovl, ovl_dx, ovl_dy = self.ovl_id(str(ovl_name))
                    if ovl != NONE:
                        ovl_x = max(0, round(int(scene.get("ImgLeft") or 0) * SCALE)) + ovl_dx
                        ovl_y = max(0, round(int(scene.get("ImgTop") or 0) * SCALE)) + ovl_dy
                        stats["ovl"] += 1

                choices = scene.get("choices") or []
                if len(choices) > 2:
                    raise SystemExit(
                        f"{chapter['stem']} 场景 {si} 有 {len(choices)} 个选项,格式只支持 2 个")
                choices = [c for c in choices if c]
                if choices:
                    stats["choices"] += 1

                for di, dlg in enumerate(scene.get("dialogues") or []):
                    text = clean_text(str(dlg.get("text", "") or ""))
                    text_off, text_len = self.strings.add(text)
                    stats["chars"] += len(text)
                    name = self.name_id(clean_text(str(dlg.get("character", "") or "")))
                    dflags = 0
                    jump = 0
                    arg = 0
                    if "toScenes" in dlg:
                        dflags |= DLG_TO_SCENE
                        jump = int(dlg["toScenes"])
                    if "END" in dlg:
                        dflags |= DLG_END
                        arg = self.name_id(clean_text(str(dlg["END"])))
                    if "branch" in dlg:
                        if di != len(scene["dialogues"]) - 1 or si != len(scenes) - 1:
                            raise SystemExit(
                                f"{chapter['stem']} 的 branch 不在本章最后一句上,需要核对")
                        dflags |= DLG_BRANCH
                        flags |= CH_HAS_BRANCH
                        branch = dlg["branch"]
                        picks = [int(p) for p in branch.get("choices", [])]
                        if len(picks) > 8:
                            raise SystemExit("选择历史超过 8 项,u16 位图放不下")
                        pick_count = len(picks)
                        for i, p in enumerate(picks):
                            pick |= (p & 0x3) << (2 * i)
                        bad = chapter_index.get(BAD_END_CHAPTER)
                        if bad is None:
                            raise SystemExit("分支缺少 BE 章节")
                        branch_bad = bad
                    dlg_recs.append((text_off, text_len, name, dflags, jump, arg))
                stats["scenes"] += 1

                choice_names = [self.name_id(clean_text(str(c.get("text", "")))) for c in choices]
                choice_jumps = [int(c.get("nextScene", 0)) for c in choices]
                while len(choice_names) < 2:
                    choice_names.append(NONE)
                    choice_jumps.append(0)
                scene_recs.append((bg, ovl, ovl_x, ovl_y, scene_first_dlg,
                                   len(scene.get("dialogues") or []),
                                   len(choices), 0, choice_names[0], choice_names[1],
                                   choice_jumps[0], choice_jumps[1]))
                if len(scene_recs) >= NONE:
                    raise SystemExit("场景数超过 u16 上限")

            chapter_id = chapter_key(chapter["stem"])
            if chapter["stem"] == HAPPY_END_CHAPTER:
                flags |= CH_IS_HAPPY_END
            if chapter["stem"] == BAD_END_CHAPTER:
                flags |= CH_IS_BAD_END
            if chapter["stem"] == TRUE_END_CHAPTER:
                flags |= CH_IS_TRUE_END

            if flags & CH_HAS_BRANCH:
                target = chapter_index.get(_chapter_stem(branch.get("toChapter")))
                if target is None:
                    raise SystemExit(f"{chapter['stem']} 的分支目标章节不存在")
                next_index = target
            else:
                next_index = ci + 1 if ci + 1 < len(chapters) else NONE
            chapter_recs.append((chapter_id, flags, pick_count, first_scene,
                                 len(scenes), first_dlg, len(dlg_recs) - first_dlg,
                                 next_index, branch_bad, pick, 0))

        self.counts = {
            "chapter": len(chapter_recs),
            "scene": len(scene_recs),
            "dlg": len(dlg_recs),
            "bg": len(self.bg_list),
            "ovl": len(self.ovl_list),
        }
        stats["dlg"] = len(dlg_recs)

        text = self.strings.data
        name_tab = b"".join(struct.pack("<IHH", off, ln, 0) for off, ln in self.names)
        chapter_tab = b"".join(struct.pack("<HBBHHHHHHHH", *rec) for rec in chapter_recs)
        scene_tab = b"".join(struct.pack("<HHhhHHBBHHHH", *rec) for rec in scene_recs)
        dlg_tab = b"".join(struct.pack("<IHHBBH", *rec) for rec in dlg_recs)

        bg_tab = bytearray()
        bg_blob = bytearray()
        for jpeg in self.bg_blobs:
            bg_tab += struct.pack("<IIHH", len(bg_blob), len(jpeg), ART_W, ART_H)
            bg_blob += jpeg
        ovl_tab = bytearray()
        ovl_blob = bytearray()
        for color, mask, w, h in self.ovl_blobs:
            while len(ovl_blob) % 4:
                ovl_blob += b"\x00"   # RGB565 按 u16 对齐(设备端按 16 位读)
            ovl_tab += struct.pack("<IIIIHH", len(ovl_blob), len(color),
                                   len(ovl_blob) + len(color), len(mask), w, h)
            ovl_blob += color
            ovl_blob += mask

        meta_with_tables = dict(meta)
        meta_with_tables["bg_list"] = ",".join(self.bg_list)
        meta_with_tables["ovl_list"] = ",".join(self.ovl_list)
        meta_text = "\n".join(f"{k}={v}" for k, v in meta_with_tables.items()).encode("utf-8")
        sections = [
            (SEC_TEXT, text, 0),
            (SEC_NAME, name_tab, len(self.names)),
            (SEC_CHAPTER, chapter_tab, len(chapter_recs)),
            (SEC_SCENE, scene_tab, len(scene_recs)),
            (SEC_DLG, dlg_tab, len(dlg_recs)),
            (SEC_BG, bytes(bg_tab) + bytes(bg_blob), len(self.bg_list)),
            (SEC_OVL, bytes(ovl_tab) + bytes(ovl_blob), len(self.ovl_list)),
            (SEC_META, meta_text, 0),
        ]

        header_len = 8 + 4 + 4 + 4 + 16 * len(sections)
        offset = header_len
        table = bytearray()
        blob = bytearray()
        for sec_type, payload, count in sections:
            # 每段都从 4 字节边界开始:叠加颜色在设备端按 u16 读,段内偏移不能是奇数。
            while offset % 4:
                blob += b"\x00"
                offset += 1
            table += struct.pack("<IIII", sec_type, offset, count, len(payload))
            blob += payload
            offset += len(payload)

        out = bytearray(MAGIC)
        out += struct.pack("<III", VERSION, offset, len(sections))
        out += table
        out += blob
        assert len(out) == offset, (len(out), offset)

        for w in sorted(set(self.warnings)):
            log(f"  警告: {w}")
        log(f"  章节 {stats and len(chapter_recs)} / 场景 {stats['scenes']} / 对白 {stats['dlg']}"
            f" / 带叠加场景 {stats['ovl']} / 选项场景 {stats['choices']}")
        log(f"  背景 {len(self.bg_list)} 张、叠加 {len(self.ovl_list)} 张、"
            f"正文 {stats['chars']} 字")
        log(f"  文本 {len(text) / 1024:.0f} KB, 背景 {len(bg_blob) / 1024:.0f} KB, "
            f"叠加 {len(ovl_blob) / 1024:.0f} KB, 合计 {len(out) / 1024 / 1024:.2f} MB")
        return bytes(out)


def _chapter_stem(number: object) -> str:
    """源脚本用 chapterList 下标表示跳转目标,这里换算回 bNNN 名字。"""
    try:
        idx = int(number)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return str(number)
    if 0 <= idx < len(CHAPTER_ORDER):
        return CHAPTER_ORDER[idx]
    return ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, help="ATRI-miband checkout 路径")
    ap.add_argument("--out", required=True, help="输出 pack 路径")
    ap.add_argument("--commit", default="", help="源仓库 commit(记录到元数据)")
    args = ap.parse_args()

    meta = {
        "source": "https://github.com/liuyuze61/ATRI-miband",
        "commit": args.commit or "unknown",
        "generator": GEN_VERSION,
        "art": f"{ART_W}x{ART_H}",
        "scale": f"{SRC_W}x{SRC_H}->{ART_W}x{round(SRC_H * SCALE)}",
        "bg_quality": str(BG_QUALITY),
        "ovl_format": "rgb565+4bpp-mask",
    }
    data = PackBuilder(args.source).build(meta)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(data)
    log(f"  写出 {args.out} ({len(data)} 字节) sha256={hashlib.sha256(data).hexdigest()[:16]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
