#!/usr/bin/env python3
"""Build the Starry Sky Railroad resource pack for the AI Passport port.

Source project: https://github.com/liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P
  - 小米手环 9 Pro(Xiaomi Vela / aiot quick app)上的《星空列车与白的旅行》同人移植。
  - 素材与译文版权归原作品与移植者所有;本仓库只保存转换产物,不再分发源素材。
  - 源仓库自带免责声明:请支持正版。

The pack is a single little-endian binary read straight out of flash by the firmware
(no decompression, no per-record parsing). Layout:

  header  : magic "SSRPK001", version, total size, section count, section table
  section : { type u32, offset u32, count u32, size u32 }
    SEC_TEXT    UTF-8 blob; every string is addressed by (offset, length)
    SEC_NAME    { off u32, len u16, pad u16 }       角色名 / 选项文案 / 结局名
    SEC_CHAPTER { id u16, first_scene u16, scene_count u16,
                  first_dlg u16, dlg_count u16, next u16 }   next=0xFFFF 表示无后续章
    SEC_SCENE   { bg u16, choice_count u8, pad u8, first_dlg u16, dlg_count u16,
                  choice_name[2] u16, choice_target[2] u16 }  target = 全局场景下标
    SEC_DLG     { text_off u32, text_len u16, name u16, sprite u16, flags u8,
                  jump u8, arg u16 }   sprite=0xFFFF 表示本句未指定(沿用上一句)
    SEC_BG      { off u32, len u32, w u16, h u16, flags u16 }(off 相对 SEC_BG 数据区)
    SEC_FG      { color_off u32, color_len u32, mask_off u32, mask_len u32,
                  w u16, h u16, x u16, y u16, owner u16 }
    SEC_META    UTF-8 key=value 文本(来源仓库 / commit / 转换参数)

Image conversion (固定版面 240x320,画面铺满整屏,文本框压在画面底部):
  背景 336x480 调色板 PNG -> 等比缩放到宽 240(高 343)-> 取顶部 320 行 -> JPEG
  标题画面(title_bg.png)  -> 同上,但取底部 320 行,让标题字样落在文本框上方,
                             固定放在背景表 0 号(脚本不引用它)
  立绘 336x480 RGBA       -> 按 alpha 包围盒裁剪、等比缩放进 168x252,贴右边、
                             底边贴屏幕底 -> 无损 RGB565(小端) + 4bpp 遮罩
  纯色背景(bg_white 等)   -> 同样走 JPEG 流程

立绘为什么存原始 RGB565 而不是 JPEG:ATRI 版的画布(240x320 = 150KB)是整屏合成的,
立绘直接从 Flash 逐行 blit 进画布,不再需要 84KB 的立绘解码缓冲;同时避开了 JPEG
解码器的块化伪影。背景继续用 JPEG(解码目标就是画布本身,无额外缓冲)。

Usage:
  python tools/starry_pack.py --source <source checkout> \
      --out main/starry_data/starry_pack.bin [--preview DIR]

The generated pack is committed so a plain checkout can build the firmware;
only regenerating it needs the source checkout.
"""

from __future__ import annotations

import argparse
import datetime
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

MAGIC = b"SSRPK001"
# 版本 2:立绘段改为无损 RGB565 + 4bpp 遮罩(与 main/starry_pack.h 的
# STARRY_PACK_VERSION 一致)。剧本/背景/场景段的字节布局不变。
VERSION = 2
GEN_VERSION = "starry_pack/2"

SCREEN_W, SCREEN_H = 240, 320
# 文本框顶边:与 main/starry_render.h 的 STARRY_BOX_Y 一致。
BOX_Y = 214
# 立绘:裁到不透明边界后等比缩放,贴右边、底边贴屏幕底(下半身自然落到文本框下面,
# 文本框是半透明的,所以立绘是"被压暗"而不是"被截断")。
# 这两个上限必须与 main/starry_render.h 的 STARRY_SPRITE_MAX_W / STARRY_SPRITE_H 一致,
# 否则固件侧的缓冲检查会拒收(打包时会断言,不会悄悄出错)。
SPRITE_MAX_W = 168
SPRITE_MAX_H = 252
SPRITE_MARGIN_RIGHT = 4
SPRITE_MARGIN_BOTTOM = 6
# 事件 CG 与纯色幕:源素材里 evcg* 是带人物的插图,bg_black/red/white 是纯色幕 ——
# 这两种画面都不该再往上叠立绘(会被人物盖住,或者本来就不该有人)。
NO_SPRITE_PREFIXES = ("evcg",)
NO_SPRITE_KEYS = ("bg_black", "bg_red", "bg_white")
BG_QUALITY = 82

