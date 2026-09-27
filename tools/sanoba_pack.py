#!/usr/bin/env python3
"""Build the Sanoba Witch image pack for the AI Passport port.

Source project: https://github.com/hrk666666/Sanoba-Witch-MiBand-10
  - 小米手环 9 / 10(Xiaomi Vela / aiot 快应用)上的《魔女的夜宴》移植版。
  - 背景与 SD 装饰图版权归 Yuzusoft 所有;本仓库只保存转换产物(main/sanoba_data/),
    不分发源素材。先用 tools/sanoba_fetch_source.py 把源素材拉到 build/sanoba-source/:
      python tools/sanoba_fetch_source.py --dest build/sanoba-source

The pack is a single little-endian binary read straight out of flash: no
decompression, no runtime parsing, no rescaling.  版面与源工程一致:背景铺满整屏
240x320,SD 装饰是 240x144 的横条贴在画面上半部,正文带压在画面下沿。

  header  : magic "SANOBPK1", version u16=1, header_size u16, section_count u16,
            reserved u16, total_size u32                       (20 B)
  section : { type u32, offset u32, count u32, size u32 }      (16 B × N)
    SEC_NAME  UTF-8 names, NUL-separated, one per asset(条目按 (pool, name) 升序,
              固件可二分查找)
    SEC_ASSET fixed 36-byte entry table:
              { name_off u32, name_len u16, kind u8, pool u8, base u16,
                w u16, h u16, dx u16, dy u16, dw u16, dh u16,
                data_off u32, data_len u32, flags u32, reserved u16 }
              data_off 相对 SEC_BLOB
    SEC_BLOB  载荷区,逐条目一段完整 JPEG
    SEC_META  key=value UTF-8 文本(来源仓库 / ref / 转换参数 / 体积)

  kind : 0=BG JPEG | 1=SPRITE(本作没有)| 2=SD | 3=CG(本作没有)| 4=CG_DIFF(本作没有)
         | 5=MISSING 占位(不画)| 6=EFFECT(本作没有)
  pool : 0=background 1=sprite 2=event —— 对应剧本里的 bg / 立绘 / ev 三个名字空间
  flags: bit0 = alias 条目(没有自己的载荷,画 base 指向的条目)

与同仓库千恋＊万花图片包(SENRNPK2)的关系:
  头/段/条目三张结构的字节布局完全一致,所以 main/senren_pack.c 那层"字节->结构"
  的解析可以直接照搬(只改魔数与版本常量)。差异有三点,都写进 SEC_META:
    1. 魔数/版本换成 SANOBPK1 / 1,避免读取方误以为补丁语义相同;
    2. dx/dy/dw/dh 对每个条目都是"绘制矩形"(BG 固定 0,0,240,320;SD 是 0,24,240,144),
       不是 SENRNPK2 里只有补丁才用的变化矩形;
    3. SD 载荷是 JPEG,不是调色板 PNG —— 源工程这 292 张 SD 本来就是 240x144 JPEG,
       量化成 255 色调色板 PNG 后是 7.32 MB(实测),而原样存只有 1.86 MB。
       与之一致,固件侧 SD 的解码目标是"画布上的一条整宽横条":
       SD 宽 == 画布宽且 dx == 0,于是 JPEG 直接解进 canvas + dy*240,不需要额外缓冲。

素材转换规则(尺寸依据来自源工程 .ux 与 336x480 的源素材):
  背景 107 张 336x480 JPEG -> 等比缩到宽 240(高 343)-> 取顶部 320 行 -> JPEG。
    --bg-quality 默认 75:实测 106 张合计 1.20 MB,与缩放后理想图的平均绝对误差
    2.57/255(q70 是 1.09 MB / 2.77,q85 是 1.58 MB / 1.98)。源体积是 1.40 MB,
    所以这一步几乎没有省,主要是把解码目标变成原生几何(JPEG 解码器不负责缩放)。
  标题图 title_bg.jpg 336x480 -> 同上,作为背景名字空间里的一个条目打进包,
    名字固定 "标题画面";剧本不会引用它,标题页按下标或名字查。
  SD 292 张 240x144 JPEG -> 原样字节入库(0 次转码,1.86 MB),绘制矩形 0,24,240,144。
    重编码只会更大(q75 2.34 MB),量化调色板 PNG 更大(7.32 MB)。
  事件 CG:源工程没有 ev/ 目录,剧本引用的 2,862 次 ev* / 1,196 个不同名字全部无图。
    默认不写条目(固件查不到就跳过绘制,与源引擎的静默行为一致);
    --missing-placeholders 可以写 kind=5 占位条目,便于核对缺失清单。

Usage:
  python tools/sanoba_fetch_source.py --dest build/sanoba-source
  python tools/sanoba_pack.py --source build/sanoba-source --out build/sanoba-pack/sanoba_pack.bin
  python tools/sanoba_pack.py --check build/sanoba-pack/sanoba_pack.bin
  python tools/sanoba_pack.py --compare build/sanoba-pack/sanoba_pack.bin --source build/sanoba-source
  python tools/sanoba_pack.py --source build/sanoba-source --out ... --preview build/sanoba-pack/preview
"""

