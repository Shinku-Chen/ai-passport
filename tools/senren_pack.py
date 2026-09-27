#!/usr/bin/env python3
"""Build the Senren * Banka image pack for the AI Passport port.

Source project: https://github.com/hrk666666/Senren-Banka-MiBand-10
  - 小米手环 10(小米 Vela / aiot 快应用)上的《千恋＊万花》移植版;素材内容来自
    https://github.com/hezdaaa/qlwh-mibandported。
  - 素材版权归 SAGA PLANETS 所有;本仓库只保存转换产物,不分发源素材。
    先用 tools/senren_fetch_source.py 把源素材拉到 build/senren-source/。

The pack is a single little-endian binary read straight out of flash.  Every
payload is already at the device's native geometry, so the firmware never
rescales: backgrounds, event CGs and effect overlays are exactly 240x320,
sprites are x280 (bottom-anchored), SD decorations are 240x144.

  header  : magic "SENRNPK2", version u16, header_size u16, section_count u16,
            reserved u16, total_size u32                       (20 B)
  section : { type u32, offset u32, count u32, size u32 }      (16 B × N)
    SEC_NAME  UTF-8 names, NUL-separated, one per asset
    SEC_ASSET fixed 36-byte entry table:
              { name_off u32, name_len u16, kind u8, pool u8, base u16,
                w u16, h u16, dx u16, dy u16, dw u16, dh u16,
                data_off u32, data_len u32, flags u32, reserved u16 }
              data_off is relative to SEC_BLOB
    SEC_BLOB  payload area, one blob per entry
    SEC_META  key=value UTF-8 text (source repo / conversion parameters)

  kind : 0=BG JPEG | 1=SPRITE PNG | 2=SD PNG | 3=CG JPEG | 4=CG_DIFF masked patch
         | 5=MISSING placeholder | 6=EFFECT PNG
  pool : 0=background 1=sprite 2=event —— matches the script's bg / 立绘 / ev namespaces
  flags: bit0 = alias entry: no payload, draw the `base` entry instead

  CG_DIFF payload (kind 4, rect in the entry's dx/dy/dw/dh):
    { mask_len u32, pixels_len u32, deflate(mask), deflate(pixels) }
    mask   : 1bpp, row-major over the rect, MSB first
    pixels : RGB565 little-endian, one per set mask bit, in scan order
  The firmware draws the base CG, then walks the mask row by row and overwrites
  the marked pixels, so a patch costs one row of scratch RAM and is lossless
  with respect to the scaled source.

Conversion rules (dimensions come from the source project: 336x480 assets
authored for a 212x520 band canvas):

  背景 92 张 336x480 JPEG   -> cover 缩到 240x320(裁下方:那一块本来就压在正文带下
                              面),JPEG 质量按「像素数等比缩小后的源体积」标定
  立绘 123 张 紧裁调色板 PNG -> 缩到 <=280 高(底部对齐画在屏幕上),RGBA 量化成
                               <=255 色调色板 PNG。_1/_2/_3 是同一套衣服的不同姿势,
                               不是表情差分,所以逐张存
  SD   216 张 内容 336x201   -> 每组只留一张(按引用次数加权的中心张),裁到内容缩到
                               240x144(源工程 .sd-image 的显示框);其余变体写成
                               别名条目指向它
  CG   570 张 336x480 JPEG   -> 每组留引用最多的一张作基准(cover 到 240x320 + 标定
                               质量的 JPEG);其余张先算变化掩码,能比整张 JPEG 更省
                               就存掩码补丁(无损),否则整张存
  其它 28 张 特效/道具/画面 PNG -> cover 到 240x320,按调色板 PNG 重存

Usage:
  python tools/senren_fetch_source.py --dest build/senren-source --chunks
  python tools/senren_pack.py --source build/senren-source --out build/senren-pack/senren_pack.bin
  python tools/senren_pack.py --check build/senren-pack/senren_pack.bin
  python tools/senren_pack.py --compare build/senren-pack/senren_pack.bin --source build/senren-source

社区投稿版本用 --drop-file 排掉 R18 事件图:被排掉的名字会保留为 MISSING 占位,
剧本引用它时跳过绘制而不是报错。
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
from typing import Iterable

try:
    from PIL import Image
except ImportError:  # pragma: no cover - 环境问题
    sys.exit("需要 Pillow: python -m pip install pillow")

MAGIC = b"SENRNPK2"
VERSION = 2
HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
ENTRY = struct.Struct("<IHBBHHHHHHHIIIH")
assert ENTRY.size == 36, ENTRY.size

# 段类型
SEC_NAME, SEC_ASSET, SEC_BLOB, SEC_META = range(4)
# 条目类型
KIND_BG, KIND_SPRITE, KIND_SD, KIND_CG, KIND_CG_DIFF, KIND_MISSING, KIND_EFFECT = range(7)
KIND_NAMES = {
    KIND_BG: "BG", KIND_SPRITE: "SPRITE", KIND_SD: "SD", KIND_CG: "CG",
    KIND_CG_DIFF: "CG_DIFF", KIND_MISSING: "MISSING", KIND_EFFECT: "EFFECT",
}
# 名字空间
POOL_BG, POOL_CH, POOL_EV = 0, 1, 2
FLAG_ALIAS = 1 << 0

# 源素材的固定尺寸(源工程画布)
SRC_FULL_W, SRC_FULL_H = 336, 480
SD_RE = re.compile(r"^sd(\d+)([a-z])([a-z])$")
EV_RE = re.compile(r"^ev(\d+)([a-z]*)$")


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def human(count: int) -> str:
    if abs(count) >= 1048576:
        return f"{count / 1048576:.2f} MB"
    return f"{count / 1024:.1f} KB"


# --------------------------------------------------------------------------
# 素材条目
# --------------------------------------------------------------------------


@dataclass
class Asset:
    name: str                       # 剧本里引用的名字(不含扩展名)
    pool: int
    kind: int
    payload: bytes = b""
    w: int = 0
    h: int = 0
    base_name: str | None = None    # 别名/差分补丁指向的基准名字
    rect: tuple[int, int, int, int] | None = None   # 差分补丁:dx, dy, dw, dh
    source_bytes: int = 0           # 源文件体积(统计用)
    note: str = ""
    verify: float | None = None     # 与源素材(按转换规则重放)的平均绝对误差
    verify_base: float | None = None
    alias: bool = False

    @property
    def stored_bytes(self) -> int:
        return len(self.payload)


def read_refs(script_dir: Path | None) -> dict[tuple[int, str], int]:
    """统计剧本对每个素材名的引用次数(选基准张用)。"""
    refs: dict[tuple[int, str], int] = {}
    if script_dir is None or not script_dir.is_dir():
        return refs
    for path in sorted(script_dir.glob("*.txt")):
        for node in json.loads(path.read_text(encoding="utf-8")):
            kind = node[0]
            names: list[tuple[int, str]] = []
            if kind == 2 and len(node) > 1 and node[1]:
                names.append((POOL_BG, str(node[1])))
            elif kind == 5 and len(node) > 1 and node[1]:
                names.append((POOL_EV, str(node[1])))
            elif kind == 3:
                for slot in (node[3] if len(node) > 3 else None) or []:
                    if slot and slot[0]:
                        names.append((POOL_CH, str(slot[0])))
            elif kind == 7 and len(node) > 1 and node[1] and node[1] != "*":
                names.append((POOL_CH, str(node[1])))
            for key in names:
                refs[key] = refs.get(key, 0) + 1
    return refs


def source_ref(source: Path) -> str:
    """从取源清单里带出上游 ref(取源工具写的 MANIFEST.json),让 pack 元数据可追溯。"""
    manifest = source / "MANIFEST.json"
    if manifest.is_file():
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
            ref = str(data.get("ref", "unknown"))
            requested = str(data.get("requested_ref", ""))
            return ref if requested in ("", ref) else f"{ref} (请求 {requested})"
        except (json.JSONDecodeError, OSError):
            pass
    return "unknown"


def pick_base(members: list[str], refs: dict[tuple[int, str], int], pool: int) -> str:
    """基准张 = 剧本引用最多的一张,并列时取字典序最小的,保证可复现。"""
    return sorted(members, key=lambda name: (-refs.get((pool, name), 0), name))[0]


def nearest_of(name: str, retained: set[str], thumbs: dict[str, "Image.Image"]) -> str:
    """在保留张里找跟 name 最像的一张:同姿势不同表情优先,避免放错表情。"""
    if name in retained:
        return name
    source = thumbs.get(name)
    best = None
    best_error = None
    for candidate in sorted(retained):
        target = thumbs.get(candidate)
        if source is None or target is None:
            return sorted(retained)[0]
        error = mean_abs_error(source, target)
        if best_error is None or error < best_error:
            best, best_error = candidate, error
    return best or sorted(retained)[0]


def weighted_medoid(order: list[str], refs: dict[tuple[int, str], int], thumbs: dict[str, "Image.Image"]) -> str:
    """按引用次数加权的中心张:单张保留时,它让「放错图」的总代价最小,
    而不是单纯选引用最多的那张(那张可能是组里的怪姿势)。"""
    if len(order) == 1 or not thumbs:
        return order[0]
    best, best_cost = order[0], None
    for candidate in order:
        cost = 0.0
        for other in order:
            if other == candidate:
                continue
            cost += refs.get((POOL_EV, other), 0) * mean_abs_error(thumbs[candidate], thumbs[other])
        if best_cost is None or cost < best_cost:
            best, best_cost = candidate, cost
    return best


# --------------------------------------------------------------------------
# 图像工具
# --------------------------------------------------------------------------


def cover(image: "Image.Image", width: int, height: int, bias: str = "top") -> "Image.Image":
    """等比缩放到铺满 width x height,再按 bias 裁掉多余的一边。

    源素材是 336x480(比例 0.70),屏幕是 240x320(0.75):铺满后高度会多出约 23 px。
    默认裁下方 —— 那一块本来就压在正文带底下,保住上方构图更重要。
    """
    scale = width / image.width
    resized = image.resize((width, max(height, round(image.height * scale))), Image.LANCZOS)
    if resized.height == height:
        return resized
    extra = resized.height - height
    if bias == "bottom":
        top = extra
    elif bias == "center":
        top = extra // 2
    else:
        top = 0
    return resized.crop((0, top, width, top + height))


def scale_to_height(image: "Image.Image", height: int) -> "Image.Image":
    if image.height <= height:
        return image
    width = max(1, round(image.width * height / image.height))
    return image.resize((width, height), Image.LANCZOS)


def png_bytes(image: "Image.Image") -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, "PNG", optimize=True)
    return buffer.getvalue()


def palettize(image: "Image.Image", colors: int) -> bytes:
    """量化成 <=colors 色调色板 PNG;透明通道与颜色一起进调色板(FASTOCTREE)。"""
    converted = image if image.mode == "RGBA" else image.convert("RGBA")
    return png_bytes(converted.quantize(colors=colors, method=Image.FASTOCTREE))


def jpeg_bytes(image: "Image.Image", quality: int) -> bytes:
    buffer = io.BytesIO()
    image.convert("RGB").save(buffer, "JPEG", quality=quality, optimize=True, subsampling=2)
    return buffer.getvalue()


def matched_quality(image: "Image.Image", target_bytes: int, low: int = 40, high: int = 92) -> int:
    """找与目标体积相当的 JPEG 质量:目标体积按像素数等比折算源文件,保住观感。"""
    if jpeg_bytes(image, high).__len__() <= target_bytes:
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


def content_box(image: "Image.Image") -> tuple[int, int, int, int]:
    """内容包围盒(alpha>8);没有透明信息时返回整幅。"""
    if image.mode not in ("RGBA", "LA", "P"):
        return 0, 0, image.width, image.height
    alpha = image.convert("RGBA").getchannel("A")
    box = alpha.point(lambda value: 255 if value > 8 else 0).getbbox()
    return box or (0, 0, image.width, image.height)


def sd_display_reference(path: Path, args: argparse.Namespace) -> "Image.Image":
    """SD 在屏幕上的样子:裁到内容再缩到显示框。量误差必须按这个尺寸,不然会被画布空白边稀释。"""
    with Image.open(path) as raw:
        image = raw.convert("RGBA")
    if args.sd_crop:
        image = image.crop(content_box(image))
    return image.resize((args.sd_width, args.sd_height), Image.LANCZOS)


def flatten(image: "Image.Image") -> "Image.Image":
    """把带透明通道的图合成到中灰底上:屏幕上的立绘/装饰也是叠在画面上的,
    透明区域直接比 RGB 会让误差虚高。"""
    rgba = image if image.mode == "RGBA" else image.convert("RGBA")
    background = Image.new("RGBA", rgba.size, (128, 128, 128, 255))
    return Image.alpha_composite(background, rgba).convert("RGB")


def mean_abs_error(left: "Image.Image", right: "Image.Image") -> float:
    """两张图的平均绝对误差(0..255),透明区按中灰底合成后比。"""
    a = flatten(left)
    b = flatten(right)
    if a.size != b.size:
        b = b.resize(a.size, Image.LANCZOS)
    import numpy as np

    diff = np.abs(np.asarray(a, dtype="int16") - np.asarray(b, dtype="int16"))
    return float(diff.mean())


# --------------------------------------------------------------------------
# CG 差分:稀疏掩码补丁
# --------------------------------------------------------------------------


def to565(image: "Image.Image"):
    """转成 RGB565 数值矩阵(用 int32 存,避免 16 位相减出负号)。"""
    import numpy as np

    array = np.asarray(image.convert("RGB"), dtype="int32")
    return (((array[:, :, 0] >> 3) << 11) | ((array[:, :, 1] >> 2) << 5) | (array[:, :, 2] >> 3)).astype(np.int32)


def change_mask(base565, other565, threshold: int):
    """逐像素比较两个 RGB565 画面,返回 (bbox, 掩码, 变化像素的 RGB565 列表)。"""
    import numpy as np

    red = np.abs((base565 >> 11) - (other565 >> 11)) * 8
    green = np.abs(((base565 >> 5) & 63) - ((other565 >> 5) & 63)) * 4
    blue = np.abs((base565 & 31) - (other565 & 31)) * 8
    mask = np.maximum(np.maximum(red, green), blue) > threshold
    if not mask.any():
        return None
    rows, cols = np.where(mask)
    x0, x1 = int(cols.min()), int(cols.max())
    y0, y1 = int(rows.min()), int(rows.max())
    patch = mask[y0:y1 + 1, x0:x1 + 1]
    pixels = other565[mask]
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1), patch, pixels


def encode_patch(rect: tuple[int, int, int, int], patch, pixels) -> bytes:
    """掩码补丁载荷:1bpp 掩码 + 按扫描序排列的 RGB565 像素,各自 deflate 一段。"""
    import numpy as np

    packed = np.packbits(patch.reshape(-1).astype(np.uint8)).tobytes()
    mask_blob = zlib.compress(packed, 9)
    pixels_blob = zlib.compress(pixels.astype("<u2").tobytes(), 9)
    return struct.pack("<II", len(mask_blob), len(pixels_blob)) + mask_blob + pixels_blob


def apply_patch(base: "Image.Image", payload: bytes, rect: tuple[int, int, int, int]) -> "Image.Image":
    """按 pack 自身的信息把补丁贴到基准图上(核验与固件同一条路径)。"""
    import numpy as np

    mask_len = struct.unpack_from("<I", payload, 0)[0]
    mask_blob = payload[8:8 + mask_len]
    pixels_blob = payload[8 + mask_len:]
    x, y, width, height = rect
    assert rect[2:] == (width, height)
    mask = np.unpackbits(np.frombuffer(zlib.decompress(mask_blob), dtype=np.uint8))[:width * height].astype(bool)
    mask = mask.reshape(height, width)
    values = np.frombuffer(zlib.decompress(pixels_blob), dtype="<u2").astype(np.int32)
    assert int(mask.sum()) == values.size, (int(mask.sum()), values.size)
    canvas = np.asarray(base.convert("RGB"), dtype="int32").copy()
    region = canvas[y:y + height, x:x + width]
    red = ((values >> 11) & 31) << 3
    green = ((values >> 5) & 63) << 2
    blue = (values & 31) << 3
    region[mask] = np.stack([red, green, blue], axis=1)
    canvas[y:y + height, x:x + width] = region
    return Image.fromarray(canvas.astype("uint8"))


# --------------------------------------------------------------------------
# 打包
# --------------------------------------------------------------------------


@dataclass
class BuildStats:
    groups: dict[str, dict[str, int]] = field(default_factory=dict)

    def add(self, label: str, source: int, stored: int, entries: int) -> None:
        row = self.groups.setdefault(label, {"source": 0, "stored": 0, "entries": 0})
        row["source"] += source
        row["stored"] += stored
        row["entries"] += entries


class PackBuilder:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.assets: list[Asset] = []
        self.stats = BuildStats()
        self.quality: list[tuple[str, float, float | None]] = []
        self.alias_quality: list[float] = []      # SD 别名:放错图的观感代价
        self.diff_choice: dict[str, int] = {}     # CG 变体最终用了哪种编码

    # -- 素材加载 ---------------------------------------------------------
    def load(self, source: Path, refs: dict[tuple[int, str], int], drop: set[str]) -> None:
        self.add_backgrounds(source, drop)
        self.add_sprites(source, drop)
        self.add_event_images(source, refs, drop)
        self.add_effects(source, drop)

    def target_bytes_for(self, source_bytes: int, source_pixels: int) -> int:
        """等比缩小后应占的体积:像素少了,同样的观感就该占更少字节。"""
        ratio = (self.args.screen_w * self.args.screen_h) / max(1, source_pixels)
        return max(1024, int(source_bytes * ratio * self.args.quality_scale))

    def add_backgrounds(self, source: Path, drop: set[str]) -> None:
        for path in sorted((source / "bg").glob("*.jpg")):
            asset = self.new_asset(path.stem, POOL_BG, KIND_BG, path, drop)
            if asset is None:
                continue
            with Image.open(path) as raw:
                image = cover(raw.convert("RGB"), self.args.screen_w, self.args.screen_h, self.args.crop_bias)
            target = self.target_bytes_for(path.stat().st_size, SRC_FULL_W * SRC_FULL_H)
            quality = matched_quality(image, target)
            asset.payload = jpeg_bytes(image, quality)
            asset.w, asset.h = image.size
            asset.note = f"cover {image.width}x{image.height} q{quality}"
            if self.args.verify:
                with Image.open(io.BytesIO(asset.payload)) as stored:
                    asset.verify = mean_abs_error(stored, image)
            self.finish(asset, "背景")

    def add_sprites(self, source: Path, drop: set[str]) -> None:
        for path in sorted((source / "ch").glob("*.png")):
            asset = self.new_asset(path.stem, POOL_CH, KIND_SPRITE, path, drop)
            if asset is None:
                continue
            with Image.open(path) as raw:
                image = raw.convert("RGBA")
            image = scale_to_height(image, self.args.sprite_max_h)
            asset.payload = palettize(image, self.args.sprite_colors)
            asset.w, asset.h = image.size
            asset.note = f"{image.width}x{image.height} 调色板 PNG"
            if self.args.verify:
                with Image.open(io.BytesIO(asset.payload)) as stored:
                    asset.verify = mean_abs_error(stored, image)
            self.finish(asset, "立绘")

    def add_event_images(self, source: Path, refs: dict[tuple[int, str], int], drop: set[str]) -> None:
        cg_groups: dict[str, list[str]] = {}
        sd_groups: dict[str, list[str]] = {}
        for path in sorted((source / "ev").glob("*")):
            stem = path.stem
            match = SD_RE.match(stem)
            if stem.startswith("sd") and match:
                sd_groups.setdefault("sd" + match.group(1), []).append(stem)
            elif EV_RE.match(stem) and stem.startswith("ev"):
                cg_groups.setdefault("ev" + EV_RE.match(stem).group(1), []).append(stem)
        self.build_sd(source, sd_groups, refs, drop)
        self.build_cg(source, cg_groups, refs, drop)

    def build_sd(self, source: Path, groups: dict[str, list[str]], refs: dict[tuple[int, str], int], drop: set[str]) -> None:
        for group, members in sorted(groups.items()):
            kept = [name for name in members if name not in drop]
            if not kept:
                for name in members:
                    self.new_asset(name, POOL_EV, KIND_MISSING, source / "ev" / f"{name}.png", drop, force_missing=True)
                continue
            order = sorted(kept, key=lambda name: (-refs.get((POOL_EV, name), 0), name))
            thumbs: dict[str, "Image.Image"] = {}
            # 缩略图同时用于选中心张与最近邻映射:一张 SD 才 336x201,不必省这点算力
            for name in order:
                with Image.open(source / "ev" / f"{name}.png") as raw:
                    thumbs[name] = flatten(raw).resize((48, 29), Image.LANCZOS)
            retained = self.retain_sd(order, refs, thumbs)
            for name in sorted(members):
                path = source / "ev" / f"{name}.png"
                asset = self.new_asset(name, POOL_EV, KIND_SD, path, drop)
                if asset is None:
                    continue
                if name in retained:
                    self.encode_sd(asset, path)
                else:
                    asset.alias = True
                    asset.base_name = nearest_of(name, retained, thumbs) if self.args.sd_alias == "nearest" else sorted(retained)[0]
                    asset.note = f"变体→{asset.base_name}"
                    if self.args.verify:
                        wanted = sd_display_reference(path, self.args)
                        shown = sd_display_reference(source / "ev" / f"{asset.base_name}.png", self.args)
                        self.alias_quality.append(mean_abs_error(wanted, shown))
                self.finish(asset, "SD 装饰")

    def retain_sd(self, order: list[str], refs: dict[tuple[int, str], int], thumbs: dict[str, "Image.Image"]) -> set[str]:
        """要保留哪些 SD:基准张必有(默认选加权中心张);再按引用次数/覆盖率多留。"""
        base = weighted_medoid(order, refs, thumbs)
        retained = {base}
        if self.args.sd_keep_all:
            return set(order)
        if self.args.sd_min_refs > 1:
            retained.update(name for name in order if refs.get((POOL_EV, name), 0) >= self.args.sd_min_refs)
            return retained
        coverage = self.args.sd_keep_coverage
        if coverage <= 0:
            return retained
        total = sum(refs.get((POOL_EV, name), 0) for name in order) or 1
        counted = 0
        for name in order:
            if name in retained:
                continue
            retained.add(name)
            counted += refs.get((POOL_EV, name), 0)
            if counted >= coverage * total:
                break
        return retained

    def encode_sd(self, asset: Asset, path: Path) -> None:
        with Image.open(path) as raw:
            image = raw.convert("RGBA")
        if self.args.sd_crop:
            image = image.crop(content_box(image))
        width, height = image.size
        target = (self.args.sd_width, max(1, round(height * self.args.sd_width / width)))
        if target[1] > self.args.sd_height:
            target = (max(1, round(width * self.args.sd_height / height)), self.args.sd_height)
        if target != image.size:
            image = image.resize(target, Image.LANCZOS)
        asset.payload = palettize(image, self.args.sprite_colors)
        asset.w, asset.h = image.size
        if self.args.verify:
            with Image.open(io.BytesIO(asset.payload)) as stored:
                asset.verify = mean_abs_error(stored, image)

    def build_cg(self, source: Path, groups: dict[str, list[str]], refs: dict[tuple[int, str], int], drop: set[str]) -> None:
        for group, members in sorted(groups.items()):
            kept = [name for name in members if name not in drop]
            if not kept:
                for name in members:
                    self.new_asset(name, POOL_EV, KIND_MISSING, source / "ev" / f"{name}.jpg", drop, force_missing=True)
                continue
            base_name = pick_base(kept, refs, POOL_EV)
            base_path = source / "ev" / f"{base_name}.jpg"
            with Image.open(base_path) as raw:
                base_image = cover(raw.convert("RGB"), self.args.screen_w, self.args.screen_h, self.args.crop_bias)
            base_target = self.target_bytes_for(base_path.stat().st_size, SRC_FULL_W * SRC_FULL_H)
            base_quality = matched_quality(base_image, base_target)
            base_565 = to565(base_image)
            for name in sorted(members):
                path = source / "ev" / f"{name}.jpg"
                asset = self.new_asset(name, POOL_EV, KIND_CG, path, drop)
                if asset is None:
                    continue
                if name == base_name:
                    asset.payload = jpeg_bytes(base_image, base_quality)
                    asset.w, asset.h = base_image.size
                    asset.note = f"基准张 q{base_quality}"
                    self.diff_choice["CG 基准"] = self.diff_choice.get("CG 基准", 0) + 1
                    self.finish(asset, "事件 CG")
                    continue
                with Image.open(path) as raw:
                    other = cover(raw.convert("RGB"), self.args.screen_w, self.args.screen_h, self.args.crop_bias)
                full = jpeg_bytes(other, base_quality)
                change = change_mask(base_565, to565(other), self.args.diff_threshold)
                patch = encode_patch(*change) if change else b""
                use_patch = bool(change) and len(patch) <= len(full) * self.args.max_patch_ratio
                asset.w, asset.h = base_image.size
                if use_patch:
                    rect, patch_mask, _ = change
                    asset.payload = patch
                    asset.kind = KIND_CG_DIFF
                    asset.rect = rect
                    asset.base_name = base_name
                    asset.note = f"掩码补丁 {rect[2]}x{rect[3]}@{rect[0]},{rect[1]} ({100.0 * patch_mask.mean():.1f}% 像素)"
                    self.diff_choice["掩码补丁"] = self.diff_choice.get("掩码补丁", 0) + 1
                    if self.args.verify:
                        composited = apply_patch(base_image, asset.payload, rect)
                        asset.verify = mean_abs_error(composited, other)
                        asset.verify_base = mean_abs_error(base_image, other)
                else:
                    asset.payload = full
                    asset.note = "整张(补丁不划算)" if change else "整张(与基准相同)"
                    self.diff_choice["整张"] = self.diff_choice.get("整张", 0) + 1
                self.finish(asset, "事件 CG")

    def add_effects(self, source: Path, drop: set[str]) -> None:
        for path in sorted((source / "ev").glob("*.png")):
            stem = path.stem
            if stem.startswith("sd") and SD_RE.match(stem):
                continue
            asset = self.new_asset(stem, POOL_EV, KIND_EFFECT, path, drop)
            if asset is None:
                continue
            with Image.open(path) as raw:
                image = cover(raw.convert("RGBA"), self.args.screen_w, self.args.screen_h, self.args.crop_bias)
            asset.payload = palettize(image, self.args.sprite_colors)
            asset.w, asset.h = image.size
            asset.note = "cover 到屏幕"
            if self.args.verify:
                with Image.open(io.BytesIO(asset.payload)) as stored:
                    asset.verify = mean_abs_error(stored, image)
            self.finish(asset, "特效/道具/画面")

    # -- 条目管理 ---------------------------------------------------------
    def new_asset(self, name: str, pool: int, kind: int, path: Path, drop: set[str], force_missing: bool = False) -> Asset | None:
        if name in drop or force_missing:
            asset = Asset(name=name, pool=pool, kind=KIND_MISSING,
                          source_bytes=path.stat().st_size if path.is_file() else 0,
                          note="已在 --drop-file 中排除" if name in drop else "整组被排除")
            self.assets.append(asset)
            return None
        return Asset(name=name, pool=pool, kind=kind, source_bytes=path.stat().st_size)

    def finish(self, asset: Asset, label: str) -> None:
        self.assets.append(asset)
        self.stats.add(label, asset.source_bytes, asset.stored_bytes, 1)
        if asset.verify is not None:
            self.quality.append((f"{label} {asset.name}", asset.verify, asset.verify_base))

    # -- 序列化 -----------------------------------------------------------
    def serialize(self, meta: dict[str, str]) -> bytes:
        ordered = sorted(self.assets, key=lambda asset: (asset.pool, asset.name))
        names = bytearray()
        name_refs: dict[str, tuple[int, int]] = {}
        for asset in ordered:
            if asset.name in name_refs:
                continue
            encoded = asset.name.encode("utf-8")
            name_refs[asset.name] = (len(names), len(encoded))
            names += encoded + b"\x00"
        index_of = {(asset.pool, asset.name): position for position, asset in enumerate(ordered)}
        for asset in ordered:
            # 别名条目自带 0 尺寸:显示尺寸一律取基准条目,固件不需要再查
            if asset.alias and asset.base_name and not (asset.w and asset.h):
                reference = ordered[index_of[(asset.pool, asset.base_name)]]
                asset.w, asset.h = reference.w, reference.h
        # 名字空间参与定位:画面_白 / 画面_黒 在背景与事件图两个名字空间都存在
        missing = [(asset.name, asset.base_name) for asset in ordered
                   if asset.base_name and (asset.pool, asset.base_name) not in index_of]
        if missing:
            sys.exit(f"ERROR: 基准名不存在: {missing[:3]}")

        blobs = bytearray()
        records = bytearray()
        for asset in ordered:
            offset = len(blobs)
            blobs += asset.payload
            name_off, name_len = name_refs[asset.name]
            base = index_of[(asset.pool, asset.base_name)] if asset.base_name else 0xFFFF
            flags = FLAG_ALIAS if asset.alias else 0
            dx, dy, dw, dh = asset.rect or (0, 0, 0, 0)
            try:
                records += ENTRY.pack(
                    name_off, name_len, asset.kind, asset.pool, base,
                    asset.w, asset.h, dx, dy, dw, dh,
                    offset, len(asset.payload), flags, 0,
                )
            except struct.error as exc:
                sys.exit(
                    f"ERROR: 条目 {asset.name}({KIND_NAMES.get(asset.kind, asset.kind)}) 字段越界: {exc}\n"
                    f"  name_off={name_off} name_len={name_len} pool={asset.pool} base={base} "
                    f"w={asset.w} h={asset.h} rect={asset.rect} payload={len(asset.payload)}"
                )

        meta_blob = "\n".join(f"{key}={value}" for key, value in sorted(meta.items())).encode("utf-8") + b"\n"
        sections = (
            (SEC_NAME, bytes(names), len(ordered), len(names)),
            (SEC_ASSET, bytes(records), len(ordered), len(records)),
            (SEC_BLOB, bytes(blobs), len(blobs), len(blobs)),
            (SEC_META, meta_blob, 0, len(meta_blob)),
        )
        header_size = HEADER.size + SECTION.size * len(sections)
        offset = header_size
        table = bytearray()
        for section_type, body, count, size in sections:
            table += SECTION.pack(section_type, offset, count, size)
            offset += len(body) + (-len(body) % 4)   # 段按 4 字节对齐
        total = offset
        head = HEADER.pack(MAGIC, VERSION, header_size, len(sections), 0, total)
        chunks = [head, bytes(table)]
        for _, body, _, _ in sections:
            chunks.append(body)
            chunks.append(b"\x00" * (-len(body) % 4))
        return b"".join(chunks)

    # -- 报告 -------------------------------------------------------------
    def report(self, pack: bytes, out_path: Path, meta: dict[str, str]) -> None:
        log("")
        log(f"{'分类':<12}{'条目':>6}{'源体积':>12}{'包内体积':>12}{'变化':>9}")
        total_source = total_stored = 0
        for label, row in self.stats.groups.items():
            delta = row["stored"] - row["source"]
            log(f"{label:<12}{row['entries']:>6}{human(row['source']):>12}{human(row['stored']):>12}{delta / 1048576:>+8.2f}M")
            total_source += row["source"]
            total_stored += row["stored"]
        log(f"{'素材合计':<12}{len(self.assets):>6}{human(total_source):>12}{human(total_stored):>12}{(total_stored - total_source) / 1048576:>+8.2f}M")
        log(f"{'包总大小':<12}{'':>6}{'':>12}{human(len(pack)):>12}")
        if self.diff_choice:
            log("CG 变体编码选择: " + ", ".join(f"{key} {value} 张" for key, value in sorted(self.diff_choice.items())))
        if self.quality:
            diffs = [item for item in self.quality if item[2] is not None]
            plain = [item for item in self.quality if item[2] is None]
            if plain:
                values = sorted(item[1] for item in plain)
                log(f"质量自检: {len(plain)} 张转码图,与源图平均绝对误差 均值 {sum(values) / len(values):.2f} / 最大 {values[-1]:.2f}(0..255)")
            if diffs:
                patch = sorted(item[1] for item in diffs)
                base = sorted(item[2] for item in diffs)
                log(f"质量自检: {len(diffs)} 张 CG 差分,合成后误差 均值 {sum(patch) / len(patch):.2f};"
                    f"只贴基准图不贴补丁 均值 {sum(base) / len(base):.2f}(补丁把误差降到 {100 * sum(patch) / sum(base):.0f}%)")
        if self.alias_quality:
            values = sorted(self.alias_quality)
            log(f"质量代价: {len(values)} 条 SD 引用会显示同组另一张(每组只留 1 张),与剧本想要的变体平均差 "
                f"{sum(values) / len(values):.2f}/255,最差 {values[-1]:.2f};想降低就用 --sd-keep-coverage 多留,或 --sd-keep-all")
        budget = self.args.budget_content_mb * 1048576 - self.args.script_pack_mb * 1048576
        log("")
        log(f"预算: 内容可用 {self.args.budget_content_mb:.2f} MB − 脚本包 {self.args.script_pack_mb:.2f} MB"
            f" = 图片 {budget / 1048576:.2f} MB")
        log(f"图片包 {human(len(pack))} → {'放得下,余 ' + human(int(budget - len(pack))) if len(pack) <= budget else '超出 ' + human(int(len(pack) - budget))}")
        log(f"写出 {out_path}")
        if self.args.json:
            payload = {
                "meta": meta,
                "entries": [
                    {
                        "name": asset.name, "pool": asset.pool, "kind": KIND_NAMES[asset.kind],
                        "w": asset.w, "h": asset.h, "bytes": len(asset.payload),
                        "base": asset.base_name, "rect": asset.rect, "note": asset.note,
                    }
                    for asset in sorted(self.assets, key=lambda item: (item.pool, item.name))
                ],
            }
            Path(self.args.json).parent.mkdir(parents=True, exist_ok=True)
            Path(self.args.json).write_text(json.dumps(payload, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
            log(f"写出清单 {self.args.json}")


# --------------------------------------------------------------------------
# 自检 / 核验
# --------------------------------------------------------------------------


def load_pack(path: Path) -> tuple[bytes, dict[int, tuple[int, int, int]], list[dict]]:
    """解出 (原始字节, 段表, 条目表);条目字段全部保留供上层使用。"""
    raw = path.read_bytes()
    if len(raw) < HEADER.size:
        sys.exit(f"ERROR: {path} 太短")
    magic, version, header_size, section_count, _, total = HEADER.unpack_from(raw)
    if magic != MAGIC:
        sys.exit(f"ERROR: {path} 魔数不符({magic!r})")
    if total != len(raw):
        sys.exit(f"ERROR: 头里写的 {total} 字节,实际 {len(raw)}")
    sections: dict[int, tuple[int, int, int]] = {}
    for index in range(section_count):
        section_type, offset, count, size = SECTION.unpack_from(raw, HEADER.size + SECTION.size * index)
        sections[section_type] = (offset, count, size)
    names_off, _, names_size = sections[SEC_NAME]
    asset_off, asset_count, _ = sections[SEC_ASSET]
    names = raw[names_off:names_off + names_size]
    entries: list[dict] = []
    for index in range(asset_count):
        (name_off, name_len, kind, pool, base, w, h, dx, dy, dw, dh,
         data_off, data_len, flags, _) = ENTRY.unpack_from(raw, asset_off + ENTRY.size * index)
        entries.append(dict(
            name=names[name_off:name_off + name_len].decode("utf-8"), kind=kind, pool=pool, base=base,
            w=w, h=h, dx=dx, dy=dy, dw=dw, dh=dh, data_off=data_off, data_len=data_len, flags=flags,
        ))
    return raw, sections, entries


def payload_of(raw: bytes, sections: dict[int, tuple[int, int, int]], entry: dict) -> bytes:
    blob_off = sections[SEC_BLOB][0]
    return raw[blob_off + entry["data_off"]:blob_off + entry["data_off"] + entry["data_len"]]


def payload_matches_source(raw: bytes, sections: dict[int, tuple[int, int, int]], entry: dict, source_path: Path) -> bool:
    """载荷与源文件逐字节一致(只有原样搬运的条目会命中)。"""
    if not entry["data_len"]:
        return False
    return payload_of(raw, sections, entry) == source_path.read_bytes()


def check_pack(path: Path) -> int:
    raw, sections, entries = load_pack(path)
    asset_count = len(entries)
    errors: list[str] = []
    for required in (SEC_NAME, SEC_ASSET, SEC_BLOB, SEC_META):
        if required not in sections:
            errors.append(f"缺段 {required}")
    if errors:
        log("自检: FAIL")
        for line in errors:
            log("  " + line)
        return 1
    names_off, count, names_size = sections[SEC_NAME]
    blob_off, _, blob_size = sections[SEC_BLOB]
    kinds: dict[str, int] = {}
    for entry in entries:
        name, kind, base, flags = entry["name"], entry["kind"], entry["base"], entry["flags"]
        data_off, data_len = entry["data_off"], entry["data_len"]
        kinds[KIND_NAMES.get(kind, f"?{kind}")] = kinds.get(KIND_NAMES.get(kind, f"?{kind}"), 0) + 1
        if data_off + data_len > blob_size:
            errors.append(f"{name}: 载荷越界")
        start = raw[blob_off + data_off:blob_off + data_off + 4]
        if kind in (KIND_BG, KIND_CG) and data_len and start[:1] != b"\xff":
            errors.append(f"{name}: 不是 JPEG")
        if kind in (KIND_SPRITE, KIND_SD, KIND_EFFECT) and data_len and start != b"\x89PNG":
            errors.append(f"{name}: 不是 PNG")
        if kind == KIND_MISSING and data_len:
            errors.append(f"{name}: MISSING 条目不应该有载荷")
        if kind == KIND_CG_DIFF:
            if not entry["dw"] or not entry["dh"]:
                errors.append(f"{name}: 补丁矩形为空")
            elif data_len < 8:
                errors.append(f"{name}: 补丁载荷太短")
            else:
                mask_len, pixels_len = struct.unpack_from("<II", raw, blob_off + data_off)
                if 8 + mask_len + pixels_len != data_len:
                    errors.append(f"{name}: 补丁载荷长度对不上(头里 {mask_len}+{pixels_len}+8,实际 {data_len})")
                else:
                    width, height = entry["dw"], entry["dh"]
                    expected_mask = (width * height + 7) // 8
                    mask = zlib.decompress(raw[blob_off + data_off + 8:blob_off + data_off + 8 + mask_len])
                    pixels = zlib.decompress(raw[blob_off + data_off + 8 + mask_len:])
                    if len(mask) != expected_mask:
                        errors.append(f"{name}: 掩码字节数 {len(mask)} != {expected_mask}")
                    else:
                        marked = sum(bin(byte).count("1") for byte in mask)
                        if len(pixels) != marked * 2:
                            errors.append(f"{name}: 像素数 {len(pixels) // 2} != 掩码置位数 {marked}")
    # 跨条目校验放第二趟:基准条目可能排在变体后面(基准 = 剧本引用最多的那张)
    for entry in entries:
        name, kind, base, flags = entry["name"], entry["kind"], entry["base"], entry["flags"]
        if flags & FLAG_ALIAS:
            if base >= asset_count:
                errors.append(f"{name}: 别名基准越界")
            else:
                reference = entries[base]
                if reference["kind"] not in (KIND_SPRITE, KIND_SD, KIND_EFFECT):
                    errors.append(f"{name}: 别名基准类型不对({KIND_NAMES.get(reference['kind'])}）")
                elif reference["flags"] & FLAG_ALIAS:
                    errors.append(f"{name}: 别名基准指向了另一个别名")
            if entry["data_len"]:
                errors.append(f"{name}: 别名条目不应该有载荷")
        if kind == KIND_CG_DIFF:
            if flags & FLAG_ALIAS:
                errors.append(f"{name}: 差分条目同时被标成别名")
            if base >= asset_count:
                errors.append(f"{name}: 差分基准越界")
            else:
                reference = entries[base]
                if reference["kind"] != KIND_CG:
                    errors.append(f"{name}: 差分基准不是完整 CG")
                elif entry["dx"] + entry["dw"] > reference["w"] or entry["dy"] + entry["dh"] > reference["h"]:
                    errors.append(f"{name}: 补丁超出基准画面 {reference['w']}x{reference['h']}")
    if asset_count != count:
        errors.append(f"条目数不符: 段里 {count},表里 {asset_count}")
    log(f"自检: {'PASS' if not errors else 'FAIL'}  {path.name} {len(raw)} 字节,"
        f"{asset_count} 条目,名称表 {human(names_size)},载荷 {human(blob_size)}")
    log("  条目类型: " + ", ".join(f"{key}×{value}" for key, value in sorted(kinds.items())))
    for line in errors[:10]:
        log("  " + line)
    return 1 if errors else 0


def source_index(source: Path) -> dict[tuple[int, str], Path]:
    """源素材表:(名字空间, 名字) → 源文件路径。"""
    index: dict[tuple[int, str], Path] = {}
    for path in sorted((source / "bg").glob("*.jpg")):
        index[(POOL_BG, path.stem)] = path
    for path in sorted((source / "ch").glob("*.png")):
        index[(POOL_CH, path.stem)] = path
    for path in sorted((source / "ev").glob("*")):
        index[(POOL_EV, path.stem)] = path
    return index


def stored_image(raw: bytes, sections: dict[int, tuple[int, int, int]], entries: list[dict], entry: dict, depth: int = 0) -> "Image.Image":
    """按 pack 自身的信息还原一条资产(别名取基准、差分合成补丁)。"""
    if depth > 3:
        sys.exit("ERROR: 别名/差分引用成环")
    if entry["flags"] & FLAG_ALIAS:
        return stored_image(raw, sections, entries, entries[entry["base"]], depth + 1)
    data = payload_of(raw, sections, entry)
    if entry["kind"] == KIND_CG_DIFF:
        base = stored_image(raw, sections, entries, entries[entry["base"]], depth + 1)
        return apply_patch(base, data, (entry["dx"], entry["dy"], entry["dw"], entry["dh"]))
    return Image.open(io.BytesIO(data))


def expected_source(image: "Image.Image", source_path: Path, kind: int, args: argparse.Namespace) -> "Image.Image":
    """对比基准:把源素材按转换规则重放一遍(独立核验用)。"""
    with Image.open(source_path) as raw:
        mode = "RGBA" if raw.mode in ("P", "LA", "RGBA") else "RGB"
        reference = raw.convert(mode)
    if kind == KIND_SPRITE:
        return scale_to_height(reference, image.height)
    if kind == KIND_SD and args.sd_crop:
        cropped = reference.crop(content_box(reference))
        return cropped if cropped.size == image.size else cropped.resize(image.size, Image.LANCZOS)
    if kind in (KIND_BG, KIND_CG, KIND_CG_DIFF, KIND_EFFECT):
        return cover(reference, args.screen_w, args.screen_h, args.crop_bias)
    return reference


def compare_pack(args: argparse.Namespace) -> int:
    pack_path = Path(args.compare)
    raw, sections, entries = load_pack(pack_path)
    source = Path(args.source)
    index = source_index(source)
    rows: dict[str, list[float]] = {}
    identity: dict[str, int] = {}
    unmatched: list[str] = []
    for entry in entries:
        label = KIND_NAMES.get(entry["kind"], str(entry["kind"]))
        if entry["flags"] & FLAG_ALIAS:
            label = "SD 别名"
        elif label == "CG_DIFF":
            label = "CG 差分"
        elif label == "CG":
            label = "CG 基准"
        elif label == "SD":
            label = "SD 基准"
        source_path = index.get((entry["pool"], entry["name"]))
        if source_path is None:
            unmatched.append(f"{label} {entry['name']}")
            continue
        stored = stored_image(raw, sections, entries, entry)
        reference = expected_source(stored, source_path, entry["kind"], args)
        if payload_matches_source(raw, sections, entry, source_path):
            identity[label] = identity.get(label, 0) + 1
        rows.setdefault(label, []).append(mean_abs_error(stored, reference))
    log(f"独立核验: {pack_path.name}  {len(entries)} 条目,源素材 {len(index)} 个")
    log(f"{'分类':<14}{'条目':>6}{'逐字节一致':>10}{'平均误差':>10}{'最大误差':>10}")
    for label in sorted(rows, key=lambda name: -len(rows[name])):
        values = rows[label]
        log(f"{label:<14}{len(values):>6}{identity.get(label, 0):>10}"
            f"{sum(values) / len(values):>10.2f}{max(values):>10.2f}")
    total = [value for values in rows.values() for value in values]
    log(f"{'合计':<14}{len(total):>6}{sum(identity.values()):>10}{sum(total) / len(total):>10.2f}{max(total):>10.2f}")
    if unmatched:
        log(f"源目录里找不到 {len(unmatched)} 条: {unmatched[:5]}")
    return 0


# --------------------------------------------------------------------------
# 入口
# --------------------------------------------------------------------------


def build(args: argparse.Namespace) -> int:
    source = Path(args.source)
    if not source.is_dir():
        sys.exit(f"ERROR: 源素材目录不存在: {source}\n先跑: python tools/senren_fetch_source.py --dest {source} --chunks")
    script_dir = Path(args.script_dir) if args.script_dir else source / "scn"
    refs = read_refs(script_dir if script_dir.is_dir() else None)
    if not refs:
        log(f"警告: 没有读到剧本({script_dir}),基准张改为按字典序选择,SD/CG 差分的收益会略低")
    drop: set[str] = set()
    if args.drop_file:
        drop = {line.strip() for line in Path(args.drop_file).read_text(encoding="utf-8").splitlines()
                if line.strip() and not line.startswith("#")}
        log(f"--drop-file: 排除 {len(drop)} 个名字")

    builder = PackBuilder(args)
    builder.load(source, refs, drop)
    if not builder.assets:
        sys.exit("ERROR: 没有找到任何素材")
    meta = {
        "source_repo": "https://github.com/hrk666666/Senren-Banka-MiBand-10",
        "content_repo": "https://github.com/hezdaa/qlwh-mibandported",
        "commit": args.commit or source_ref(source),
        "screen": f"{args.screen_w}x{args.screen_h}",
        "crop_bias": args.crop_bias,
        "sprite_max_h": str(args.sprite_max_h),
        "sprite_colors": str(args.sprite_colors),
        "sd_width": str(args.sd_width),
        "sd_height": str(args.sd_height),
        "sd_keep_coverage": str(args.sd_keep_coverage),
        "sd_min_refs": str(args.sd_min_refs),
        "sd_alias": args.sd_alias,
        "diff_threshold": str(args.diff_threshold),
        "max_patch_ratio": str(args.max_patch_ratio),
        "entries": str(len(builder.assets)),
    }
    pack = builder.serialize(meta)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(pack)
    builder.report(pack, out_path, meta)
    return check_pack(out_path)


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", default="build/senren-source", help="源素材目录(默认 build/senren-source)")
    parser.add_argument("--out", default="build/senren-pack/senren_pack.bin", help="输出 pack 路径")
    parser.add_argument("--check", metavar="PACK", help="只校验一个已生成的 pack")
    parser.add_argument("--compare", metavar="PACK", help="独立核验:只读 pack 还原每条资产,与 --source 比误差")
    parser.add_argument("--json", help="额外写出条目清单 JSON")
    parser.add_argument("--commit", default="", help="源仓库 commit(记录到元数据)")
    parser.add_argument("--script-dir", help="剧本目录(默认 <source>/scn;用于选基准张)")
    parser.add_argument("--drop-file", help="要排除的素材名字清单(一行一个,# 注释)")
    parser.add_argument("--screen-w", type=int, default=240, help="屏幕宽(默认 240)")
    parser.add_argument("--screen-h", type=int, default=320, help="屏幕高(默认 320)")
    parser.add_argument("--crop-bias", choices=("top", "center", "bottom"), default="top",
                        help="cover 缩放后裁哪边(默认 top:裁掉下方,那一块压在正文带下面)")
    parser.add_argument("--quality-scale", type=float, default=1.0,
                        help="JPEG 目标体积系数(默认 1.0 = 按像素数等比折算源体积)")
    parser.add_argument("--sprite-max-h", type=int, default=280, help="立绘最大高度(默认 280)")
    parser.add_argument("--sprite-colors", type=int, default=255, help="立绘/SD/特效调色板颜色数(默认 255)")
    parser.add_argument("--sd-width", type=int, default=240, help="SD 装饰宽度(默认 240)")
    parser.add_argument("--sd-height", type=int, default=144, help="SD 装饰高度上限(默认 144)")
    parser.add_argument("--sd-crop", action="store_true", default=True, help="SD 裁到内容包围盒(默认开)")
    parser.add_argument("--no-sd-crop", dest="sd_crop", action="store_false", help="SD 不裁,整幅缩到显示框")
    parser.add_argument("--sd-keep-all", action="store_true", help="SD 全部保留(默认每组只留基准张)")
    parser.add_argument("--sd-min-refs", type=int, default=1,
                        help="连同引用次数 >= N 的 SD 变体一起保留(默认 1 = 只留基准张)")
    parser.add_argument("--sd-keep-coverage", type=float, default=0.0,
                        help="按引用覆盖率保留 SD 变体(0=只留基准张,0.8=保留覆盖 80%% 引用的那些)")
    parser.add_argument("--sd-alias", choices=("nearest", "base"), default="nearest",
                        help="被丢弃的 SD 变体指向哪张(默认 nearest=像素最接近的保留张)")
    parser.add_argument("--diff-threshold", type=int, default=48,
                        help="CG 差分判定像素变化的通道差阈值(默认 48,低于它的算 JPEG 噪声)")
    parser.add_argument("--max-patch-ratio", type=float, default=0.85,
                        help="补丁相对整张 JPEG 的体积上限,超过就整张存(默认 0.85)")
    parser.add_argument("--verify", action="store_true", default=True, help="做转码/差分的误差自检(默认开)")
    parser.add_argument("--no-verify", dest="verify", action="store_false", help="跳过误差自检")
    parser.add_argument("--budget-content-mb", type=float, default=6.87, help="可用内容预算 MB(默认 6.87)")
    parser.add_argument("--script-pack-mb", type=float, default=1.16, help="脚本包体积 MB(默认 1.16)")
    args = parser.parse_args(argv)
    if args.check:
        return check_pack(Path(args.check))
    if args.compare:
        return compare_pack(args)
    return build(args)


if __name__ == "__main__":
    raise SystemExit(main())
