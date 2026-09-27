#!/usr/bin/env python3
"""Build the DRACU-RIOT image pack for the AI Passport port.

Source project: https://github.com/hezdaaa/dracu-riot-miband
  - 小米手环快应用上的《DRACU-RIOT!》(Yuzusoft)移植;素材版权归 Yuzusoft 所有。
    本仓库只保存转换产物 (main/dracu_data/),源素材用 tools/dracu_fetch_source.py 拉取。

The pack is one little-endian binary read straight out of flash.  Everything is
already at the device's native geometry, so the firmware never rescales.

  header   : magic "DRACUPK1", version u16, header_size u16, section_count u16,
             reserved u16, total_size u32                          (20 B)
  section  : { type u32, offset u32, count u32, size u32 }         (16 B × N)
    SEC_NAME   UTF-8 名字,NUL 连接(条目按 (pool, name) 升序,可人工核对)
    SEC_ASSET  固定 36 字节条目(见下)
    SEC_BLOB   载荷区(条目里的 data_off 相对本段)
    SEC_SPRITE 立绘合成表:u32 count + count × 8 字节
               { body u16, face u16, facex i16, facey i16 }
               body/face 是条目下标(0xFFFF = 没有);facex/facey 是表情层相对
               身体图左上角的偏移(设备像素)
    SEC_META   转换参数与来源信息

  entry : { name_off u32, name_len u16, kind u8, pool u8, base u16,
            w u16, h u16, dx u16, dy u16, dw u16, dh u16,
            data_off u32, data_len u32, flags u32, reserved u16 }  (36 B)
  kind  : 0=BG JPEG | 1=CG JPEG | 2=CG_DIFF 掩码补丁 | 3=SD | 4=BODY | 5=FACE
          | 6=MISSING 占位
  pool  : 0=bg 1=cg 2=sd 3=body 4=face(同一个 pool 内按名字升序 = 剧本里的 id)
  flags : bit0 = alias(没有载荷,画 base 指向的条目)

  掩码补丁载荷(kind 2;矩形在 dx/dy/dw/dh):
    u32 掩码长度(压缩), u32 块数, u32 每块压缩长度[块数], deflate(1bpp 掩码),
    逐块 deflate(RGB565 小端,按扫描序,每块 ≤1024 像素)
  固件先解基准 CG,再按掩码逐行覆写 —— 一行临时缓冲即可,无损于缩放后的源图。

  分块调色板载荷(kind 3/4/5;不是 PNG):
    u16 宽, u16 高, u16 调色板数, u16 每块行数(打包用 24), u16 块数, u16 保留,
    u32 每块解压字节(= 每块行数 x (宽 + 1)),
    u16 调色板[RGB565 小端], u8 alpha[每色一项], u32 每块压缩长度[块数], 逐块 zlib。
    每块解压成若干行,每行 = 1 字节 PNG 滤波类型 + 宽度个索引;上一行跨块沿用。
    固件用一块 24 x (240 + 1) = 5.8 KB 的静态行缓冲解块,不需要 32 KB inflate 字典。

Conversion rules:

  背景 85 张 336x480 JPEG  -> cover 到 240x320(裁下方:那一块压在正文带下),
                              质量由「按像素数折算的源体积」标定
  标题图 title_bg.jpg       -> 同上,质量固定高一些,占背景 id 0
  事件 CG 244 张 336x480    -> 同组(ev217aa/ab/..)里引用最多的一张存整张 JPEG,
                              其余先算变化掩码,补丁比整张小就用补丁(无损)
  SD 195 张 336x480 PNG    -> 裁到内容再缩到 0.45 倍,分块调色板载荷 + 放置位置
  立绘 115 张身体 + N 张表情 -> 身体缩到 0.55 倍并裁掉屏幕外的下半身,表情缩到
                              同样比例后裁到 alpha 包围盒;两者都是分块调色板载荷,
                              设备侧先画身体再叠表情(合成表在 SEC_SPRITE)

Usage:
  python tools/dracu_fetch_source.py --dest build/dracu-source
  python tools/dracu_pack.py --source build/dracu-source --out build/dracu-pack/dracu_pack.bin
  python tools/dracu_pack.py --check build/dracu-pack/dracu_pack.bin
"""

from __future__ import annotations

import argparse
import io
import json
import re
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dracu_source as S  # noqa: E402

MAGIC = b"DRACUPK1"
VERSION = 1
HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
ENTRY = struct.Struct("<IHBBHHHHHHHIIIH")
assert ENTRY.size == 36
SPRITE_ENTRY = struct.Struct("<HHhh")   # body u16, face u16, facex i16, facey i16
assert SPRITE_ENTRY.size == 8

SEC_NAME, SEC_ASSET, SEC_BLOB, SEC_SPRITE, SEC_META = range(5)

# 分块调色板载荷
BLOCK_ROWS = 24
MAX_COLORS = 256
BLOCK_HEADER = struct.Struct("<6HI")
assert BLOCK_HEADER.size == 16
FILTER_NONE, FILTER_SUB, FILTER_UP, FILTER_AVERAGE, FILTER_PAETH = range(5)

# 条目类型 / 名字空间
KIND_BG, KIND_CG, KIND_CG_DIFF, KIND_SD, KIND_BODY, KIND_FACE, KIND_MISSING = range(7)
KIND_NAMES = {
    KIND_BG: "BG", KIND_CG: "CG", KIND_CG_DIFF: "CG_DIFF", KIND_SD: "SD",
    KIND_BODY: "BODY", KIND_FACE: "FACE", KIND_MISSING: "MISSING",
}
POOL_BG, POOL_CG, POOL_SD, POOL_BODY, POOL_FACE = range(5)
POOL_NAMES = {POOL_BG: "bg", POOL_CG: "cg", POOL_SD: "sd", POOL_BODY: "body", POOL_FACE: "face"}
FLAG_ALIAS = 1 << 0