from __future__ import annotations

import argparse
import io
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:  # pragma: no cover - 环境问题
    sys.exit("需要 Pillow: python -m pip install pillow")

MAGIC = b"SANOBPK1"
VERSION = 1
HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
ENTRY = struct.Struct("<IHBBHHHHHHHIIIH")
assert HEADER.size == 20 and SECTION.size == 16 and ENTRY.size == 36

SEC_NAME, SEC_ASSET, SEC_BLOB, SEC_META = range(4)

KIND_BG, KIND_SPRITE, KIND_SD, KIND_CG, KIND_CG_DIFF, KIND_MISSING, KIND_EFFECT = range(7)
KIND_NAMES = {
    KIND_BG: "BG", KIND_SPRITE: "SPRITE", KIND_SD: "SD", KIND_CG: "CG",
    KIND_CG_DIFF: "CG_DIFF", KIND_MISSING: "MISSING", KIND_EFFECT: "EFFECT",
}

POOL_BG, POOL_CH, POOL_EV = 0, 1, 2
POOL_NAMES = {POOL_BG: "bg", POOL_CH: "ch", POOL_EV: "ev"}

FLAG_ALIAS = 1 << 0
NO_BASE = 0xFFFF

SCREEN_W, SCREEN_H = 240, 320
SD_W, SD_H = 240, 144
SD_X, SD_Y = 0, 24
TITLE_ART_NAME = "标题画面"
DEFAULT_BG_QUALITY = 75
DEFAULT_BUDGET_MB = 3.40


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def human(count: float) -> str:
    if count >= 1048576:
        return f"{count / 1048576:.2f} MB"
    return f"{count / 1024:.1f} KB"


@dataclass
class Asset:
    name: str
    kind: int
    pool: int
    w: int
    h: int
    dx: int
    dy: int
    dw: int
    dh: int
    payload: bytes
    base: int = NO_BASE
    alias: bool = False
    note: str = ""

    @property
    def stored(self) -> int:
        return len(self.payload)


@dataclass
class BuildStats:
    groups: dict[str, dict[str, int]] = field(default_factory=dict)

    def add(self, label: str, source: int, stored: int, entries: int) -> None:
        row = self.groups.setdefault(label, {"source": 0, "stored": 0, "entries": 0})
        row["source"] += source
        row["stored"] += stored
        row["entries"] += entries


# --------------------------------------------------------------------------
# 源读取
# --------------------------------------------------------------------------


def read_script_refs(source: Path) -> dict[str, set[str]]:
    """从源剧本里收集 bg / 立绘 / 事件 三个名字空间实际引用到的名字。"""
    refs = {"bg": set(), "ch": set(), "ev": set(), "sd": set()}
    scn_dir = source / "scn"
    if not scn_dir.is_dir():
        log(f"警告: 没有 {scn_dir},跳过剧本引用统计(基准与缺失清单会不完整)")
        return refs
    for path in sorted(scn_dir.glob("*.ks.txt")):
        try:
            nodes = json.loads(path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError) as exc:
            log(f"警告: {path.name} 读不了({exc}),跳过")
            continue
        for node in nodes:
            if not isinstance(node, list) or not node:
                continue
            kind = node[0]
            if kind == 2 and len(node) > 1 and node[1]:
                refs["bg"].add(str(node[1]))
            elif kind == 5 and len(node) > 1 and node[1]:
                name = str(node[1])
                if name.startswith("sd"):
                    refs["sd"].add(name)
                elif name.startswith("ev"):
                    refs["ev"].add(name)
                else:
                    refs["ev"].add(name)
            elif kind == 3 and len(node) > 3 and node[3]:
                for sprite in node[3]:
                    if isinstance(sprite, list) and sprite:
                        refs["ch"].add(str(sprite[0]))
    return refs


