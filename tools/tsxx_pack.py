#!/usr/bin/env python3
"""把《天使☆騒々 RE-BOOT!》小米手环移植版的素材打成 AI Passport 用的单一资源包。

源项目: https://github.com/hezdaaa/tsxxreboot-miband
  - 小米手环快应用(Xiaomi Vela)上的非官方移植;剧本、立绘、CG、背景的版权归
    柚子社(Yuzusoft)及原发行方所有。
  - 本仓库只保存转换产物,不再分发源素材;重新生成需要自备源 checkout。
  - 请支持正版。

产物是一个小端二进制,固件直接从 Flash 内存映射读取:没有解压、没有 JSON 解析、
没有逐帧分配。布局:

  header  : magic "TSXXPK01", version u32, total_size u32, section_count u32   (20 B)
  section : { type u32, offset u32, count u32, size u32 }                       (16 B)
    带目录的段(背景 / 立绘 / 事件帧 / 事件补丁)格式为 [目录][数据],
    目录项里的 off 相对该段数据区起点。

  段                                    条目     说明
  SEC_SYM     u32                       码位数   码位表,下标 = 符号 id(按词频降序)
  SEC_TEXT    u8                        字节数   正文变长码流:id<255 → id+1;否则 0x00+u16
  SEC_TOFF    u32                       检查点   每 CHECKPOINT_PAGES 页一个正文字节偏移
  SEC_TLEN    u8                        页数     每页字符数(0 = 本页没有正文)
  SEC_PBG     u8                        页数     每页背景下标
  SEC_PSPK    u8                        页数     每页说话人下标(0xFF = 无)
  SEC_PSPR    u8                        页数     每页立绘下标(0xFF = 无)
  SEC_PFLAG   u8                        页数     每页标志:bit0 = 有 z(缩放提示)
  SEC_PCGB    u8                        位图     有事件图的页位图
  SEC_PCG     u16                       有图页数 该页在 SEC_CGDIR 里的下标
  SEC_BGNAME  u8                        字节数   背景名,\\n 分隔(下标与 SEC_BG 对齐)
  SEC_SPKNAME u8                        字节数   说话人名,\\n 分隔
  SEC_SPRNAME u8                        字节数   立绘名,\\n 分隔(下标与 SEC_FG 对齐)
  SEC_CGNAME  u8                        字节数   事件图名,\\n 分隔(下标与 SEC_CGDIR 对齐)
  SEC_CHOICE  {page u32, count u8, pad u8, pad u16, first u32}          12 B  选项点
  SEC_CHOICEOPT {off u32, len u8, pad u8, pad u16, target u32}          12 B  选项
  SEC_BG      {off u32, len u32, w u16, h u16}                          12 B  背景 JPEG
  SEC_FG      {off u32, len u32, mask_off u32, mask_len u32,
               w u16, h u16, x u16, y u16}                              24 B  立绘
  SEC_EVB     {off u32, len u32, w u16, h u16}                          12 B  事件整帧 JPEG
  SEC_EVC     {off u32, len u32, w u16, h u16}                          12 B  事件补丁 JPEG
  SEC_CGDIR   {kind u8, pad u8, pad u16, base u32,
               x u16, y u16, w u16, h u16, img u32}                     20 B  事件渲染配方
  SEC_META    u8                        字节数   key=value 来源信息

  正文里的字符按出现频率排序:前 255 个码位用 1 字节,其余用 0x00 + u16。
  源数据 116 万字符只落在 3417 个不同码位上,平均 1.39 字节/字(UTF-8 要 2.86)。
  选项文案也进同一段码流,按绝对偏移读取;SEC_TOFF 的检查点只覆盖页正文。

  SEC_CGDIR 的 kind:
    0 整帧   — 直接画 SEC_EVB[img]
    1 补丁   — 先画 SEC_CGDIR[base](必须是 kind 0),再把 SEC_EVC[img] 贴到 (x, y)
  补丁矩形用的是基准帧自己的坐标系;帧是美术层尺寸,固件负责放大到整屏。
  事件图的 a/b/c… 差分里只有"同场景换表情/动手"那部分适合补丁;背景差分是整幅换
  时段、立绘差分是重画姿势,都不做补丁(实测省不到 8% / 0%)。

美术层在 --art-width 宽(默认 180)的缩小画布上渲染,固件把整层放大到 240×320
显示:帧缓冲只要 180×240×2 = 84 KB,是整屏 240×320×2 = 150 KB 的一半多,
所有美术素材也随之变小。文字层仍由固件在原生 240×320 上绘制,不受影响。
美术高度按屏幕长宽比推导(180 → 240),放大后不会拉伸。

图像转换(用美术层坐标;文本框顶边 = 画面区底边,与 ATRI / 星空列车一致):
  背景(bcgi)   等比放大到铺满美术层 180×240 后居中裁切 → JPEG
  立绘(cimg)   取 alpha 包围盒 → 等比缩放到 --sprite-height(默认 = 美术高度)
               → 水平居中、底边贴 --sprite-bottom → 裁到画面区 [0, --box-y)
               → JPEG + 1bpp 遮罩。透明区域先用相邻不透明像素的颜色膨胀填充,
               否则 JPEG 会在立绘边缘留下暗边。默认参数与源工程取景一致(源工程
               是 height:100% 全屏画,文本框压住下半身,画面上可见约 66%;
               这里 160/240 = 67%)。
  事件图(evig)  同背景(与美术层同尺寸);同一差分组的成员按差异外接框裁成补丁,
               补丁比整帧还大时退回整帧,整组差异为空时直接复用基准帧。基准帧取
               该组的中位图(到其余成员差异总量最小),而不是第一个,以缩小补丁面积。

只打包剧本真正引用到的素材;引用不到的文件会被丢弃并在报告里列出。源数据本身
有缺陷时会如实报告并降级(例如某页的立绘名缺前缀、cg 字段是空串)。

用法:
  python tools/tsxx_pack.py --source <源 checkout> --out main/tsxx_data/tsxx_pack.bin
  python tools/tsxx_pack.py --verify main/tsxx_data/tsxx_pack.bin
  python tools/tsxx_pack.py --source <源 checkout> --out ... --report --symbols-out <file>

源 checkout 里放 src/common/{script,bcgi,cimg,evig};也可以只放这四个目录
(仓库内的 assets/tsxx-source 就是后者),或用 --script-dir / --bg-dir /
--cimg-dir / --evig-dir 分别指定。
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

MAGIC = b"TSXXPK01"
VERSION = 1
GEN_VERSION = "tsxx_pack/1"
# META 里记的来源标识。默认写上游项目名而不是本地路径,这样从任何位置重建
# 都得到同一个资源包(已提交的包与 assets/README.md 里那条命令逐字节一致)。
DEFAULT_SOURCE_LABEL = "hezdaaa/tsxxreboot-miband"

SCREEN_W, SCREEN_H = 240, 320
# 美术层:背景/立绘/事件图都渲染在这个缩小的画布上,固件放大到整屏显示。
# 必须与 main/tsxx_pack.h 的 TSXX_ART_W 一致;实际尺寸记录在 META 的 art= 里。
ART_W = 180


def art_height(width: int) -> int:
    """美术高度:与整屏同长宽比,固件放大后不会变形。"""
    return max(1, int(round(width * SCREEN_H / SCREEN_W)))


ART_H = art_height(ART_W)
# 文本框顶边 = 画面区底边,坐标是美术层坐标(214 × 180/240 取整 = 160);
# 必须与 main 侧布局常量一致。
# 立绘裁切下界(美术坐标)。0 = 不裁:整身画到画面底,由文本框压住下半身 ——
# 源工程就是这么画的(height:100% 全屏画 + 半透明文本框盖在底部)。
DEFAULT_BOX_Y = 0
# 立绘等比缩放到这个高度(美术坐标),底边贴 DEFAULT_SPRITE_BOTTOM。
# 160:角色占画面下 2/3,身体一直延伸到底边被文本框压住 —— 源工程的版式。
# 裁切下界 DEFAULT_BOX_Y = 0 表示不裁,整身存下来(下半身靠文本框盖住)。
DEFAULT_SPRITE_HEIGHT = ART_H * 2 // 3
DEFAULT_SPRITE_BOTTOM = ART_H
# 差异判定阈值:JPEG 有损,低于它的差值按压缩噪声处理。
DEFAULT_PATCH_TOLERANCE = 20
# 每个检查点覆盖的页数;随机跳页最多重扫这么多行。
CHECKPOINT_PAGES = 256

SEC_SYM, SEC_TEXT, SEC_TOFF, SEC_TLEN = 0, 1, 2, 3
SEC_PBG, SEC_PSPK, SEC_PSPR, SEC_PFLAG = 4, 5, 6, 7
SEC_PCGB, SEC_PCG = 8, 9
SEC_BGNAME, SEC_SPKNAME, SEC_SPRNAME, SEC_CGNAME = 10, 11, 12, 13
SEC_CHOICE, SEC_CHOICEOPT = 14, 15
SEC_BG, SEC_FG, SEC_EVB, SEC_EVC, SEC_CGDIR = 16, 17, 18, 19, 20
SEC_META = 21
SECTION_COUNT = 22

SEC_NAMES = {
    SEC_SYM: "SYM", SEC_TEXT: "TEXT", SEC_TOFF: "TOFF", SEC_TLEN: "TLEN",
    SEC_PBG: "PBG", SEC_PSPK: "PSPK", SEC_PSPR: "PSPR", SEC_PFLAG: "PFLAG",
    SEC_PCGB: "PCGB", SEC_PCG: "PCG",
    SEC_BGNAME: "BGNAME", SEC_SPKNAME: "SPKNAME", SEC_SPRNAME: "SPRNAME",
    SEC_CGNAME: "CGNAME", SEC_CHOICE: "CHOICE", SEC_CHOICEOPT: "CHOICEOPT",
    SEC_BG: "BG", SEC_FG: "FG", SEC_EVB: "EVB", SEC_EVC: "EVC",
    SEC_CGDIR: "CGDIR", SEC_META: "META",
}

CH_ENTRY, CHOPT_ENTRY = 12, 12
BG_ENTRY, FG_ENTRY, EV_ENTRY, CGD_ENTRY = 12, 24, 12, 20

CG_KIND_FRAME, CG_KIND_PATCH = 0, 1
NONE8 = 0xFF
PAGE_FLAG_ZOOM = 1 << 0
# 源数据里 z 只用于选缩放类,全库 19,991 处都是 2;包内只存"有没有"一个比特
# (61 KB → 61 KB 位图不值得)。出现其他值就构建失败,而不是悄悄丢掉缩放提示。
ZOOM_VALUE = 2
# 正文里 1 字节码的上界;必须与解码端一致。
SHORT_CODE_LIMIT = 255
MAX_PAGE_CHARS = 255

SCRIPT_FILE_RE = re.compile(r"^scriptData(\d+)\.txt$")
# 事件图差分组:ev<编号><单字母后缀>。
EVENT_GROUP_RE = re.compile(r"^(ev\d+)([a-z])$")
IMAGE_SUFFIXES = (".jpg", ".jpeg", ".png")

Image = None  # type: ignore[assignment]
np = None  # type: ignore[assignment]


def require_pillow() -> None:
    global Image
    require_numpy()
    if Image is None:
        try:
            from PIL import Image as pil_image
        except ImportError:  # pragma: no cover - 工具依赖
            sys.exit("需要 Pillow: python -m pip install pillow")
        Image = pil_image


def require_numpy() -> None:
    global np
    if np is None:
        try:
            import numpy
        except ImportError:  # pragma: no cover - 工具依赖
            sys.exit("需要 numpy: python -m pip install numpy")
        np = numpy


# --------------------------------------------------------------------------- #
# 纯逻辑(不依赖 Pillow / numpy,宿主机测试直接调用)
# --------------------------------------------------------------------------- #

def build_symbol_table(texts: Iterable[str]) -> Tuple[List[str], Dict[str, int]]:
    """按出现频率降序建码位表;前 SHORT_CODE_LIMIT 个用 1 字节码。"""
    counts: Dict[str, int] = {}
    for text in texts:
        for ch in text:
            counts[ch] = counts.get(ch, 0) + 1
    # 频率相同时按码位排序,保证同样输入产出同样的包。
    symbols = sorted(counts, key=lambda c: (-counts[c], c))
    return symbols, {ch: i for i, ch in enumerate(symbols)}


def encode_text(text: str, symbol_id: Dict[str, int]) -> bytes:
    out = bytearray()
    for ch in text:
        index = symbol_id[ch]
        if index < SHORT_CODE_LIMIT:
            out.append(index + 1)
        else:
            out.append(0)
            out += struct.pack("<H", index)
    return bytes(out)


def decode_text(blob: bytes, offset: int, length: int,
                symbols: Sequence[str]) -> Tuple[str, int]:
    """从 offset 解出 length 个字符,返回 (文本, 结束后的字节偏移)。"""
    out: List[str] = []
    pos = offset
    for _ in range(length):
        code = blob[pos]
        if code == 0:
            out.append(symbols[struct.unpack_from("<H", blob, pos + 1)[0]])
            pos += 3
        else:
            out.append(symbols[code - 1])
            pos += 1
    return "".join(out), pos


def text_byte_offset(checkpoints: Sequence[int], lengths: bytes, text: bytes,
                     page: int) -> int:
    """由检查点 + 每页字符数还原某一页正文的起始字节偏移。"""
    block = page // CHECKPOINT_PAGES
    remaining = int(sum(lengths[block * CHECKPOINT_PAGES:page]))
    pos = checkpoints[block]
    while remaining > 0:
        pos += 3 if text[pos] == 0 else 1
        remaining -= 1
    return pos


def event_group(name: str) -> Optional[str]:
    """ev123a.jpg → ev123;不属于差分组的返回 None。"""
    stem = name
    for suffix in IMAGE_SUFFIXES:
        if stem.lower().endswith(suffix):
            stem = stem[: -len(suffix)]
            break
    match = EVENT_GROUP_RE.match(stem)
    return match.group(1) if match else None


def encode_name_table(names: Sequence[str]) -> bytes:
    for name in names:
        if "\n" in name:
            raise ValueError(f"资源名不能含换行: {name!r}")
    return "\n".join(names).encode("utf-8")


def decode_name_table(blob: bytes) -> List[str]:
    if not blob:
        return []
    return blob.decode("utf-8").split("\n")


def script_sort_key(filename: str) -> Tuple[int, str]:
    match = SCRIPT_FILE_RE.match(filename)
    return (int(match.group(1)) if match else 1 << 30, filename)


def checkpoint_count(pages: int) -> int:
    return (pages + CHECKPOINT_PAGES - 1) // CHECKPOINT_PAGES + 1


# --------------------------------------------------------------------------- #
# 源数据读取
# --------------------------------------------------------------------------- #

def load_pages(script_dir: str) -> List[Tuple[str, int, dict]]:
    """按 scriptData<编号>.txt 的顺序读入全部页,返回 (文件, 页号, 页字典)。"""
    import json

    files = sorted((f for f in os.listdir(script_dir) if SCRIPT_FILE_RE.match(f)),
                   key=script_sort_key)
    if not files:
        raise SystemExit(f"{script_dir} 里没有 scriptData*.txt")
    pages: List[Tuple[str, int, dict]] = []
    for name in files:
        with open(os.path.join(script_dir, name), encoding="utf-8") as handle:
            data = json.load(handle)
        for key, page in data.items():
            if not isinstance(page, dict):
                raise SystemExit(f"{name}: 第 {key} 页不是对象")
            pages.append((name, int(key), page))
    return pages


def referenced_names(pages: Sequence[Tuple[str, int, dict]]) -> Dict[str, List[str]]:
    """按出现次数降序返回脚本引用到的背景 / 说话人 / 立绘 / 事件图名。"""
    fields = {"bg": "b", "speaker": "s", "sprite": "c", "event": "cg"}
    seen: Dict[str, Dict[str, int]] = {key: {} for key in fields}
    for _, _, page in pages:
        for key, field in fields.items():
            value = page.get(field)
            if isinstance(value, str) and value:
                seen[key][value] = seen[key].get(value, 0) + 1
    return {key: sorted(counts, key=lambda n: (-counts[n], n))
            for key, counts in seen.items()}


def index_assets(directory: str) -> Dict[str, str]:
    """文件名(含扩展名) → 完整路径;同时提供去扩展名的别名。"""
    index: Dict[str, str] = {}
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            continue
        index.setdefault(name, path)
        index.setdefault(os.path.splitext(name)[0], path)
    return index


def resolve_assets(used: Sequence[str],
                   index: Dict[str, str]) -> Tuple[List[str], List[str]]:
    """把脚本里的名字解析成实际文件,返回 (存在的名字表, 缺失的名字)。"""
    names: List[str] = []
    missing: List[str] = []
    for name in used:
        (names if name in index else missing).append(name)
    return names, missing


# --------------------------------------------------------------------------- #
# 图像转换
# --------------------------------------------------------------------------- #

def cover_crop(source, width: int, height: int):
    """等比放大到铺满目标框后居中裁切。"""
    scale = max(width / source.width, height / source.height)
    resized = source.resize((max(width, int(round(source.width * scale))),
                             max(height, int(round(source.height * scale)))),
                            Image.LANCZOS)
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    return resized.crop((left, top, left + width, top + height))


def encode_jpeg(image, quality: int) -> bytes:
    import io

    buffer = io.BytesIO()
    image.convert("RGB").save(buffer, "JPEG", quality=quality, optimize=True,
                              subsampling=2)
    return buffer.getvalue()


def dilate_rgb(rgb, alpha, rounds: int = 6):
    """把不透明像素的颜色向外扩散填掉透明区域,避免 JPEG 边缘出现暗边。"""
    if rounds <= 0:
        return rgb
    known = alpha > 8
    if not known.any():
        return rgb
    totals = rgb.astype("int32") * known[:, :, None]
    weights = known.astype("int32")[:, :, None]
    for _ in range(rounds):
        if known.all():
            break
        for axis, shift in ((0, 1), (0, -1), (1, 1), (1, -1)):
            source = np.roll(np.roll(totals, shift, axis=axis), 0, axis=0)
            source_w = np.roll(weights, shift, axis=axis)
            source_known = np.roll(known, shift, axis=axis)
            fillable = source_known & ~known
            if not fillable.any():
                continue
            mask = fillable[:, :, None]
            totals = np.where(mask, source, totals)
            weights = np.where(mask, source_w, weights)
            known = known | fillable
    return (totals / np.maximum(weights, 1)).astype("uint8")


def encode_mask(alpha) -> bytes:
    """1bpp 遮罩,stride = (w + 7) // 8,位 1 = 不透明。"""
    height, width = alpha.shape
    stride = (width + 7) // 8
    bits = (alpha > 8).astype("uint8")
    padding = stride * 8 - width
    if padding:
        bits = np.pad(bits, ((0, 0), (0, padding)))
    return np.packbits(bits, axis=1).tobytes()


