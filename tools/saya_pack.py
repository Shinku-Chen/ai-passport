#!/usr/bin/env python3
"""Build the Saya no Uta resource pack for the AI Passport port.

Source project: https://github.com/liuyuze61/Saya-miband10
  - 小米手环 10(Xiaomi Vela / aiot quick app)上的《沙耶之歌》同人移植。
  - 素材与译文版权归原作品与移植者所有;本仓库只保存转换产物,不再分发源素材。
  - 源仓库自带免责声明:请支持正版。

The pack is a single little-endian binary read straight out of flash by the
firmware (no decompression, no per-record parsing).  Layout:

  header  : magic "SAYAPK01", version, total size, section count, section table
  section : { type u32, offset u32, count u32, size u32 }
    SEC_TEXT    UTF-8 blob; every string is addressed by (offset, length)
    SEC_NAME    记录名表: { off u32, len u16, pad u16 }  (角色名 / 选项文案 / 结局名)
    SEC_CHAPTER { id u16, first_scene u16, scene_count u16,
                  first_dlg u16, dlg_count u16, next u16 }   next=0xFFFF 表示无后续章
    SEC_SCENE   { bg u16, choice_count u8, pad u8, first_dlg u16, dlg_count u16,
                  choice_name[2] u16, choice_href[2] u16 }
    SEC_DLG     { text_off u32, text_len u16, name u16, fg u16, flags u8,
                  jump u8, arg u16 }  fg=0xFFFF 表示本句未指定(沿用上一句)
    SEC_BG      { off u32, len u32, w u16, h u16 }
    SEC_FG      { jpeg_off u32, jpeg_len u32, mask_off u32, mask_len u32, w u16, h u16 }
    SEC_STRIP   { off u32, len u32, w u16, h u16 }(与背景表同下标;文本框背后的半分辨率条带)
    SEC_FG_LOW  { jpeg_off, jpeg_len, mask_off, mask_len(全 u32), w u16, h u16 }(与立绘表同下标;立绘下半段)
    SEC_META    UTF-8 key=value 文本(来源仓库 / commit / 转换参数)

Image conversion (fixed screen layout 320x240, drawn as two 1:1 canvases):
  背景 283x212 调色板 PNG -> cover 成 320x240 -> 上半 320x150 + 下半 320x90(均 1:1)-> JPEG
  标题画面(bg.png)       -> 同上,固定放在背景表 0 号(SAYA_BG_TITLE,脚本不引用它)
  立绘 212xN RGBA      -> 按屏幕高度 240 缩放(宽度上限 180)-> 上半 150 行 + 下半 90 行
                          -> JPEG + 1bpp 遮罩(按行打包)

Usage:
  python tools/saya_pack.py --source assets/saya-source \
      --out main/saya_data/saya_pack.bin
  # 也可以指向上游 Saya-miband10 checkout(素材在 src/common/ 下)

仓库内提交的 assets/saya-source/ 就是打包所需的全部基础素材(剧本 + 背景 + 立绘),
所以 clone 之后不需要外部 checkout 就能重建 pack;只有含补丁的 release 变体还需要
上游仓库的 补丁/ 目录(该目录不提交)。

Variants (发布规则,必须分清):
  community(默认) 只读 src/common/{sy,cg,fg},不含源仓库 补丁/ 里的任何内容。
                  仓库里提交的 main/saya_data/saya_pack.bin 就是这个版本 ——
                  可以随固件一起发布到 AI Passport 社区市场。
  release(--patch)  用 补丁/ 里的同名章节替换基础脚本,并把 补丁/*.png(追加 CG)
                  并入背景表。**只允许自用**:生成的 pack 不要提交、不要上传,
                  也不要用它构建面/发布给社区的固件。
                  补丁可能引入新汉字:重建后用两个 pack 一起重生成字体
                  (见 tools/saya_font.py --pack 可给多次),否则会缺字。

The generated pack is committed so a plain checkout can build the firmware;
only regenerating it needs the source checkout.  The committed one is always
the community variant.
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

MAGIC = b"SAYAPK01"
VERSION = 1
GEN_VERSION = "saya_pack/1"

ART_W, ART_H = 320, 150          # 画面区(横屏 320x240;其下是 82px 文本框 + 8px 底边距)
SCREEN_H = 240                   # 横屏屏幕高度(幅面/条带按它换算)
SPRITE_SCREEN_H = 240            # 立绘按屏幕高度缩放
# 整幅画面完整显示:上半 150 行进画面区画布,下半 90 行(第 150..239 行)1:1 存成
# 下半块,设备端原尺寸铺在文本框背后。两块拼起来就是完整的 320x240,不裁切不缩放。
STRIP_W, STRIP_H = 320, 90
STRIP_QUALITY = 75
SPRITE_MAX_W = 180               # 立绘宽度上限,限制设备端解码缓冲
BG_QUALITY = 80
FG_QUALITY = 80

SEC_TEXT, SEC_NAME, SEC_CHAPTER, SEC_SCENE, SEC_DLG, SEC_BG, SEC_FG, SEC_META = range(8)
# 8 = 文本框背后那条带:每张背景一份「主画面正下方」的小图,让半透明文本框透出真实画面。
SEC_STRIP = 8
# 9 = 立绘下半段(与立绘表同下标):全分辨率打包,设备端按 2:1 采样合成进条带画布。
SEC_FG_LOW = 9

DLG_END = 1 << 0        # arg = 结局名(SEC_NAME 下标)
DLG_BRANCH = 1 << 1     # 选择分支(本移植数据里未使用,保留语义)
DLG_TO_SCENE = 1 << 2   # 本句后跳转到 current_scene + jump

NONE = 0xFFFF
FG_KEEP = 0xFFFF
# 背景表 0 号固定是标题画面(与 main/saya_pack.h 的 SAYA_BG_TITLE 对应)。
TITLE_SOURCE = "bg"   # cg/bg.png,脚本里没有引用
# 源剧本里的音效指令:本移植没有音效资源,打包时剥掉,不要当正文显示。
SE_TAG_RE = re.compile(r"<se\b[^<>]*>", re.IGNORECASE)
# 打包期间的清理统计(结束时打日志)。
STRIPPED = {"se": 0}


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


class Strings:
    """UTF-8 blob with byte-exact dedup."""

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
    """源数据里混了不可显示的控制字符、音效指令与 CJK 兼容汉字,转换时清理掉。

    - C0 控制字符(如 U+001F)直接删除,它们不是可排版字符;
    - 音效指令(如 <se id="1" src="門開け" mode="normal" loop="off">)剔除:
      本移植没有音效资源,留在正文里会在对话框里当文字显示出来;
    - CJK 兼容汉字区(U+F900-U+FAFF)按 NFKC 归一到标准汉字(如 U+FA12 -> 晴),
      字形完全等价,只是换成了字体一定覆盖的码位。
    """
    text, removed = SE_TAG_RE.subn("", text)
    if removed:
        STRIPPED["se"] += removed
    out = []
    for ch in text:
        if ord(ch) < 0x20:
            continue
        if 0xF900 <= ord(ch) <= 0xFAFF:
            ch = unicodedata.normalize("NFKC", ch)
        out.append(ch)
    return "".join(out)


def chapter_sort_key(name: str) -> Tuple[int, str]:
    m = re.match(r"(\d+)", name)
    return (int(m.group(1)) if m else 1 << 30, name)


def cover_crop(im: Image.Image, size: Tuple[int, int], top: bool = False) -> Image.Image:
    """按 cover 缩放后裁切。top=True 时纵向贴顶(画面取景用),否则居中。"""
    tw, th = size
    sw, sh = im.size
    scale = max(tw / sw, th / sh)
    resized = im.resize((max(1, round(sw * scale)), max(1, round(sh * scale))), Image.LANCZOS)
    x = (resized.width - tw) // 2
    y = 0 if top else (resized.height - th) // 2
    return resized.crop((x, y, x + tw, y + th))


def sprite_crop(im: Image.Image) -> Image.Image:
    """立绘 -> 按屏幕高度缩放、限宽,保留整高(上半段进画面区,下半段进条带)。"""
    w, h = im.size
    scale = SPRITE_SCREEN_H / h
    if round(w * scale) > SPRITE_MAX_W:
        scale = SPRITE_MAX_W / w
    w2 = max(1, round(w * scale))
    h2 = max(1, round(h * scale))
    resized = im.resize((w2, h2), Image.LANCZOS)
    if h2 > SCREEN_H:
        resized = resized.crop((0, 0, w2, SCREEN_H))
    return resized


def pack_mask(alpha: Image.Image) -> bytes:
    """1bpp 遮罩,按行打包(每行 ceil(w/8) 字节);PIL 的 "1" 模式本来就是行打包。"""
    if alpha.mode != "1":
        alpha = alpha.point(lambda v: 255 if v > 127 else 0).convert("1")
    w, h = alpha.size
    stride = (w + 7) // 8
    data = alpha.tobytes()
    return data[: stride * h]


def source_dirs(source: str) -> Tuple[str, str, str]:
    """定位剧本/背景/立绘目录。

    两种布局都支持:
      1. 仓库内提交的 assets/saya-source/(直接放 sy、cg、fg);
      2. 上游 Saya-miband10 checkout(素材在 src/common/ 下)。
    """
    nested = os.path.join(source, "src", "common")
    root = nested if os.path.isdir(os.path.join(nested, "sy")) else source
    dirs = tuple(os.path.join(root, name) for name in ("sy", "cg", "fg"))
    missing = [d for d in dirs if not os.path.isdir(d)]
    if missing:
        raise SystemExit("源素材目录缺失: " + ", ".join(missing)
                         + "(用 --source 指向 assets/saya-source 或上游 checkout)")
    return dirs  # type: ignore[return-value]


class PackBuilder:
    def __init__(self, source: str, patch_dir: str = "") -> None:
        self.src = source
        self.sy_dir, self.cg_dir, self.fg_dir = source_dirs(source)
        # 补丁目录(完整版 内容,只给 release 变体用):同名章节覆盖基础脚本,PNG 并入背景表。
        self.patch_dir = patch_dir
        self.patched_chapters: List[str] = []
        self.strings = Strings()
        self.names: List[Tuple[int, int]] = []
        self._name_index: Dict[str, int] = {}
        self.bg_files = self._scan(self.cg_dir)
        if patch_dir:
            self.bg_files.update(self._scan(patch_dir))
        self.fg_files = self._scan(self.fg_dir)
        self.bg_list: List[str] = []
        self.bg_blobs: List[bytes] = []
        self.bg_strips: List[bytes] = []
        self.fg_list: List[str] = []
        self.fg_blobs: List[Tuple[bytes, bytes, int, int]] = []  # jpeg, mask, w, h(上半段)
        # 立绘下半段(进条带):(jpeg, mask, w, h) 或 None
        self.fg_lowers: List[Optional[Tuple[bytes, bytes, int, int]]] = []
        self._bg_index: Dict[str, int] = {}
        self._fg_index: Dict[str, int] = {}
        self.counts: Dict[str, int] = {}

    @staticmethod
    def _scan(folder: str) -> Dict[str, str]:
        return {os.path.splitext(fn)[0].lower(): os.path.join(folder, fn)
                for fn in os.listdir(folder) if fn.lower().endswith(".png")}

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

    # ---------------------------------------------------------------- images
    def bg_id(self, name: str) -> int:
        key = (name or "").lower()
        if key not in self.bg_files:
            return NONE
        got = self._bg_index.get(key)
        if got is not None:
            return got
        im = Image.open(self.bg_files[key]).convert("RGB")
        # 先一次性 cover 成整幅 320x240(纵向贴顶),再从里面切上下两段 —— 两次分开
        # cover 会因为取景锚点/缩放差异让两段对不上(竖构图的标题图尤其明显)。
        full = cover_crop(im, (ART_W, SCREEN_H), top=True)
        art = full.crop((0, 0, ART_W, ART_H))
        buf = io.BytesIO()
        art.save(buf, "JPEG", quality=BG_QUALITY, optimize=True, subsampling=2)
        # 下半块:整幅画面的第 150..239 行,原尺寸(1:1)打包。
        strip = full.crop((0, ART_H, ART_W, ART_H + STRIP_H))
        sbuf = io.BytesIO()
        strip.save(sbuf, "JPEG", quality=STRIP_QUALITY, optimize=True, subsampling=2)
        idx = len(self.bg_list)
        self.bg_list.append(key)
        self.bg_blobs.append(buf.getvalue())
        self.bg_strips.append(sbuf.getvalue())
        self._bg_index[key] = idx
        return idx

    def fg_id(self, name: str) -> int:
        key = (name or "").lower()
        if key not in self.fg_files:
            return NONE
        got = self._fg_index.get(key)
        if got is not None:
            return got
        im = Image.open(self.fg_files[key]).convert("RGBA")
        full = sprite_crop(im)
        upper = full if full.height <= ART_H else full.crop((0, 0, full.width, ART_H))
        lower = None
        if full.height > ART_H:
            # 下半段:全幅第 150..239 行,与上半段拼起来就是完整人物。
            lower = full.crop((0, ART_H, full.width, min(full.height, ART_H + STRIP_H)))
        jpeg = io.BytesIO()
        upper.convert("RGB").save(jpeg, "JPEG", quality=FG_QUALITY, optimize=True, subsampling=2)
        mask = pack_mask(upper.getchannel("A"))
        if lower is not None and lower.height > 0:
            ljpeg = io.BytesIO()
            lower.convert("RGB").save(ljpeg, "JPEG", quality=FG_QUALITY, optimize=True,
                                      subsampling=2)
            lower_blob = (ljpeg.getvalue(), pack_mask(lower.getchannel("A")),
                          lower.width, lower.height)
        else:
            lower_blob = None
        idx = len(self.fg_list)
        self.fg_list.append(key)
        self.fg_blobs.append((jpeg.getvalue(), mask, upper.width, upper.height))
        self.fg_lowers.append(lower_blob)
        self._fg_index[key] = idx
        return idx

    # ---------------------------------------------------------------- scripts
    def script_path(self, fn: str) -> str:
        """release 变体里同名补丁章节优先;没有对应补丁就用基础脚本。"""
        if self.patch_dir:
            patched = os.path.join(self.patch_dir, fn)
            if os.path.exists(patched):
                return patched
        return os.path.join(self.sy_dir, fn)

    def load_chapters(self) -> List[dict]:
        chapters = []
        for fn in sorted(os.listdir(self.sy_dir), key=chapter_sort_key):
            if not fn.endswith(".txt"):
                continue
            path = self.script_path(fn)
            raw = open(path, "rb").read()
            if not raw.strip():
                log(f"  跳过空脚本 {fn}")
                continue
            try:
                data = json.loads(raw.decode("utf-8"))
            except Exception as exc:  # pragma: no cover - source data issue
                log(f"  跳过无法解析的脚本 {fn}: {exc}")
                continue
            if path != os.path.join(self.sy_dir, fn):
                self.patched_chapters.append(fn)
            chapters.append({"file": os.path.splitext(fn)[0], "scenes": data})
        chapters.sort(key=lambda c: chapter_sort_key(c["file"]))
        return chapters

    def build(self, meta: Dict[str, str]) -> bytes:
        # 0 号先放标题画面:标题页复用同一张画面区画布。
        title_id = self.bg_id(TITLE_SOURCE)
        if title_id != 0:
            raise SystemExit(f"标题画面缺失或不是 0 号背景: {TITLE_SOURCE}")
        chapters = self.load_chapters()
        by_file = {c["file"]: i for i, c in enumerate(chapters)}

        scene_recs: List[tuple] = []
        dlg_recs: List[tuple] = []
        chapter_recs: List[tuple] = []

        for ci, chapter in enumerate(chapters):
            first_scene = len(scene_recs)
            first_dlg = len(dlg_recs)
            for scene in chapter["scenes"]:
                scene_first_dlg = len(dlg_recs)
                bg = self.bg_id(scene.get("background", ""))
                if bg == NONE and scene.get("background"):
                    log(f"  警告: 章节 {chapter['file']} 背景缺失 {scene['background']}")
                choices = scene.get("choices") or []
                if len(choices) > 2:
                    raise SystemExit(
                        f"章节 {chapter['file']} 出现 {len(choices)} 个选项,打包格式只支持 2 个")
                dlg_list = scene.get("dialogues") or []
                for dlg in dlg_list:
                    text_off, text_len = self.strings.add(clean_text(dlg.get("text", "") or ""))
                    name = self.name_id(dlg.get("character", "") or "")
                    fg = FG_KEEP
                    if dlg.get("fg"):
                        fg = self.fg_id(dlg["fg"])
                        if fg == NONE:
                            log(f"  警告: 立绘缺失 {dlg['fg']} (章节 {chapter['file']})")
                    flags = 0
                    arg = 0
                    jump = 0
                    if "toScenes" in dlg:
                        flags |= DLG_TO_SCENE
                        jump = int(dlg["toScenes"])
                    if "branch" in dlg:
                        flags |= DLG_BRANCH
                    if "END" in dlg:
                        flags |= DLG_END
                        arg = self.name_id(str(dlg["END"]))
                    dlg_recs.append((text_off, text_len, name, fg, flags, jump, arg))
                choice_names = [self.name_id(clean_text(c.get("text", "") or ""))
                                for c in choices]
                choice_hrefs = []
                for c in choices:
                    target = str(c.get("href", ""))
                    if target not in by_file:
                        raise SystemExit(
                            f"章节 {chapter['file']} 的选项指向不存在的章节 {target!r}")
                    choice_hrefs.append(by_file[target])
                while len(choice_names) < 2:
                    choice_names.append(NONE)
                    choice_hrefs.append(NONE)
                scene_recs.append((bg, len(choices), 0, scene_first_dlg, len(dlg_list),
                                   choice_names[0], choice_names[1],
                                   choice_hrefs[0], choice_hrefs[1]))
            chapter_recs.append((chapter_sort_key(chapter["file"])[0], first_scene,
                                 len(scene_recs) - first_scene, first_dlg,
                                 len(dlg_recs) - first_dlg,
                                 ci + 1 if ci + 1 < len(chapters) else NONE))

        self.counts = {"chapter": len(chapter_recs), "scene": len(scene_recs),
                       "dlg": len(dlg_recs)}

        # ------------------------------------------------------------- 组装
        text = self.strings.data
        name_tab = b"".join(struct.pack("<IHH", off, ln, 0) for off, ln in self.names)
        chapter_tab = b"".join(struct.pack("<HHHHHH", *rec) for rec in chapter_recs)
        scene_tab = b"".join(struct.pack("<HBBHHHHHH", *rec) for rec in scene_recs)
        dlg_tab = b"".join(struct.pack("<IHHHBBH", *rec) for rec in dlg_recs)

        bg_blob = bytearray()
        bg_tab = bytearray()
        for jpeg in self.bg_blobs:
            bg_tab += struct.pack("<IIHH", len(bg_blob), len(jpeg), ART_W, ART_H)
            bg_blob += jpeg
        flow_blob = bytearray()
        flow_tab = bytearray()
        for entry in self.fg_lowers:
            if entry is None:
                flow_tab += struct.pack("<IIIIHH", 0, 0, 0, 0, 0, 0)
                continue
            jpeg, mask, w, h = entry
            flow_tab += struct.pack("<IIIIHH", len(flow_blob), len(jpeg),
                                    len(flow_blob) + len(jpeg), len(mask), w, h)
            flow_blob += jpeg
            flow_blob += mask
        strip_blob = bytearray()
        strip_tab = bytearray()
        for jpeg in self.bg_strips:
            strip_tab += struct.pack("<IIHH", len(strip_blob), len(jpeg), STRIP_W, STRIP_H)
            strip_blob += jpeg
        fg_blob = bytearray()
        fg_tab = bytearray()
        for jpeg, mask, w, h in self.fg_blobs:
            fg_tab += struct.pack("<IIIIHH", len(fg_blob), len(jpeg),
                                  len(fg_blob) + len(jpeg), len(mask), w, h)
            fg_blob += jpeg
            fg_blob += mask

        meta_text = "\n".join(f"{k}={v}" for k, v in meta.items()).encode("utf-8")
        sections = [
            (SEC_TEXT, text, 0),
            (SEC_NAME, name_tab, len(self.names)),
            (SEC_CHAPTER, chapter_tab, len(chapter_recs)),
            (SEC_SCENE, scene_tab, len(scene_recs)),
            (SEC_DLG, dlg_tab, len(dlg_recs)),
            (SEC_BG, bytes(bg_tab) + bytes(bg_blob), len(self.bg_list)),
            (SEC_FG, bytes(fg_tab) + bytes(fg_blob), len(self.fg_list)),
            (SEC_STRIP, bytes(strip_tab) + bytes(strip_blob), len(self.bg_strips)),
            (SEC_FG_LOW, bytes(flow_tab) + bytes(flow_blob), len(self.fg_lowers)),
            (SEC_META, meta_text, 0),
        ]

        header_len = 8 + 4 + 4 + 4 + 16 * len(sections)
        offset = header_len
        table = bytearray()
        blob = bytearray()
        for sec_type, payload, count in sections:
            table += struct.pack("<IIII", sec_type, offset, count, len(payload))
            blob += payload
            offset += len(payload)

        out = bytearray(MAGIC)
        out += struct.pack("<III", VERSION, offset, len(sections))
        out += table
        out += blob
        assert len(out) == offset, (len(out), offset)

        log(f"  章节 {len(chapter_recs)} / 场景 {len(scene_recs)} / 对白 {len(dlg_recs)}"
            f" / 背景 {len(self.bg_list)} / 立绘 {len(self.fg_list)}")
        log(f"  文本 {len(text) / 1024:.0f} KB, 背景 {len(bg_blob) / 1024:.0f} KB, "
            f"立绘 {len(fg_blob) / 1024:.0f} KB, 合计 {len(out) / 1024 / 1024:.2f} MB")
        return bytes(out)


def check_pack(path: str, deep: bool = True) -> int:
    """自检一个已生成的 pack:结构边界、图像尺寸、上下块接缝、立绘遮罩与元数据。

    这里检查的是"设备端会直接消费"的不变量,也就是真正把画面弄坏过的几类问题:
    图像尺寸与画布不符、上下块取景不一致(接缝跳变)、遮罩长度与实际像素不符。
    剧情/文本的语义由 tests/test_saya_model.c 用真实 pack 覆盖,这里不重复。
    """
    import struct
    import sys
    from PIL import Image

    # 与 main/saya_pack.h 保持一致:头部 20 字节;背景/下半块条目 12B,立绘条目 20B。
    HEADER_SIZE = 8 + 4 + 4 + 4
    BG_ENTRY = 12
    FG_ENTRY = 20

    errors = []

    def fail(msg: str) -> None:
        errors.append(msg)

    data = open(path, "rb").read()
    pack_size = len(data)
    if len(data) < HEADER_SIZE or data[:8] != MAGIC:
        print(f"ERROR: {path} 不是资源包(魔数不符)", file=sys.stderr)
        return 1
    version, total, section_count = struct.unpack_from("<III", data, 8)
    if version != VERSION:
        fail(f"版本 {version} != {VERSION}")
    if total != len(data):
        fail(f"头部记录的总大小 {total} != 实际 {len(data)}")
    if not 1 <= section_count <= 32:
        fail(f"分段数 {section_count} 不合理")

    sections = {}
    spans = []
    for i in range(section_count):
        sec_type, off, count, size = struct.unpack_from("<IIII", data, HEADER_SIZE + 16 * i)
        if off + size > len(data):
            fail(f"段 {sec_type} 越界: off={off} size={size}")
            continue
        if sec_type in sections:
            fail(f"段 {sec_type} 重复")
        sections[sec_type] = (off, count, size, data[off:off + size])
        spans.append((off, off + size, sec_type))
    ordered = sorted(spans)
    for (a1, b1, t1), (a2, _b2, t2) in zip(ordered, ordered[1:]):
        if b1 > a2:
            fail(f"段 {t1} 与段 {t2} 数据重叠")

    def image_table(sec_type: int, entry: int):
        """返回(记录表, 数据区, count):图像分段布局是"表 + 数据区",条目偏移相对数据区。"""
        _off, count, size, blob = sections[sec_type]
        if size < count * entry:
            fail(f"段 {sec_type} 的表({count} x {entry}B)超出段大小 {size}")
            return b"", b"", count
        return blob, blob[count * entry:], count

    text_size = sections[0][2] if 0 in sections else 0

    def strings_in_text(off: int, length: int, where: str) -> None:
        if 0 not in sections:
            fail(f"{where} 引用文本但缺少文本段")
            return
        if off + length > text_size:
            fail(f"{where} 文本越界: {off}+{length} > {text_size}")

    def jpeg_ok(blob: bytes, off: int, length: int, where: str) -> bool:
        if off + length > len(blob) or length < 4:
            fail(f"{where} 图像越界: off={off} len={length}")
            return False
        if blob[off:off + 2] != b"\xff\xd8" or blob[off + length - 2:off + length] != b"\xff\xd9":
            fail(f"{where} 不是完整的 JPEG 数据")
            return False
        return True

    # ---- 文本 / 名称 ----
    if 0 in sections:
        try:
            sections[0][3].decode("utf-8")
        except UnicodeDecodeError as exc:
            fail(f"文本段不是合法 UTF-8: {exc}")
    name_count = sections.get(1, (0, 0, 0, b""))[1]
    if 1 in sections:
        blob, count = sections[1][3], sections[1][1]
        if len(blob) < count * 8:
            fail(f"名称段大小 {len(blob)} < {count} x 8")
        for i in range(count):
            off, length, _pad = struct.unpack_from("<IHH", blob, 8 * i)
            strings_in_text(off, length, f"名称 {i}")
    dlg_total = sections.get(4, (0, 0, 0, b""))[1]
    scene_total = sections.get(3, (0, 0, 0, b""))[1]
    chapter_total = sections.get(2, (0, 0, 0, b""))[1]
    fg_count = sections.get(6, (0, 0, 0, b""))[1]
    bg_count = sections.get(5, (0, 0, 0, b""))[1]

    # ---- 章节 / 场景 / 对白 ----
    if 2 in sections:
        blob, count = sections[2][3], sections[2][1]
        if len(blob) < count * 12:
            fail(f"章节段大小 {len(blob)} < {count} x 12")
        for i in range(count):
            _id, first_scene, scene_count, first_dlg, dlg_count, nxt = struct.unpack_from(
                "<HHHHHH", blob, 12 * i)
            if first_scene + scene_count > scene_total:
                fail(f"章节 {i} 的场景范围越界")
            if first_dlg + dlg_count > dlg_total:
                fail(f"章节 {i} 的对白范围越界")
            if nxt != NONE and nxt >= chapter_total:
                fail(f"章节 {i} 的后继 {nxt} 越界")
    if 3 in sections:
        blob, count = sections[3][3], sections[3][1]
        if len(blob) < count * 16:
            fail(f"场景段大小 {len(blob)} < {count} x 16")
        for i in range(count):
            bg, choice_count, _pad, first_dlg, dlg_count = struct.unpack_from("<HBBHH", blob, 16 * i)
            choice_names = struct.unpack_from("<HH", blob, 16 * i + 8)   # choice_name[2]
            if bg != NONE and bg >= bg_count:
                fail(f"场景 {i} 的背景 {bg} 越界")
            if choice_count > 2:
                fail(f"场景 {i} 的选项数 {choice_count} > 2")
            if first_dlg + dlg_count > dlg_total:
                fail(f"场景 {i} 的对白范围越界")
            for name_id in choice_names:
                if name_id != NONE and name_id >= name_count:
                    fail(f"场景 {i} 的选项名 {name_id} 越界")
    if 4 in sections:
        blob, count = sections[4][3], sections[4][1]
        if len(blob) < count * 14:
            fail(f"对白段大小 {len(blob)} < {count} x 14")
        for i in range(count):
            off, length, name_id, fg_id = struct.unpack_from("<IHHH", blob, 14 * i)
            strings_in_text(off, length, f"对白 {i}")
            if name_id != NONE and name_id >= name_count:
                fail(f"对白 {i} 的角色名 {name_id} 越界")
            if fg_id != NONE and fg_id >= fg_count:
                fail(f"对白 {i} 的立绘 {fg_id} 越界")

    # ---- 背景 / 下半块 ----
    if 5 in sections:
        bg_table, bg_data, bg_n = image_table(5, BG_ENTRY)
        for i in range(bg_n):
            off, length, w, h = struct.unpack_from("<IIHH", bg_table, BG_ENTRY * i)
            if (w, h) != (ART_W, ART_H):
                fail(f"背景 {i} 尺寸 {w}x{h} != 画面区 {ART_W}x{ART_H}")
            jpeg_ok(bg_data, off, length, f"背景 {i}")
    if 8 in sections:
        st_table, st_data, st_n = image_table(8, BG_ENTRY)
        if st_n != bg_count:
            fail(f"下半块条目 {st_n} != 背景条目 {bg_count}")
        for i in range(st_n):
            off, length, w, h = struct.unpack_from("<IIHH", st_table, BG_ENTRY * i)
            if (w, h) != (STRIP_W, STRIP_H):
                fail(f"下半块 {i} 尺寸 {w}x{h} != {STRIP_W}x{STRIP_H}")
            jpeg_ok(st_data, off, length, f"下半块 {i}")

    # ---- 立绘上下两段 ----
    fg_widths = {}
    fg_heights = {}
    for sec_type, label, max_h in ((6, "立绘上半段", ART_H), (9, "立绘下半段", STRIP_H)):
        if sec_type not in sections:
            continue
        fg_table, fg_data, fg_n = image_table(sec_type, FG_ENTRY)
        if fg_n != fg_count:
            fail(f"{label} 条目 {fg_n} != 立绘条目 {fg_count}")
        for i in range(fg_n):
            jpeg_off, jpeg_len, mask_off, mask_len, w, h = struct.unpack_from(
                "<IIIIHH", fg_table, FG_ENTRY * i)
            if w == 0 and h == 0:
                # 立绘没有下半段时写一条空记录(设备端按 w/h 为 0 直接跳过)。
                if sec_type == 9:
                    fg_heights.setdefault(i, ART_H)
                continue
            if w > SPRITE_MAX_W or h > max_h:
                fail(f"{label} {i} 尺寸 {w}x{h} 超上限 {SPRITE_MAX_W}x{max_h}")
            if mask_len != ((w + 7) // 8) * h:
                fail(f"{label} {i} 遮罩长度 {mask_len} != 预期 {((w + 7) // 8) * h}")
            if mask_off + mask_len > len(fg_data):
                fail(f"{label} {i} 遮罩越界")
            jpeg_ok(fg_data, jpeg_off, jpeg_len, f"{label} {i}")
            if sec_type == 6:
                fg_widths[i] = w
                fg_heights[i] = h
            else:
                if i in fg_widths and fg_widths[i] != w:
                    fail(f"立绘 {i} 上下段宽度不一致: {fg_widths[i]} vs {w}")
                if fg_heights.get(i, ART_H) != ART_H:
                    fail(f"立绘 {i} 有下半段但上半段不是满高"
                         f"({fg_heights.get(i, 0)} 行),中间会缺一条")

    # ---- 元数据 ----
    meta = {}
    if 7 in sections:
        for line in sections[7][3].decode("utf-8", "replace").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                meta[key.strip()] = value.strip()
        for key in ("source", "commit", "generator", "variant", "frame"):
            if not meta.get(key):
                fail(f"元数据缺少 {key}")
        if meta.get("frame") and meta["frame"] != f"{ART_W}x{SCREEN_H}":
            fail(f"元数据 frame={meta['frame']} 与工具不符 {ART_W}x{SCREEN_H}")
        if meta.get("variant") == "release" and not meta.get("patch"):
            fail("release 变体没有记录补丁目录")
    else:
        fail("缺少元数据段")

    # ---- 解码与接缝(--no-deep 时跳过) ----
    seam = []
    if deep and 5 in sections and 8 in sections:
        bg_table, bg_data, bg_n = image_table(5, BG_ENTRY)
        st_table, st_data, st_n = image_table(8, BG_ENTRY)
        for i in range(min(bg_n, st_n)):
            bg_off, bg_len = struct.unpack_from("<II", bg_table, BG_ENTRY * i)
            st_off, st_len = struct.unpack_from("<II", st_table, BG_ENTRY * i)
            try:
                art = Image.open(io.BytesIO(bg_data[bg_off:bg_off + bg_len])).convert("RGB")
                low = Image.open(io.BytesIO(st_data[st_off:st_off + st_len])).convert("RGB")
            except Exception as exc:                      # noqa: BLE001 - 报告原始错误
                fail(f"背景 {i} 无法解码: {exc}")
                continue
            if art.size != (ART_W, ART_H) or low.size != (STRIP_W, STRIP_H):
                fail(f"背景 {i} 解码尺寸 {art.size}/{low.size} 与记录不符")
                continue
            pa, pl = art.load(), low.load()
            diff = sum(abs(pa[x, ART_H - 1][c] - pl[x, 0][c])
                       for x in range(0, ART_W, 8) for c in range(3)) / ((ART_W // 8) * 3)
            seam.append(diff)
            if diff > 90:
                fail(f"背景 {i} 上下块接缝跳变过大(平均色差 {diff:.0f})")
    if deep:
        for sec_type, label, high in ((6, "立绘上半段", ART_H), (9, "立绘下半段", STRIP_H)):
            if sec_type not in sections:
                continue
            fg_table, fg_data, fg_n = image_table(sec_type, FG_ENTRY)
            for i in range(min(fg_n, 8)):                # 抽样解码:拦住损坏的 JPEG
                jpeg_off, jpeg_len = struct.unpack_from("<II", fg_table, FG_ENTRY * i)
                try:
                    im = Image.open(io.BytesIO(fg_data[jpeg_off:jpeg_off + jpeg_len]))
                    if im.height > high:
                        fail(f"{label} {i} 解码高度 {im.height} 超 {high}")
                except Exception as exc:                  # noqa: BLE001 - 报告原始错误
                    fail(f"{label} {i} 无法解码: {exc}")

    if errors:
        for line in errors:
            print(f"ERROR: {line}", file=sys.stderr)
        print(f"资源包自检: FAIL ({len(errors)} 项问题)", file=sys.stderr)
        return 1
    detail = ""
    if seam:
        ordered_seam = sorted(seam)
        detail = (f", 接缝色差 中位 {ordered_seam[len(ordered_seam) // 2]:.1f} "
                  f"最大 {ordered_seam[-1]:.1f}")
    print(f"资源包自检: PASS ({os.path.basename(path)} {pack_size} 字节, "
          f"章节 {chapter_total} / 场景 {scene_total} / 对白 {dlg_total} / "
          f"背景 {bg_count} / 立绘 {fg_count}{detail})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", help="Saya-miband10 checkout 路径(构建时必填)")
    ap.add_argument("--out", help="输出 pack 路径(构建时必填)")
    ap.add_argument("--check", metavar="PACK",
                    help="只自检一个已生成的 pack:结构/尺寸/接缝/遮罩(不构建)")
    ap.add_argument("--no-deep", action="store_true",
                    help="自检时跳过 JPEG 解码与接缝检查,只做结构与尺寸检查")
    ap.add_argument("--commit", default="", help="源仓库 commit(记录到元数据)")
    ap.add_argument("--patch", default="",
                    help="release 变体:源仓库 补丁/ 目录(完整版)。生成的 pack 禁提交/禁发布")
    args = ap.parse_args()

    if args.check:
        return check_pack(args.check, deep=not args.no_deep)
    if not args.source or not args.out:
        ap.error("构建时必须同时给出 --source 与 --out;只做自检请用 --check <pack>")

    if args.patch:
        if not os.path.isdir(args.patch):
            raise SystemExit(f"补丁目录不存在: {args.patch}")
        log("  ⚠ release 变体:含补丁(完整版)。不要把生成的 pack 提交进仓库,")
        log("    也不要用它构建发布到 AI Passport 社区市场的固件。")

    builder = PackBuilder(args.source, args.patch)
    meta = {
        "source": "https://github.com/liuyuze61/Saya-miband10",
        "commit": args.commit or "unknown",
        "generator": GEN_VERSION,
        "variant": "release" if args.patch else "community",
        "patch": os.path.basename(args.patch.rstrip("/\\")) if args.patch else "",
        # 画面按整幅 320x240 打包,分上下两块画布显示(见 main/saya_image.h)。
        "frame": f"{ART_W}x{SCREEN_H}",
        "art": f"{ART_W}x{ART_H}",   # 上半块(画面区)
        "bg_quality": str(BG_QUALITY),
        "strip": f"{STRIP_W}x{STRIP_H}q{STRIP_QUALITY}",
        "fg_quality": str(FG_QUALITY),
        "sprite_max_w": str(SPRITE_MAX_W),
    }
    data = builder.build(meta)

    if STRIPPED["se"]:
        log(f"  剥掉音效标签 <se …> {STRIPPED['se']} 个(本移植无音效资源,不显示)")

    if builder.patched_chapters:
        log(f"  补丁章节 {len(builder.patched_chapters)} 个: "
            + " ".join(builder.patched_chapters))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(data)
    log(f"  写出 {args.out} ({len(data)} 字节) sha256={hashlib.sha256(data).hexdigest()[:16]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