def jpeg_bytes(image: "Image.Image", quality: int) -> bytes:
    buf = io.BytesIO()
    image.convert("RGB").save(buf, "JPEG", quality=quality, optimize=True, subsampling=2)
    return buf.getvalue()


def cover_to_screen(image: "Image.Image", crop: str = "top") -> "Image.Image":
    """等比缩到宽 240,再取 320 行(源素材是 0.70 竖构图,屏幕是 0.75)。"""
    scale = SCREEN_W / image.width
    resized = image.resize((SCREEN_W, max(1, round(image.height * scale))), Image.LANCZOS)
    if resized.height < SCREEN_H:
        # 比屏幕还矮(理论上不该出现):补黑边而不是拉伸
        canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
        canvas.paste(resized, (0, 0))
        return canvas
    if crop == "bottom":
        return resized.crop((0, resized.height - SCREEN_H, SCREEN_W, resized.height))
    if crop == "center":
        top = (resized.height - SCREEN_H) // 2
        return resized.crop((0, top, SCREEN_W, top + SCREEN_H))
    return resized.crop((0, 0, SCREEN_W, SCREEN_H))


def error_against(reference: "Image.Image", payload: bytes) -> float:
    decoded = Image.open(io.BytesIO(payload)).convert("RGB")
    if decoded.size != reference.size:
        return 255.0
    stat = ImageStat.Stat(ImageChops.difference(reference.convert("RGB"), decoded))
    return sum(stat.mean) / 3


# --------------------------------------------------------------------------
# 打包
# --------------------------------------------------------------------------