# 标题图在背景池里的条目名(剧本包与固件用同一个名字;id 不固定 —— 它是池里按名字
# 排序后的位置,固件在启动时按名字查一次)。
TITLE_NAME = "标题画面"

# CG 变体分组:ev217aa / ev217ab / big103ca 之类去掉末尾 1-2 个字母
CG_GROUP_RE = re.compile(r"^(.*?)([a-z]{1,2})$")

# 补丁像素块的像素数(与 main/dracu_image.c 的 DRACU_PATCH_BLOCK_PIXELS 一致)
PATCH_BLOCK_PIXELS = 1024


def log(message: str) -> None:
    print(message, file=sys.stderr)


def human(count: int) -> str:
    if abs(count) >= 1048576:
        return f"{count / 1048576:.2f} MB"
    return f"{count / 1024:.1f} KB"


# --------------------------------------------------------------------------
# 图像工具(与 main/dracu_image.c 的解码路径一一对应)
# --------------------------------------------------------------------------


def cover(image: Image.Image, width: int, height: int, bias: str = "top") -> Image.Image:
    """等比缩放到铺满 width x height,再按 bias 裁掉多余的一边。

    源素材 336x480(比例 0.70)、屏幕 240x320(0.75):铺满后高度多出约 23 px。
    默认裁下方 —— 那块本来就压在正文带底下,保住上方构图更重要。
    """
    scale = width / image.width
    resized = image.resize((width, max(height, round(image.height * scale))), Image.LANCZOS)
    if resized.height == height:
        return resized
    extra = resized.height - height
    top = extra if bias == "bottom" else (extra // 2 if bias == "center" else 0)
    return resized.crop((0, top, width, top + height))


def content_box(image: Image.Image) -> tuple[int, int, int, int]:
    """内容包围盒(alpha > 8);整幅透明时返回 None。"""
    if image.mode != "RGBA":
        image = image.convert("RGBA")
    alpha = image.getchannel("A")
    return alpha.point(lambda value: 255 if value > 8 else 0).getbbox()


def quantize_blocked(image: Image.Image, colors: int):
    converted = image if image.mode == "RGBA" else image.convert("RGBA")
    quantized = converted.quantize(colors=colors, method=Image.FASTOCTREE)
    raw = quantized.palette.palette
    if quantized.palette.mode == "RGBA":
        entries = len(raw) // 4
        table = np.frombuffer(raw, dtype=np.uint8, count=entries * 4).reshape(entries, 4)
        rgb, alpha = table[:, :3].copy(), table[:, 3].copy()
    else:
        entries = len(raw) // 3
        rgb = np.frombuffer(raw, dtype=np.uint8, count=entries * 3).reshape(entries, 3).copy()
        alpha = np.full(entries, 255, dtype=np.uint8)
        transparency = quantized.info.get("transparency")
        if isinstance(transparency, (bytes, bytearray)):
            values = np.frombuffer(bytes(transparency), dtype=np.uint8)
            alpha[:min(entries, values.size)] = values[:min(entries, values.size)]
        elif isinstance(transparency, int) and 0 <= transparency < entries:
            alpha[transparency] = 0
    used = sorted(index for _, index in quantized.getcolors(maxcolors=MAX_COLORS))
    lookup = np.zeros(256, dtype=np.uint8)
    for new, old in enumerate(used):
        lookup[old] = new
    width, height = image.size
    indices = np.frombuffer(quantized.tobytes(), dtype=np.uint8, count=width * height).reshape(height, width)
    indices = lookup[indices]
    rgb, alpha = rgb[used], alpha[used]
    packed = (((rgb[:, 0].astype(np.uint16) >> 3) << 11)
              | ((rgb[:, 1].astype(np.uint16) >> 2) << 5)
              | (rgb[:, 2].astype(np.uint16) >> 3))
    return width, height, indices, packed.astype("<u2").tobytes(), alpha.tobytes()


def _paeth_rows(left, up, upleft):
    a, b, c = left.astype(np.int16), up.astype(np.int16), upleft.astype(np.int16)
    estimate = a + b - c
    distance_left, distance_up = np.abs(estimate - a), np.abs(estimate - b)
    distance_upleft = np.abs(estimate - c)
    predictor = np.where((distance_left <= distance_up) & (distance_left <= distance_upleft), a,
                         np.where(distance_up <= distance_upleft, b, c))
    return predictor.astype(np.uint8)


def _filter_cost(row) -> int:
    return int(np.abs(row.view(np.int8).astype(np.int16)).sum())


def filter_rows(rows) -> bytes:
    """逐行选最优 PNG 滤波器(0..4);上一行跨块保留,所以分块解压结果一致。"""
    width = rows.shape[1]
    previous = np.zeros(width, dtype=np.uint8)
    stream = bytearray()
    for row in rows:
        left = np.empty(width, dtype=np.uint8)
        left[0] = 0
        left[1:] = row[:-1]
        upleft = np.empty(width, dtype=np.uint8)
        upleft[0] = 0
        upleft[1:] = previous[:-1]
        average = ((left.astype(np.uint16) + previous) // 2).astype(np.uint8)
        candidates = (
            (FILTER_NONE, row),
            (FILTER_SUB, (row - left) & 0xFF),
            (FILTER_UP, (row - previous) & 0xFF),
            (FILTER_AVERAGE, (row - average) & 0xFF),
            (FILTER_PAETH, (row - _paeth_rows(left, previous, upleft)) & 0xFF),
        )
        filter_type, best = min(candidates, key=lambda candidate: _filter_cost(candidate[1]))
        stream.append(filter_type)
        stream += best.tobytes()
        previous = row
    return bytes(stream)


def encode_blocked(image: Image.Image, colors: int, block_rows: int = BLOCK_ROWS) -> bytes:
    width, height, indices, palette, alpha = quantize_blocked(image, colors)
    block_count = (height + block_rows - 1) // block_rows
    raw_block_bytes = block_rows * (width + 1)
    stream = filter_rows(indices)
    lengths: list[int] = []
    blocks: list[bytes] = []
    for index in range(block_count):
        compressed = zlib.compress(stream[index * raw_block_bytes:(index + 1) * raw_block_bytes], 9)
        blocks.append(compressed)
        lengths.append(len(compressed))
    header = BLOCK_HEADER.pack(width, height, len(palette) // 2, block_rows, block_count, 0,
                               raw_block_bytes)
    table = b"".join(struct.pack("<I", length) for length in lengths)
    return header + palette + alpha + table + b"".join(blocks)


def unfilter_row(filter_type: int, row, previous):
    """还原一行索引字节(PNG 滤波器 0..4);previous 是上一行(首行全 0)。"""
    width = row.size
    if filter_type == FILTER_NONE:
        return row.copy()
    if filter_type == FILTER_SUB:
        return (np.cumsum(row.astype(np.uint32)) & 0xFF).astype(np.uint8)
    if filter_type == FILTER_UP:
        return ((row.astype(np.uint16) + previous) & 0xFF).astype(np.uint8)
    out = np.empty(width, dtype=np.uint8)
    left = 0
    for index in range(width):
        up = int(previous[index])
        if filter_type == FILTER_AVERAGE:
            predictor = (left + up) >> 1
        elif filter_type == FILTER_PAETH:
            upleft = int(previous[index - 1]) if index else 0
            estimate = left + up - upleft
            distance_left = abs(estimate - left)
            distance_up = abs(estimate - up)
            distance_upleft = abs(estimate - upleft)
            if distance_left <= distance_up and distance_left <= distance_upleft:
                predictor = left
            elif distance_up <= distance_upleft:
                predictor = up
            else:
                predictor = upleft
        else:
            raise ValueError(f"未知滤波器 {filter_type}")
        left = (int(row[index]) + predictor) & 0xFF
        out[index] = left
    return out


def check_blocked(name: str, payload: bytes, entry: dict, errors: list[str]) -> None:
    """自检一个分块调色板载荷(固件侧解码前的全部检查都在这里)。"""
    if len(payload) < BLOCK_HEADER.size:
        errors.append(f"{name}: 分块载荷太短({len(payload)} 字节)")
        return
    width, height, palette_count, block_rows, block_count, _reserved, raw_block_bytes =         BLOCK_HEADER.unpack_from(payload)
    if entry is not None and (width != entry.get("w") or height != entry.get("h")):
        errors.append(f"{name}: 分块尺寸 {width}x{height} != 条目 {entry.get('w')}x{entry.get('h')}")
    if width == 0 or height == 0 or palette_count == 0 or palette_count > MAX_COLORS:
        errors.append(f"{name}: 头部字段不合法({width}x{height}, {palette_count} 色)")
        return
    if block_rows == 0 or raw_block_bytes != block_rows * (width + 1):
        errors.append(f"{name}: raw_block_bytes 与 block_rows/width 不符")
        return
    expected = (height + block_rows - 1) // block_rows
    if block_count != expected:
        errors.append(f"{name}: block_count {block_count} != {expected}")
        return
    table = BLOCK_HEADER.size + palette_count * 3 + block_count * 4
    if table > len(payload):
        errors.append(f"{name}: 块表越界")
        return
    lengths = np.frombuffer(payload, dtype="<u4", count=block_count,
                            offset=BLOCK_HEADER.size + palette_count * 3)
    if table + int(lengths.sum()) != len(payload):
        errors.append(f"{name}: 块流总长 {int(lengths.sum())} 与载荷余量 {len(payload) - table} 不符")
        return
    position = table
    rows = 0
    for index, length in enumerate(lengths):
        try:
            block = zlib.decompress(payload[position:position + int(length)])
        except zlib.error as exc:
            errors.append(f"{name}: 第 {index} 块解压失败({exc})")
            return
        position += int(length)
        if len(block) > raw_block_bytes or len(block) % (width + 1) != 0:
            errors.append(f"{name}: 第 {index} 块解压后 {len(block)} 字节不合法")
            return
        rows += len(block) // (width + 1)
    if rows != height:
        errors.append(f"{name}: 解压总行数 {rows} != 高度 {height}")


def decode_blocked(payload: bytes) -> Image.Image:
    """按 pack 自身的信息还原(核验与固件同一条解码路径)。"""
    width, height, palette_count, _rows, block_count, _reserved, _raw =         BLOCK_HEADER.unpack_from(payload)
    position = BLOCK_HEADER.size
    palette = np.frombuffer(payload, dtype="<u2", count=palette_count, offset=position).astype(np.uint32)
    position += palette_count * 2
    alpha = np.frombuffer(payload, dtype=np.uint8, count=palette_count, offset=position)
    position += palette_count
    lengths = np.frombuffer(payload, dtype="<u4", count=block_count, offset=position)
    position += block_count * 4
    row_bytes = width + 1
    rows = []
    previous = np.zeros(width, dtype=np.uint8)
    for length in lengths:
        block = zlib.decompress(payload[position:position + int(length)])
        position += int(length)
        for offset in range(0, len(block), row_bytes):
            current = np.frombuffer(block, dtype=np.uint8, count=width, offset=offset + 1)
            previous = unfilter_row(block[offset], current, previous)
            rows.append(previous.copy())
    rgb = np.empty((palette_count, 3), dtype=np.uint8)
    rgb[:, 0] = ((palette >> 11) & 31).astype(np.uint8) << 3
    rgb[:, 1] = ((palette >> 5) & 63).astype(np.uint8) << 2
    rgb[:, 2] = (palette & 31).astype(np.uint8) << 3
    indices = np.stack(rows[:height])
    rgba = np.empty((height, width, 4), dtype=np.uint8)
    rgba[:, :, :3] = rgb[indices]
    rgba[:, :, 3] = alpha[indices]
    return Image.fromarray(rgba, "RGBA")


def jpeg_bytes(image: Image.Image, quality: int) -> bytes:
    buffer = io.BytesIO()
    image.convert("RGB").save(buffer, "JPEG", quality=quality, optimize=True, subsampling=2)
    return buffer.getvalue()


def matched_quality(image: Image.Image, target_bytes: int, low: int = 40, high: int = 92) -> int:
    """找与目标体积相当的 JPEG 质量:目标体积按像素数折算源文件,保住观感。"""
    if len(jpeg_bytes(image, high)) <= target_bytes:
        return high
    if len(jpeg_bytes(image, low)) >= target_bytes:
        return low
    lo, hi = low, high
    for _ in range(7):
        mid = (lo + hi) // 2
        if len(jpeg_bytes(image, mid)) > target_bytes:
            hi = mid
        else:
            lo = mid
    return lo


def flatten(image: Image.Image) -> Image.Image:
    """把带透明通道的图合成到中灰底上:屏幕上的观感误差要按合成后比。"""
    rgba = image if image.mode == "RGBA" else image.convert("RGBA")
    background = Image.new("RGBA", rgba.size, (128, 128, 128, 255))
    return Image.alpha_composite(background, rgba).convert("RGB")


def mean_abs_error(left: Image.Image, right: Image.Image) -> float:
    a, b = flatten(left), flatten(right)
    if a.size != b.size:
        b = b.resize(a.size, Image.LANCZOS)
    diff = np.abs(np.asarray(a, dtype="int16") - np.asarray(b, dtype="int16"))
    return float(diff.mean())


def to565(image: Image.Image):
    array = np.asarray(image.convert("RGB"), dtype="int32")
    return (((array[:, :, 0] >> 3) << 11) | ((array[:, :, 1] >> 2) << 5)
            | (array[:, :, 2] >> 3)).astype(np.int32)


def change_mask(base565, other565, threshold: int):
    red = np.abs((base565 >> 11) - (other565 >> 11)) * 8
    green = np.abs(((base565 >> 5) & 63) - ((other565 >> 5) & 63)) * 4
    blue = np.abs((base565 & 31) - (other565 & 31)) * 8
    mask = np.maximum(np.maximum(red, green), blue) > threshold
    if not mask.any():
        return None
    rows, cols = np.where(mask)
    x0, x1 = int(cols.min()), int(cols.max())
    y0, y1 = int(rows.min()), int(rows.max())
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1), mask[y0:y1 + 1, x0:x1 + 1], other565[mask]


def encode_patch(rect, patch, pixels) -> bytes:
    packed = np.packbits(patch.reshape(-1).astype(np.uint8)).tobytes()
    mask_blob = zlib.compress(packed, 9)
    raw = pixels.astype("<u2").tobytes()
    step = PATCH_BLOCK_PIXELS * 2
    blocks = [zlib.compress(raw[start:start + step], 9) for start in range(0, len(raw), step)]
    return (struct.pack("<II", len(mask_blob), len(blocks))
            + b"".join(struct.pack("<I", len(block)) for block in blocks)
            + mask_blob + b"".join(blocks))


def apply_patch(base: Image.Image, payload: bytes, rect) -> Image.Image:
    mask_len, block_count = struct.unpack_from("<II", payload, 0)
    lengths = [struct.unpack_from("<I", payload, 8 + 4 * index)[0] for index in range(block_count)]
    offset = 8 + 4 * block_count
    mask_blob = payload[offset:offset + mask_len]
    offset += mask_len
    pieces = []
    for length in lengths:
        pieces.append(zlib.decompress(payload[offset:offset + length]))
        offset += length
    x, y, width, height = rect
    mask = np.unpackbits(np.frombuffer(zlib.decompress(mask_blob), dtype=np.uint8))[:width * height]
    mask = mask.astype(bool).reshape(height, width)
    values = np.frombuffer(b"".join(pieces), dtype="<u2").astype(np.int32)
    assert int(mask.sum()) == values.size, (int(mask.sum()), values.size)
    canvas = np.asarray(base.convert("RGB"), dtype="int32").copy()
    region = canvas[y:y + height, x:x + width]
    region[mask] = np.stack([((values >> 11) & 31) << 3, ((values >> 5) & 63) << 2,
                             (values & 31) << 3], axis=1)
    canvas[y:y + height, x:x + width] = region
    return Image.fromarray(canvas.astype("uint8"))


# --------------------------------------------------------------------------
# 条目
# --------------------------------------------------------------------------


@dataclass
class Asset:
    name: str
    pool: int
    kind: int
    payload: bytes = b""
    w: int = 0
    h: int = 0
    dx: int = 0
    dy: int = 0
    dw: int = 0
    dh: int = 0
    base_name: str | None = None
    source_bytes: int = 0
    note: str = ""
    verify: float | None = None
    verify_base: float | None = None
    alias: bool = False

    @property
    def stored_bytes(self) -> int:
        return len(self.payload)


@dataclass
class SpriteRow:
    """SEC_SPRITE 的一行:身体/表情条目名 + 表情相对身体左上角的偏移。"""

    body: str | None
    face: str | None
    facex: int
    facey: int


@dataclass
class BuildStats:
    groups: dict[str, dict[str, int]] = field(default_factory=dict)

    def add(self, label: str, source: int, stored: int, entries: int) -> None:
        row = self.groups.setdefault(label, {"source": 0, "stored": 0, "entries": 0})
        row["source"] += source
        row["stored"] += stored
        row["entries"] += entries


# --------------------------------------------------------------------------
# 打包
# --------------------------------------------------------------------------


class PackBuilder:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.assets: list[Asset] = []
        self.sprites: list[SpriteRow] = []
        self.stats = BuildStats()
        self.quality: list[tuple[str, float]] = []
        self.diff_choice: dict[str, int] = {}

    # -- 公共 -------------------------------------------------------------
    def finish(self, asset: Asset, label: str) -> None:
        self.assets.append(asset)
        self.stats.add(label, asset.source_bytes, asset.stored_bytes, 1)
        if asset.verify is not None:
            self.quality.append((f"{label} {asset.name}", asset.verify))
        if self.args.verify and asset.kind != KIND_MISSING and asset.payload:
            log(f"  {label} {asset.name}: {human(asset.stored_bytes)} 误差 {asset.verify:.2f}"
                if asset.verify is not None else f"  {label} {asset.name}: {human(asset.stored_bytes)}")

    # -- 背景 -------------------------------------------------------------
    def add_backgrounds(self, source: Path, refs: S.Refs) -> None:
        self.add_title_art(source)
        for name in S.sorted_names(refs.bg):
            path = source / S.BG_DIR / f"{name}.jpg"
            asset = Asset(name=name, pool=POOL_BG, kind=KIND_BG,
                          source_bytes=path.stat().st_size if path.is_file() else 0)
            if not path.is_file():
                log(f"背景缺失,存占位: {name}")
                asset.kind = KIND_MISSING
                asset.note = "源文件缺失"
                self.finish(asset, "背景")
                continue
            with Image.open(path) as raw:
                image = cover(raw.convert("RGB"), self.args.screen_w, self.args.screen_h,
                              self.args.crop_bias)
            quality = self.args.bg_quality or matched_quality(
                image, self.target_bytes_for(path.stat().st_size, S.SRC_W * S.SRC_H))
            asset.payload = jpeg_bytes(image, quality)
            asset.w, asset.h = image.size
            asset.note = f"cover 240x320 q{quality}"
            if self.args.verify:
                with Image.open(io.BytesIO(asset.payload)) as stored:
                    asset.verify = mean_abs_error(stored, image)
            self.finish(asset, "背景")

    def add_title_art(self, source: Path) -> None:
        path = source / S.TITLE_BG
        asset = Asset(name=self.args.title_name, pool=POOL_BG, kind=KIND_BG,
                      source_bytes=path.stat().st_size if path.is_file() else 0)
        if not path.is_file():
            asset.kind = KIND_MISSING
            asset.note = "标题图缺失"
            self.finish(asset, "标题图")
            return
        with Image.open(path) as raw:
            image = cover(raw.convert("RGB"), self.args.screen_w, self.args.screen_h,
                          self.args.crop_bias)
        asset.payload = jpeg_bytes(image, self.args.title_quality)
        asset.w, asset.h = image.size
        asset.note = f"标题图 q{self.args.title_quality}"
        if self.args.verify:
            with Image.open(io.BytesIO(asset.payload)) as stored:
                asset.verify = mean_abs_error(stored, image)
        self.finish(asset, "标题图")

    # -- 事件 CG ----------------------------------------------------------
    def add_events(self, source: Path, refs: S.Refs) -> None:
        groups: dict[str, list[str]] = {}
        for name in S.sorted_names(refs.cg):
            stem, ext = name.rsplit(".", 1) if "." in name else (name, "jpg")
            match = CG_GROUP_RE.match(stem)
            groups.setdefault((match.group(1) if match else stem) + "." + ext, []).append(name)
        for members in groups.values():
            base_name = sorted(members, key=lambda name: (-refs.cg.get(name, 0), name))[0]
            base_path = source / S.EV_DIR / base_name
            with Image.open(base_path) as raw:
                base_image = cover(raw.convert("RGB"), self.args.screen_w, self.args.screen_h,
                                   self.args.crop_bias)
            base_quality = self.args.cg_quality or matched_quality(
                base_image, self.target_bytes_for(base_path.stat().st_size, S.SRC_W * S.SRC_H))
            base_565 = to565(base_image)
            for name in members:
                path = source / S.EV_DIR / name
                asset = Asset(name=name, pool=POOL_CG, kind=KIND_CG,
                              source_bytes=path.stat().st_size if path.is_file() else 0)
                if not path.is_file():
                    asset.kind = KIND_MISSING
                    asset.note = "源文件缺失"
                    self.finish(asset, "事件 CG")
                    continue
                if name == base_name:
                    asset.payload = jpeg_bytes(base_image, base_quality)
                    asset.w, asset.h = base_image.size
                    asset.note = f"基准张 q{base_quality}"
                    self.diff_choice["基准"] = self.diff_choice.get("基准", 0) + 1
                    if self.args.verify:
                        with Image.open(io.BytesIO(asset.payload)) as stored:
                            asset.verify = mean_abs_error(stored, base_image)
                    self.finish(asset, "事件 CG")
                    continue
                with Image.open(path) as raw:
                    other = cover(raw.convert("RGB"), self.args.screen_w, self.args.screen_h,
                                  self.args.crop_bias)
                full = jpeg_bytes(other, base_quality)
                change = change_mask(base_565, to565(other), self.args.diff_threshold)
                patch = encode_patch(*change) if change else b""
                # 补齐像素流的长度校验:掩码里置位像素数必须与像素数一致
                if change and int(change[1].sum()) * 2 != len(change[2]) * 2:
                    raise AssertionError(f"{name}: 掩码与像素数不一致")
                use_patch = bool(change) and len(patch) <= len(full) * self.args.max_patch_ratio
                asset.w, asset.h = base_image.size
                if use_patch:
                    rect = change[0]
                    asset.payload = patch
                    asset.kind = KIND_CG_DIFF
                    asset.base_name = base_name
                    asset.dx, asset.dy, asset.dw, asset.dh = rect
                    asset.note = (f"掩码补丁 {rect[2]}x{rect[3]}@{rect[0]},{rect[1]}"
                                  f" ({100.0 * change[1].mean():.1f}% 像素)")
                    self.diff_choice["补丁"] = self.diff_choice.get("补丁", 0) + 1
                    if self.args.verify:
                        composited = apply_patch(base_image, asset.payload, rect)
                        asset.verify = mean_abs_error(composited, other)
                        asset.verify_base = mean_abs_error(base_image, other)
                else:
                    asset.payload = full
                    asset.note = "整张(补丁不划算)" if change else "整张(与基准相同)"
                    self.diff_choice["整张"] = self.diff_choice.get("整张", 0) + 1
                    if self.args.verify:
                        with Image.open(io.BytesIO(asset.payload)) as stored:
                            asset.verify = mean_abs_error(stored, other)
                self.finish(asset, "事件 CG")

    # -- SD 小人 ----------------------------------------------------------
    def add_sd(self, source: Path, refs: S.Refs) -> None:
        for name in S.sorted_names(refs.sd):
            path = source / S.EV_DIR / name
            asset = Asset(name=name, pool=POOL_SD, kind=KIND_SD,
                          source_bytes=path.stat().st_size if path.is_file() else 0)
            if not path.is_file():
                asset.kind = KIND_MISSING
                asset.note = "源文件缺失"
                self.finish(asset, "SD 小人")
                continue
            with Image.open(path) as raw:
                image = raw.convert("RGBA")
            box = content_box(image)
            if box is None:
                asset.kind = KIND_MISSING
                asset.note = "整幅透明"
                self.finish(asset, "SD 小人")
                continue
            image = image.crop(box)
            scaled = self.sd_scale()
            width = max(1, round(image.width * scaled))
            height = max(1, round(image.height * scaled))
            image = image.resize((width, height), Image.LANCZOS)
            asset.payload = encode_blocked(image, self.args.sd_colors)
            asset.w, asset.h = image.size
            asset.dx = max(0, round(box[0] * scaled))
            asset.dy = max(0, round(box[1] * scaled))
            asset.note = f"{image.width}x{image.height}@{asset.dx},{asset.dy} 分块调色板"
            if self.args.verify:
                asset.verify = mean_abs_error(decode_blocked(asset.payload), image)
            self.finish(asset, "SD 小人")

    def sd_scale(self) -> float:
        """SD 的等比缩放:0.45 是相对源工程设备像素(336x480)的比例。"""
        return self.args.sd_scale

    # -- 立绘 -------------------------------------------------------------
    def add_sprites(self, source: Path, refs: S.Refs) -> None:
        parts = S.sprite_parts(source, refs)
        scale = self.args.sprite_scale
        # 1) 每条描述算出身体/表情的条目名与偏移
        rows: dict[str, SpriteRow] = {}
        body_specs: dict[str, list[tuple[str, float]]] = {}
        for raw, part in parts.items():
            body_name = f"{part.body.img}"
            face_name = part.face.img if part.face else None
            facex = facey = 0
            if part.face is not None:
                facex = round((part.face.left - part.body.left) * part.scale * scale)
                facey = round((part.face.top - part.body.top) * part.scale * scale)
            rows[raw] = SpriteRow(body=body_name, face=face_name, facex=facex, facey=facey)
            body_specs.setdefault(body_name, []).append((raw, facey))
        # 2) 身体:裁到"屏幕上一定看不到的下半身"(按用到它的最高位立绘算)
        body_crop: dict[str, int] = {}
        for body_name, usages in body_specs.items():
            min_top = min(S.SPRITE_HEAD_Y - facey for _, facey in usages)
            body_crop[body_name] = max(1, min(self.args.screen_h, self.args.screen_h - min_top))
        for body_name in sorted(body_specs):
            path = source / S.CH_DIR / body_name
            asset = Asset(name=body_name, pool=POOL_BODY, kind=KIND_BODY,
                          source_bytes=path.stat().st_size if path.is_file() else 0)
            if not path.is_file():
                asset.kind = KIND_MISSING
                asset.note = "源文件缺失"
                self.finish(asset, "立绘身体")
                continue
            with Image.open(path) as raw:
                image = raw.convert("RGBA")
            width = max(1, round(image.width * scale))
            height = max(1, round(image.height * scale))
            image = image.resize((width, height), Image.LANCZOS)
            crop_h = min(height, body_crop[body_name])
            image = image.crop((0, 0, width, crop_h))
            asset.payload = encode_blocked(image, self.args.body_colors)
            asset.w, asset.h = image.size
            asset.note = f"{image.width}x{image.height}(可见部分) 分块调色板"
            if self.args.verify:
                asset.verify = mean_abs_error(decode_blocked(asset.payload), image)
            self.finish(asset, "立绘身体")
        # 3) 表情:缩到同样比例,裁到 alpha 包围盒(dx/dy 记包围盒偏移)
        face_files = sorted({row.face for row in rows.values() if row.face})
        for face_name in face_files:
            path = source / S.CH_DIR / face_name
            asset = Asset(name=face_name, pool=POOL_FACE, kind=KIND_FACE,
                          source_bytes=path.stat().st_size if path.is_file() else 0)
            if not path.is_file():
                asset.kind = KIND_MISSING
                asset.note = "源文件缺失"
                self.finish(asset, "立绘表情")
                continue
            with Image.open(path) as raw:
                image = raw.convert("RGBA")
            width = max(1, round(image.width * scale))
            height = max(1, round(image.height * scale))
            image = image.resize((width, height), Image.LANCZOS)
            box = content_box(image)
            if box is None:
                asset.kind = KIND_MISSING
                asset.note = "整幅透明"
                self.finish(asset, "立绘表情")
                continue
            asset.dx, asset.dy = box[0], box[1]
            image = image.crop(box)
            asset.payload = encode_blocked(image, self.args.face_colors)
            asset.w, asset.h = image.size
            asset.note = f"{image.width}x{image.height}@{asset.dx},{asset.dy} 分块调色板"
            if self.args.verify:
                asset.verify = mean_abs_error(decode_blocked(asset.payload), image)
            self.finish(asset, "立绘表情")
        # 4) 合成表(剧本引用的每条描述一行,顺序 = 剧本包里的立绘 id)
        for raw in S.sorted_names(refs.sprite):
            row = rows.get(raw)
            if row is None:
                # 解析不出来的描述(如纯角色名):留空行,设备侧不画立绘
                self.sprites.append(SpriteRow(body=None, face=None, facex=0, facey=0))
                continue
            self.sprites.append(row)

    def target_bytes_for(self, source_bytes: int, source_pixels: int) -> int:
        ratio = (self.args.screen_w * self.args.screen_h) / max(1, source_pixels)
        return max(1024, int(source_bytes * ratio * self.args.quality_scale))

    # -- 序列化 -----------------------------------------------------------
    def serialize(self, meta: dict[str, str]) -> bytes:
        entries = sorted(self.assets, key=lambda asset: (asset.pool, asset.name))
        index_of = {id(asset): position for position, asset in enumerate(entries)}
        names = bytearray()
        name_offsets: list[tuple[int, int]] = []
        for asset in entries:
            encoded = asset.name.encode("utf-8")
            name_offsets.append((len(names), len(encoded)))
            names += encoded + b"\x00"

        blob = bytearray()
        records = bytearray()
        for position, asset in enumerate(entries):
            base = 0xFFFF
            if asset.base_name:
                base = next((slot for slot, other in enumerate(entries)
                             if other.pool == asset.pool and other.name == asset.base_name), 0xFFFF)
                if base == 0xFFFF:
                    raise AssertionError(f"{asset.name}: 找不到基准 {asset.base_name}")
            data_off = len(blob)
            blob += asset.payload
            records += ENTRY.pack(
                name_offsets[position][0], name_offsets[position][1], asset.kind, asset.pool,
                base, asset.w, asset.h, asset.dx, asset.dy, asset.dw, asset.dh,
                data_off, len(asset.payload), FLAG_ALIAS if asset.alias else 0, 0)

        sprite_blob = bytearray(struct.pack("<I", len(self.sprites)))
        for row in self.sprites:
            body = index_of[id(next(asset for asset in entries if asset.name == row.body
                                    and asset.pool == POOL_BODY))] if row.body else 0xFFFF
            face = index_of[id(next(asset for asset in entries if asset.name == row.face
                                    and asset.pool == POOL_FACE))] if row.face else 0xFFFF
            sprite_blob += SPRITE_ENTRY.pack(body, face, row.facex, row.facey)

        meta_text = "".join(f"{key}={value}\n" for key, value in meta.items()).encode("utf-8")
        sections = [
            (SEC_NAME, len(entries), bytes(names)),
            (SEC_ASSET, len(entries), bytes(records)),
            (SEC_BLOB, len(entries), bytes(blob)),
            (SEC_SPRITE, len(self.sprites), bytes(sprite_blob)),
            (SEC_META, 0, meta_text),
        ]
        header_size = HEADER.size + SECTION.size * len(sections)
        offset = header_size
        table = bytearray()
        payload = bytearray()
        for kind, count, data in sections:
            table += SECTION.pack(kind, offset, count, len(data))
            payload += data
            offset += len(data)
        total = header_size + len(payload)
        return bytes(HEADER.pack(MAGIC, VERSION, header_size, len(sections), 0, total)) \
            + bytes(table) + bytes(payload)

    def report(self, pack: bytes, out_path: Path, meta: dict[str, str]) -> None:
        log(f"包: {out_path} ({human(len(pack))})")
        for label, row in sorted(self.stats.groups.items()):
            log(f"  {label:<10} {row['entries']:>5} 条  源 {human(row['source']):>9}"
                f" -> {human(row['stored']):>9}")
        if self.diff_choice:
            log("  CG 编码: " + ", ".join(f"{key} {value}" for key, value in self.diff_choice.items()))
        if self.quality:
            worst = sorted(self.quality, key=lambda item: -item[1])[:5]
            log("  观感误差最高的几项: " + "; ".join(f"{name} {value:.2f}" for name, value in worst))
        log("  " + " ".join(f"{key}={value}" for key, value in meta.items()))


# --------------------------------------------------------------------------
# 自检
# --------------------------------------------------------------------------


def load_pack(path: Path):
    raw = path.read_bytes()
    magic, version, header_size, section_count, _reserved, total = HEADER.unpack_from(raw, 0)
    if magic != MAGIC or version != VERSION or total != len(raw):
        raise SystemExit(f"{path}: 魔数/版本/长度不符")
    sections = {}
    for index in range(section_count):
        kind, offset, count, size = SECTION.unpack_from(raw, HEADER.size + index * SECTION.size)
        sections[kind] = (offset, count, size)
    return raw, sections


def check_pack(path: Path) -> int:
    raw, sections = load_pack(path)
    errors: list[str] = []
    name_off, name_count, name_size = sections[SEC_NAME]
    asset_off, asset_count, asset_size = sections[SEC_ASSET]
    blob_off, _blob_count, blob_size = sections[SEC_BLOB]
    sprite_off, sprite_count, sprite_size = sections[SEC_SPRITE]
    if asset_size != asset_count * ENTRY.size:
        errors.append(f"SEC_ASSET 长度 {asset_size} != {asset_count} x {ENTRY.size}")
    if sprite_size != 4 + sprite_count * SPRITE_ENTRY.size:
        errors.append(f"SEC_SPRITE 长度 {sprite_size} 与 {sprite_count} 条不符")
    counts: dict[int, int] = {}
    for index in range(asset_count):
        record = ENTRY.unpack_from(raw, asset_off + index * ENTRY.size)
        (entry_name_off, name_len, kind, pool, base, w, h, dx, dy, dw, dh,
         data_off, data_len, flags, _reserved) = record
        counts[pool] = counts.get(pool, 0) + 1
        if entry_name_off + name_len > name_size:
            errors.append(f"条目 {index}: 名字越界")
        if data_off + data_len > blob_size:
            errors.append(f"条目 {index}: 载荷越界")
        if kind == KIND_CG_DIFF:
            if base >= asset_count:
                errors.append(f"条目 {index}: 补丁基准越界")
            if 8 + 4 * 1 + len(raw[blob_off + data_off:blob_off + data_off + data_len]) > data_len + 8:
                pass
            mask_len, block_count = struct.unpack_from("<II", raw, blob_off + data_off)
            if 8 + 4 * block_count + mask_len > data_len:
                errors.append(f"条目 {index}: 补丁头长度不合法")
        if kind in (KIND_SD, KIND_BODY, KIND_FACE):
            payload = raw[blob_off + data_off:blob_off + data_off + data_len]
            if len(payload) < BLOCK_HEADER.size:
                errors.append(f"条目 {index}: 分块载荷太短")
                continue
            width, height, palette_count, block_rows, block_count, _r, raw_block = \
                BLOCK_HEADER.unpack_from(payload)
            if width != w or height != h:
                errors.append(f"条目 {index}: 分块尺寸 {width}x{height} != 条目 {w}x{h}")
            if raw_block != block_rows * (width + 1):
                errors.append(f"条目 {index}: raw_block_bytes 不符")
            table_end = BLOCK_HEADER.size + palette_count * 3 + block_count * 4
            if table_end > len(payload):
                errors.append(f"条目 {index}: 块表越界")
    for index in range(sprite_count):
        body, face, _fx, _fy = SPRITE_ENTRY.unpack_from(raw, sprite_off + 4 + index * SPRITE_ENTRY.size)
        for slot in (body, face):
            if slot != 0xFFFF and slot >= asset_count:
                errors.append(f"立绘 {index}: 条目下标 {slot} 越界")
    if errors:
        for message in errors[:40]:
            log(f"错误: {message}")
        return 1
    log(f"OK: {path} 条目 {asset_count} 立绘 {sprite_count} "
        f"名字区 {human(name_size)} 载荷 {human(blob_size)}")
    log("  各名字空间: " + ", ".join(f"{POOL_NAMES.get(pool, pool)}={count}"
                                     for pool, count in sorted(counts.items())))
    return 0


# --------------------------------------------------------------------------
# 命令行
# --------------------------------------------------------------------------


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="构建 DRACU-RIOT 图片包")
    parser.add_argument("--source", type=Path, default=Path("build/dracu-source"),
                        help="dracu-riot-miband 的 checkout(tools/dracu_fetch_source.py)")
    parser.add_argument("--out", type=Path, default=Path("build/dracu-pack/dracu_pack.bin"))
    parser.add_argument("--check", type=Path, help="只做结构自检")
    parser.add_argument("--screen-w", type=int, default=S.SCREEN_W)
    parser.add_argument("--screen-h", type=int, default=S.SCREEN_H)
    parser.add_argument("--crop-bias", default="top", choices=("top", "center", "bottom"))
    parser.add_argument("--quality-scale", type=float, default=1.0,
                        help="背景/CG 目标体积的倍率(按像素折算后的源体积)")
    parser.add_argument("--bg-quality", type=int, default=0, help="固定背景 JPEG 质量(0=按体积标定)")
    parser.add_argument("--cg-quality", type=int, default=0, help="固定 CG JPEG 质量(0=按体积标定)")
    parser.add_argument("--title-quality", type=int, default=88)
    parser.add_argument("--title-name", default=TITLE_NAME)
    parser.add_argument("--sprite-scale", type=float, default=0.55,
                        help="立绘相对源设备像素(336x480)的缩放")
    parser.add_argument("--sd-scale", type=float, default=0.45, help="SD 小人相对源设备像素的缩放")
    parser.add_argument("--body-colors", type=int, default=255)
    parser.add_argument("--face-colors", type=int, default=96)
    parser.add_argument("--sd-colors", type=int, default=192)
    parser.add_argument("--diff-threshold", type=int, default=10)
    parser.add_argument("--max-patch-ratio", type=float, default=0.95)
    parser.add_argument("--verify", action="store_true", help="逐张回读核验并打印误差")
    parser.add_argument("--no-sprites", action="store_true")
    parser.add_argument("--no-sd", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.check:
        return check_pack(args.check)
    source = S.find_source(args.source)
    pages = S.load_pages(source)
    refs = S.collect_refs(pages)
    log(f"源: {source}\n剧本 {len(pages)} 页;引用 背景 {len(refs.bg)} / CG {len(refs.cg)} / "
        f"SD {len(refs.sd)} / 立绘描述 {len(refs.sprite)}")
    builder = PackBuilder(args)
    builder.add_backgrounds(source, refs)
    builder.add_events(source, refs)
    if not args.no_sd:
        builder.add_sd(source, refs)
    if not args.no_sprites:
        builder.add_sprites(source, refs)
    meta = {
        "source": "github.com/hezdaaa/dracu-riot-miband",
        "sprites": str(len(builder.sprites)),
        "screen": f"{args.screen_w}x{args.screen_h}",
        "sprite_scale": f"{args.sprite_scale}",
        "sd_scale": f"{args.sd_scale}",
        "bg_quality": str(args.bg_quality or "auto"),
        "cg_quality": str(args.cg_quality or "auto"),
        "generator": "tools/dracu_pack.py",
        "python": S.human_env()["python"],
    }
    pack = builder.serialize(meta)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(pack)
    builder.report(pack, args.out, meta)
    return check_pack(args.out)


if __name__ == "__main__":
    raise SystemExit(main())
