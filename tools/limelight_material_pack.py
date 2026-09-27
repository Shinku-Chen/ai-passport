#!/usr/bin/env python3
"""Build the Limelight Lemonade Jam material pack for the AI Passport port.

Source project: https://github.com/skdkzzx/limelight-lemonade-jam-xiaomi-band10
  - 小米手环 9 Pro / 9 / 10 上的《ライムライト・レモネードジャム》同人移植
    (Vela quick app,基于 hezdaaa 的 limelight-lemonade-jam-miband)。
  - 素材与译文版权归原作品与移植者所有;本仓库只保存转换工具,不分发源素材。
    请支持正版。

Container layout (little-endian;固件按偏移直读 Flash,不需要解压):

  header 32 B : magic "LLMPK001", version u32, entries u32, blobs u32,
                quality u16, screen_w u16, screen_h u16, bg_rows u16, flags u32
  index       : entries × { off u32, len u32, w u16, h u16, kind u8, flags u8 }
                kind == KIND_SPRITE 时追加 { jpeg_len u32 };索引尾补 0 对齐到 4 字节
  blobs       : 去重后的段各写一次,每段 4 字节对齐(即每个条目 off 都是 4 的倍数)
                KIND_IMAGE  → JPEG
                KIND_SPRITE → JPEG 紧跟 1bpp RLE 遮罩
                KIND_META   → UTF-8 文本(生成参数 + 条目名表)

Image conversion (竖屏 240x320,半透明文本框压在画面底部):

  背景 bcgi 336x480  -> 等比缩放宽到 240(高 343)-> 取顶部 bg_rows 行 -> JPEG
  CG   evig/ev*      -> 同上,取顶部 cg_rows 行
  立绘 cimg/*、evig/sd* -> 按 alpha 包围盒裁剪、等比缩放进 168x252 -> JPEG + 1bpp 遮罩
  其它(标题图等)      -> 等比缩放宽到 240 -> JPEG

  背景默认只存 214 行:该行以下是文本框的地盘,存了也只是被压暗。
  CG 默认存满 320 行,因为鉴赏模式要整屏看。

立绘遮罩为什么不用 PNG:设备端没有 PNG 解码器,而 1bpp RLE 的解码器只有十几行,
体积与 PNG 同级(实测)。遮罩编码:

  <u32 raw_len><u8 count><u8 value>...   解到 raw_len 字节为止
  raw = 逐行 1bpp 位打包,每行 ceil(w/8) 字节,行首在最高位,1 = 不透明,
        行与行不共用字节。

素材裁剪:脚本(src/common/script/scriptDataN.txt)与鉴赏列表(src/pages/cgs/cgs.ux)
都没引用的素材默认丢弃(--keep-all 保留),丢弃量写进报告与 meta。

Usage:
  python tools/limelight_material_pack.py --source <手环移植版仓库根> \\
      --out build/limelight_pack.bin [--quality 35] [--bg-rows 214] \\
      [--cg-rows 320] [--keep-all] [--report]

生成的包暂不提交:仓库里还没有消费它的阅读器应用,4.9 MiB 素材在没有读取方之前
不该进固件(见 docs/development/ai-guide.md 的素材放置与交付要求)。
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import io
import json
import re
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageFilter
    _PIL_IMAGE = Image.Image
    _PIL_MISSING = None
except ImportError:  # pragma: no cover - 纯逻辑(容器/裁剪规则)不需要 Pillow
    Image = None
    _PIL_IMAGE = None
    _PIL_MISSING = "需要 Pillow 才能转换图片: python -m pip install pillow"

MAGIC = b"LLMPK001"
VERSION = 1
GEN_VERSION = "limelight_material_pack/1"

SCREEN_W, SCREEN_H = 240, 320
SPRITE_MAX_W, SPRITE_MAX_H = 168, 252
# 立绘取景:源立绘多是细高的全身站姿(实测约 238x924)。整张缩进 252 行时缩放比
# 只有 0.27,脸部被压到十几像素,真机上就是"糊"。按 SPRITE_BUST_ASPECT 的比例裁到
# 膝盖,把像素集中在头肩胸;本来就矮/半身的图(坐姿、特写)不裁。
# 取景到膝盖:对话框是半透明的,下半身会透出轮廓,所以不能只留上半身;
# 但小腿及以下在框里也看不清,裁到膝盖能省下的像素留给头肩。1:2.4 大致就是
# 站姿人物"头顶到膝盖"的比例(素材实测头顶到膝盖约占全身 58~62%)。
SPRITE_BUST_ASPECT = 2.4
# 缩到 ~120x240 这个量级后 LANCZOS 会明显发软,轻量 USM 把线条找回来。
# 参数是拿真机尺寸的立绘做 A/B 定的:半径 1px、强度 60%、阈值 2(不动平坦区)。
# 强度再高(110%)观感提升有限,但 JPEG 熵增会把素材包顶上去约 0.4 MiB,不划算。
SPRITE_SHARPEN_RADIUS = 1.0
SPRITE_SHARPEN_PERCENT = 60
SPRITE_SHARPEN_THRESHOLD = 2
# 透明区的 RGB 要向外扩散几圈再编码:JPEG 会在边缘混合相邻像素,如果透明处是
# 黑色,立绘轮廓就会出现一圈暗边(真机观感就是"糊+脏")。扩散轮数 = 像素圈数。
SPRITE_EDGE_BLEED = 3
# 立绘像素上限:固件按 LIME_SPRITE_MAX_PIXELS(23,000)申请静态解码缓冲,必须放得下。
# 取 22,000 px:上半身取景后立绘约 105x210 = 22k px(全身取景时约 65x252 = 16.4k)。
# 实测(2026-09-27,q30/4:4:4)上半身 16k/18k/20k/22k 相对旧包只 +47/+42/+110/+162 KiB,
# 因为裁掉下半身后总像素并没有增加,而字节率几乎不变(0.225 B/px)。
SPRITE_MAX_PIXELS = 22000
DEFAULT_QUALITY = 30
# 立绘单独一套参数:它是画面主体,和背景共用 q30/4:2:0 会明显发糊(色度被砍半、
# 高频细节被量化掉)。背景大片色块对压缩不敏感,立绘细节敏感,所以分开。
# 实测各配置的包体积见 tools/README 或 REPORT 注释。
DEFAULT_SPRITE_QUALITY = 30
DEFAULT_SPRITE_SUBSAMPLING = 0        # 0 = 4:4:4;立绘色度细节比省下的几十 KB 值钱
# 实测(2026-09-27,全部素材 + 名字表 + 1bpp RLE 遮罩,图片预算 5,590,384 B):
#   q35/CG 320 行 = 5,533,336 B(仅余 57 KiB,太紧);q32 = 5,240,392 B(余 0.33 MiB);
#   q30 = 5,050,124 B(余 0.50 MiB)。q30~q35 相对 Lanczos 参考的 PSNR 只差约 0.5 dB,
#   所以默认取 q30 换余量。
DEFAULT_BG_ROWS = 214      # 画面区高度(下面被正文带压住)
# CG 也只存 214 行:画布要能整块塞进 DRAM(240x320 画布 + 立绘缓冲会超),
# 而正文流程里 CG 的下 1/3 本来就被正文带盖住;鉴赏页把它贴在画布上沿显示。
DEFAULT_CG_ROWS = 214

KIND_IMAGE, KIND_SPRITE, KIND_META = 0, 1, 2
FLAG_FILTERED = 1 << 0
FLAG_BG_CROPPED = 1 << 1

HEADER = struct.Struct("<8sIIIHHHHI")      # 32 B
INDEX = struct.Struct("<IIHHBB")           # 14 B
FAMILIES = ("bg", "cg", "sprite", "misc")
FAMILY_ORDER = {"bg": 0, "cg": 1, "sprite": 2, "misc": 3}
SOURCE_DIRS = ("bcgi", "cimg", "evig")


# --------------------------------------------------------------------------
# 容器(纯逻辑,不依赖 Pillow)
# --------------------------------------------------------------------------

def build_container(entries, blobs, quality, bg_rows, flags) -> bytes:
    """entries: [(kind, name, w, h, blob_id, jpeg_len)];blobs: 去重后的字节段。

    数据段按 blob 顺序各写一次,条目通过 blob_id 复用同一段(内容相同的素材因此
    只存一份)。索引尾部补 0 使数据段起点 4 字节对齐,所以每个条目 off 都是 4 的倍数。
    """
    assert HEADER.size == 32, HEADER.size
    idx_len = sum(INDEX.size + (4 if entry[0] == KIND_SPRITE else 0) for entry in entries)
    idx_pad = (-idx_len) % 4
    base = 32 + idx_len + idx_pad

    blob_offsets = []
    pos = base
    for i, blob in enumerate(blobs):
        blob_offsets.append(pos)
        pos += len(blob)
        if i != len(blobs) - 1:
            pos += (-pos) % 4

    body = bytearray()
    for i, blob in enumerate(blobs):
        body += blob
        if i != len(blobs) - 1:
            while (base + len(body)) % 4:
                body += b"\x00"
    assert len(body) == pos - base, (len(body), pos - base, len(blobs))

    index = bytearray()
    for kind, _name, w, h, blob_id, jpeg_len in entries:
        total = len(blobs[blob_id])
        index += INDEX.pack(blob_offsets[blob_id], total, w, h, kind, 0)
        if kind == KIND_SPRITE:
            index += struct.pack("<I", total if jpeg_len is None else jpeg_len)
    assert len(index) == idx_len, (len(index), idx_len)

    header = HEADER.pack(MAGIC, VERSION, len(entries), len(blobs), quality,
                         SCREEN_W, SCREEN_H, bg_rows, flags)
    return header + bytes(index) + b"\x00" * idx_pad + bytes(body)


def parse_container(blob: bytes) -> tuple[dict, list[dict]]:
    """解析容器,返回 (头部信息, 条目列表)。自检与宿主测试共用。"""
    if len(blob) < 32 or blob[:8] != MAGIC:
        raise ValueError("魔数或长度不对")
    _magic, version, n_entries, n_blobs, quality, width, height, bg_rows, flags = \
        HEADER.unpack_from(blob, 0)
    if version != VERSION:
        raise ValueError(f"版本 {version} != {VERSION}")
    pos = 32
    entries = []
    for i in range(n_entries):
        off, length, w, h, kind, entry_flags = INDEX.unpack_from(blob, pos)
        pos += INDEX.size
        jpeg_len = None
        if kind == KIND_SPRITE:
            jpeg_len = struct.unpack_from("<I", blob, pos)[0]
            pos += 4
        if kind not in (KIND_IMAGE, KIND_SPRITE, KIND_META):
            raise ValueError(f"条目 {i} 的 kind={kind} 非法")
        if off < 32 or off + length > len(blob):
            raise ValueError(f"条目 {i} 越界 off={off} len={length} size={len(blob)}")
        entries.append({"index": i, "off": off, "len": length, "w": w, "h": h,
                        "kind": kind, "flags": entry_flags, "jpeg_len": jpeg_len})
    head = {"version": version, "n_entries": n_entries, "n_blobs": n_blobs,
            "quality": quality, "screen": (width, height), "bg_rows": bg_rows,
            "flags": flags, "index_end": pos}
    return head, entries


class BlobStore:
    """按内容去重的段存储;记录每条新段归属的家族,便于分家族记账。"""

    def __init__(self) -> None:
        self.blobs: list[bytes] = []
        self.by_key: dict[str, int] = {}
        self.owner: list[str] = []
        self.dedup_hits = 0
        self.dedup_saved = 0

    def put(self, key: str, data: bytes, family: str) -> int:
        if key in self.by_key:
            self.dedup_hits += 1
            self.dedup_saved += len(data)
            return self.by_key[key]
        blob_id = len(self.blobs)
        self.blobs.append(data)
        self.by_key[key] = blob_id
        self.owner.append(family)
        return blob_id


# --------------------------------------------------------------------------
# 素材转换
# --------------------------------------------------------------------------

def family_of(folder: str, name: str) -> str:
    if folder == "bcgi":
        return "bg"
    if folder == "cimg":
        return "sprite"
    if folder == "evig":
        if name.startswith("ev"):
            return "cg"
        if name.startswith("sd"):
            return "sprite"
    return "misc"


def fit_width(im: Image.Image, width: int, rows: int | None = None) -> Image.Image:
    """等比缩放到宽 width;给了 rows 就裁/补到 rows 行。

    高出的部分取顶部(画面主体通常在上方),不足的部分上下居中留黑
    —— 标题图(336x189 -> 240x135)就是这样居中放进 240x214 画布的。
    固件按"图片尺寸 == 画布尺寸"解码,所以打包器必须保证每条都是画布尺寸。
    """
    im = im.convert("RGB")
    height = max(1, round(im.height * width / im.width))
    im = im.resize((width, height), Image.LANCZOS)
    if rows is None:
        return im
    if height >= rows:
        return im.crop((0, 0, width, rows))
    canvas = Image.new("RGB", (width, rows), (0, 0, 0))
    canvas.paste(im, (0, (rows - height) // 2))
    return canvas


def sprite_canvas(im: Image.Image, max_pixels: int = SPRITE_MAX_PIXELS) -> Image.Image:
    """裁到不透明边界 -> 等比缩放进 168x252 -> 再夹到 max_pixels 像素以内。

    像素上限是为了让固件能把立绘直接解进画布的空闲区域(见 SPRITE_MAX_PIXELS 注释)。
    """
    rgba = im.convert("RGBA")
    box = rgba.getchannel("A").getbbox()
    if box:
        rgba = rgba.crop(box)
    bust_h = min(rgba.height, max(1, round(rgba.width * SPRITE_BUST_ASPECT)))
    if bust_h < rgba.height:
        rgba = rgba.crop((0, 0, rgba.width, bust_h))
    scale = min(SPRITE_MAX_W / rgba.width, SPRITE_MAX_H / rgba.height)
    size = (max(1, round(rgba.width * scale)), max(1, round(rgba.height * scale)))
    rgba = rgba.resize(size, Image.LANCZOS)
    if rgba.width * rgba.height > max_pixels:
        # 用 floor 而不是 round:四舍五入会让 w*h 略微超过上限(实测超 145 px),
        # 而固件是按上限申请静态缓冲的。缩完再断言一次,别让契约靠"应该差不多"。
        shrink = (max_pixels / (rgba.width * rgba.height)) ** 0.5
        rgba = rgba.resize((max(1, int(rgba.width * shrink)),
                            max(1, int(rgba.height * shrink))), Image.LANCZOS)
        while rgba.width * rgba.height > max_pixels and rgba.width > 1 and rgba.height > 1:
            rgba = rgba.resize((max(1, rgba.width - 1), max(1, rgba.height - 1)), Image.LANCZOS)
    assert rgba.width * rgba.height <= max_pixels, (rgba.size, max_pixels)
    return rgba


def sharpen_sprite(rgba: Image.Image) -> Image.Image:
    """只在 RGB 上做 USM;alpha 原样留给遮罩。"""
    alpha = rgba.getchannel("A")
    rgb = rgba.convert("RGB").filter(
        ImageFilter.UnsharpMask(radius=SPRITE_SHARPEN_RADIUS,
                                percent=SPRITE_SHARPEN_PERCENT,
                                threshold=SPRITE_SHARPEN_THRESHOLD))
    return Image.merge("RGBA", (*rgb.split(), alpha))


def bleed_edges(rgba: Image.Image, rounds: int = SPRITE_EDGE_BLEED) -> Image.Image:
    """把不透明像素的颜色往透明区扩散,避免 JPEG 在轮廓上掺进黑色。"""
    rgb = rgba.convert("RGB")
    alpha = rgba.getchannel("A")
    transparent = alpha.point(lambda v: 255 if v < 128 else 0)
    for _ in range(rounds):
        rgb = Image.composite(rgb.filter(ImageFilter.MaxFilter(3)), rgb, transparent)
    return Image.merge("RGBA", (*rgb.split(), alpha))


def encode_jpeg(im: Image.Image, quality: int, subsampling: int = 2) -> bytes:
    """subsampling: 0 = 4:4:4(保留色度细节,立绘用), 2 = 4:2:0(默认,省体积)。"""
    buf = io.BytesIO()
    im.convert("RGB").save(buf, format="JPEG", quality=quality, optimize=True,
                           subsampling=subsampling)
    return buf.getvalue()


def encode_mask_1bpp_rle(alpha: Image.Image) -> bytes:
    """逐行 1bpp 位打包后按字节做 RLE,格式见模块文档。"""
    width, height = alpha.size
    data = alpha.tobytes()
    raw = bytearray()
    row_bytes = (width + 7) // 8
    for y in range(height):
        row = bytearray(row_bytes)
        base = y * width
        for x in range(width):
            if data[base + x] >= 128:
                row[x >> 3] |= 0x80 >> (x & 7)
        raw += row
    out = bytearray(struct.pack("<I", len(raw)))
    i = 0
    while i < len(raw):
        value = raw[i]
        run = 1
        while i + run < len(raw) and raw[i + run] == value and run < 255:
            run += 1
        out += bytes((run, value))
        i += run
    return bytes(out)


def decode_mask_1bpp_rle(blob: bytes) -> bytes:
    raw_len = struct.unpack_from("<I", blob, 0)[0]
    out = bytearray()
    pos = 4
    while len(out) < raw_len and pos + 1 < len(blob):
        run, value = blob[pos], blob[pos + 1]
        pos += 2
        out += bytes((value,)) * run
    if len(out) != raw_len:
        raise ValueError(f"遮罩 RLE 长度不符: 得到 {len(out)}, 期望 {raw_len}")
    return bytes(out)


# --------------------------------------------------------------------------
# 引用关系
# --------------------------------------------------------------------------

def scan_references(source: Path) -> dict:
    """汇总脚本与鉴赏列表引用的素材名。"""
    refs = {"bg": set(), "char": set(), "cg": set(), "gallery": set(), "scripts": 0}
    script_dir = source / "src" / "common" / "script"
    scripts = sorted(script_dir.glob("scriptData*.txt"),
                     key=lambda p: int(re.sub(r"\D", "", p.stem) or 0))
    for path in scripts:
        try:
            data = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError):
            continue
        refs["scripts"] += 1
        for record in data.values():
            for field, key in (("b", "bg"), ("c", "char"), ("cg", "cg")):
                value = record.get(field)
                if not value:
                    continue
                for name in re.split(r"[;,]", str(value)):
                    name = name.strip()
                    if name:
                        refs[key].add(name)
    page = source / "src" / "pages" / "cgs" / "cgs.ux"
    if page.is_file():
        text = page.read_text(encoding="utf-8", errors="replace")
        refs["gallery"] = {Path(m).name for m in re.findall(r'"(/common/evig/[^"]+)"', text)}
    return refs


def is_referenced(family: str, stem: str, name: str, refs: dict) -> bool:
    if family == "bg":
        return stem in refs["bg"]
    if family == "cg":
        return name in refs["cg"] or name in refs["gallery"]
    if family == "sprite":
        return stem in refs["char"] or name in refs["cg"] or name in refs["gallery"]
    return True                     # 标题图等由界面直接引用,不做裁剪


def filter_enabled(family: str, refs: dict) -> bool:
    """只有拿到能覆盖该家族的引用源时才允许裁剪。

    bg/sprite 靠脚本(b/c 字段),缺脚本就一律保留 —— 否则会把整个背景/立绘
    家族误删。cg 有鉴赏列表兜底(它列全了 evig 下的图)。
    """
    if family == "bg":
        return refs["scripts"] > 0
    if family == "sprite":
        return refs["scripts"] > 0
    if family == "cg":
        return refs["scripts"] > 0 or bool(refs["gallery"])
    return False


def collect_sources(source: Path) -> list[tuple[str, str, Path]]:
    """返回 [(family, folder, path)],顺序固定便于复现。"""
    items = []
    common = source / "src" / "common"
    for folder in SOURCE_DIRS:
        directory = common / folder
        if not directory.is_dir():
            continue
        for path in sorted(directory.iterdir()):
            if path.is_file() and path.suffix.lower() in (".jpg", ".jpeg", ".png"):
                items.append((family_of(folder, path.name), folder, path))
    for path in sorted(common.glob("title_bg*.jpg")):
        items.append(("misc", "", path))
    items.sort(key=lambda item: (FAMILY_ORDER[item[0]], item[1], item[2].name))
    return items


# --------------------------------------------------------------------------
# 打包
# --------------------------------------------------------------------------

def build_pack(source: Path, quality: int, bg_rows: int, cg_rows: int,
               keep_all: bool, max_pixels: int = SPRITE_MAX_PIXELS,
               sprite_quality: int = DEFAULT_SPRITE_QUALITY,
               sprite_subsampling: int = DEFAULT_SPRITE_SUBSAMPLING) -> tuple[bytes, dict]:
    if Image is None:
        raise RuntimeError(_PIL_MISSING)
    refs = scan_references(source)
    store = BlobStore()
    entries: list[tuple] = []
    stats = {family: {"count": 0, "src_bytes": 0, "out_bytes": 0} for family in FAMILIES}
    names: list[str] = []
    skipped = collections.Counter()

    for family, folder, path in collect_sources(source):
        raw = path.read_bytes()
        key = hashlib.sha256(raw).hexdigest()
        pruning = (not keep_all) and filter_enabled(family, refs)
        if pruning and not is_referenced(family, path.stem, path.name, refs):
            skipped["count"] += 1
            skipped["src_bytes"] += len(raw)
            continue

        with Image.open(io.BytesIO(raw)) as im:
            im.load()
            if family == "sprite":
                canvas = bleed_edges(sharpen_sprite(sprite_canvas(im, max_pixels)))
                jpeg = encode_jpeg(canvas, sprite_quality, sprite_subsampling)
                mask = encode_mask_1bpp_rle(
                    canvas.getchannel("A").point(lambda v: 255 if v >= 128 else 0))
                blob_ids = (store.put(key + ":sprite", jpeg + mask, family),)
                kind, jpeg_len = KIND_SPRITE, len(jpeg)
            else:
                # 三类都裁到画布高:固件只接受"尺寸正好等于画布"的图
                # (misc 里的标题图/演出图也一样,否则解码缓冲对不上)。
                rows = {"bg": bg_rows, "cg": cg_rows, "misc": cg_rows}.get(family, bg_rows)
                canvas = fit_width(im, SCREEN_W, rows)
                blob_ids = (store.put(key + ":jpeg", encode_jpeg(canvas, quality), family),)
                kind, jpeg_len = KIND_IMAGE, None
            size = canvas.size

        entries.append((kind, path.name, size[0], size[1], blob_ids[0], jpeg_len))
        # 名字表只存「家族 + 基名」(不带扩展名):剧本里的 b/c 值就是基名,cg 值带
        # 扩展名,固件查询时统一截断到扩展名之前。
        names.append(f"{family}\t{path.stem}")
        stats[family]["count"] += 1
        stats[family]["src_bytes"] += len(raw)

    out_bytes = collections.Counter()
    for blob_id, family in enumerate(store.owner):
        out_bytes[family] += len(store.blobs[blob_id])

    flags = FLAG_FILTERED if any(filter_enabled(f, refs) for f in FAMILIES) and not keep_all else 0
    if bg_rows < SCREEN_H:
        flags |= FLAG_BG_CROPPED

    meta_lines = [
        f"generator={GEN_VERSION}",
        "source=limelight-lemonade-jam-xiaomi-band10",
        f"quality={quality}",
        f"screen={SCREEN_W}x{SCREEN_H}",
        f"bg_rows={bg_rows}",
        f"sprite_max_pixels={SPRITE_MAX_PIXELS}",
        f"cg_rows={cg_rows}",
        f"filtered={'yes' if flags & FLAG_FILTERED else 'no'}",
        f"skipped={skipped['count']} entries, {skipped['src_bytes']} bytes",
        f"references=scripts:{refs['scripts']},gallery:{len(refs['gallery'])}",
        f"blobs={len(store.blobs) + 1}",              # +1 是即将写入的 meta 段本身
        f"dedup={store.dedup_hits} hits, {store.dedup_saved} bytes saved",
    ]
    for family in FAMILIES:
        if stats[family]["count"]:
            meta_lines.append(f"family_{family}={stats[family]['count']} entries, "
                              f"{out_bytes[family]} bytes")
    # 名字表保持"一行对一个条目":meta 条目也占一行,固件按行号取名字才不会错位。
    # 必须在拼 meta_text 之前追加(先拼后加就写不进包里)。
    names.append("meta\tmeta")
    meta_text = "\n".join(meta_lines + [f"entries={len(entries)}", "[names]"] + names) + "\n"
    meta_id = store.put("meta", meta_text.encode("utf-8"), "meta")
    entries.append((KIND_META, "meta", 0, 0, meta_id, None))
    # 名字表保持"一行对一个条目":meta 条目也占一行,这样固件按行号取名字不会错位。
    names.append("meta	meta")

    blob = build_container(entries, store.blobs, quality, bg_rows, flags)
    report = {
        "pack_bytes": len(blob),
        "entries": len(entries),
        "blobs": len(store.blobs),
        "dedup_hits": store.dedup_hits,
        "dedup_saved": store.dedup_saved,
        "skipped": dict(skipped),
        "stats": {family: dict(stats[family], out_bytes=out_bytes[family])
                  for family in FAMILIES},
        "meta_bytes": len(meta_text.encode("utf-8")),
        "pruned_families": [f for f in FAMILIES if (not keep_all) and filter_enabled(f, refs)],
        "refs": {key: (len(value) if isinstance(value, set) else value)
                 for key, value in refs.items()},
    }
    return blob, report


def self_check(blob: bytes) -> dict:
    """逐条解码,核对尺寸、遮罩长度与索引一致。"""
    if Image is None:
        raise RuntimeError(_PIL_MISSING)
    head, entries = parse_container(blob)
    for entry in entries:
        off, length, kind = entry["off"], entry["len"], entry["kind"]
        if kind == KIND_IMAGE:
            with Image.open(io.BytesIO(blob[off:off + length])) as im:
                im.load()
                if im.size != (entry["w"], entry["h"]):
                    raise ValueError(f"条目 {entry['index']} 尺寸 {im.size} != 索引 {entry}")
        elif kind == KIND_SPRITE:
            jpeg_len = entry["jpeg_len"]
            with Image.open(io.BytesIO(blob[off:off + jpeg_len])) as im:
                im.load()
                if im.size != (entry["w"], entry["h"]):
                    raise ValueError(f"条目 {entry['index']} 尺寸 {im.size} != 索引 {entry}")
            raw = decode_mask_1bpp_rle(blob[off + jpeg_len:off + length])
            expected = ((entry["w"] + 7) // 8) * entry["h"]
            if len(raw) != expected:
                raise ValueError(f"条目 {entry['index']} 遮罩 {len(raw)} != {expected}")
    head["checked"] = len(entries)
    return head


def main() -> int:
    parser = argparse.ArgumentParser(
        description="打包 limelight 素材(供 AI Passport 移植;需要 Pillow)")
    parser.add_argument("--source", required=True, type=Path,
                        help="手环移植版仓库根(须含 src/common/{bcgi,cimg,evig})")
    parser.add_argument("--out", required=True, type=Path, help="输出 pack 路径")
    parser.add_argument("--quality", type=int, default=DEFAULT_QUALITY,
                        help=f"JPEG 质量(默认 {DEFAULT_QUALITY})")
    parser.add_argument("--bg-rows", type=int, default=DEFAULT_BG_ROWS,
                        help=f"背景保留行数(默认 {DEFAULT_BG_ROWS},下面被文本框压住)")
    parser.add_argument("--cg-rows", type=int, default=DEFAULT_CG_ROWS,
                        help=f"CG 保留行数(默认 {DEFAULT_CG_ROWS},鉴赏要整屏)")
    parser.add_argument("--sprite-quality", type=int, default=DEFAULT_SPRITE_QUALITY,
                        help="立绘 JPEG 质量(默认 %d;立绘是画面主体,比背景给得高)"
                             % DEFAULT_SPRITE_QUALITY)
    parser.add_argument("--sprite-subsampling", type=int, default=DEFAULT_SPRITE_SUBSAMPLING,
                        choices=(0, 1, 2), help="立绘色度抽样:0=4:4:4(默认),2=4:2:0")
    parser.add_argument("--keep-all", action="store_true",
                        help="不做引用裁剪(默认丢弃脚本与鉴赏都没引用的素材)")
    parser.add_argument("--report", action="store_true", help="打印分家族体积")
    args = parser.parse_args()

    if not (1 <= args.quality <= 100):
        parser.error("--quality 必须在 1..100")
    for name in ("bg_rows", "cg_rows"):
        if not (1 <= getattr(args, name) <= SCREEN_H):
            parser.error(f"--{name.replace('_', '-')} 必须在 1..{SCREEN_H}")
    if not (args.source / "src" / "common").is_dir():
        parser.error(f"--source 下找不到 src/common: {args.source}")

    blob, report = build_pack(args.source, args.quality, args.bg_rows,
                              args.cg_rows, args.keep_all,
                              sprite_quality=args.sprite_quality,
                              sprite_subsampling=args.sprite_subsampling)
    head = self_check(blob)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)

    total = report["pack_bytes"]
    print(f"pack {args.out}: {total} B = {total / 1048576:.3f} MiB")
    print(f"  条目 {report['entries']}(含 1 条 meta),段 {report['blobs']},"
          f"去重命中 {report['dedup_hits']} 次省 {report['dedup_saved']} B,"
          f"meta {report['meta_bytes']} B")
    if not report["pruned_families"]:
        print("  引用裁剪: 未启用(源里没有脚本,或已 --keep-all),全部保留")
    elif args.keep_all:
        print("  引用裁剪: 已关闭(--keep-all)")
    else:
        print(f"  引用裁剪: 覆盖 {','.join(report['pruned_families'])};丢弃 "
              f"{report['skipped'].get('count', 0)} 张 / {report['skipped'].get('src_bytes', 0)} B"
              f"(脚本 {report['refs']['scripts']} 份,鉴赏 {report['refs']['gallery']} 条)")
    if args.report:
        for family in FAMILIES:
            item = report["stats"][family]
            if item["count"]:
                print(f"  {family:6} {item['count']:5d} 张  源 {item['src_bytes'] / 1048576:7.2f} MiB"
                      f"  打包 {item['out_bytes'] / 1048576:7.2f} MiB"
                      f"  平均 {item['out_bytes'] / item['count'] / 1024:6.1f} KiB")
    print(f"  自检: {head['checked']} 条全部解码通过;背景 {head['bg_rows']} 行,"
          f"质量 {head['quality']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