class PackBuilder:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.assets: list[Asset] = []
        self.stats = BuildStats()
        self.quality: list[tuple[str, float]] = []
        self.notes: list[str] = []

    # ---- 各类素材 ----
    def add_backgrounds(self, source: Path, refs: dict[str, set[str]]) -> None:
        paths = sorted((source / "bg").glob("*.jpg"))
        if not paths:
            sys.exit(f"ERROR: {source / 'bg'} 里没有背景图\n先跑: python tools/sanoba_fetch_source.py --dest {source}")
        source_total = 0
        stored_total = 0
        unused = []
        for path in paths:
            name = path.stem
            image = Image.open(path).convert("RGB")
            source_total += path.stat().st_size
            staged = cover_to_screen(image, self.args.crop)
            payload = jpeg_bytes(staged, self.args.bg_quality)
            stored_total += len(payload)
            self.quality.append((f"bg/{name}", error_against(staged, payload)))
            if name not in refs["bg"]:
                unused.append(name)
            self.assets.append(Asset(name=name, kind=KIND_BG, pool=POOL_BG, w=SCREEN_W, h=SCREEN_H,
                                     dx=0, dy=0, dw=SCREEN_W, dh=SCREEN_H, payload=payload))
        self.stats.add("背景", source_total, stored_total, len(paths))
        if unused:
            self.notes.append("unreferenced_backgrounds=" + ",".join(sorted(unused)))
            log(f"提示: {len(unused)} 张背景源剧本没引用,照样打进包: {sorted(unused)[:4]}")

    def add_title(self, source: Path) -> None:
        path = Path(self.args.title_art) if self.args.title_art else source / "title_bg.jpg"
        if not path.is_file():
            log(f"提示: 没有标题图 {path},包里不放标题条目")
            return
        image = Image.open(path).convert("RGB")
        staged = cover_to_screen(image, self.args.crop)
        payload = jpeg_bytes(staged, self.args.bg_quality)
        self.quality.append(("标题画面", error_against(staged, payload)))
        self.assets.append(Asset(name=TITLE_ART_NAME, kind=KIND_BG, pool=POOL_BG, w=SCREEN_W, h=SCREEN_H,
                                 dx=0, dy=0, dw=SCREEN_W, dh=SCREEN_H, payload=payload))
        self.stats.add("标题图", path.stat().st_size, len(payload), 1)

    def add_sd(self, source: Path) -> None:
        paths = sorted((source / "sd").glob("*.jpg"))
        if not paths:
            sys.exit(f"ERROR: {source / 'sd'} 里没有 SD 图\n先跑: python tools/sanoba_fetch_source.py --dest {source}")
        source_total = 0
        stored_total = 0
        sizes: dict[tuple[int, int], int] = {}
        for path in paths:
            raw = path.read_bytes()
            image = Image.open(io.BytesIO(raw))
            width, height = image.size
            # 源工程 .sd-image 的显示框是 240x144:292 张里 291 张正好是它,sd607w 是
            # 240x136。宽必须等于屏宽(dx=0 才能把 JPEG 直接解进画布那一段),高按原生存,不拉伸。
            if width != SCREEN_W or height <= 0 or height > SD_H:
                sys.exit(f"ERROR: {path.name} 是 {width}x{height},不满足 SD 规则"
                         f"(宽必须 {SCREEN_W}、高 <= {SD_H});要先在打包器里定好处理规则")
            sizes[(width, height)] = sizes.get((width, height), 0) + 1
            kind = KIND_SD
            if self.args.sd_mode == "palette":
                # 备选路径:与千恋＊万花一样存调色板 PNG(C 侧 PNG 路径零改动),实测更大
                quantized = image.convert("RGB").quantize(colors=255, method=Image.MEDIANCUT)
                buf = io.BytesIO()
                quantized.save(buf, "PNG", optimize=True)
                payload = buf.getvalue()
                self.notes.append("sd_encoding=palette-png")
            else:
                payload = raw
            source_total += len(raw)
            stored_total += len(payload)
            self.assets.append(Asset(name=path.stem, kind=kind, pool=POOL_EV, w=width, h=height,
                                     dx=SD_X, dy=SD_Y, dw=width, dh=height, payload=payload))
        self.notes.append("sd_sizes=" + ",".join(f"{w}x{h}:{count}" for (w, h), count in sorted(sizes.items())))
        self.stats.add("SD", source_total, stored_total, len(paths))

    def add_missing(self, refs: dict[str, set[str]]) -> None:
        have = {asset.name for asset in self.assets if asset.pool == POOL_EV}
        names = sorted(name for name in refs["ev"] if name not in have)
        if not names:
            return
        self.notes.append(f"missing_event_names={len(names)}")
        if not self.args.missing_placeholders:
            log(f"提示: 剧本引用 {len(names)} 个事件图名字在源工程里没有图(ev* 全部 + item_*/画面_*),"
                f"默认不写占位条目,固件查不到就跳过绘制;要显式占位用 --missing-placeholders")
            return
        for name in names:
            self.assets.append(Asset(name=name, kind=KIND_MISSING, pool=POOL_EV, w=0, h=0,
                                     dx=0, dy=0, dw=0, dh=0, payload=b""))
        self.stats.add("MISSING 占位", 0, 0, len(names))

    # ---- 序列化 ----
    def serialize(self, source: Path, ref: str) -> bytes:
        assets = sorted(self.assets, key=lambda item: (item.pool, item.name))
        seen = set()
        for asset in assets:
            key = (asset.pool, asset.name)
            if key in seen:
                sys.exit(f"ERROR: 条目重复: {POOL_NAMES[asset.pool]}/{asset.name}")
            seen.add(key)

        names = bytearray()
        entries = bytearray()
        blob = bytearray()
        for asset in assets:
            encoded = asset.name.encode("utf-8")
            offset = len(names)
            names += encoded + b"\0"
            data_off = len(blob)
            blob += asset.payload
            entries += ENTRY.pack(
                offset, len(encoded), asset.kind, asset.pool, asset.base,
                asset.w, asset.h, asset.dx, asset.dy, asset.dw, asset.dh,
                data_off, len(asset.payload), FLAG_ALIAS if asset.alias else 0, 0,
            )

        meta_lines = [
            "format=SANOBPK1",
            "version=1",
            "layout=SENRNPK2-compatible header/section/entry structs",
            "source_repo=https://github.com/hrk666666/Sanoba-Witch-MiBand-10",
            f"source_ref={ref}",
            # 只记目录名(同 sanoba_scn_pack.py):保证入库素材重建逐字节一致。
            f"source_dir_name={source.name}",
            f"screen={SCREEN_W}x{SCREEN_H}",
            f"bg_crop={self.args.crop}",
            f"bg_quality={self.args.bg_quality}",
            f"sd_rect={SD_X},{SD_Y},{SD_W},{SD_H}",
            f"sd_encoding={'palette-png' if self.args.sd_mode == 'palette' else 'jpeg-verbatim'}",
            f"entries={len(assets)}",
            f"payload_bytes={len(blob)}",
            "kind_enum=0=BG,1=SPRITE,2=SD,3=CG,4=CG_DIFF,5=MISSING,6=EFFECT",
            "pool_enum=0=background,1=sprite,2=event",
            "dx_dy_dw_dh=draw rect for every entry (BG 0,0,240,320; SD 0,24,240,144)",
            "sd_note=SD width equals the screen width and dx is 0, so the firmware can decode the JPEG straight into canvas + dy*screen_w; the height is the native source height (291 assets are 240x144, sd607w is 240x136)",
            "sd_encoding_note=jpeg-verbatim keeps the source bytes untouched; palette PNG measured 7.32 MB versus 1.86 MB here",
            f"missing_placeholders={'yes' if self.args.missing_placeholders else 'no'}",
            f"title_art={TITLE_ART_NAME}",
        ]
        counts: dict[str, int] = {}
        for asset in assets:
            counts[KIND_NAMES[asset.kind]] = counts.get(KIND_NAMES[asset.kind], 0) + 1
        meta_lines.append("kind_counts=" + ",".join(f"{key}={value}" for key, value in sorted(counts.items())))
        meta_lines.extend(self.notes)
        meta = ("\n".join(meta_lines) + "\n").encode("utf-8")

        sections = [
            (SEC_NAME, bytes(names), len(assets)),
            (SEC_ASSET, bytes(entries), len(assets)),
            (SEC_BLOB, bytes(blob), len(assets)),
            (SEC_META, meta, 0),
        ]
        header_size = HEADER.size + SECTION.size * len(sections)
        offset = header_size
        table = []
        body = bytearray()
        for section_type, payload, count in sections:
            pad = (-offset) % 4
            if pad:
                body += b"\0" * pad
                offset += pad
            table.append((section_type, offset, count, len(payload)))
            body += payload
            offset += len(payload)
        total = header_size + len(body)
        head = bytearray(HEADER.pack(MAGIC, VERSION, header_size, len(sections), 0, total))
        for section_type, position, count, size in table:
            head += SECTION.pack(section_type, position, count, size)
        return bytes(head) + bytes(body)