SEC_TEXT, SEC_NAME, SEC_CHAPTER, SEC_SCENE, SEC_DLG, SEC_BG, SEC_FG, SEC_META = range(8)
BG_FLAG_NO_SPRITE = 1 << 0      # CG / 纯色幕:不叠立绘

DLG_END = 1 << 0        # arg = 结局名(SEC_NAME 下标)
DLG_TO_SCENE = 1 << 1   # 本句后跳转到 current_scene + jump
DLG_BRANCH = 1 << 2     # 选择分支(本移植数据里未使用,保留语义)

NONE = 0xFFFF
BG_KEEP = 0xFFFE        # 场景没写背景:沿用上一张
FG_KEEP = 0xFFFF        # 本句没写立绘:沿用上一句

TITLE_SOURCE = "title_bg"


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
    """源数据里混了不可显示的控制字符与 CJK 兼容汉字,转换时清理掉。

    - C0 控制字符(如 U+001F)直接删除,它们不是可排版字符;
    - CJK 兼容汉字区(U+F900-U+FAFF)按 NFKC 归一到标准汉字(字形完全等价),
      换成字体一定覆盖的码位。
    """
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


def scale_width(im: Image.Image, width: int) -> Image.Image:
    """等比缩放到指定宽度(高度四舍五入)。"""
    w, h = im.size
    return im.resize((width, max(1, round(h * width / w))), Image.LANCZOS)


def bg_art(im: Image.Image, bottom_align: bool = False) -> Image.Image:
    """背景 -> 240 宽等比缩放后取 320 行。

    底部对齐(bottom_align)用于标题画面:把标题字样往上挪,避免被文本框压住。
    """
    resized = scale_width(im, SCREEN_W)
    if resized.height < SCREEN_H:  # 源图比 3:4 矮时补黑边,保持尺寸断言成立
        canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
        canvas.paste(resized, (0, 0))
        return canvas
    top = resized.height - SCREEN_H if bottom_align else 0
    return resized.crop((0, top, SCREEN_W, top + SCREEN_H))


def sprite_target(im: Image.Image) -> Tuple[Image.Image, int, int]:
    """立绘 -> 裁到不透明边界,等比缩放塞进 (SPRITE_MAX_W x SPRITE_MAX_H),
    贴右边、底边贴屏幕底;返回(图, x, y)。"""
    bbox = im.getchannel("A").getbbox() or (0, 0, im.width, im.height)
    tight = im.crop(bbox)
    scale = min(SPRITE_MAX_W / tight.width, SPRITE_MAX_H / tight.height)
    width = max(1, min(SPRITE_MAX_W, round(tight.width * scale)))
    height = max(1, min(SPRITE_MAX_H, round(tight.height * scale)))
    resized = tight.resize((width, height), Image.LANCZOS)
    return resized, SCREEN_W - width - SPRITE_MARGIN_RIGHT, SCREEN_H - height - SPRITE_MARGIN_BOTTOM


def rgb565_bytes(rgb: Image.Image) -> bytes:
    """RGB888 -> RGB565 小端两字节(与设备端 uint16_t 直接读 Flash 一致)。"""
    w, h = rgb.size
    px = rgb.load()
    out = bytearray(w * h * 2)
    for y in range(h):
        row = y * w * 2
        for x in range(w):
            r, g, b = px[x, y]
            value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            off = row + x * 2
            out[off] = value & 0xFF
            out[off + 1] = (value >> 8) & 0xFF
    return bytes(out)


