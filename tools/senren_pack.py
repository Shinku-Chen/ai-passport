#!/usr/bin/env python3
"""Build the Senren * Banka image pack for the AI Passport port.

Source project: https://github.com/hrk666666/Senren-Banka-MiBand-10
  - 小米手环 10(小米 Vela / aiot 快应用)上的《千恋＊万花》移植版;素材内容来自
    https://github.com/hezdaaa/qlwh-mibandported。
  - 素材版权归 SAGA PLANETS 所有;本仓库只保存转换产物,不分发源素材。
    先用 tools/senren_fetch_source.py 把源素材拉到 build/senren-source/。

The pack is a single little-endian binary read straight out of flash:

  header  : magic "SENRNPK1", version u16, header_size u16, section_count u16,
            reserved u16, total_size u32                     (共 20 字节)
  section : { type u32, offset u32, count u32, size u32 }     (16 字节 × N)
    SEC_NAME  UTF-8 名称 blob(名字以 NUL 结尾连排放置)
    SEC_ASSET 定长 36 字节条目表:
              { name_off u32, name_len u16, kind u8, pool u8, base u16,
                w u16, h u16, dx u16, dy u16, dw u16, dh u16,
                data_off u32, data_len u32, flags u32, reserved u16 }
              data_off 相对 SEC_BLOB 数据区
    SEC_BLOB  载荷区,逐条目一段完整文件字节(PNG / JPEG)
    SEC_META  UTF-8 key=value 文本(来源仓库 / 转换参数),每行一条

  kind : 0=BG 背景 JPEG | 1=SPRITE 立绘 PNG | 2=SD 装饰 PNG | 3=CG 事件图 JPEG
         4=CG_DIFF 事件图差分补丁 JPEG | 5=MISSING 名字占位(不画) | 6=EFFECT 特效/道具/画面 PNG
  pool : 0=背景 1=立绘 2=事件图 —— 对应剧本里的 bg / 立绘 / ev 三个名字空间
  flags: bit0 = 变体映射:本条目没有载荷,画 base 指向的那一条

  CG 差分的画法:把补丁 JPEG 解码后贴到基准图的 (dx, dy),尺寸 (dw, dh)。

素材转换规则(尺寸依据来自源工程):
  背景   92 张 336x480 JPEG            -> 原样(源工程已把它压到极限,重编码只会更大)
  立绘  123 张 紧裁调色板 PNG(高 480)   -> 缩到 --sprite-max-h 高(默认 280;UI 用同一高度 1:1 画,
                                          别放大,否则会发软) -> RGBA 量化成 <=255 色调色板 PNG
  立绘组的 _1/_2/_3 是同一套衣服的不同姿势,不是表情差分,所以逐张存,不做差分。
  SD    216 张 内容固定 336x201        -> 默认每组只留 1 张(剧本引用最多的);丢掉的变体指向组里
                                          跟它像素最接近的保留张(同姿势优先),而不是一律指向基准;
                                          --sd-keep-coverage 可按引用覆盖率多留几张
                                          -> 保留张裁到内容 -> 缩到 240x144(源工程 .sd-image 的显示框)
  CG    570 张 336x480 JPEG            -> 每组留引用最多的 1 张原样;
                                          组内差异 <= --cg-diff-max-pct 的其余张改成补丁
                                          (变化矩形按 8px 对齐 + 2px 外扩,JPEG 质量按源体积标定);
                                          差异超过阈值的组原样逐张存(那是不同画面,差分不划算)
  其它  28 张 特效 / 道具 / 画面 PNG     -> 原样(按原调色板重存,无损)

Usage:
  python tools/senren_fetch_source.py --dest build/senren-source --chunks
  python tools/senren_pack.py --source build/senren-source --out build/senren-pack/senren_pack.bin
  python tools/senren_pack.py --check build/senren-pack/senren_pack.bin

出版本注意:本工具只做资源包。R18 事件图用 --drop-file 列名字排除(社区投稿版本
走这个开关),被排除的名字会保留为 MISSING 占位,剧本引用它时跳过绘制而不是报错。
"""

from __future__ import annotations

import argparse
import io
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

try:
    from PIL import Image
except ImportError:  # pragma: no cover - 环境问题
    sys.exit("需要 Pillow: python -m pip install pillow")

MAGIC = b"SENRNPK1"
VERSION = 1
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

# 源素材的固定尺寸(与 bsp 无关,是源工程的画布)
SRC_W, SRC_H = 336, 480
SD_CONTENT_ASPECT = 336 / 201