# --------------------------------------------------------------------------
# 读回 / 自检 / 核验
# --------------------------------------------------------------------------


class Pack:
    def __init__(self, raw: bytes) -> None:
        if len(raw) < HEADER.size:
            raise ValueError("文件太短")
        magic, version, header_size, section_count, _reserved, total = HEADER.unpack_from(raw)
        if magic != MAGIC:
            raise ValueError(f"魔数不符: {magic!r}")
        if version != VERSION:
            raise ValueError(f"版本不支持: {version}")
        if total != len(raw):
            raise ValueError(f"头里写 {total} 字节,实际 {len(raw)}")
        if header_size != HEADER.size + SECTION.size * section_count:
            raise ValueError("header_size 与段数不一致")
        self.raw = raw
        self.sections: dict[int, tuple[int, int, int]] = {}
        for index in range(section_count):
            section_type, offset, count, size = SECTION.unpack_from(raw, HEADER.size + SECTION.size * index)
            self.sections[section_type] = (offset, count, size)
        for required in (SEC_NAME, SEC_ASSET, SEC_BLOB, SEC_META):
            if required not in self.sections:
                raise ValueError(f"缺少段 {required}")

        name_off, _, name_size = self.sections[SEC_NAME]
        self.names = raw[name_off:name_off + name_size]
        asset_off, self.entry_count, _ = self.sections[SEC_ASSET]
        self.entries: list[dict] = []
        for index in range(self.entry_count):
            fields = ENTRY.unpack_from(raw, asset_off + ENTRY.size * index)
            (name_offset, name_len, kind, pool, base, w, h, dx, dy, dw, dh,
             data_off, data_len, flags, _reserved) = fields
            name = self.names[name_offset:name_offset + name_len].decode("utf-8")
            self.entries.append({
                "index": index, "name": name, "name_off": name_offset, "name_len": name_len,
                "kind": kind, "pool": pool, "base": base,
                "w": w, "h": h, "dx": dx, "dy": dy, "dw": dw, "dh": dh,
                "data_off": data_off, "data_len": data_len, "flags": flags,
                "alias": bool(flags & FLAG_ALIAS),
            })
        blob_off, _, blob_size = self.sections[SEC_BLOB]
        self.payload = raw[blob_off:blob_off + blob_size]
        meta_off, _, meta_size = self.sections[SEC_META]
        self.meta: dict[str, str] = {}
        for line in raw[meta_off:meta_off + meta_size].decode("utf-8").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                self.meta[key] = value

    def find(self, pool: int, name: str) -> dict | None:
        """与固件的二分查找等价:条目已按 (pool, name) 升序。"""
        low, high = 0, len(self.entries) - 1
        while low <= high:
            mid = (low + high) // 2
            entry = self.entries[mid]
            key = (entry["pool"], entry["name"])
            target = (pool, name)
            if key == target:
                return entry
            if key < target:
                low = mid + 1
            else:
                high = mid - 1
        return None

    def data(self, entry: dict) -> bytes:
        return self.payload[entry["data_off"]:entry["data_off"] + entry["data_len"]]

    def validate_structure(self) -> list[str]:
        problems: list[str] = []
        keys = [(entry["pool"], entry["name"]) for entry in self.entries]
        if keys != sorted(keys):
            problems.append("条目没有按 (pool, name) 升序排列,固件无法二分查找")
        if len(set(keys)) != len(keys):
            problems.append("条目有重复的 (pool, name)")
        for entry in self.entries:
            if entry["kind"] not in KIND_NAMES:
                problems.append(f"{entry['name']}: 未知类型 {entry['kind']}")
            if entry["pool"] not in POOL_NAMES:
                problems.append(f"{entry['name']}: 未知名字空间 {entry['pool']}")
            if entry["kind"] == KIND_MISSING:
                if entry["data_len"] != 0:
                    problems.append(f"{entry['name']}: MISSING 条目不该有载荷")
                continue
            if entry["data_off"] + entry["data_len"] > len(self.payload):
                problems.append(f"{entry['name']}: 载荷越界")
            if entry["data_len"] == 0:
                problems.append(f"{entry['name']}: 载荷为空")
            if entry["alias"]:
                problems.append(f"{entry['name']}: 本包不该有 alias 条目")
            if entry["kind"] == KIND_BG and (entry["w"], entry["h"]) != (SCREEN_W, SCREEN_H):
                problems.append(f"{entry['name']}: 背景尺寸 {entry['w']}x{entry['h']} 不是 {SCREEN_W}x{SCREEN_H}")
            if entry["kind"] == KIND_SD:
                if entry["w"] != SCREEN_W or entry["h"] == 0 or entry["h"] > SD_H:
                    problems.append(f"{entry['name']}: SD 尺寸 {entry['w']}x{entry['h']} 不满足整宽横条规则")
                if entry["dx"] != 0:
                    problems.append(f"{entry['name']}: SD 的 dx={entry['dx']},整宽横条解码要求 dx=0")
        if not self.payload:
            problems.append("载荷区为空")
        return problems

    def report(self, out: Path) -> None:
        by_kind: dict[int, list[int]] = {}
        for entry in self.entries:
            by_kind.setdefault(entry["kind"], []).append(entry["data_len"])
        log("")
        log(f"{'类型':<10}{'条目':>6}{'载荷':>12}")
        for kind, sizes in sorted(by_kind.items()):
            log(f"{KIND_NAMES[kind]:<10}{len(sizes):>6}{human(sum(sizes)):>12}")
        log(f"{'合计':<10}{len(self.entries):>6}{human(len(self.payload)):>12}  包 {human(len(self.raw))}")
        log(f"写出 {out}")