def pack_alpha_4bpp(alpha: Image.Image) -> bytes:
    """4bpp alpha 遮罩,每行按字节对齐,高半字节是左侧像素(与 ATRI 版一致)。"""
    w, h = alpha.size
    stride = (w + 1) // 2
    px = alpha.load()
    out = bytearray(stride * h)
    for y in range(h):
        row = y * stride
        for x in range(w):
            nib = min(15, (px[x, y] * 15 + 127) // 255)
            if x % 2 == 0:
                out[row + x // 2] |= nib << 4
            else:
                out[row + x // 2] |= nib
    return bytes(out)


class PackBuilder:
    def __init__(self, source: str) -> None:
        self.src = source
        self.scene_dir = os.path.join(source, "src", "common", "scene")
        self.common_dir = os.path.join(source, "src", "common")
        self.body_dir = os.path.join(source, "src", "common", "body")
        self.strings = Strings()
        self.names: List[Tuple[int, int]] = []
        self._name_index: Dict[str, int] = {}
        self.bg_files = self._scan(self.common_dir)
        self.fg_files = self._scan(self.body_dir)
        self.bg_list: List[str] = []
        self.bg_blobs: List[bytes] = []
        self.fg_list: List[str] = []
        # color(RGB565 小端),mask(4bpp),w,h,x,y
        self.fg_blobs: List[Tuple[bytes, bytes, int, int, int, int]] = []
        self.bg_no_sprite: List[bool] = []          # 与 bg_blobs 平行:CG / 纯色幕
        self.fg_votes: List[Dict[int, int]] = []    # 与 fg_blobs 平行:该立绘被哪些角色用过
        self._bg_index: Dict[str, int] = {}
        self._fg_index: Dict[str, int] = {}
        self.counts: Dict[str, int] = {}

    @staticmethod
    def bg_hides_sprite(key: str) -> bool:
        """这张背景是否禁止叠立绘(CG / 纯色幕)。"""
        return key.startswith(NO_SPRITE_PREFIXES) or key in NO_SPRITE_KEYS

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
        art = bg_art(im, bottom_align=(key == TITLE_SOURCE))
        buf = io.BytesIO()
        art.save(buf, "JPEG", quality=BG_QUALITY, optimize=True, subsampling=2)
        idx = len(self.bg_list)
        self.bg_list.append(key)
        self.bg_blobs.append(buf.getvalue())
        self.bg_no_sprite.append(self.bg_hides_sprite(key))
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
        sprite, sprite_x, sprite_y = sprite_target(im)
        assert sprite.width <= SPRITE_MAX_W and sprite.height <= SPRITE_MAX_H
        # 无损 RGB565 + 4bpp 遮罩:设备端直接从 Flash 逐行 blit,不需要解码缓冲。
        color = rgb565_bytes(sprite.convert("RGB"))
        mask = pack_alpha_4bpp(sprite.getchannel("A"))
        idx = len(self.fg_list)
        self.fg_list.append(key)
        self.fg_blobs.append((color, mask, sprite.width, sprite.height, sprite_x,
                              sprite_y))
        self.fg_votes.append({})
        self._fg_index[key] = idx
        return idx

    @staticmethod
    def fg_owner(votes: Dict[int, int]) -> int:
        """这张立绘属于谁:引用它最多的那个说话人。没有说话人引用 = NONE(不显示)。"""
        if not votes:
            return NONE
        return max(votes.items(), key=lambda kv: (kv[1], -kv[0]))[0]

    # ---------------------------------------------------------------- scripts
    def load_chapters(self) -> List[dict]:
        chapters = []
        for fn in sorted(os.listdir(self.scene_dir), key=chapter_sort_key):
            if not fn.endswith(".txt"):
                continue
            raw = open(os.path.join(self.scene_dir, fn), "rb").read()
            if not raw.strip():
                log(f"  跳过空脚本 {fn}")
                continue
            try:
                data = json.loads(raw.decode("utf-8"))
            except Exception as exc:  # pragma: no cover - source data issue
                log(f"  跳过无法解析的脚本 {fn}: {exc}")
                continue
            chapters.append({"file": os.path.splitext(fn)[0], "scenes": data})
        chapters.sort(key=lambda c: chapter_sort_key(c["file"]))
        return chapters

    def build(self, meta: Dict[str, str]) -> bytes:
        # 0 号先放标题画面:标题页复用整屏画面。
        title_id = self.bg_id(TITLE_SOURCE)
        if title_id != 0:
            raise SystemExit(f"标题画面缺失或不是 0 号背景: {TITLE_SOURCE}")

        chapters = self.load_chapters()
        scene_recs: List[tuple] = []
        dlg_recs: List[tuple] = []
        chapter_recs: List[tuple] = []
        counts = {"chapters": 0, "scenes": 0, "dialogues": 0, "choices": 0,
                  "jumps": 0, "endings": 0, "bg_keep": 0, "missing_bg": 0,
                  "missing_fg": 0}

        for chapter in chapters:
            first_scene = len(scene_recs)
            first_dlg = len(dlg_recs)
            for scene in chapter["scenes"]:
                scene_first_dlg = len(dlg_recs)
                bg_name = scene.get("background", "") or ""
                if bg_name:
                    bg = self.bg_id(bg_name)
                    if bg == NONE:
                        counts["missing_bg"] += 1
                        log(f"  警告: 章节 {chapter['file']} 背景缺失 {bg_name}")
                else:
                    bg = BG_KEEP
                    counts["bg_keep"] += 1
                choices = scene.get("choices") or []
                if len(choices) > 2:
                    raise SystemExit(
                        f"章节 {chapter['file']} 出现 {len(choices)} 个选项,打包格式只支持 2 个")
                dlg_list = scene.get("dialogues") or []
                for dlg in dlg_list:
                    text_off, text_len = self.strings.add(clean_text(dlg.get("text", "") or ""))
                    name = self.name_id(clean_text(dlg.get("character", "") or ""))
                    sprite = FG_KEEP
                    if dlg.get("body"):
                        sprite = self.fg_id(dlg["body"])
                        if sprite == NONE:
                            counts["missing_fg"] += 1
                            log(f"  警告: 立绘缺失 {dlg['body']} (章节 {chapter['file']})")
                        elif name != NONE:
                            # 只统计"有名字的说话人":旁白句里出现的立绘不构成归属关系。
                            self.fg_votes[sprite][name] = self.fg_votes[sprite].get(name, 0) + 1
                    flags = 0
                    arg = 0
                    jump = 0
                    if "toScenes" in dlg:
                        flags |= DLG_TO_SCENE
                        jump = int(dlg["toScenes"])
                        if jump < 0 or jump > 255:
                            raise SystemExit(f"toScenes 超出 u8: {jump}")
                        counts["jumps"] += 1
                    if "branch" in dlg:
                        flags |= DLG_BRANCH
                    if "END" in dlg:
                        flags |= DLG_END
                        arg = self.name_id(clean_text(str(dlg["END"])))
                        counts["endings"] += 1
                    dlg_recs.append((text_off, text_len, name, sprite, flags, jump, arg))
                choice_names = [self.name_id(clean_text(c.get("text", "") or ""))
                                for c in choices]
                scene_recs.append({
                    "bg": bg, "choices": choices, "choice_names": choice_names,
                    "first_dlg": scene_first_dlg, "dlg_count": len(dlg_recs) - scene_first_dlg,
                    "index": len(scene_recs),
                })
                if choices:
                    counts["choices"] += 1
                counts["dialogues"] += len(dlg_list)
            chapters_rec = {
                "id": int(chapter["file"]) if chapter["file"].isdigit() else 0,
                "first_scene": first_scene,
                "scene_count": len(scene_recs) - first_scene,
                "first_dlg": first_dlg,
                "dlg_count": len(dlg_recs) - first_dlg,
                "next": NONE,
            }
            chapter_recs.append(chapters_rec)
            counts["chapters"] += 1
            counts["scenes"] += chapters_rec["scene_count"]

        for i in range(len(chapter_recs) - 1):
            chapter_recs[i]["next"] = i + 1

        # 选项目标:源数据是"当前场景 + nextScene"的相对下标,转成全局场景下标。
        for scene in scene_recs:
            targets = [NONE, NONE]
            for i, choice in enumerate(scene["choices"]):
                rel = choice.get("nextScene")
                if rel is not None:
                    target_local = scene["index"] + int(rel)
                    if 0 <= target_local < len(scene_recs):
                        targets[i] = target_local
                    else:
                        log(f"  警告: 选项目标越界 场景 {scene['index']} + {rel}")
            scene["choice_targets"] = targets

        # ------------------------------------------------------------- 组装
        # 与 main/starry_pack.h 的解析逐字节对应:SEC_BG / SEC_FG 的段体是
        # "索引表 + 数据区",索引里的偏移都相对数据区起点。
        text = self.strings.data
        name_tab = b"".join(struct.pack("<IHH", off, ln, 0) for off, ln in self.names)
        chapter_tab = b"".join(
            struct.pack("<HHHHHH", c["id"], c["first_scene"], c["scene_count"],
                        c["first_dlg"], c["dlg_count"], c["next"])
            for c in chapter_recs)
        scene_tab = b"".join(
            struct.pack("<HBBHHHHHH", NONE if s["bg"] == BG_KEEP else s["bg"],
                        len(s["choices"]), 0, s["first_dlg"], s["dlg_count"],
                        s["choice_names"][0] if s["choices"] else NONE,
                        s["choice_names"][1] if len(s["choices"]) > 1 else NONE,
                        s["choice_targets"][0], s["choice_targets"][1])
            for s in scene_recs)
        dlg_tab = b"".join(struct.pack("<IHHHBBH", t_off, t_len, name, sprite, flags, jump, arg)
                           for t_off, t_len, name, sprite, flags, jump, arg in dlg_recs)

        bg_data = bytearray()
        bg_tab = bytearray()
        for jpeg, no_sprite in zip(self.bg_blobs, self.bg_no_sprite):
            flags = BG_FLAG_NO_SPRITE if no_sprite else 0
            bg_tab += struct.pack("<IIHHH", len(bg_data), len(jpeg), SCREEN_W, SCREEN_H, flags)
            bg_data += jpeg
        fg_data = bytearray()
        fg_tab = bytearray()
        owners = [self.fg_owner(votes) for votes in self.fg_votes]
        for (color, mask, w, h, x, y), owner in zip(self.fg_blobs, owners):
            fg_tab += struct.pack("<IIIIHHHHH", len(fg_data), len(color),
                                 len(fg_data) + len(color), len(mask), w, h, x, y, owner)
            fg_data += color
            fg_data += mask

        meta["no_sprite_bgs"] = str(sum(1 for flag in self.bg_no_sprite if flag))
        meta["fg_ownerless"] = str(sum(1 for votes in self.fg_votes if not votes))
        meta_lines = [f"{k}={v}" for k, v in meta.items()]
        meta_lines += [f"{k}={v}" for k, v in sorted(counts.items())]
        meta_blob = ("\n".join(meta_lines) + "\n").encode("utf-8")

        sections = [
            (SEC_TEXT, text, 0),
            (SEC_NAME, name_tab, len(self.names)),
            (SEC_CHAPTER, chapter_tab, len(chapter_recs)),
            (SEC_SCENE, scene_tab, len(scene_recs)),
            (SEC_DLG, dlg_tab, len(dlg_recs)),
            (SEC_BG, bytes(bg_tab) + bytes(bg_data), len(self.bg_blobs)),
            (SEC_FG, bytes(fg_tab) + bytes(fg_data), len(self.fg_blobs)),
            (SEC_META, meta_blob, 0),
        ]
        return assemble(sections)

    def stats(self) -> Dict[str, int]:
        return {
            "bg_count": len(self.bg_blobs),
            "bg_bytes": sum(len(b) for b in self.bg_blobs),
            "fg_count": len(self.fg_blobs),
            "fg_bytes": sum(len(c) + len(m) for c, m, _w, _h, _x, _y in self.fg_blobs),
            "text_bytes": len(self.strings.data),
            "name_count": len(self.names),
        }


def assemble(sections: List[Tuple[int, bytes, int]]) -> bytes:
    """按 {type, offset, count, size} 表写出整包(与 saya 包的排布一致,未做对齐)。"""
    header_len = 8 + 4 + 4 + 4 + 16 * len(sections)
    offset = header_len
    table = bytearray()
    body = bytearray()
    for sec_type, payload, count in sections:
        table += struct.pack("<IIII", sec_type, offset, count, len(payload))
        body += payload
        offset += len(payload)
    out = bytearray(MAGIC)
    out += struct.pack("<III", VERSION, offset, len(sections))
    out += table
    out += body
    assert len(out) == offset, (len(out), offset)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="源移植仓库 checkout")
    ap.add_argument("--out", default="main/starry_data/starry_pack.bin")
    ap.add_argument("--preview", help="额外写出若干场景预览 PNG 到这个目录")
    ap.add_argument("--preview-scenes", type=int, default=6)
    args = ap.parse_args()

    if not os.path.isdir(os.path.join(args.source, "src", "common", "scene")):
        sys.exit(f"源仓库结构不对: {args.source}")

    builder = PackBuilder(args.source)
    meta = {
        "source_repo": "https://github.com/liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P",
        "source_commit": os.popen(f"git -C {args.source} rev-parse HEAD").read().strip(),
        "generator": GEN_VERSION,
        "generated": datetime.date.today().isoformat(),
        "screen": f"{SCREEN_W}x{SCREEN_H}",
        "box_y": str(BOX_Y),
        "sprite_box": f"{SPRITE_MAX_W}x{SPRITE_MAX_H}",
        "bg_quality": str(BG_QUALITY),
        "fg_format": "rgb565+4bpp-mask",
    }
    blob = builder.build(meta)
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(blob)
    stats = builder.stats()
    log(f"写出 {args.out}: {len(blob)} 字节({len(blob)/1024/1024:.2f} MB)")
    log("  " + " ".join(f"{k}={v}" for k, v in stats.items()))

    if args.preview:
        os.makedirs(args.preview, exist_ok=True)
        write_previews(builder, args.preview, args.preview_scenes)
    return 0


def rgb565_to_rgb(data: bytes, w: int, h: int) -> Image.Image:
    """RGB565 小端 -> RGB888,只给预览图用。"""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 2
            value = data[i] | (data[i + 1] << 8)
            r = ((value >> 11) & 0x1F) * 255 // 31
            g = ((value >> 5) & 0x3F) * 255 // 63
            b = (value & 0x1F) * 255 // 31
            px[x, y] = (r, g, b)
    return img


def unpack_alpha_4bpp(mask: bytes, w: int, h: int) -> Image.Image:
    """4bpp 遮罩 -> L 图(预览用)。"""
    stride = (w + 1) // 2
    img = Image.new("L", (w, h), 0)
    px = img.load()
    for y in range(h):
        row = y * stride
        for x in range(w):
            packed = mask[row + x // 2]
            nib = (packed >> 4) if x % 2 == 0 else (packed & 0x0F)
            px[x, y] = nib * 17
    return img


def write_previews(builder: PackBuilder, out_dir: str, count: int) -> None:
    """把若干背景 + 立绘 + 文本框画成 PNG,用来肉眼核对裁切/定位是否符合预期。"""
    ceil = min(count, len(builder.bg_blobs))
    for idx in range(ceil):
        bg = Image.open(io.BytesIO(builder.bg_blobs[idx])).convert("RGB")
        canvas = bg.resize((SCREEN_W, SCREEN_H), Image.LANCZOS)
        # 找第一张立绘贴在画面上,顺序预览即可
        if builder.fg_blobs:
            color, mask, w, h, x, y = builder.fg_blobs[idx % len(builder.fg_blobs)]
            sprite = rgb565_to_rgb(color, w, h)
            m = unpack_alpha_4bpp(mask, w, h)
            canvas.paste(sprite, (x, y), m)
        overlay = Image.new("RGB", (SCREEN_W, SCREEN_H - BOX_Y), (16, 18, 22))
        canvas.paste(overlay, (0, BOX_Y))
        canvas.save(os.path.join(out_dir, f"preview_{idx:02d}.png"))
    log(f"预览图写出到 {out_dir} ({ceil} 张)")


if __name__ == "__main__":
    raise SystemExit(main())
