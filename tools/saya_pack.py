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
    SEC_META    UTF-8 key=value 文本(来源仓库 / commit / 转换参数)

Image conversion (fixed screen layout 320x240, art area 320x150):
  背景 283x212 调色板 PNG -> cover 裁切成 320x150 -> JPEG
  标题画面(bg.png)       -> 同上,固定放在背景表 0 号(SAYA_BG_TITLE,脚本不引用它)
  立绘 212xN RGBA      -> 按屏幕高度 240 缩放(宽度上限 180)-> 取顶部 150 行
                          -> JPEG + 1bpp 遮罩(按行打包)

Usage:
  python tools/saya_pack.py --source <Saya-miband10 checkout> \
      --out main/saya_data/saya_pack.bin

Variants (发布规则,必须分清):
  community(默认) 只读 src/common/{sy,cg,fg},不含源仓库 补丁/ 里的任何内容。
                  仓库里提交的 main/saya_data/saya_pack.bin 就是这个版本 ——
                  可以随固件一起发布到 AI Passport 社区市场。
  release(--patch)  用 补丁/ 里的同名章节替换基础脚本,并把 补丁/*.png(R18 CG)
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
SPRITE_SCREEN_H = 240            # 立绘按屏幕高度缩放
SPRITE_MAX_W = 180               # 立绘宽度上限,限制设备端解码缓冲
BG_QUALITY = 80
FG_QUALITY = 80

SEC_TEXT, SEC_NAME, SEC_CHAPTER, SEC_SCENE, SEC_DLG, SEC_BG, SEC_FG, SEC_META = range(8)

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


def cover_crop(im: Image.Image, size: Tuple[int, int]) -> Image.Image:
    tw, th = size
    sw, sh = im.size
    scale = max(tw / sw, th / sh)
    resized = im.resize((max(1, round(sw * scale)), max(1, round(sh * scale))), Image.LANCZOS)
    x = (resized.width - tw) // 2
    y = (resized.height - th) // 2
    return resized.crop((x, y, x + tw, y + th))


def sprite_crop(im: Image.Image) -> Image.Image:
    """立绘 -> 按屏幕高度缩放、限宽、取可见的顶部 ART_H 行。"""
    w, h = im.size
    scale = SPRITE_SCREEN_H / h
    if round(w * scale) > SPRITE_MAX_W:
        scale = SPRITE_MAX_W / w
    w2 = max(1, round(w * scale))
    h2 = max(1, round(h * scale))
    resized = im.resize((w2, h2), Image.LANCZOS)
    if h2 > ART_H:
        resized = resized.crop((0, 0, w2, ART_H))
    return resized


def pack_mask(alpha: Image.Image) -> bytes:
    """1bpp 遮罩,按行打包(每行 ceil(w/8) 字节);PIL 的 "1" 模式本来就是行打包。"""
    if alpha.mode != "1":
        alpha = alpha.point(lambda v: 255 if v > 127 else 0).convert("1")
    w, h = alpha.size
    stride = (w + 7) // 8
    data = alpha.tobytes()
    return data[: stride * h]


class PackBuilder:
    def __init__(self, source: str, patch_dir: str = "") -> None:
        self.src = source
        self.sy_dir = os.path.join(source, "src", "common", "sy")
        self.cg_dir = os.path.join(source, "src", "common", "cg")
        self.fg_dir = os.path.join(source, "src", "common", "fg")
        # 补丁目录(R18 内容,只给 release 变体用):同名章节覆盖基础脚本,PNG 并入背景表。
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
        self.fg_list: List[str] = []
        self.fg_blobs: List[Tuple[bytes, bytes, int, int]] = []  # jpeg, mask, w, h
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
        art = cover_crop(im, (ART_W, ART_H))
        buf = io.BytesIO()
        art.save(buf, "JPEG", quality=BG_QUALITY, optimize=True, subsampling=2)
        idx = len(self.bg_list)
        self.bg_list.append(key)
        self.bg_blobs.append(buf.getvalue())
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
        crop = sprite_crop(im)
        jpeg = io.BytesIO()
        crop.convert("RGB").save(jpeg, "JPEG", quality=FG_QUALITY, optimize=True, subsampling=2)
        mask = pack_mask(crop.getchannel("A"))
        idx = len(self.fg_list)
        self.fg_list.append(key)
        self.fg_blobs.append((jpeg.getvalue(), mask, crop.width, crop.height))
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, help="Saya-miband10 checkout 路径")
    ap.add_argument("--out", required=True, help="输出 pack 路径")
    ap.add_argument("--commit", default="", help="源仓库 commit(记录到元数据)")
    ap.add_argument("--patch", default="",
                    help="release 变体:源仓库 补丁/ 目录(R18)。生成的 pack 禁提交/禁发布")
    args = ap.parse_args()

    if args.patch:
        if not os.path.isdir(args.patch):
            raise SystemExit(f"补丁目录不存在: {args.patch}")
        log("  ⚠ release 变体:含补丁(R18)。不要把生成的 pack 提交进仓库,")
        log("    也不要用它构建发布到 AI Passport 社区市场的固件。")

    builder = PackBuilder(args.source, args.patch)
    meta = {
        "source": "https://github.com/liuyuze61/Saya-miband10",
        "commit": args.commit or "unknown",
        "generator": GEN_VERSION,
        "variant": "release" if args.patch else "community",
        "patch": os.path.basename(args.patch.rstrip("/\\")) if args.patch else "",
        "art": f"{ART_W}x{ART_H}",
        "bg_quality": str(BG_QUALITY),
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