def read_pack(path: Path) -> Pack:
    return Pack(path.read_bytes())


def check_pack(path: Path) -> int:
    try:
        pack = read_pack(path)
    except (ValueError, OSError) as exc:
        log(f"ERROR: {path} 读不了: {exc}")
        return 1
    problems = pack.validate_structure()
    log(f"包 {path}: {human(len(pack.raw))},条目 {len(pack.entries)},载荷 {human(len(pack.payload))}")
    if problems:
        for problem in problems:
            log(f"  ✗ {problem}")
        return 1
    log("  ✓ 结构自检通过")
    return 0


def compare_pack(path: Path, source: Path) -> int:
    """独立核验:把包里的每条资产解回来,和源素材(同一几何变换)比误差。"""
    try:
        pack = read_pack(path)
        from PIL import Image
    except (ValueError, OSError) as exc:
        log(f"ERROR: 读包失败: {exc}")
        return 1
    errors: list[tuple[str, float]] = []
    for entry in pack.entries:
        if entry["kind"] == KIND_MISSING:
            continue
        if entry["kind"] == KIND_BG:
            if entry["name"] == TITLE_ART_NAME:
                candidate = source / "title_bg.jpg"
            else:
                candidate = source / "bg" / f"{entry['name']}.jpg"
            if not candidate.is_file():
                log(f"  ✗ {entry['name']}: 找不到源文件 {candidate}")
                return 1
            reference = cover_to_screen(Image.open(candidate).convert("RGB"))
        elif entry["kind"] == KIND_SD:
            candidate = source / "sd" / f"{entry['name']}.jpg"
            if not candidate.is_file():
                log(f"  ✗ {entry['name']}: 找不到源文件 {candidate}")
                return 1
            source_bytes = candidate.read_bytes()
            if pack.data(entry) != source_bytes:
                log(f"  ✗ {entry['name']}: SD 载荷与源文件字节不一致(约定原样入库)")
                return 1
            continue
        else:
            continue
        errors.append((entry["name"], error_against(reference, pack.data(entry))))
    if not errors:
        log("核验: 没有可比对的条目")
        return 1
    values = sorted(error for _name, error in errors)
    worst = max(errors, key=lambda item: item[1])
    log(f"核验 {len(errors)} 张转码图:与源图平均绝对误差 均值 {sum(values) / len(values):.2f} / "
        f"p90 {values[int(len(values) * 0.9)]:.2f} / 最大 {worst[1]:.2f}({worst[0]})(0..255)")
    log(f"核验 {sum(1 for entry in pack.entries if entry['kind'] == KIND_SD)} 张 SD:与源文件逐字节一致")
    return 0