def sprite_layout(source_w: int, source_h: int, sprite_height: int,
                  sprite_bottom: int, box_y: int,
                  art_w: int) -> Tuple[int, int, int, int, int, int]:
    """算出立绘在美术层里的落位。

    返回 (缩放后宽, 缩放后高, 美术层 x, 裁切起始行, 裁后宽, 裁后高)。
    先按 --sprite-height 等比缩放;若这样会比美术层还宽,改用美术层宽约束再等比
    缩一次,否则水平居中会出现负的 x。
    """
    draw_h = max(1, int(round(sprite_height)))
    draw_w = max(1, int(round(source_w * draw_h / source_h)))
    if draw_w > art_w:
        draw_w = art_w
        draw_h = max(1, int(round(source_h * draw_w / source_w)))
    top = sprite_bottom - draw_h
    visible_top = max(0, top)
    visible_bottom = min(box_y, top + draw_h)
    if visible_bottom <= visible_top:
        raise ValueError("立绘位置完全落在画面区之外")
    left = max(0, (art_w - draw_w) // 2)
    return draw_w, draw_h, left, visible_top - top, draw_w, visible_bottom - visible_top


def convert_sprite(path: str, quality: int, sprite_height: int, sprite_bottom: int,
                   box_y: int, art_w: int) -> Tuple[bytes, bytes, int, int, int, int]:
    """裁到画面区的立绘:返回 (JPEG, 1bpp 遮罩, w, h, x, y)。"""
    with Image.open(path) as raw:
        rgba = raw.convert("RGBA")
    opaque = rgba.split()[3].point(lambda value: 255 if value > 8 else 0)
    box = opaque.getbbox()
    if box is None:
        raise ValueError(f"{path} 完全没有不透明像素")
    rgba = rgba.crop(box)
    draw_w, draw_h, left, row_start, width, height = sprite_layout(
        rgba.width, rgba.height, sprite_height, sprite_bottom, box_y, art_w)
    resized = rgba.resize((draw_w, draw_h), Image.LANCZOS)
    cropped = resized.crop((0, row_start, width, row_start + height))
    if cropped.size != (width, height):
        raise ValueError(f"{path} 裁切结果 {cropped.size} != {(width, height)}")
    rgb = dilate_rgb(np.asarray(cropped.convert("RGB")),
                     np.asarray(cropped.split()[3]))
    # 记录里存的是裁后图在美术层上的落点 y(= 画面区内起始行),不是裁切行。
    visible_top = sprite_bottom - draw_h + row_start
    return (encode_jpeg(Image.fromarray(rgb), quality),
            encode_mask(np.asarray(cropped.split()[3])),
            width, height, left, visible_top)


def diff_bbox(base, variant, tolerance: int) -> Optional[Tuple[int, int, int, int]]:
    """两张同尺寸图的差异外接框 (x0, y0, x1, y1);无差异返回 None。"""
    if base.shape != variant.shape:
        return None
    delta = np.abs(base.astype("int16") - variant.astype("int16")).max(axis=2)
    mask = delta > tolerance
    if not mask.any():
        return None
    # 3x3 邻域投票,滤掉有损压缩的孤立噪声点。
    padded = np.pad(mask.astype("uint8"), 1)
    neighbours = (padded[0:-2, 0:-2] + padded[0:-2, 1:-1] + padded[0:-2, 2:] +
                  padded[1:-1, 0:-2] + padded[1:-1, 1:-1] + padded[1:-1, 2:] +
                  padded[2:, 0:-2] + padded[2:, 1:-1] + padded[2:, 2:])
    mask &= neighbours >= 5
    if not mask.any():
        return None
    rows, cols = np.nonzero(mask)
    return (int(cols.min()), int(rows.min()),
            int(cols.max()) + 1, int(rows.max()) + 1)


def choose_medoid(members: Sequence[str], pixels: Dict[str, object]) -> str:
    """选差异总量最小的成员做基准帧,让补丁面积更小。"""
    best, best_cost = members[0], None
    for candidate in members:
        cost = 0
        for other in members:
            if other != candidate:
                cost += int((np.abs(pixels[candidate] - pixels[other]).max(axis=2)
                             > DEFAULT_PATCH_TOLERANCE).sum())
        if best_cost is None or cost < best_cost:
            best, best_cost = candidate, cost
    return best


# --------------------------------------------------------------------------- #
# 段与容器
# --------------------------------------------------------------------------- #

class Section:
    """目录项 + 数据区的段。"""

    def __init__(self, kind: int, entry_size: int = 0):
        self.kind = kind
        self.entry_size = entry_size
        self.entries = bytearray()
        self.data = bytearray()
        self.count = 0

    def add(self, entry: bytes, payload: bytes = b"") -> int:
        if self.entry_size and len(entry) != self.entry_size:
            raise AssertionError(f"{SEC_NAMES[self.kind]} 目录项 {len(entry)} 字节")
        self.entries += entry
        self.data += payload
        self.count += 1
        return self.count - 1

    def add_image(self, payload: bytes, width: int, height: int) -> int:
        offset = len(self.data)
        return self.add(struct.pack("<IIHH", offset, len(payload), width, height), payload)

    def as_bytes(self) -> bytes:
        return bytes(self.entries) + bytes(self.data)


def plain_section(kind: int, payload: bytes, count: int) -> Section:
    section = Section(kind)
    section.data = bytearray(payload)
    section.count = count
    return section


def frame_record(frame_index: int) -> bytes:
    return struct.pack("<BBHIHHHHI", CG_KIND_FRAME, 0, 0, 0, 0, 0, 0, 0, frame_index)


def patch_record(base_index: int, rect: Sequence[int], patch_index: int) -> bytes:
    return struct.pack("<BBHIHHHHI", CG_KIND_PATCH, 0, 0, base_index,
                       rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1],
                       patch_index)