SD_RE = re.compile(r"^sd(\d+)([a-z])([a-z])$")
EV_RE = re.compile(r"^ev(\d+)([a-z]*)$")


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def human(count: int) -> str:
    if abs(count) >= 1048576:
        return f"{count / 1048576:.2f} MB"
    return f"{count / 1024:.1f} KB"


# --------------------------------------------------------------------------
# 资产条目
# --------------------------------------------------------------------------


@dataclass
class Asset:
    name: str                       # 剧本里引用的名字(不含扩展名)
    pool: int
    kind: int
    payload: bytes = b""
    w: int = 0
    h: int = 0
    base_name: str | None = None    # 变体映射 / 差分补丁指向的基准名字
    rect: tuple[int, int, int, int] | None = None   # 差分补丁:dx, dy, dw, dh
    source_bytes: int = 0           # 源文件体积(统计用)
    note: str = ""
    verify: float | None = None     # 与源素材的平均绝对误差(质量自检)
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
    best = None
    best_error = None
    source = thumbs.get(name)
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
# 图像转换
# --------------------------------------------------------------------------


def png_bytes(image: "Image.Image") -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, "PNG", optimize=True)
    return buffer.getvalue()


def palettize(image: "Image.Image", colors: int) -> bytes:
    """量化成 <=colors 色调色板 PNG;透明通道与颜色一起进调色板(FASTOCTREE)。"""
    converted = image if image.mode == "RGBA" else image.convert("RGBA")
    return png_bytes(converted.quantize(colors=colors, method=Image.FASTOCTREE))


def resave_lossless(image: "Image.Image") -> bytes:
    """源 PNG 原样重存:调色板不变,只让 zlib 再优化一遍(无损)。"""
    return png_bytes(image)


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


def jpeg_size(image: "Image.Image", quality: int) -> int:
    buffer = io.BytesIO()
    image.convert("RGB").save(buffer, "JPEG", quality=quality, optimize=True, subsampling=2)
    return buffer.tell()


def calibrate_quality(image: "Image.Image", target_bytes: int, low: int = 45, high: int = 92) -> int:
    """找与源文件体积相当的 JPEG 质量:补丁用同一质量编码,避免出现明显质量断层。"""
    if jpeg_size(image, high) <= target_bytes:
        return high
    if jpeg_size(image, low) >= target_bytes:
        return low
    lo, hi = low, high
    for _ in range(7):
        mid = (lo + hi) // 2
        if jpeg_size(image, mid) > target_bytes:
            hi = mid
        else:
            lo = mid
    return lo