# --------------------------------------------------------------------------
# 预览
# --------------------------------------------------------------------------


def render_preview(pack: Pack, source: Path, out_dir: Path, names: list[str]) -> int:
    """把 bg + SD + 正文带拼成 240x320 的预览图,再放大 2 倍便于肉眼检查。"""
    from PIL import Image, ImageDraw

    out_dir.mkdir(parents=True, exist_ok=True)
    if not names:
        names = [entry["name"] for entry in pack.entries if entry["kind"] == KIND_BG][:3]
    sd_names = [entry["name"] for entry in pack.entries if entry["kind"] == KIND_SD]
    written = 0
    for index, name in enumerate(names):
        bg = pack.find(POOL_BG, name)
        if bg is None:
            log(f"  ✗ 预览: 背景 {name} 不在包里")
            continue
        canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
        canvas.paste(Image.open(io.BytesIO(pack.data(bg))).convert("RGB"), (0, 0))
        if sd_names:
            sd = pack.find(POOL_EV, sd_names[(index * 7) % len(sd_names)])
            if sd is not None:
                canvas.paste(Image.open(io.BytesIO(pack.data(sd))).convert("RGB"), (sd["dx"], sd["dy"]))
        draw = ImageDraw.Draw(canvas)
        draw.rectangle((0, 210, SCREEN_W, SCREEN_H), fill=(18, 18, 22))
        draw.rectangle((0, 210, SCREEN_W, 214), fill=(60, 90, 160))
        path = out_dir / f"preview-{index:02d}.png"
        canvas.resize((SCREEN_W * 2, SCREEN_H * 2), Image.NEAREST).save(path)
        written += 1
        log(f"  预览 {path}")
    return 0 if written else 1


# --------------------------------------------------------------------------
# 入口
# --------------------------------------------------------------------------