def write_pack(sections: Sequence[Section]) -> bytes:
    header = MAGIC + struct.pack("<III", VERSION, 0, len(sections))
    offset = len(header) + 16 * len(sections)
    table = bytearray()
    body = bytearray()
    for section in sections:
        payload = section.as_bytes()
        table += struct.pack("<IIII", section.kind, offset, section.count, len(payload))
        body += payload
        offset += len(payload)
    blob = bytearray(header) + table + body
    struct.pack_into("<I", blob, 12, len(blob))  # total_size
    return bytes(blob)


def read_pack(path: str) -> dict:
    with open(path, "rb") as handle:
        blob = handle.read()
    return parse_pack(blob, path)


def parse_pack(blob: bytes, label: str = "pack") -> dict:
    if len(blob) < 20:
        raise ValueError(f"{label}: 文件太短({len(blob)} 字节)")
    if blob[:8] != MAGIC:
        raise ValueError(f"{label}: 魔数不对(先跑 tools/tsxx_pack.py)")
    version, total, count = struct.unpack_from("<III", blob, 8)
    if version != VERSION:
        raise ValueError(f"{label}: 版本 {version} != {VERSION}")
    if total != len(blob):
        raise ValueError(f"{label}: 头部长度 {total} != 实际 {len(blob)}")
    sections: Dict[int, Tuple[int, int, int]] = {}
    for index in range(count):
        kind, offset, items, size = struct.unpack_from("<IIII", blob, 20 + 16 * index)
        if kind >= SECTION_COUNT:
            raise ValueError(f"{label}: 未知段类型 {kind}")
        if offset + size > len(blob):
            raise ValueError(f"{label}: 段 {SEC_NAMES.get(kind, kind)} 越界")
        sections[kind] = (offset, items, size)
    missing = set(range(SECTION_COUNT)) - set(sections)
    if missing:
        raise ValueError(f"{label}: 缺段 {sorted(SEC_NAMES[m] for m in missing)}")
    return {"blob": blob, "sections": sections}