def diff_rect(base: "Image.Image", other: "Image.Image", threshold: int, pad: int, align: int) -> tuple[int, int, int, int] | None:
    """变化区域矩形;按 align 对齐、pad 外扩,并夹在画布内。没有变化时返回 None。"""
    import numpy as np

    a = np.asarray(base.convert("RGB"), dtype="int16")
    b = np.asarray(other.convert("RGB"), dtype="int16")
    if a.shape != b.shape:
        b = np.asarray(other.convert("RGB").resize(base.size, Image.LANCZOS), dtype="int16")
    mask = np.abs(a - b).max(axis=2) > threshold
    if not mask.any():
        return None
    ys, xs = np.where(mask)
    x0, x1 = int(xs.min()), int(xs.max())
    y0, y1 = int(ys.min()), int(ys.max())
    x0 = max(0, (x0 - pad) // align * align)
    y0 = max(0, (y0 - pad) // align * align)
    x1 = min(base.width - 1, ((x1 + pad) // align + 1) * align - 1)
    y1 = min(base.height - 1, ((y1 + pad) // align + 1) * align - 1)
    return x0, y0, x1 - x0 + 1, y1 - y0 + 1


def diff_percent(base: "Image.Image", other: "Image.Image", threshold: int) -> float:
    import numpy as np

    a = np.asarray(base.convert("RGB"), dtype="int16")
    b = np.asarray(other.convert("RGB"), dtype="int16")
    if a.shape != b.shape:
        b = np.asarray(other.convert("RGB").resize(base.size, Image.LANCZOS), dtype="int16")
    return 100.0 * float((np.abs(a - b).max(axis=2) > threshold).mean())


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
        self.alias_quality: list[float] = []   # SD 变体映射:放错图的观感代价

    # -- 素材加载 ---------------------------------------------------------
    def load(self, source: Path, refs: dict[tuple[int, str], int], drop: set[str]) -> None:
        self.add_backgrounds(source, drop)
        self.add_sprites(source, drop)
        self.add_event_images(source, refs, drop)
        self.add_effects(source, drop)

    def add_backgrounds(self, source: Path, drop: set[str]) -> None:
        for path in sorted((source / "bg").glob("*.jpg")):
            name = path.stem
            asset = self.new_asset(name, POOL_BG, KIND_BG, path, drop)
            if asset is None:
                continue
            asset.payload = path.read_bytes()
            with Image.open(io.BytesIO(asset.payload)) as image:
                asset.w, asset.h = image.size
            self.finish(asset, "背景")

    def add_sprites(self, source: Path, drop: set[str]) -> None:
        for path in sorted((source / "ch").glob("*.png")):
            name = path.stem
            asset = self.new_asset(name, POOL_CH, KIND_SPRITE, path, drop)
            if asset is None:
                continue
            with Image.open(path) as raw:
                image = raw.convert("RGBA")
            max_h = self.args.sprite_max_h
            if image.height > max_h:
                scale = max_h / image.height
                image = image.resize((max(1, round(image.width * scale)), max_h), Image.LANCZOS)
                asset.note = f"缩到 {image.width}x{image.height}"
            else:
                asset.note = "原尺寸"
            asset.w, asset.h = image.size
            asset.payload = palettize(image, self.args.sprite_colors)
            if self.args.verify:
                with Image.open(io.BytesIO(asset.payload)) as stored:
                    asset.verify = mean_abs_error(stored, image)
            self.finish(asset, "立绘")

    def add_event_images(self, source: Path, refs: dict[tuple[int, str], int], drop: set[str]) -> None:
        cg_groups: dict[str, list[str]] = {}
        sd_groups: dict[str, list[str]] = {}
        for path in sorted((source / "ev").glob("*")):
            name = path.stem
            stem = path.stem
            if stem.startswith("sd") and SD_RE.match(stem):
                sd_groups.setdefault("sd" + SD_RE.match(stem).group(1), []).append(name)
            elif EV_RE.match(stem) and stem.startswith("ev"):
                cg_groups.setdefault("ev" + EV_RE.match(stem).group(1), []).append(name)
            else:
                continue
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
        """要保留哪些 SD:基准张必有(默认选加权中心张);--sd-keep-coverage 再按引用累计占比多留。"""
        base = weighted_medoid(order, refs, thumbs)
        retained = {base}
        if self.args.sd_keep_all:
            return set(order)
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
            box = content_box(image)
            image = image.crop(box)
        width, height = image.size
        scale = self.args.sd_width / width
        target = (self.args.sd_width, max(1, round(height * scale)))
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
                base_image = raw.convert("RGB")
            percent = 0.0
            for name in kept:
                if name == base_name:
                    continue
                with Image.open(source / "ev" / f"{name}.jpg") as raw:
                    percent = max(percent, diff_percent(base_image, raw.convert("RGB"), self.args.diff_threshold))
            use_diff = bool(self.args.cg_diff) and percent <= self.args.cg_diff_max_pct and len(kept) > 1
            quality = calibrate_quality(base_image, base_path.stat().st_size) if use_diff else 0
            for name in sorted(members):
                path = source / "ev" / f"{name}.jpg"
                asset = self.new_asset(name, POOL_EV, KIND_CG, path, drop)
                if asset is None:
                    continue
                if name == base_name:
                    asset.payload = path.read_bytes()
                    asset.w, asset.h = base_image.size
                    asset.note = "基准张·原样"
                elif use_diff:
                    with Image.open(path) as raw:
                        other = raw.convert("RGB")
                    rect = diff_rect(base_image, other, self.args.diff_threshold, self.args.patch_pad, self.args.patch_align)
                    if rect is None:
                        asset.payload = path.read_bytes()
                        asset.w, asset.h = other.size
                        asset.note = "无变化·原样"
                    else:
                        dx, dy, dw, dh = rect
                        crop = other.crop((dx, dy, dx + dw, dy + dh))
                        buffer = io.BytesIO()
                        crop.save(buffer, "JPEG", quality=quality, optimize=True, subsampling=2)
                        asset.payload = buffer.getvalue()
                        asset.kind = KIND_CG_DIFF
                        asset.rect = rect
                        asset.base_name = base_name
                        asset.w, asset.h = base_image.size
                        asset.note = f"补丁 {dw}x{dh}@{dx},{dy} q{quality}"
                        if self.args.verify:
                            patch = Image.open(io.BytesIO(asset.payload)).convert("RGB")
                            composited = base_image.copy()
                            composited.paste(patch, (dx, dy))
                            asset.verify = mean_abs_error(composited, other)
                            asset.verify_base = mean_abs_error(base_image, other)
                else:
                    with Image.open(path) as raw:
                        asset.payload = path.read_bytes()
                        asset.w, asset.h = raw.size
                    asset.note = f"原样(组内差异 {percent:.1f}%)"
                self.finish(asset, "事件 CG")

    def add_effects(self, source: Path, drop: set[str]) -> None:
        for path in sorted((source / "ev").glob("*.png")):
            stem = path.stem
            if stem.startswith("sd") and SD_RE.match(stem):
                continue
            asset = self.new_asset(stem, POOL_EV, KIND_EFFECT, path, drop)
            if asset is None:
                continue
            with Image.open(path) as image:
                asset.w, asset.h = image.size
                asset.payload = resave_lossless(image)
            self.finish(asset, "特效/道具/画面")

    # -- 条目管理 ---------------------------------------------------------
    def new_asset(self, name: str, pool: int, kind: int, path: Path, drop: set[str], force_missing: bool = False) -> Asset | None:
        if name in drop or force_missing:
            asset = Asset(name=name, pool=pool, kind=KIND_MISSING, source_bytes=path.stat().st_size if path.is_file() else 0,
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
            # 变体映射条目自带 0 尺寸:显示尺寸一律取基准条目,固件不需要再查
            if asset.alias and asset.base_name and not (asset.w and asset.h):
                reference = ordered[index_of[(asset.pool, asset.base_name)]]
                asset.w, asset.h = reference.w, reference.h
        # 名字空间参与定位:画面_白 / 画面_黒 在背景与事件图两个名字空间都存在
        missing = [(asset.name, asset.base_name) for asset in ordered
                   if asset.base_name and (asset.pool, asset.base_name) not in index_of]
        if missing:
            sys.exit(f"ERROR: 变体基准名不存在: {missing[:3]}")

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
            f" = 图片 {budget / 1048576:.2f} MB(取舍见 docs 或会话结论)")
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
# 自检
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
    """载荷与源文件逐字节一致(背景 / CG 基准 / 特效这类原样搬运的条目)。"""
    if not entry["data_len"]:
        return False
    return payload_of(raw, sections, entry) == source_path.read_bytes()


def check_pack(path: Path) -> int:
    raw, sections, entries = load_pack(path)
    section_count = len(sections)
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
        if kind in (KIND_BG, KIND_CG, KIND_CG_DIFF) and data_len and raw[blob_off + data_off] != 0xFF:
            errors.append(f"{name}: 不是 JPEG")
        if kind in (KIND_SPRITE, KIND_SD, KIND_EFFECT) and data_len and raw[blob_off + data_off:blob_off + data_off + 4] != b"\x89PNG":
            errors.append(f"{name}: 不是 PNG")
        if kind == KIND_MISSING and data_len:
            errors.append(f"{name}: MISSING 条目不应该有载荷")
        if kind == KIND_CG_DIFF and (not entry["dw"] or not entry["dh"]):
            errors.append(f"{name}: 差分补丁尺寸为 0")
    # 跨条目校验放第二趟:基准条目可能排在变体后面(基准 = 剧本引用最多的那张)
    for entry in entries:
        name, kind, base, flags = entry["name"], entry["kind"], entry["base"], entry["flags"]
        if flags & FLAG_ALIAS:
            if base >= asset_count:
                errors.append(f"{name}: 变体基准越界")
            else:
                reference = entries[base]
                if reference["kind"] not in (KIND_SPRITE, KIND_SD, KIND_EFFECT):
                    errors.append(f"{name}: 变体基准类型不对({KIND_NAMES.get(reference['kind'])})")
                elif reference["flags"] & FLAG_ALIAS:
                    errors.append(f"{name}: 变体基准指向了另一个变体")
            if entry["data_len"]:
                errors.append(f"{name}: 变体条目不应该有载荷")
        if kind == KIND_CG_DIFF:
            if flags & FLAG_ALIAS:
                errors.append(f"{name}: 差分条目同时被标成变体")
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
    """按 pack 自身的信息还原一条资产(变体取基准、差分合成补丁)。"""
    if depth > 3:
        sys.exit("ERROR: 变体/差分引用成环")
    if entry["flags"] & FLAG_ALIAS:
        return stored_image(raw, sections, entries, entries[entry["base"]], depth + 1)
    data = payload_of(raw, sections, entry)
    if entry["kind"] == KIND_CG_DIFF:
        base = entries[entry["base"]]
        canvas = stored_image(raw, sections, entries, base, depth + 1).convert("RGB")
        patch = Image.open(io.BytesIO(data)).convert("RGB")
        canvas.paste(patch, (entry["dx"], entry["dy"]))
        return canvas
    return Image.open(io.BytesIO(data))


def expected_source(image: "Image.Image", source_path: Path, kind: int, args: argparse.Namespace) -> "Image.Image":
    """对比基准:把源素材按转换规则走一遍(用于独立核验)。"""
    with Image.open(source_path) as raw:
        reference = raw.convert("RGBA" if raw.mode in ("P", "LA", "RGBA") else "RGB")
    if kind == KIND_SPRITE and reference.height > image.height:
        scale = image.height / reference.height
        reference = reference.resize((max(1, round(reference.width * scale)), image.height), Image.LANCZOS)
    elif kind == KIND_SD and args.sd_crop:
        box = content_box(reference)
        cropped = reference.crop(box)
        if cropped.size != image.size:
            reference = cropped.resize(image.size, Image.LANCZOS)
        else:
            reference = cropped
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
        key = (entry["pool"], entry["name"])
        label = KIND_NAMES.get(entry["kind"], str(entry["kind"]))
        if entry["flags"] & FLAG_ALIAS:
            label = "SD 变体映射"
        elif label == "CG_DIFF":
            label = "CG 差分"
        elif label == "CG":
            label = "CG 基准"
        elif label == "SD":
            label = "SD 基准"
        source_path = index.get(key)
        if source_path is None:
            unmatched.append(f"{label} {entry['name']}")
            continue
        stored = stored_image(raw, sections, entries, entry)
        reference = expected_source(stored, source_path, entry["kind"], args)
        if entry["data_len"] and payload_matches_source(raw, sections, entry, source_path):
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
        log(f"警告: 没有读到剧本({script_dir}),基准张改为按字典序选择,SD/CG 差分收益会略低")
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
        "sprite_max_h": str(args.sprite_max_h),
        "sprite_colors": str(args.sprite_colors),
        "sd_width": str(args.sd_width),
        "sd_height": str(args.sd_height),
        "sd_keep_coverage": str(args.sd_keep_coverage),
        "sd_alias": args.sd_alias,
        "cg_diff_max_pct": str(args.cg_diff_max_pct),
        "diff_threshold": str(args.diff_threshold),
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
    parser.add_argument("--sprite-max-h", type=int, default=280, help="立绘最大高度(默认 280;320 会超出图片预算)")
    parser.add_argument("--sprite-colors", type=int, default=255, help="立绘/SD 调色板颜色数(默认 255)")
    parser.add_argument("--sd-width", type=int, default=240, help="SD 装饰宽度(默认 240)")
    parser.add_argument("--sd-height", type=int, default=144, help="SD 装饰高度上限(默认 144)")
    parser.add_argument("--sd-crop", action="store_true", default=True, help="SD 裁到内容包围盒(默认开)")
    parser.add_argument("--no-sd-crop", dest="sd_crop", action="store_false", help="SD 不裁,整幅缩到显示框")
    parser.add_argument("--sd-keep-all", action="store_true", help="SD 全部保留(默认每组只留基准张)")
    parser.add_argument("--sd-keep-coverage", type=float, default=0.0,
                        help="按引用覆盖率保留 SD 变体(0=只留基准张,0.8=保留覆盖 80%% 引用的那些)")
    parser.add_argument("--sd-alias", choices=("nearest", "base"), default="nearest",
                        help="被丢弃的 SD 变体指向哪张(默认 nearest=像素最接近的保留张)")
    parser.add_argument("--cg-diff", action="store_true", default=True, help="CG 做差分补丁(默认开)")
    parser.add_argument("--no-cg-diff", dest="cg_diff", action="store_false", help="CG 不做差分,逐张原样")
    parser.add_argument("--cg-diff-max-pct", type=float, default=30.0, help="组内差异超过这个百分比就不做差分(默认 30)")
    parser.add_argument("--diff-threshold", type=int, default=24, help="判定像素变化的通道差阈值(默认 24)")
    parser.add_argument("--patch-pad", type=int, default=2, help="补丁外扩像素(默认 2)")
    parser.add_argument("--patch-align", type=int, default=8, help="补丁矩形对齐(默认 8)")
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