def build(args: argparse.Namespace) -> int:
    source = Path(args.source)
    if not source.is_dir():
        sys.exit(f"ERROR: 源目录不存在: {source}\n先跑: python tools/sanoba_fetch_source.py --dest {source}")
    ref = "unknown"
    manifest = source / "MANIFEST.json"
    if manifest.is_file():
        try:
            ref = str(json.loads(manifest.read_text(encoding="utf-8")).get("ref", "unknown"))
        except (json.JSONDecodeError, OSError):
            pass

    refs = read_script_refs(source)
    builder = PackBuilder(args)
    builder.add_backgrounds(source, refs)
    builder.add_title(source)
    builder.add_sd(source)
    builder.add_missing(refs)
    blob = builder.serialize(source, ref)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)
    pack = Pack(blob)
    problems = pack.validate_structure()
    if problems:
        for problem in problems:
            log(f"  ✗ {problem}")
        return 1

    log("")
    log(f"{'分类':<12}{'条目':>6}{'源体积':>12}{'包内体积':>12}")
    total_source = total_stored = 0
    for label, row in builder.stats.groups.items():
        log(f"{label:<12}{row['entries']:>6}{human(row['source']):>12}{human(row['stored']):>12}")
        total_source += row["source"]
        total_stored += row["stored"]
    log(f"{'素材合计':<12}{len(pack.entries):>6}{human(total_source):>12}{human(total_stored):>12}")
    if builder.quality:
        values = sorted(value for _name, value in builder.quality)
        worst = max(builder.quality, key=lambda item: item[1])
        log(f"质量自检: {len(values)} 张背景,与缩放后理想图平均绝对误差 均值 {sum(values) / len(values):.2f} / "
            f"最大 {worst[1]:.2f}({worst[0]})(0..255)")
    pack.report(out_path)
    budget = args.budget_mb * 1048576
    verdict = "放得下" if len(pack.raw) <= budget else "超出"
    log(f"预算: 图片包 {human(len(pack.raw))} vs 预算 {args.budget_mb:.2f} MB → {verdict}"
        f"{' 余 ' + human(int(budget - len(pack.raw))) if len(pack.raw) <= budget else ' 超 ' + human(int(len(pack.raw) - budget))}")
    if args.json:
        payload = {
            "meta": pack.meta,
            "entries": [
                {key: value for key, value in entry.items() if key not in ("index", "name_off", "name_len")}
                for entry in pack.entries
            ],
        }
        json_path = Path(args.json)
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        log(f"写出清单 {json_path}")
    if args.preview:
        render_preview(pack, source, Path(args.preview), [])
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", default="build/sanoba-source", help="源素材目录(默认 build/sanoba-source)")
    parser.add_argument("--out", default="build/sanoba-pack/sanoba_pack.bin", help="输出图片包")
    parser.add_argument("--json", help="可选:同时写一份可读清单")
    parser.add_argument("--check", metavar="PACK", help="只做只读自检")
    parser.add_argument("--compare", metavar="PACK", help="独立核验:解回每条资产与源素材比误差")
    parser.add_argument("--preview", metavar="DIR", help="渲染 240x320 预览图(bg + SD + 正文带)")
    parser.add_argument("--title-art", help="标题图路径(默认 <source>/title_bg.jpg)")
    parser.add_argument("--bg-quality", type=int, default=DEFAULT_BG_QUALITY, help=f"背景 JPEG 质量(默认 {DEFAULT_BG_QUALITY})")
    parser.add_argument("--crop", choices=("top", "bottom", "center"), default="top",
                        help="336x480 缩到宽 240 后取 320 行的位置(默认 top,与千恋＊万花一致)")
    parser.add_argument("--sd-mode", choices=("jpeg", "palette"), default="jpeg",
                        help="SD 存法:jpeg=原样入库(默认,1.86MB)/ palette=调色板 PNG(7.32MB,仅作对照)")
    parser.add_argument("--missing-placeholders", action="store_true",
                        help="给源工程没有图的 ev* 等名字写 kind=5 占位条目")
    parser.add_argument("--budget-mb", type=float, default=DEFAULT_BUDGET_MB, help=f"图片包预算 MB(默认 {DEFAULT_BUDGET_MB})")
    args = parser.parse_args(argv)
    if args.check:
        return check_pack(Path(args.check))
    if args.compare:
        return compare_pack(Path(args.compare), Path(args.source))
    return build(args)


if __name__ == "__main__":
    raise SystemExit(main())