def section_bytes(pack: dict, kind: int) -> bytes:
    offset, _, size = pack["sections"][kind]
    return pack["blob"][offset:offset + size]


def directory_entry(pack: dict, kind: int, stride: int, index: int) -> bytes:
    offset = pack["sections"][kind][0]
    start = offset + index * stride
    return pack["blob"][start:start + stride]


def blob_area(pack: dict, kind: int, stride: int) -> Tuple[int, int]:
    """返回 (数据区起点, 数据区长度)。"""
    offset, count, size = pack["sections"][kind]
    return offset + count * stride, size - count * stride


# --------------------------------------------------------------------------- #
# 打包
# --------------------------------------------------------------------------- #

class PackBuilder:
    def __init__(self, options: argparse.Namespace):
        self.options = options

    def log(self, line: str) -> None:
        print(line)

    def build(self) -> Tuple[bytes, Dict[str, object]]:
        options = self.options
        art_w = options.art_width
        if art_w <= 0 or art_w > SCREEN_W:
            raise SystemExit(f"--art-width 必须在 1..{SCREEN_W} 之间")
        art_h = art_height(art_w)
        pages = load_pages(options.script_dir)
        used = referenced_names(pages)
        self.log(f"剧本 {len(pages)} 页 / {len(used['speaker'])} 个说话人")

        bg_names, bg_missing = resolve_assets(used["bg"], index_assets(options.bg_dir))
        sprite_names, sprite_missing = resolve_assets(
            used["sprite"], index_assets(options.cimg_dir))
        # 同组立绘只留一张:姿势差分只影响姿态,保留一张能换回大量空间,
        # 用来把这张推到满高 + 高画质。被丢掉的差分名映射到同组保留的那张,
        # 所以剧本引用它们时仍会画出这个角色。
        if options.sprite_pose_limit > 0:
            kept: List[str] = []
            alias: Dict[str, str] = {}
            for name in sprite_names:
                group = sprite_group(name)
                if group in alias:
                    continue
                alias[group] = name
                kept.append(name)
            dropped = len(sprite_names) - len(kept)
            sprite_names = kept
            for name in used["sprite"]:
                if name not in sprite_names:
                    alias.setdefault(name, alias.get(sprite_group(name), name))
            options.sprite_alias = alias
            if dropped:
                self.log(f"立绘差分合并: {len(used['sprite'])} → {len(kept)} 张"
                         f"(同组姿势差分丢 {dropped} 张,引用保留那张)")
        else:
            options.sprite_alias = {}
        event_names, event_missing = resolve_assets(
            used["event"], index_assets(options.evig_dir))
        bg_index = index_assets(options.bg_dir)
        sprite_index = index_assets(options.cimg_dir)
        event_index = index_assets(options.evig_dir)

        missing: Dict[str, List[str]] = {"背景": bg_missing, "立绘": sprite_missing,
                                         "事件图": event_missing}
        for kind, names in missing.items():
            if names:
                # 源数据本身有缺陷(立绘名缺前缀、cg 是空串),如实报告、不静默吞掉。
                self.log(f"警告: 脚本引用了 {len(names)} 个不存在的{kind}: {names[:6]}")

        # ---- 台词收集(含选项文案,它们共用同一张码位表)----
        page_texts: List[str] = []
        choices: List[Tuple[int, List[Tuple[str, int]]]] = []
        for index, (_, _, page) in enumerate(pages):
            raw = page.get("t")
            if isinstance(raw, str) and raw:
                if len(raw) > MAX_PAGE_CHARS:
                    raise SystemExit(f"第 {index} 页正文 {len(raw)} 字超过 u8,需要先分页")
                page_texts.append(raw)
            options_list = [(page[f"c{slot}"], int(page[f"c{slot}t"]))
                            for slot in range(1, 6) if f"c{slot}" in page]
            if options_list:
                choices.append((index, options_list))
        option_texts = [text for _, options_list in choices for text, _ in options_list]

        symbols, symbol_id = build_symbol_table(page_texts + option_texts)
        self.log(f"正文 {len(page_texts)} 行 / {sum(len(t) for t in page_texts)} 字符"
                 f" / {len(symbols)} 个码位")

        # ---- 正文码流 + 页检查点 ----
        text_blob = bytearray()
        lengths = bytearray()
        checkpoints = [0]
        for index, (_, _, page) in enumerate(pages):
            raw = page.get("t")
            if isinstance(raw, str) and raw:
                text_blob += encode_text(raw, symbol_id)
                lengths.append(len(raw))
            else:
                lengths.append(0)
            if (index + 1) % CHECKPOINT_PAGES == 0:
                checkpoints.append(len(text_blob))
        checkpoints.append(len(text_blob))

        choice_blob = bytearray()
        choice_opt_blob = bytearray()
        first_option = 0
        for page_index, options_list in choices:
            choice_blob += struct.pack("<IBBHI", page_index, len(options_list), 0, 0,
                                       first_option)
            for text, target in options_list:
                choice_opt_blob += struct.pack("<IBBHI", len(text_blob), len(text), 0, 0,
                                               target)
                text_blob += encode_text(text, symbol_id)
            first_option += len(options_list)
        self.log(f"选项点 {len(choices)} 个 / 选项 {first_option} 条")

        # ---- 页表 ----
        bg_of = {name: i for i, name in enumerate(bg_names)}
        sprite_of = {name: i for i, name in enumerate(sprite_names)}
        for name, target in getattr(options, "sprite_alias", {}).items():
            if target in sprite_of:
                sprite_of[name] = sprite_of[target]
        speaker_of = {name: i for i, name in enumerate(used["speaker"])}
        event_of = {name: i for i, name in enumerate(event_names)}
        pbg = bytearray()
        pspk = bytearray()
        pspr = bytearray()
        pflag = bytearray()
        cg_bitmap = bytearray((len(pages) + 7) // 8)
        cg_pages: List[int] = []
        normalized = {"empty_cg": 0, "empty_sprite": 0, "unknown_cg": 0}
        for index, (_, _, page) in enumerate(pages):
            pbg.append(bg_of[page["b"]])
            pspk.append(speaker_of.get(page.get("s", ""), NONE8))
            # 源数据有两类缺陷:cg 是空串(2049 页)、立绘名缺前缀只剩 _1/_2/_3(650 页)。
            # 两类都归一到"本页没有这张图",并统计出来。
            sprite_key = page.get("c")
            if sprite_key in sprite_of:
                pspr.append(sprite_of[sprite_key])
            else:
                if isinstance(sprite_key, str) and sprite_key:
                    normalized["empty_sprite"] += 1
                pspr.append(NONE8)
            pflag.append(0)
            if "z" in page:
                if page["z"] != ZOOM_VALUE:
                    raise SystemExit(
                        f"第 {index} 页 z={page['z']!r},包格式只支持 {ZOOM_VALUE}")
                pflag[index] = PAGE_FLAG_ZOOM
            name = page.get("cg")
            if name in event_of:
                cg_bitmap[index >> 3] |= 1 << (index & 7)
                cg_pages.append(event_of[name])
            elif name:
                normalized["unknown_cg"] += 1
            elif "cg" in page:
                normalized["empty_cg"] += 1
        for key, count in normalized.items():
            if count:
                self.log(f"规范化: {count} 页 {key}")

        # ---- 背景 ----
        bg_section = Section(SEC_BG, BG_ENTRY)
        for name in bg_names:
            with Image.open(bg_index[name]) as raw:
                frame = cover_crop(raw.convert("RGB"), art_w, art_h)
            bg_section.add_image(encode_jpeg(frame, options.bg_quality), art_w, art_h)
        self.log(f"背景 {bg_section.count} 张 ({art_w}×{art_h})")
        # 标题背景:追加到背景表末尾,名字固定为 __title__,下标写进 META。
        # 源素材里没有可用的标题画,所以由 --title-image 另外给一张。
        title_bg = 0
        if options.title_image:
            with Image.open(options.title_image) as raw:
                title_frame = cover_crop(raw.convert("RGB"), art_w, art_h)
            title_bg = bg_section.add_image(
                encode_jpeg(title_frame, options.bg_quality), art_w, art_h)
            bg_names = list(bg_names) + ["__title__"]
            self.log(f"标题背景 #{title_bg} ← {os.path.basename(options.title_image)}")

        # ---- 立绘 ----
        fg_section = Section(SEC_FG, FG_ENTRY)
        sprite_max_px = 0
        # box_y = 0 表示不裁切(整身到底);否则裁到该行。
        sprite_visible_bottom = options.box_y if options.box_y > 0 else art_h
        for name in sprite_names:
            body, mask, width, height, x, y = convert_sprite(
                sprite_index[name], options.sprite_quality, options.sprite_height,
                options.sprite_bottom, sprite_visible_bottom, art_w)
            body_offset = len(fg_section.data)
            fg_section.add(struct.pack("<IIIIHHHH", body_offset, len(body),
                                       body_offset + len(body), len(mask),
                                       width, height, x, y), body + mask)
            sprite_max_px = max(sprite_max_px, width * height)
        self.log(f"立绘 {fg_section.count} 张(最大 {sprite_max_px} 像素)")
        # 固件用一块「立绘与补丁取最大值」的暂存区解码。把补丁也卡在立绘预算以内,
        # 暂存区就只由立绘决定(最大 157x160 = 50 KB,而不是补丁的 146x240 = 70 KB);
        # 超限的补丁退化成整帧 —— 整帧解码直接进画布,根本不占暂存区。
        patch_budget_px = sprite_max_px if options.patch_max_pixels <= 0 \
            else min(options.patch_max_pixels, sprite_max_px)

        # ---- 事件图:整帧 + 差分补丁 ----
        # 事件图与美术层同尺寸,固件负责放大到整屏。补丁矩形与差异判定都在美术
        # 坐标里做,固件按 EVB 的 w/h 与整屏的比例换算。
        event_w, event_h = art_w, art_h
        event_section = Section(SEC_EVB, EV_ENTRY)
        patch_section = Section(SEC_EVC, EV_ENTRY)
        groups: Dict[str, List[str]] = {}
        for name in event_names:
            key = event_group(name)
            if key:
                groups.setdefault(key, []).append(name)

        pixels: Dict[str, object] = {}

        def raster(name: str):
            if name not in pixels:
                with Image.open(event_index[name]) as raw:
                    pixels[name] = cover_crop(raw.convert("RGB"), event_w, event_h)
            return pixels[name]

        records: Dict[str, bytes] = {}
        dir_index_of = {name: i for i, name in enumerate(event_names)}
        stats = {"groups": 0, "patches": 0, "identical": 0, "full": 0}
        for name in event_names:
            if name in records:
                continue
            key = event_group(name)
            members = groups.get(key, ()) if key else ()
            if len(members) < 2:
                frame = raster(name)
                index = event_section.add_image(
                    encode_jpeg(frame, options.event_quality), event_w, event_h)
                records[name] = frame_record(index)
                pixels.pop(name, None)
                continue
            stats["groups"] += 1
            base_name = choose_medoid(members, {m: np.asarray(raster(m)) for m in members})
            base_frame = raster(base_name)
            base_index = event_section.add_image(
                encode_jpeg(base_frame, options.event_quality), event_w, event_h)
            records[base_name] = frame_record(base_index)
            base_array = np.asarray(base_frame)
            for member in members:
                if member == base_name:
                    continue
                variant = raster(member)
                rect = diff_bbox(base_array, np.asarray(variant),
                                 options.patch_tolerance)
                if rect is None:
                    stats["identical"] += 1
                    records[member] = records[base_name]
                    pixels.pop(member, None)
                    continue
                patch_payload = encode_jpeg(variant.crop(rect), options.event_quality)
                full_payload = encode_jpeg(variant, options.event_quality)
                rect_px = (rect[2] - rect[0]) * (rect[3] - rect[1])
                if rect_px > patch_budget_px or len(patch_payload) >= len(full_payload):
                    stats["full"] += 1
                    index = event_section.add_image(full_payload, event_w, event_h)
                    records[member] = frame_record(index)
                else:
                    stats["patches"] += 1
                    index = patch_section.add_image(patch_payload,
                                                    rect[2] - rect[0], rect[3] - rect[1])
                    records[member] = patch_record(dir_index_of[base_name], rect, index)
                pixels.pop(member, None)

        cg_dir = Section(SEC_CGDIR, CGD_ENTRY)
        for name in event_names:                 # 目录顺序必须与 CGNAME 一致
            cg_dir.add(records[name])
        self.log(f"事件图 {event_section.count} 帧 + {patch_section.count} 补丁"
                 f"(差分组 {stats['groups']} 个 / 完全相同 {stats['identical']} 张 /"
                 f" 补丁 {stats['patches']} 张 / 退回整帧 {stats['full']} 张)")

        meta = [
            f"generator={GEN_VERSION}",
            f"source={options.source_label}",
            f"pages={len(pages)}",
            f"lines={len(page_texts)}",
            f"codepoints={len(symbols)}",
            f"backgrounds={bg_section.count}",
            f"sprites={fg_section.count}",
            f"event_names={len(event_names)}",
            f"event_frames={event_section.count}",
            f"event_patches={patch_section.count}",
            f"choices={len(choices)}",
            f"title_bg={title_bg}",
            f"screen={SCREEN_W}x{SCREEN_H}",
            f"art={art_w}x{art_h}",
            f"box_y={sprite_visible_bottom}",
            f"sprite_height={options.sprite_height}",
            f"bg_quality={options.bg_quality}",
            f"sprite_quality={options.sprite_quality}",
            f"event_quality={options.event_quality}",
            f"missing_references={sum(len(v) for v in missing.values())}",
            f"normalized_pages={sum(normalized.values())}",
        ]

        sections = [
            plain_section(SEC_SYM, b"".join(struct.pack("<I", ord(c)) for c in symbols),
                          len(symbols)),
            plain_section(SEC_TEXT, bytes(text_blob), len(text_blob)),
            plain_section(SEC_TOFF, b"".join(struct.pack("<I", o) for o in checkpoints),
                          len(checkpoints)),
            plain_section(SEC_TLEN, bytes(lengths), len(pages)),
            plain_section(SEC_PBG, bytes(pbg), len(pages)),
            plain_section(SEC_PSPK, bytes(pspk), len(pages)),
            plain_section(SEC_PSPR, bytes(pspr), len(pages)),
            plain_section(SEC_PFLAG, bytes(pflag), len(pages)),
            plain_section(SEC_PCGB, bytes(cg_bitmap), len(cg_bitmap)),
            plain_section(SEC_PCG, b"".join(struct.pack("<H", v) for v in cg_pages),
                          len(cg_pages)),
            plain_section(SEC_BGNAME, encode_name_table(bg_names), len(bg_names)),
            plain_section(SEC_SPKNAME, encode_name_table(used["speaker"]),
                          len(used["speaker"])),
            plain_section(SEC_SPRNAME, encode_name_table(sprite_names), len(sprite_names)),
            plain_section(SEC_CGNAME, encode_name_table(event_names), len(event_names)),
            plain_section(SEC_CHOICE, bytes(choice_blob), len(choices)),
            plain_section(SEC_CHOICEOPT, bytes(choice_opt_blob), first_option),
            bg_section, fg_section, event_section, patch_section, cg_dir,
            plain_section(SEC_META, ("\n".join(meta) + "\n").encode("utf-8"), len(meta)),
        ]
        sections.sort(key=lambda s: s.kind)
        report = {
            "pages": len(pages), "symbols": len(symbols),
            "backgrounds": bg_section.count, "sprites": fg_section.count,
            "event_frames": event_section.count, "event_patches": patch_section.count,
            "events": len(event_names), "choices": len(choices),
            "missing": {k: v for k, v in missing.items() if v},
            "normalized": {k: v for k, v in normalized.items() if v},
        }
        return write_pack(sections), report


# --------------------------------------------------------------------------- #
# 校验
# --------------------------------------------------------------------------- #

def unpack_symbols(pack: dict) -> List[str]:
    blob = section_bytes(pack, SEC_SYM)
    return [chr(value) for value in struct.unpack(f"<{len(blob) // 4}I", blob)]


def meta_size(pack: dict, key: str) -> Optional[Tuple[int, int]]:
    """读 META 里的 <key>=<W>x<H>;没有这条或格式不对返回 None。"""
    for line in section_bytes(pack, SEC_META).decode("utf-8", "replace").splitlines():
        name, _, value = line.partition("=")
        if name != key:
            continue
        width, separator, height = value.partition("x")
        if separator and width.isdigit() and height.isdigit():
            return int(width), int(height)
    return None


def verify(pack_path: str, script_dir: Optional[str] = None) -> Dict[str, object]:
    """读回来逐项自检;给了源剧本目录还会逐页比对内容。"""
    with open(pack_path, "rb") as handle:
        pack = parse_pack(handle.read(), pack_path)
    stats: Dict[str, object] = {}

    def array_check(kind: int, stride: int, expected: Optional[int] = None) -> int:
        """纯数组段:段长必须正好是 count × stride。"""
        _, count, size = pack["sections"][kind]
        if count * stride != size:
            raise ValueError(f"{SEC_NAMES[kind]}: {count} × {stride} != {size}")
        if expected is not None and count != expected:
            raise ValueError(f"{SEC_NAMES[kind]}: count {count} != {expected}")
        return count

    def image_check(kind: int, stride: int, expected: Optional[int] = None) -> int:
        """带数据区的段:段长 = 目录 + 数据,只能要求不小于目录长度。"""
        _, count, size = pack["sections"][kind]
        if size < count * stride:
            raise ValueError(f"{SEC_NAMES[kind]}: 段长 {size} 装不下 {count} 个目录项")
        if expected is not None and count != expected:
            raise ValueError(f"{SEC_NAMES[kind]}: count {count} != {expected}")
        return count

    symbols = unpack_symbols(pack)
    # 美术层尺寸由 META 记录:BG / EVB / FG 都在这个坐标系里,固件按比例放大到整屏。
    art_size = meta_size(pack, "art")
    if art_size is None:
        raise ValueError("META 缺 art=<宽>x<高>,无法核对美术层尺寸")
    art_w, art_h = art_size
    if art_w * SCREEN_H != art_h * SCREEN_W:
        raise ValueError(f"美术层 {art_w}×{art_h} 与屏幕 {SCREEN_W}×{SCREEN_H} 长宽比不同")
    stats["art_size"] = art_size
    text = section_bytes(pack, SEC_TEXT)
    checkpoints = struct.unpack(f"<{len(section_bytes(pack, SEC_TOFF)) // 4}I",
                                section_bytes(pack, SEC_TOFF))
    lengths = section_bytes(pack, SEC_TLEN)
    pages = len(lengths)
    stats["pages"], stats["codepoints"] = pages, len(symbols)
    if len(checkpoints) != checkpoint_count(pages):
        raise ValueError("TOFF 检查点数量不对")
    array_check(SEC_SYM, 4)
    for kind, stride, expected in ((SEC_TLEN, 1, pages), (SEC_PBG, 1, pages),
                                   (SEC_PSPK, 1, pages), (SEC_PSPR, 1, pages),
                                   (SEC_PFLAG, 1, pages), (SEC_PCG, 2, None)):
        array_check(kind, stride, expected)
    array_check(SEC_PCGB, 1, (pages + 7) // 8)
    array_check(SEC_CHOICE, CH_ENTRY)
    array_check(SEC_CHOICEOPT, CHOPT_ENTRY)

    bg_names = decode_name_table(section_bytes(pack, SEC_BGNAME))
    sprite_names = decode_name_table(section_bytes(pack, SEC_SPRNAME))
    speaker_names = decode_name_table(section_bytes(pack, SEC_SPKNAME))
    event_names = decode_name_table(section_bytes(pack, SEC_CGNAME))
    stats["backgrounds"], stats["sprites"] = len(bg_names), len(sprite_names)
    stats["event_names"], stats["speakers"] = len(event_names), len(speaker_names)
    # 名表的 count 字段也要与 \n 分出来的条数一致,否则固件按下标取值会错位。
    for kind, names in ((SEC_BGNAME, bg_names), (SEC_SPKNAME, speaker_names),
                        (SEC_SPRNAME, sprite_names), (SEC_CGNAME, event_names)):
        if pack["sections"][kind][1] != len(names):
            raise ValueError(f"{SEC_NAMES[kind]}: count {pack['sections'][kind][1]} "
                             f"!= 名表条数 {len(names)}")

    bg_count = image_check(SEC_BG, BG_ENTRY)
    fg_count = image_check(SEC_FG, FG_ENTRY)
    frame_count = image_check(SEC_EVB, EV_ENTRY)
    patch_count = image_check(SEC_EVC, EV_ENTRY)
    cg_count = array_check(SEC_CGDIR, CGD_ENTRY)
    stats["event_frames"], stats["event_patches"] = frame_count, patch_count
    if bg_count != len(bg_names):
        raise ValueError("BG 与 BGNAME 数量不一致")
    if fg_count != len(sprite_names):
        raise ValueError("FG 与 SPRNAME 数量不一致")
    if cg_count != len(event_names):
        raise ValueError("CGDIR 与 CGNAME 数量不一致")

    # 整帧类目录:数据必须落在数据区内且是 JPEG
    for kind in (SEC_BG, SEC_EVB, SEC_EVC):
        stride = EV_ENTRY if kind != SEC_BG else BG_ENTRY
        count = pack["sections"][kind][1]
        data_start, data_size = blob_area(pack, kind, stride)
        for index in range(count):
            entry = directory_entry(pack, kind, stride, index)
            offset, length, width, height = struct.unpack("<IIHH", entry)
            if offset + length > data_size:
                raise ValueError(f"{SEC_NAMES[kind]}[{index}] 数据越界")
            if width == 0 or height == 0 or width > SCREEN_W or height > SCREEN_H:
                raise ValueError(f"{SEC_NAMES[kind]}[{index}] 尺寸 {width}x{height} 非法")
            if kind == SEC_BG and (width, height) != (art_w, art_h):
                raise ValueError(f"BG[{index}] 必须是美术层尺寸 {art_w}×{art_h}")
            if kind != SEC_EVC and width * art_h != height * art_w:
                raise ValueError(f"{SEC_NAMES[kind]}[{index}] 尺寸 {width}x{height} "
                                 f"与美术层 {art_w}×{art_h} 长宽比不同,固件放大后会变形")
            if pack["blob"][data_start + offset] != 0xFF or \
                    pack["blob"][data_start + offset + 1] != 0xD8:
                raise ValueError(f"{SEC_NAMES[kind]}[{index}] 不是 JPEG")

    # 事件整帧必须与美术层同长宽比(固件放大),且同一包内尺寸统一
    frame_sizes = {struct.unpack("<HH", directory_entry(pack, SEC_EVB, EV_ENTRY, index)[8:12])
                   for index in range(frame_count)}
    if len(frame_sizes) > 1:
        raise ValueError(f"EVB 里出现了多种帧尺寸: {sorted(frame_sizes)}")
    stats["event_frame_size"] = next(iter(frame_sizes)) if frame_sizes else (0, 0)

    fg_data_start, fg_data_size = blob_area(pack, SEC_FG, FG_ENTRY)
    for index in range(fg_count):
        entry = directory_entry(pack, SEC_FG, FG_ENTRY, index)
        body_offset, body_len, mask_offset, mask_len, width, height, x, y = \
            struct.unpack("<IIIIHHHH", entry)
        if body_offset + body_len > fg_data_size or mask_offset + mask_len > fg_data_size:
            raise ValueError(f"FG[{index}] 数据越界")
        if mask_len != ((width + 7) // 8) * height:
            raise ValueError(f"FG[{index}] 遮罩长度 {mask_len} != stride×{height}")
        if x + width > art_w or y + height > art_h:
            raise ValueError(f"FG[{index}] 超出美术层 {art_w}×{art_h}")
        if pack["blob"][fg_data_start + body_offset] != 0xFF or \
                pack["blob"][fg_data_start + body_offset + 1] != 0xD8:
            raise ValueError(f"FG[{index}] 不是 JPEG")

    # 事件渲染配方
    for index in range(cg_count):
        entry = directory_entry(pack, SEC_CGDIR, CGD_ENTRY, index)
        kind, _, _, base, x, y, width, height, image = struct.unpack("<BBHIHHHHI", entry)
        if kind == CG_KIND_FRAME:
            if image >= frame_count:
                raise ValueError(f"CGDIR[{index}] 引用了不存在的整帧 {image}")
        elif kind == CG_KIND_PATCH:
            if base == index or base >= cg_count:
                raise ValueError(f"CGDIR[{index}] 的基准下标 {base} 非法")
            base_entry = directory_entry(pack, SEC_CGDIR, CGD_ENTRY, base)
            if base_entry[0] != CG_KIND_FRAME:
                raise ValueError(f"CGDIR[{index}] 的基准不是整帧")
            if image >= patch_count:
                raise ValueError(f"CGDIR[{index}] 引用了不存在的补丁 {image}")
            if width == 0 or height == 0:
                raise ValueError(f"CGDIR[{index}] 补丁矩形为空")
            # 补丁贴在基准帧上,坐标系是基准帧自己的尺寸(即美术层尺寸)。
            base_frame = struct.unpack_from("<I", base_entry, 16)[0]
            frame_entry = directory_entry(pack, SEC_EVB, EV_ENTRY, base_frame)
            frame_w, frame_h = struct.unpack_from("<HH", frame_entry, 8)
            if x + width > frame_w or y + height > frame_h:
                raise ValueError(f"CGDIR[{index}] 补丁矩形 {x},{y} {width}x{height} "
                                 f"超出基准帧 {frame_w}x{frame_h}")
        else:
            raise ValueError(f"CGDIR[{index}] 未知 kind {kind}")

    # 页表下标与位图条数
    pbg, pspk, pspr = (section_bytes(pack, kind)
                       for kind in (SEC_PBG, SEC_PSPK, SEC_PSPR))
    cg_bitmap = section_bytes(pack, SEC_PCGB)
    cg_order = struct.unpack(f"<{len(section_bytes(pack, SEC_PCG)) // 2}H",
                             section_bytes(pack, SEC_PCG))
    cursor = 0
    for page in range(pages):
        if pbg[page] >= bg_count:
            raise ValueError(f"第 {page} 页背景下标越界")
        if pspk[page] != NONE8 and pspk[page] >= len(speaker_names):
            raise ValueError(f"第 {page} 页说话人下标越界")
        if pspr[page] != NONE8 and pspr[page] >= fg_count:
            raise ValueError(f"第 {page} 页立绘下标越界")
        if cg_bitmap[page >> 3] & (1 << (page & 7)):
            if cursor >= len(cg_order) or cg_order[cursor] >= cg_count:
                raise ValueError(f"第 {page} 页事件图下标越界")
            cursor += 1
    if cursor != len(cg_order):
        raise ValueError("有事件图的页数与 PCG 条数不一致")

    decoded = 0
    for page in range(pages):
        count = lengths[page]
        if count:
            try:
                offset = text_byte_offset(checkpoints, lengths, text, page)
                _, end = decode_text(text, offset, count, symbols)
            except (IndexError, struct.error) as error:
                raise ValueError(f"第 {page} 页正文越界: {error}") from error
            if end > len(text):
                raise ValueError(f"第 {page} 页正文越界")
            decoded += count
    stats["text_chars"], stats["text_bytes"] = decoded, len(text)

    if script_dir:
        verified, normalized = compare_with_source(pack, symbols, checkpoints,
                                                   lengths, text, script_dir)
        stats["verified_pages"] = verified
        for key, count in sorted(normalized.items()):
            stats[f"normalized_{key}"] = count
    return stats


def normalize_source_page(page: dict, known: Dict[str, Sequence[str]]) -> Tuple[dict, Optional[str]]:
    """把源页里那两类上游缺陷归一成"字段不存在",并返回归一了哪个字段。

    包格式用"下标 = 0xFF"表示没有这张图,没有地方存"字段在但值是空串"。
    所以这两类必须在两边用同一套规则归一,否则逐页比对得不到 0 差异:
      - cg 是空串(2049 页)
      - 立绘名在源仓里不存在(650 页,例如只剩 _1/_2/_3 前缀丢了)
    空串的 s / t 同理。归一数量会打印出来,不静默吞掉。
    """
    out = dict(page)
    touched: List[str] = []
    for key in ("s", "c", "cg"):
        value = out.get(key)
        if key in out and (not value or value not in known[key]):
            out.pop(key)
            touched.append(key)
    if not out.get("t"):
        if "t" in out:
            out.pop("t")
            touched.append("t")
    return out, ("+".join(touched) if touched else None)


def compare_with_source(pack: dict, symbols, checkpoints, lengths, text,
                        script_dir: str) -> Tuple[int, Dict[str, int]]:
    """逐页比对剧本:字段、正文、资源归属必须与源 JSON 一致(允许上游缺陷归一)。"""
    pages = load_pages(script_dir)
    if len(pages) != len(lengths):
        raise ValueError(f"页数不一致:源 {len(pages)} / 包 {len(lengths)}")
    bg_names = decode_name_table(section_bytes(pack, SEC_BGNAME))
    sprite_names = decode_name_table(section_bytes(pack, SEC_SPRNAME))
    speaker_names = decode_name_table(section_bytes(pack, SEC_SPKNAME))
    event_names = decode_name_table(section_bytes(pack, SEC_CGNAME))
    known = {"b": set(bg_names), "s": set(speaker_names),
             "c": set(sprite_names), "cg": set(event_names)}
    pbg, pspk, pspr, pflag = (section_bytes(pack, kind) for kind in
                              (SEC_PBG, SEC_PSPK, SEC_PSPR, SEC_PFLAG))
    cg_bitmap = section_bytes(pack, SEC_PCGB)
    cg_order = struct.unpack(f"<{len(section_bytes(pack, SEC_PCG)) // 2}H",
                             section_bytes(pack, SEC_PCG))

    choice_offset, choice_count, _ = pack["sections"][SEC_CHOICE]
    opt_offset = pack["sections"][SEC_CHOICEOPT][0]
    choices: Dict[int, List[Tuple[str, int]]] = {}
    for index in range(choice_count):
        page, count, _, _, first = struct.unpack_from("<IBBHI", pack["blob"],
                                                      choice_offset + index * CH_ENTRY)
        options = []
        for slot in range(count):
            off, length, _, _, target = struct.unpack_from(
                "<IBBHI", pack["blob"], opt_offset + (first + slot) * CHOPT_ENTRY)
            options.append((decode_text(text, off, length, symbols)[0], target))
        choices[page] = options

    cursor = 0
    mismatch: List[str] = []
    normalized: Dict[str, int] = {}
    for index, (filename, number, page) in enumerate(pages):
        expected, touched = normalize_source_page(page, known)
        if touched:
            normalized[touched] = normalized.get(touched, 0) + 1
        got: Dict[str, object] = {"b": bg_names[pbg[index]]}
        if pspk[index] != NONE8:
            got["s"] = speaker_names[pspk[index]]
        if pspr[index] != NONE8:
            got["c"] = sprite_names[pspr[index]]
        if cg_bitmap[index >> 3] & (1 << (index & 7)):
            got["cg"] = event_names[cg_order[cursor]]
            cursor += 1
        if pflag[index] & PAGE_FLAG_ZOOM:
            got["z"] = ZOOM_VALUE
        if lengths[index]:
            offset = text_byte_offset(checkpoints, lengths, text, index)
            got["t"] = decode_text(text, offset, lengths[index], symbols)[0]
        if index in choices:
            got["co"] = True
            for slot, (value, target) in enumerate(choices[index], 1):
                got[f"c{slot}"] = value
                got[f"c{slot}t"] = target
        if got != expected:
            mismatch.append(f"{filename}:{number}")
    if mismatch:
        raise ValueError(f"{len(mismatch)} 页与源不一致,例如 {mismatch[:5]}")
    return len(pages), normalized


# --------------------------------------------------------------------------- #
# 命令行
# --------------------------------------------------------------------------- #

def font_inventory(symbols: Sequence[str], speaker_names: Sequence[str]) -> List[str]:
    """字体子集要覆盖的码位 = 正文/选项的符号表 ∪ 要显示的名字 ∪ ASCII 可打印区。

    符号表只包含**正文与选项**的字符,但界面上还会显示:
      - 说话人名(带【】与日语字形,例如 風実花);
      - 章节标签的编号与连字符(源文里可能一次也没出现,例如 5、9、-);
      - 菜单/提示等 UI 自己的串(由 tools/tsxx_ui_font_check.py 核对)。
    ASCII 可打印区只有 95 个字形(16px 约 12 KB),一次补上比逐个碰运气便宜。
    """
    out: List[str] = []
    seen = set()
    for source in (symbols, [ch for name in speaker_names for ch in name],
                   [chr(cp) for cp in range(0x20, 0x7F)]):
        for ch in source:
            if ch not in seen:
                seen.add(ch)
                out.append(ch)
    return out


def sprite_group(name: str) -> str:
    """立绘差分归组:fsh01_1/2/3 是同一个角色的同一件衣服、只是姿势不同。

    源工程的立绘命名是 <角色><编号>_<差分号>,所以去掉末尾的 _N 就是一组。
    """
    stem = name
    for suffix in IMAGE_SUFFIXES:
        if stem.lower().endswith(suffix):
            stem = stem[: -len(suffix)]
            break
    match = re.match(r"^(.*)_\d+$", stem)
    return match.group(1) if match else stem


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="打包《天使☆騒々 RE-BOOT!》手环移植版素材。",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    source = parser.add_argument_group("输入")
    source.add_argument("--source", help="源 checkout 根目录(含 src/common)")
    source.add_argument("--script-dir", help="scriptData*.txt 目录")
    source.add_argument("--bg-dir", help="背景目录(源 bcgi)")
    source.add_argument("--cimg-dir", help="立绘目录(源 cimg)")
    source.add_argument("--evig-dir", help="事件图目录(源 evig)")
    source.add_argument("--source-label", default=DEFAULT_SOURCE_LABEL,
                        help="写进 META 的来源描述")

    target = parser.add_argument_group("输出")
    target.add_argument("--title-image", default="",
                        help="标题背景图(任意尺寸,按美术层尺寸 cover 裁切后追加到背景表末尾)")
    target.add_argument("--out", help="资源包输出路径")
    target.add_argument("--symbols-out", help="码位清单输出路径(给字体子集工具)")
    target.add_argument("--report", action="store_true", help="打印分段体积")
    target.add_argument("--max-bytes", type=int, default=0,
                        help="超过这个字节数就让构建失败(0 = 不限制)")
    target.add_argument("--verify", metavar="PACK", help="只校验已有资源包")
    target.add_argument("--verify-source", action="store_true",
                        help="校验时逐页比对源剧本")

    layout = parser.add_argument_group("版面")
    layout.add_argument("--art-width", type=int, default=ART_W,
                        help="美术层宽度;高度按屏幕长宽比推导(固件放大到整屏)")
    layout.add_argument("--box-y", type=int, default=DEFAULT_BOX_Y,
                        help="文本框顶边(= 画面区底边),美术层坐标")
    layout.add_argument("--sprite-pose-limit", type=int, default=1,
                        help="每组立绘最多保留几张姿态差分(0 = 全保留,默认 1)")
    layout.add_argument("--sprite-height", type=int, default=DEFAULT_SPRITE_HEIGHT)
    layout.add_argument("--sprite-bottom", type=int, default=DEFAULT_SPRITE_BOTTOM)
    layout.add_argument("--bg-quality", type=int, default=72)
    layout.add_argument("--sprite-quality", type=int, default=72)
    layout.add_argument("--event-quality", type=int, default=72)
    layout.add_argument("--patch-tolerance", type=int, default=DEFAULT_PATCH_TOLERANCE)
    layout.add_argument("--patch-max-pixels", type=int, default=0,
                        help="补丁允许的最大像素数(0 = 取最大立绘的面积)")
    return parser.parse_args(argv)


def common_root(source: str) -> str:
    """源 checkout 可以是完整手环工程,也可以只放 script/bcgi/cimg/evig 四个目录。"""
    nested = os.path.join(source, "src", "common")
    return nested if os.path.isdir(nested) else source


def fill_source_dirs(options: argparse.Namespace, only_script: bool = False) -> None:
    """

    only_script = True 时只解析 --script-dir(校验模式只需要剧本)。
    """
    common = common_root(options.source) if options.source else None
    for attr, sub in (("script_dir", "script"), ("bg_dir", "bcgi"),
                      ("cimg_dir", "cimg"), ("evig_dir", "evig")):
        if only_script and attr != "script_dir":
            continue
        if not getattr(options, attr):
            if not common:
                raise SystemExit("需要 --source,或分别给出 --script-dir/--bg-dir/"
                                 "--cimg-dir/--evig-dir")
            setattr(options, attr, os.path.join(common, sub))
        if not os.path.isdir(getattr(options, attr)):
            raise SystemExit(f"{getattr(options, attr)} 不是目录")


def report_sections(blob: bytes) -> None:
    pack = parse_pack(blob)
    for kind in range(SECTION_COUNT):
        _, count, size = pack["sections"][kind]
        print(f"  {SEC_NAMES[kind]:<9} {size:>10,} B   count={count:,}")


def main(argv: Optional[Sequence[str]] = None) -> int:
    options = parse_args(argv)

    if options.verify:
        script_dir = None
        if options.verify_source:
            fill_source_dirs(options, only_script=True)
            script_dir = options.script_dir
        stats = verify(options.verify, script_dir)
        print(f"校验通过: {options.verify}")
        for key in sorted(stats):
            print(f"  {key} = {stats[key]}")
        return 0

    if not options.out:
        raise SystemExit("需要 --out,或改用 --verify PACK")
    require_pillow()
    fill_source_dirs(options)

    blob, report = PackBuilder(options).build()
    with open(options.out, "wb") as handle:
        handle.write(blob)

    if options.symbols_out:
        symbols = font_inventory(unpack_symbols(parse_pack(blob)),
                                 decode_name_table(section_bytes(parse_pack(blob),
                                                                 SEC_SPKNAME)))
        with open(options.symbols_out, "w", encoding="utf-8") as handle:
            handle.write("".join(symbols))
        print(f"字体码位清单 {len(symbols)} 个 → {options.symbols_out}")

    print(f"资源包 {len(blob):,} B ({len(blob) / 1048576:.2f} MiB) → {options.out}")
    if options.report:
        report_sections(blob)
    verify(options.out, options.script_dir)
    print(f"自检通过: 逐页比对 {report['pages']} 页无差异")

    if options.max_bytes and len(blob) > options.max_bytes:
        raise SystemExit(f"资源包 {len(blob)} B 超过上限 {options.max_bytes} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
