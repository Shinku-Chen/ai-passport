#!/usr/bin/env python3
"""Build the Cafe Stella (cafestella-rw5 @ 90ddb9e) offline pack for AI Passport.

The converter is deliberately independent from the watch app's runtime packer:
it reads the converted graph + per-node frame JSON (the authoritative source
semantics), verifies every reference, and writes a bounded binary pack.

Outputs (all inside this worktree):
  assets/cafe/cafe.pak                 binary pack (text + metadata + images)
  assets/cafe/manifest.json            sizes, hashes, counts, image budget
  reports/cafestella/budget.json       detailed flash budget
  reports/cafestella/audit.json        source-to-pack audit counters
  assets/fonts/gal_cjk_symbols.txt     character inventory for tools/gal_font.py

The pack is intentionally not a reimplementation of the watch JSON chunks: text
blocks are zlib-compressed, every frame carries its own decoded art state, and
sprite coordinates are pre-laid-out for a 224x126 stage (the source stage is
432x243; 224/432 == 126/243 exactly).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import os
import re
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

from cafe_jpeg_dedup import dedup_image_records

# ---------------------------------------------------------------------------
# Fixed source and target geometry
# ---------------------------------------------------------------------------
SRC_COMMIT = "90ddb9e020970907b61ad4181f25c65267e6e98b"
SRC_REPO = "https://github.com/xuanyide01/cafestella-rw5"
SRC = Path(r"C:/tmp/ai-passport-six-20261123-79f0/sources/cafestella-rw5")
OUT_PACK = Path("assets/cafe/cafe.pak")
OUT_MANIFEST = Path("assets/cafe/manifest.json")
OUT_BUDGET = Path("reports/cafestella/budget.json")
OUT_AUDIT = Path("reports/cafestella/audit.json")
OUT_CHARS = Path("assets/fonts/gal_cjk_symbols.txt")

STAGE_W, STAGE_H = 224, 126          # packed stage; displayed at 320x180
BG_MAX = (224, 126)
PORTRAIT_H = 126
ATLAS_SIZE = 133                     # 256 * 126 / 243, rounded
BG_QUALITY = 10
PORTRAIT_QUALITY = 18
ATLAS_QUALITY = 18
TEXT_BLOCK_MAX_RAW = 12288
TEXT_BLOCK_MAX_FRAMES = 96

# section types
SEC_META = 1
SEC_NODES = 2
SEC_NEXT = 3
SEC_WHEN = 4
SEC_CHOICE = 5
SEC_COMPLETE = 6
SEC_RULE = 7
SEC_FLAGS = 8
SEC_CHAPTERS = 9
SEC_AFTER = 10
SEC_ENDINGS = 11
SEC_GALLERY = 12
SEC_GALLERY_VAR = 13
SEC_ASSETS = 14
SEC_STRINGS = 15
SEC_TEXT_BLOCKS = 16
SEC_TEXT_DATA = 17
SEC_IMAGES = 18
SEC_IMAGE_DATA = 19
SEC_ROUTES = 20
SEC_PROGRESS = 21
SEC_UI = 22
SEC_GALLERY_ALIAS = 23
SEC_AUDIO = 24
SEC_AUDIO_DATA = 25
SEC_OP = 26
SEC_JPEG_POOL = 27
SEC_JPEG_DESC = 28

ROUTE_ORDER = ["common", "kanna", "natsume", "nozomi", "mei", "suzune", "after"]
ROUTE_ID = {name: i for i, name in enumerate(ROUTE_ORDER)}
ROUTE_LABEL = {
    "common": "共通剧情",
    "kanna": "明月 栞那",
    "natsume": "四季 夏目",
    "nozomi": "墨染 希",
    "mei": "火打谷 爱衣",
    "suzune": "汐山 凉音",
    "after": "后日谈",
}
PROGRESS_KEYS = [
    "bad", "normal", "kanna", "natsume", "nozomi", "mei", "suzune",
    "kanna_after", "natsume_after", "nozomi_after", "mei_after", "suzune_after",
    "lastCleared",
]
PROGRESS_BIT = {k: i for i, k in enumerate(PROGRESS_KEYS)}
MAIN_ENDINGS = ["bad", "normal", "kanna", "natsume", "nozomi", "mei", "suzune"]
AFTER_ENDINGS = ["kanna_after", "natsume_after", "nozomi_after", "mei_after", "suzune_after"]
AFTER_REQUIRES = {
    "kanna_after": "kanna", "natsume_after": "natsume", "nozomi_after": "nozomi",
    "mei_after": "mei", "suzune_after": "suzune",
}
ENDING_TITLE = {
    "bad": ("BAD END", "幕落"),
    "normal": ("NORMAL END", "通往明天的路"),
    "kanna": ("KANNA END", "明月 栞那"),
    "natsume": ("NATSUME END", "四季 夏目"),
    "nozomi": ("NOZOMI END", "墨染 希"),
    "mei": ("MEI END", "火打谷 爱衣"),
    "suzune": ("SUZUNE END", "汐山 凉音"),
    "kanna_after": ("AFTER STORY", "栞那后日谈"),
    "natsume_after": ("AFTER STORY", "夏目后日谈"),
    "nozomi_after": ("AFTER STORY", "希后日谈"),
    "mei_after": ("AFTER STORY", "爱衣后日谈"),
    "suzune_after": ("AFTER STORY", "凉音后日谈"),
}
ENDING_BODY_MAIN = {
    "bad": "故事在此中断。",
    "normal": "沿着没有选择任何人的道路，走到了四季的尽头。",
    "kanna": "与明月栞那一起迎来的结局。",
    "natsume": "与四季夏目一起迎来的结局。",
    "nozomi": "与墨染希一起迎来的结局。",
    "mei": "与火打谷爱衣一起迎来的结局。",
    "suzune": "与汐山凉音一起迎来的结局。",
}
ENDING_BODY_AFTER = "通关后日谈。"

XPOS_FALLBACK = {
    "中": 0, "左接": -240, "右接": 240, "左近": -360, "右近": 360,
    "左中": -480, "右中": 480, "左": -600, "右": 600,
    "左奥": -720, "右奥": 720, "左外": -840, "右外": 840,
    "左大外": -960, "右大外": 960, "左枠外": -1080, "右枠外": 1080,
}

TITLE = "星光咖啡馆与死神之蝶"
SUBTITLE = "Cafe Stella and the Reaper's Butterflies"
VERSION = "v0.1.0-cafestella"
ABOUT = (
    "基于 ai-passport feature/galgame-framework 的离线移植。\n"
    "原作：星光咖啡馆与死神之蝶（YUZUSOFT）。\n"
    "固定源：xuanyide01/cafestella-rw5 @ 90ddb9e0。\n"
    "收录 923 个剧情节点、53,727 句正文、141 个选项、\n"
    "7 个结局、5 篇后日谈、121 组鉴赏 CG。\n"
    "图像按 224x126 舞台降采样并在运行时放大到 320x180。\n"
    "音频与 OP 视频不包含在本固件中；原因见构建文档。\n"
    "本固件仅供个人学习与离线阅读，素材版权归原作者所有。"
)


def u16(v: int) -> bytes:
    return struct.pack("<H", v)


def i16(v: int) -> bytes:
    return struct.pack("<h", max(-32768, min(32767, int(v))))


def u32(v: int) -> bytes:
    return struct.pack("<I", v & 0xFFFFFFFF)


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def asset_num(key: str) -> int:
    m = re.fullmatch(r"a(\d+)", key)
    if not m:
        raise ValueError(f"bad asset id {key!r}")
    return int(m.group(1))


class Strings:
    def __init__(self) -> None:
        self.buf = bytearray(b"\x00")
        self.offsets: dict[str, int] = {"": 0}

    def add(self, s: str | None) -> int:
        if not s:
            return 0
        if s in self.offsets:
            return self.offsets[s]
        off = len(self.buf)
        self.buf += s.encode("utf-8") + b"\x00"
        if len(self.buf) > 0xFFFFFFFF:
            raise ValueError("string table too large")
        self.offsets[s] = off
        return off


class PngEncoder:
    """Encoder for the pack's RGBA PNG-like portrait/atlas images.

    We keep the source's alpha channel (1-bit, thresholded) so portraits stay
    transparent.  A custom two-part record is smaller and easier to decode than
    a PNG: JPEG colour + zlib 1bpp/2bpp mask.
    """

    def __init__(self) -> None:
        self.data = bytearray()
        self.records: list[dict] = []

    def add_raw_jpeg(self, raw: bytes, width: int, height: int,
                     gallery_group: int = 0xFFFF) -> int:
        rid = len(self.records)
        off = len(self.data)
        self.data += raw
        self.records.append({
            "id": rid, "kind": 0, "w": width, "h": height,
            "off": off, "len": len(raw), "mask_off": 0, "mask_len": 0,
            "mask_raw": 0, "crc": crc32(raw), "gallery": gallery_group,
        })
        return rid

    def add_jpeg(self, img: Image.Image, quality: int, gallery_group: int = 0xFFFF) -> int:
        buf = io.BytesIO()
        img.convert("RGB").save(buf, "JPEG", quality=quality, optimize=True, subsampling=2)
        raw = buf.getvalue()
        rid = len(self.records)
        off = len(self.data)
        self.data += raw
        self.records.append({
            "id": rid, "kind": 0, "w": img.width, "h": img.height,
            "off": off, "len": len(raw), "mask_off": 0, "mask_len": 0,
            "mask_raw": 0, "crc": crc32(raw), "gallery": gallery_group,
        })
        return rid

    def add_jpeg_mask(self, img: Image.Image, quality: int, gallery_group: int = 0xFFFF) -> int:
        rgba = img.convert("RGBA")
        # 透明区 RGB 用最近的不透明边缘颜色填充，避免 JPEG 把白底渗进立绘边缘形成白边。
        arr = np.array(rgba)
        rgb = arr[:, :, :3].astype(np.uint16)
        alpha = arr[:, :, 3]
        known = alpha >= 250
        h, w = known.shape
        for _ in range(max(h, w)):
            if known.all():
                break
            n_rgb = rgb.copy()
            n_known = known.copy()
            for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                srgb = np.roll(rgb, (dy, dx), axis=(0, 1))
                sknown = np.roll(known, (dy, dx), axis=(0, 1))
                if dy == -1:
                    sknown[-1, :] = False
                elif dy == 1:
                    sknown[0, :] = False
                if dx == -1:
                    sknown[:, -1] = False
                elif dx == 1:
                    sknown[:, 0] = False
                fill = (~n_known) & sknown
                n_rgb[fill] = srgb[fill]
                n_known |= sknown
            rgb, known = n_rgb, n_known
        arr[:, :, :3] = rgb.astype(np.uint8)
        rgba = Image.fromarray(arr, "RGBA")
        buf = io.BytesIO()
        # 立绘/图集用 4:4:4，减少色度下采样造成的透明边缘渗色。
        rgba.convert("RGB").save(buf, "JPEG", quality=quality, optimize=True, subsampling=0)
        raw = buf.getvalue()
        alpha = rgba.getchannel("A")
        w, h = rgba.size
        bits = 2
        stride = (w * bits + 7) // 8
        mask = bytearray(stride * h)
        px = alpha.load()
        for y in range(h):
            base = y * stride
            for x in range(w):
                level = min(3, (px[x, y] * 3 + 127) // 255)
                if level:
                    bitpos = x * bits
                    shift = 8 - bits - (bitpos & 7)
                    mask[base + (bitpos >> 3)] |= level << shift
        mraw = bytes(mask)
        mcomp = zlib.compress(mraw, 9)
        rid = len(self.records)
        off = len(self.data)
        self.data += raw
        mask_off = len(self.data)
        self.data += mcomp
        self.records.append({
            "id": rid, "kind": 2, "w": w, "h": h,
            "off": off, "len": len(raw), "mask_off": mask_off,
            "mask_len": len(mcomp), "mask_raw": len(mraw),
            "crc": crc32(bytes(raw) + mcomp), "gallery": gallery_group,
        })
        return rid


def pack_node_record(nid: int, node: dict, strings: Strings) -> bytes:
    return struct.pack(
        "<HHHHHHHBBBBBBB3xIII",
        nid,
        node["frame_count"],
        node["block_first"],
        node["block_count"],
        node["next_first"],
        node["choice_first"],
        node["complete_first"],
        node["next_count"],
        node["choice_count"],
        node["complete_count"],
        ROUTE_ID.get(node["route"], 0),
        1 if node.get("adult") else 0,
        1 if node.get("after") else 0,
        0,
        strings.add(node.get("title", "")),
        strings.add(node.get("chapter", "")),
        strings.add(node.get("source_label", "")),
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=str(SRC))
    ap.add_argument("--out", default=str(OUT_PACK))
    ap.add_argument("--no-images", action="store_true", help="metadata/text only (budget experiments)")
    ap.add_argument("--media", default="assets/cafe/media/media.json",
                    help="media manifest generated by tools/cafe_media.py; empty disables")
    ap.add_argument("--report-dir", default=os.environ.get("CAFE_REPORT_DIR",
                    "reports/cafestella-media-20261123-1130"),
                    help="versioned budget/audit output directory; never overwrites old reports")
    args = ap.parse_args()
    global OUT_BUDGET, OUT_AUDIT, OUT_MANIFEST
    OUT_BUDGET = Path(args.report_dir) / "budget.json"
    OUT_MANIFEST = Path(args.out).parent / "manifest.json"
    OUT_AUDIT = Path(args.report_dir) / "audit.json"
    src = Path(args.src)
    out_pack = Path(args.out)

    graph = json.loads((src / "converted/graph.json").read_text("utf-8"))
    image_map = json.loads((src / "converted/image-map.json").read_text("utf-8"))
    gallery = json.loads((src / "converted/gallery.json").read_text("utf-8"))
    nodes_json = graph["nodes"]
    assert graph["version"] == 2, graph.get("version")
    assert graph["adultEnabled"] is True
    assert graph["initial"] == "n0"

    # ------------------------------------------------------------------
    # Source audit: ids, frames, choices, conditions, after stories.
    # ------------------------------------------------------------------
    node_ids: dict[str, int] = {}
    for key in nodes_json:
        m = re.fullmatch(r"n(\d+)", key)
        if not m:
            raise SystemExit(f"unexpected node id {key!r}")
        node_ids[key] = int(m.group(1))
    if sorted(node_ids.values()) != list(range(len(node_ids))):
        raise SystemExit("node ids are not contiguous n0..nN")

    frame_total = 0
    op_total = 0
    text_frames = 0
    choice_nodes = 0
    choice_total = 0
    all_asset_refs: set[int] = set()
    all_complete: set[str] = set()
    all_effects: set[str] = set()
    all_conditions: set[str] = set()
    fake_ops: list[tuple[str, int]] = []
    for key, node in nodes_json.items():
        count = int(node["count"])
        nid = node_ids[key]
        if node.get("choices"):
            choice_nodes += 1
            choice_total += len(node["choices"])
        if int(node["count"]):
            raw = json.loads((src / "converted" / f"{key}.json").read_text("utf-8"))
            if len(raw) != count:
                raise SystemExit(f"{key}: n json has {len(raw)} frames, graph says {count}")
            for i, frame in enumerate(raw):
                frame_total += 1
                if frame.get("kind") == "op":
                    op_total += 1
                    fake_ops.append((key, i))
                    continue
                text_frames += 1
                art = frame.get("art")
                if not art:
                    raise SystemExit(f"{key}[{i}]: missing art")
                bg = art.get("background")
                if not bg:
                    raise SystemExit(f"{key}[{i}]: missing background")
                if bg not in image_map:
                    raise SystemExit(f"{key}[{i}]: unknown background {bg}")
                all_asset_refs.add(asset_num(bg))
                for sp in art.get("sprites", []):
                    aid = sp.get("asset")
                    if not aid or aid not in image_map:
                        raise SystemExit(f"{key}[{i}]: unknown sprite asset {aid!r}")
                    all_asset_refs.add(asset_num(aid))
        else:
            if (src / "converted" / f"{key}.json").exists():
                extra = json.loads((src / "converted" / f"{key}.json").read_text("utf-8"))
                if extra:
                    raise SystemExit(f"{key}: graph count=0 but n json is not empty")
        for c in node.get("choices", []):
            all_effects.add(c["effect"][0])
        for w in node.get("next", []):
            if w.get("when"):
                expr, negate = w["when"]
                for part in expr.split("&&"):
                    part = part.strip()
                    if part == ".checkAnyClear":
                        continue
                    m = re.fullmatch(r"(\w+)\s*(==|!=)\s*(\d+)", part)
                    if not m:
                        raise SystemExit(f"unknown condition {part!r} in {key}")
                    all_conditions.add(m.group(1))
        all_complete.update(node.get("complete") or [])

    if frame_total != 53728 or text_frames != 53727 or op_total != 1:
        raise SystemExit(f"frame count mismatch: total={frame_total} text={text_frames} op={op_total}")
    if len(nodes_json) != 923:
        raise SystemExit(f"node count mismatch: {len(nodes_json)}")
    if choice_nodes != 69 or choice_total != 141:
        raise SystemExit(f"choice mismatch: nodes={choice_nodes} options={choice_total}")
    if len(graph["afterStories"]) != 5:
        raise SystemExit("afterStory count mismatch")
    if set(graph["endings"]) != set(MAIN_ENDINGS):
        raise SystemExit(f"ending keys mismatch: {graph['endings']}")
    if len(gallery) != 121:
        raise SystemExit(f"gallery group mismatch: {len(gallery)}")
    gallery_variants = sum(len(g["variants"]) for g in gallery)
    if gallery_variants != 1631:
        raise SystemExit(f"gallery variant mismatch: {gallery_variants}")

    # ------------------------------------------------------------------
    # Image encoding.  Encode every image file once; asset records point at it.
    # ------------------------------------------------------------------
    enc = PngEncoder()
    image_id_by_src: dict[str, int] = {}
    image_meta: dict[int, dict] = {}
    encoded_bytes = 0

    def encode_bg(path: Path) -> int:
        key = str(path)
        if key in image_id_by_src:
            return image_id_by_src[key]
        img = Image.open(path).convert("RGB")
        img.thumbnail(BG_MAX, Image.Resampling.LANCZOS)
        rid = enc.add_jpeg(img, BG_QUALITY)
        image_id_by_src[key] = rid
        image_meta[rid] = {"src": key, "kind": "bg", "w": img.width, "h": img.height}
        return rid

    def encode_portrait(path: Path) -> int:
        key = str(path)
        if key in image_id_by_src:
            return image_id_by_src[key]
        img = Image.open(path).convert("RGBA")
        w = max(1, round(img.width * PORTRAIT_H / img.height))
        img = img.resize((w, PORTRAIT_H), Image.Resampling.LANCZOS)
        rid = enc.add_jpeg_mask(img, PORTRAIT_QUALITY)
        image_id_by_src[key] = rid
        image_meta[rid] = {"src": key, "kind": "portrait", "w": w, "h": PORTRAIT_H}
        return rid

    def encode_atlas(path: Path) -> int:
        key = str(path)
        if key in image_id_by_src:
            return image_id_by_src[key]
        img = Image.open(path).convert("RGBA")
        img = img.resize((ATLAS_SIZE, ATLAS_SIZE), Image.Resampling.LANCZOS)
        rid = enc.add_jpeg_mask(img, ATLAS_QUALITY)
        image_id_by_src[key] = rid
        image_meta[rid] = {"src": key, "kind": "atlas", "w": ATLAS_SIZE, "h": ATLAS_SIZE}
        return rid

    # Gallery group per image id (first group wins).
    gallery_groups_by_image: dict[int, list[int]] = {}
    asset_to_src: dict[int, str] = {}
    for aid_key, info in image_map.items():
        asset_to_src[asset_num(aid_key)] = info["src"]

    if not args.no_images:
        for gi, group in enumerate(gallery):
            for var in group["variants"]:
                src_path = src / ("src" + var["src"])
                if not src_path.exists():
                    raise SystemExit(f"gallery image missing: {src_path}")
                # gallery images are all background assets
                bg_id = None
                for aid_key, info in image_map.items():
                    if info["src"] == var["src"] and not info.get("portrait"):
                        bg_id = encode_bg(src_path)
                        break
                if bg_id is not None:
                    gallery_groups_by_image.setdefault(bg_id, []).append(gi)

    # 一张源图可能被多个鉴赏组复用（本作 25/45.jpg 属于 100/105/110）。
    # 图片记录保留首组，额外组写 alias 表，阅读时一次把所有组标为已看。
    gallery_alias_records = bytearray()
    for rid, groups in sorted(gallery_groups_by_image.items()):
        if rid >= len(enc.records):
            continue
        enc.records[rid]["gallery"] = groups[0]
        for gi in groups[1:]:
            gallery_alias_records += u16(rid) + u16(gi)

    asset_records = bytearray()
    max_asset = max(asset_to_src) if asset_to_src else 0
    default = struct.pack("<HHHHHHHHHHHHBB", 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    asset_records += default * (max_asset + 1)
    missing_assets = 0
    for aid, info in sorted(((asset_num(k), v) for k, v in image_map.items())):
        src_path = src / ("src" + info["src"])
        if not src_path.exists():
            raise SystemExit(f"image missing: {src_path}")
        if info.get("portrait"):
            base_id = encode_portrait(src_path) if not args.no_images else 0xFFFF
            atlas_id = 0xFFFF
            ax = ay = aw = ah = 0
            px = py = pw = ph = 0
            if info.get("patch"):
                atlas_path = src / ("src" + info["patch"])
                if not atlas_path.exists():
                    raise SystemExit(f"atlas missing: {atlas_path}")
                atlas_id = encode_atlas(atlas_path) if not args.no_images else 0xFFFF
                x, y, w, h = info["patchRect"]
                scale_p = PORTRAIT_H / 243.0
                px, py = round(x * scale_p), round(y * scale_p)
                pw, ph = max(1, round(w * scale_p)), max(1, round(h * scale_p))
                atlas = info.get("patchAtlas", [256, 256, 0, 0])
                scale_a = ATLAS_SIZE / 256.0
                ax, ay = round(atlas[2] * scale_a), round(atlas[3] * scale_a)
                aw, ah = max(1, round(w * scale_a)), max(1, round(h * scale_a))
            rec = struct.pack(
                "<HHHHHHHHHHHHBB",
                base_id, atlas_id, info.get("width", 0), info.get("height", 0),
                px, py, pw, ph, ax, ay, aw, ah, 1, 0,
            )
        else:
            bg_id = encode_bg(src_path) if not args.no_images else 0xFFFF
            rec = struct.pack(
                "<HHHHHHHHHHHHBB",
                bg_id, 0xFFFF, image_meta.get(bg_id, {}).get("w", 0) if not args.no_images else 0,
                image_meta.get(bg_id, {}).get("h", 0) if not args.no_images else 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            )
        off = aid * 26
        asset_records[off:off + 26] = rec

    # Title / UI images (7 unlockable title backgrounds).
    ui_images = []
    ui_entries = [
        ("default", "title-hero.jpg"),
        ("bg2", "title-bg-2.jpg"),
        ("kanna", "title-bg-kan.jpg"),
        ("natsume", "title-bg-nat.jpg"),
        ("nozomi", "title-bg-noz.jpg"),
        ("mei", "title-bg-mei.jpg"),
        ("suzune", "title-bg-4.jpg"),
    ]
    for name, fname in ui_entries:
        path = src / "src/common/ui" / fname
        if not path.exists():
            raise SystemExit(f"UI image missing: {path}")
        if args.no_images:
            rid = 0xFFFF
        else:
            # 标题背景只有 7 张，单独用 q18；不挤占剧情背景的 q10 预算。
            img = Image.open(path).convert("RGB")
            img.thumbnail(BG_MAX, Image.Resampling.LANCZOS)
            rid = enc.add_jpeg(img, 18)
        ui_images.append((name, rid))

    # ------------------------------------------------------------------
    # Strings and metadata tables
    # ------------------------------------------------------------------
    strings = Strings()
    # Reserve core UI strings early so the offsets are stable/diagnosable.
    for s in (TITLE, SUBTITLE, VERSION, ABOUT):
        strings.add(s)
    for name in ROUTE_ORDER:
        strings.add(ROUTE_LABEL[name])
    for key in PROGRESS_KEYS:
        strings.add(key)
    for key in sorted(all_effects | all_conditions | set(graph.get("flagRules", {}))):
        strings.add(key)

    # Frame blocks + text data.
    text_data = bytearray()
    block_records = bytearray()
    frame_block_map: dict[tuple[int, int], int] = {}
    blocks_meta: list[dict] = []
    first_node_by_image: dict[int, int] = {}
    for key, node in nodes_json.items():
        nid = node_ids[key]
        count = int(node["count"])
        if not count:
            node["block_first"] = 0
            node["block_count"] = 0
            node["frame_count"] = 0
            continue
        raw_frames = json.loads((src / "converted" / f"{key}.json").read_text("utf-8"))
        chunk = bytearray()
        chunk_frames = 0
        first_frame = 0
        node_blocks = []

        def flush_chunk() -> None:
            nonlocal chunk, chunk_frames, first_frame
            if not chunk_frames:
                return
            comp = zlib.compress(bytes(chunk), 9)
            off = len(text_data)
            text_data.extend(comp)
            bidx = len(blocks_meta)
            blocks_meta.append({
                "node": nid, "first": first_frame, "count": chunk_frames,
                "raw": len(chunk), "off": off, "comp": len(comp),
                "crc": crc32(bytes(chunk)),
            })
            node_blocks.append(bidx)
            first_frame += chunk_frames
            chunk = bytearray()
            chunk_frames = 0

        for i, frame in enumerate(raw_frames):
            if i > 0 and (chunk_frames >= TEXT_BLOCK_MAX_FRAMES or
                          len(chunk) >= TEXT_BLOCK_MAX_RAW):
                flush_chunk()
            if frame.get("kind") == "op":
                chunk += b"\x01" + u16(0) + u16(0) + u16(0xFFFF) + b"\x00" + b"\x00\x00"
            else:
                art = frame["art"]
                bg = asset_num(art["background"])
                bg_src = asset_to_src.get(bg)
                if bg_src:
                    bg_img = image_id_by_src.get(str(src / ("src" + bg_src)))
                    if bg_img is not None:
                        first_node_by_image.setdefault(bg_img, nid)
                sprites = art.get("sprites", [])
                speaker = frame.get("speaker", "").replace("　", " ").encode("utf-8")
                text = frame.get("text", "").replace("　", " ").encode("utf-8")
                if len(speaker) > 0xFFFF or len(text) > 0xFFFF:
                    raise SystemExit(f"{key}[{i}]: text too long")
                chunk += b"\x00" + u16(len(speaker)) + u16(len(text)) + u16(bg) + bytes([len(sprites)])
                # Pre-layout sprites for the 224x126 stage using the watch
                # packer's balanced composition (see tools/pack-story.py).
                laid = []
                for sp in sprites:
                    aid = asset_num(sp["asset"])
                    info = image_map[sp["asset"]]
                    w = max(1, round(info["width"] * PORTRAIT_H / 243.0))
                    stage_x = float(sp.get("x", XPOS_FALLBACK.get(sp.get("position"), 0)))
                    laid.append((aid, w, stage_x, int(sp.get("opacity", 255))))
                centers = [STAGE_W / 2.0 + sx * STAGE_W / 2600.0 for _, _, sx, _ in laid]
                if len(laid) == 3:
                    denom = 2600.0 / 1.28
                    edge_pad = 22.0 * STAGE_W / 432.0
                    centers = [STAGE_W / 2.0 + sx * STAGE_W / denom for _, _, sx, _ in laid]
                else:
                    edge_pad = 16.0 * STAGE_W / 432.0
                if len(laid) >= 2:
                    safe_l = [max(56.0 * STAGE_W / 432.0, w / 2.0 - edge_pad) for _, w, _, _ in laid]
                    safe_r = [min(376.0 * STAGE_W / 432.0, STAGE_W - w / 2.0 + edge_pad) for _, w, _, _ in laid]
                    mid = (min(centers) + max(centers)) / 2.0
                    if abs(mid - STAGE_W / 2.0) < 25.0 * STAGE_W / 432.0:
                        mid = STAGE_W / 2.0
                    k = 1.0
                    for c, sl, sr in zip(centers, safe_l, safe_r):
                        dc = c - mid
                        if dc < 0 and c < sl and (mid - sl) > 0:
                            k = min(k, (mid - sl) / (-dc))
                        elif dc > 0 and c > sr and (sr - mid) > 0:
                            k = min(k, (sr - mid) / dc)
                    k = max(0.35, min(1.0, k))
                    centers = [max(sl, min(sr, mid + (c - mid) * k))
                               for c, sl, sr in zip(centers, safe_l, safe_r)]
                elif len(laid) == 1:
                    w0 = laid[0][1]
                    sl0 = max(64.0 * STAGE_W / 432.0, w0 / 2.0 - 16.0 * STAGE_W / 432.0)
                    sr0 = min(368.0 * STAGE_W / 432.0, STAGE_W - w0 / 2.0 + 16.0 * STAGE_W / 432.0)
                    centers[0] = max(sl0, min(sr0, centers[0]))
                for (aid, w, _, op), center in zip(laid, centers):
                    left = round(center - w / 2.0)
                    top = round(4.0 * STAGE_W / 432.0)
                    chunk += u16(aid) + i16(left) + i16(top) + bytes([max(0, min(255, op)), 0])
                chunk += speaker + bytes([0]) + text + bytes([0])
            chunk_frames += 1
        flush_chunk()
        node["frame_count"] = count
        node["block_first"] = node_blocks[0] if node_blocks else 0
        node["block_count"] = len(node_blocks)
        for bidx in node_blocks:
            frame_block_map[(nid, blocks_meta[bidx]["first"])] = bidx

    # Flag ids: base flags (state.flag) then derived flags (flagRules outputs).
    flag_rules = graph.get("flagRules", {})
    base_names = sorted(all_effects | {c[0] for rules in flag_rules.values() for c in rules})
    # condition names not defined anywhere are still readable flags (default 0)
    for name in sorted(all_conditions):
        if name not in base_names and name not in flag_rules:
            base_names.append(name)
    derived_names = sorted(set(flag_rules.keys()))
    # A key that is both written as an effect and derived is derived in flags()
    base_names = [n for n in base_names if n not in flag_rules]
    flag_names = base_names + derived_names
    flag_id = {name: i for i, name in enumerate(flag_names)}
    if len(flag_names) > 250:
        raise SystemExit("too many flags for uint8 ids")
    rule_records = bytearray()
    for name in derived_names:
        for cmp_name, value, add in flag_rules[name]:
            if cmp_name not in flag_id:
                raise SystemExit(f"rule references unknown flag {cmp_name}")
            rule_records += bytes([flag_id[name], flag_id[cmp_name], int(value) & 0xFF, int(add) & 0xFF])

    # Next / when / choice / complete.
    next_records = bytearray()
    when_records = bytearray()
    choice_records = bytearray()
    complete_records = bytearray()
    when_cache: dict[tuple, int] = {}

    def target_id(to: str) -> int:
        if to == "@end":
            return 0xFFFF
        return node_ids[to]

    def when_id(when) -> int:
        if not when:
            return 0xFFFF
        key = (when[0], bool(when[1]))
        if key in when_cache:
            return when_cache[key]
        expr, negate = when
        comps = []
        check_any = 0
        for part in expr.split("&&"):
            part = part.strip()
            if part == ".checkAnyClear":
                check_any = 1
                continue
            m = re.fullmatch(r"(\w+)\s*(==|!=)\s*(\d+)", part)
            if not m:
                raise SystemExit(f"unknown condition {part!r}")
            comps.append((flag_id[m.group(1)], 0 if m.group(2) == "==" else 1, int(m.group(3))))
        if len(comps) > 7:
            raise SystemExit("too many comparisons")
        rec = bytes([len(comps), 1 if negate else 0, check_any, 0])
        for fid, op, value in comps:
            rec += bytes([fid, op, value, 0])
        rec += b"\x00" * (7 * 4 - len(comps) * 4)
        idx = len(when_records) // 32
        when_records.extend(rec)
        when_cache[key] = idx
        return idx

    node_records = bytearray()
    for key, node in nodes_json.items():
        nid = node_ids[key]
        node["next_first"] = len(next_records) // 4
        node["next_count"] = len(node.get("next", []))
        for w in node.get("next", []):
            next_records += u16(target_id(w["to"])) + u16(when_id(w.get("when")))
        node["choice_first"] = len(choice_records) // 12
        node["choice_count"] = len(node.get("choices", []))
        for c in node.get("choices", []):
            choice_records += (u32(strings.add(c["text"])) + u16(target_id(c["to"])) +
                               bytes([flag_id[c["effect"][0]], int(c["effect"][1]) & 0xFF, 0, 0, 0, 0]))
        node["complete_first"] = len(complete_records) // 2
        node["complete_count"] = len(node.get("complete") or [])
        for c in node.get("complete") or []:
            complete_records += u16(PROGRESS_BIT[c])
        node["source_label"] = (node.get("source") or ["", ""])[0]
        node_records += pack_node_record(nid, node, strings)

    # Chapters.
    chapter_records = bytearray()
    for ch in graph["chapters"]:
        chapter_records += (u16(node_ids[ch["node"]]) + bytes([ROUTE_ID.get(ch["route"], 0),
                              1 if ch.get("adult") else 0]) +
                            u32(strings.add(ch["title"])) +
                            u32(strings.add(ch["chapter"])))
    # After stories.
    after_records = bytearray()
    for a in graph["afterStories"]:
        after_records += (u16(node_ids[a["node"]]) +
                          bytes([PROGRESS_BIT[a["requires"]], 1 if a.get("adult") else 0]) +
                          u32(strings.add(a["title"])) + u32(0))
    # Endings: 7 main + 5 after.
    ending_records = bytearray()
    for key in MAIN_ENDINGS + AFTER_ENDINGS:
        eyebrow, title = ENDING_TITLE[key]
        body = ENDING_BODY_MAIN.get(key, ENDING_BODY_AFTER)
        ending_records += (bytes([PROGRESS_BIT[key], 0 if key in MAIN_ENDINGS else 1]) +
                           u32(strings.add(eyebrow + " · " + title)) +
                           u32(strings.add(body)) + u32(0) + u16(0))
    # Gallery.
    gallery_records = bytearray()
    gallery_var_records = bytearray()
    first_variant = 0
    for gi, group in enumerate(gallery):
        # first node using any variant image
        first_node = 0xFFFF
        for var in group["variants"]:
            bg_id = image_id_by_src.get(str(src / ("src" + var["src"])))
            if bg_id is None:
                continue
            fn = first_node_by_image.get(bg_id, 0xFFFF)
            if fn < first_node:
                first_node = fn
        gallery_records += (u16(first_variant) + u16(len(group["variants"])) +
                           bytes([0 if group["section"] == "kan" else 1, 0]) +
                           u32(strings.add(group["id"])) + u16(first_node) + u16(0) + u16(0))
        for var in group["variants"]:
            aid = None
            for k, info in image_map.items():
                if info["src"] == var["src"] and not info.get("portrait"):
                    aid = asset_num(k)
                    break
            if aid is None:
                raise SystemExit(f"gallery variant not mapped: {var['src']}")
            gallery_var_records += u16(aid)
        first_variant += len(group["variants"])

    # Routes table.
    route_records = bytearray()
    for i, name in enumerate(ROUTE_ORDER):
        route_records += u32(strings.add(ROUTE_LABEL[name])) + bytes([i, 1 if name == "after" else 0, 0, 0])
    # Progress labels.
    progress_records = bytearray()
    for key in PROGRESS_KEYS:
        progress_records += u32(strings.add(key))
    # UI image table: name_off + image id.
    ui_records = bytearray()
    for name, rid in ui_images:
        ui_records += u32(strings.add(name)) + u16(rid) + u16(0)

    # Optional media: two IMA tracks + 287 OP JPEGs (224x126 q15, 3 fps).
    # OP images are appended to the image section; story asset IDs stay unchanged.
    audio_records = bytearray()
    audio_data = bytearray()
    op_record = b""
    media_manifest = None
    media_path = Path(args.media) if args.media else None
    if media_path and media_path.exists() and not args.no_images:
        media_manifest = json.loads(media_path.read_text("utf-8"))
        for tid, key in ((0, "title"), (1, "opening")):
            m = media_manifest["audio"][key]
            raw_path = Path(m["ima"])
            if not raw_path.is_absolute():
                raw_path = Path(__file__).resolve().parent.parent / raw_path
            raw_audio = raw_path.read_bytes()
            if len(raw_audio) != m["ima_bytes"]:
                raise SystemExit(f"media audio length mismatch: {raw_path}")
            audio_records += struct.pack(
                "<BBHBBHIIII", tid, 0 if key == "title" else 1,
                m["sample_rate"], m["channels"], m["bits"], m["block_align"],
                m["samples"], len(audio_data), len(raw_audio), crc32(raw_audio))
            audio_data += raw_audio
        op = media_manifest["op"]
        first_op_image = len(enc.records)
        for f in op["frame_files"]:
            fp = Path(f)
            if not fp.is_absolute():
                fp = Path(__file__).resolve().parent.parent / fp
            raw = fp.read_bytes()
            enc.add_raw_jpeg(raw, int(op["width"]), int(op["height"]))
        op_record = struct.pack(
            "<HHHBBIIII", first_op_image, int(op["frames"]), int(op["fps"]),
            1, 0, int(op["duration_ms"]), int(op["audio_guard_ms"]),
            int(op["audio_content_ms"]), int(media_manifest["audio"]["opening"]["samples"]))
        print(f"media: audio={len(audio_data)} op_images={int(op['frames'])} "
              f"op_jpeg={op['jpeg_bytes']} first_image={first_op_image}")

    # Losslessly deduplicate exact JPEG marker segments.  The original
    # entropy/table bytes remain in the pack; only repeated headers are pooled.
    image_count = len(enc.records)
    new_image_data, new_image_records, jpeg_pool, jpeg_desc = dedup_image_records(
        enc.records, bytes(enc.data), image_count)
    enc.data = bytearray(new_image_data)
    enc.records = new_image_records
    print(f"jpeg dedup: images={image_count} pool={len(jpeg_pool)} desc={len(jpeg_desc)} "
          f"image_data={len(enc.data)}")

    # Image records.
    image_records = bytearray()
    for rec in enc.records:
        image_records += struct.pack(
            "<HBBHHIIIIIII",
            rec["id"], rec["kind"], 0, rec["w"], rec["h"],
            rec["off"], rec["len"], rec["mask_off"], rec["mask_len"],
            rec["mask_raw"], rec["gallery"], rec["crc"],
        )

    # META.
    meta = struct.pack(
        "<16I",
        len(nodes_json), len(flag_names), len(base_names), len(ROUTE_ORDER),
        len(graph["chapters"]), len(graph["afterStories"]), len(ending_records) // 16,
        len(gallery), gallery_variants, max_asset + 1, len(enc.records),
        len(blocks_meta), frame_total, len(PROGRESS_KEYS),
        len(text_data), len(enc.data),
    )

    sections = [
        (SEC_META, meta, 1, 1),
        (SEC_NODES, bytes(node_records), len(nodes_json), 1),
        (SEC_NEXT, bytes(next_records), len(next_records) // 4, 1),
        (SEC_WHEN, bytes(when_records), len(when_records) // 32, 1),
        (SEC_CHOICE, bytes(choice_records), len(choice_records) // 12, 1),
        (SEC_COMPLETE, bytes(complete_records), len(complete_records) // 2, 1),
        (SEC_RULE, bytes(rule_records), len(rule_records) // 4, 1),
        (SEC_FLAGS, b"".join(u32(strings.add(n)) for n in flag_names), len(flag_names), 1),
        (SEC_CHAPTERS, bytes(chapter_records), len(chapter_records) // 12, 1),
        (SEC_AFTER, bytes(after_records), len(after_records) // 12, 1),
        (SEC_ENDINGS, bytes(ending_records), len(ending_records) // 16, 1),
        (SEC_GALLERY, bytes(gallery_records), len(gallery), 1),
        (SEC_GALLERY_VAR, bytes(gallery_var_records), gallery_variants, 1),
        (SEC_ASSETS, bytes(asset_records), max_asset + 1, 1),
        (SEC_STRINGS, bytes(strings.buf), len(strings.buf), 1),
        (SEC_TEXT_BLOCKS, bytes(block_records), len(blocks_meta), 1),
        (SEC_TEXT_DATA, bytes(text_data), len(text_data), 1),
        (SEC_IMAGES, bytes(image_records), len(enc.records), 1),
        (SEC_IMAGE_DATA, bytes(enc.data), len(enc.data), 1),
        (SEC_ROUTES, bytes(route_records), len(ROUTE_ORDER), 1),
        (SEC_PROGRESS, bytes(progress_records), len(PROGRESS_KEYS), 1),
        (SEC_UI, bytes(ui_records), len(ui_images), 1),
        (SEC_GALLERY_ALIAS, bytes(gallery_alias_records), len(gallery_alias_records) // 4, 1),
        (SEC_JPEG_POOL, jpeg_pool, 0, 1),
        (SEC_JPEG_DESC, jpeg_desc, image_count, 1),
    ]
    if media_manifest is not None:
        sections.extend([
            (SEC_AUDIO, bytes(audio_records), len(audio_records) // 24, 1),
            (SEC_AUDIO_DATA, bytes(audio_data), len(audio_data), 1),
            (SEC_OP, op_record, 1, 1),
        ])
    # The text block index must point at the compressed text data offsets; it
    # was built before section placement, so serialize it now with the right
    # per-block offsets (offsets are relative to SEC_TEXT_DATA, as the reader
    # adds the section base).
    block_records = bytearray()
    for b in blocks_meta:
        block_records += (u16(b["node"]) + u16(b["first"]) + u16(b["count"]) +
                          u16(b["raw"]) + u32(b["off"]) + u32(b["comp"]) + u32(b["crc"]))
    sections[15] = (SEC_TEXT_BLOCKS, bytes(block_records), len(blocks_meta), 1)

    header_size = 128
    sec_table_size = 24 * len(sections)
    data_off = header_size + sec_table_size
    data_off = (data_off + 15) & ~15
    placed = []
    cursor = data_off
    for sec_type, payload, count, _ in sections:
        cursor = (cursor + 15) & ~15
        placed.append((sec_type, cursor, len(payload), count, crc32(payload)))
        cursor += len(payload)
    total_size = cursor

    source_files = [src / "converted/graph.json", src / "converted/image-map.json",
                    src / "converted/gallery.json"]
    source_files += sorted((src / "converted").glob("n*.json"))
    source_manifest = {str(p.relative_to(src)).replace("\\", "/"): sha256_file(p)
                       for p in source_files}
    source_sha = hashlib.sha256(
        "\n".join(f"{k} {v}" for k, v in sorted(source_manifest.items())).encode()
    ).hexdigest()

    header = struct.pack(
        "<8sIIII32s", b"CSPK1\x00\x00\x00", 1, header_size, total_size,
        len(sections), bytes.fromhex(source_sha),
    )
    header += b"\x00" * (header_size - len(header))
    assert len(header) == header_size
    sec_table = bytearray()
    for sec_type, off, size, count, crc in placed:
        sec_table += struct.pack("<IIIIII", sec_type, off, size, count, crc, 0)
    out = bytearray(header + sec_table)
    out += b"\x00" * (data_off - len(out))
    for (sec_type, payload, count, _), (_, off, size, _, _) in zip(sections, placed):
        out += b"\x00" * (off - len(out))
        out += payload
    out += b"\x00" * (total_size - len(out))
    assert len(out) == total_size

    out_pack.parent.mkdir(parents=True, exist_ok=True)
    out_pack.write_bytes(out)

    # Character inventory: every character that can be shown (story + UI).
    chars = set(TITLE + SUBTITLE + VERSION + ABOUT)
    for node in nodes_json.values():
        chars |= set(node.get("title", "")) | set(node.get("chapter", ""))
        for c in node.get("choices", []):
            chars |= set(c["text"])
    for ch in graph["chapters"]:
        chars |= set(ch["title"]) | set(ch["chapter"])
    for a in graph["afterStories"]:
        chars |= set(a["title"])
    for key in ENDING_TITLE:
        chars |= set(ENDING_TITLE[key][0]) | set(ENDING_TITLE[key][1])
        chars |= set(ENDING_BODY_MAIN.get(key, ENDING_BODY_AFTER))
    for name in ROUTE_LABEL.values():
        chars |= set(name)
    # Story text itself.
    for key, node in nodes_json.items():
        if not node["count"]:
            continue
        for f in json.loads((src / "converted" / f"{key}.json").read_text("utf-8")):
            if f.get("kind") == "op":
                continue
            chars |= set(f.get("text", "")) | set(f.get("speaker", ""))
    # UI source literals (ported reader, not the framework skeleton).
    import glob as _glob
    for path in _glob.glob("main/cafe/*.c") + ["main/main.c"]:
        try:
            src_text = Path(path).read_text("utf-8")
        except OSError:
            continue
        for literal in re.findall(r'"([^"]*)"', src_text):
            chars |= {c for c in literal if ord(c) > 0x7F}
    chars.discard("　")
    chars.discard("盏")
    chars.discard("；")
    chars.discard("é")
    chars -= set("\r\n\t")
    OUT_CHARS.parent.mkdir(parents=True, exist_ok=True)
    OUT_CHARS.write_text(
        "# Generated by tools/cafe_pack.py: every character in the Cafe Stella pack + UI.\n"
        + "".join(sorted(chars)) + "\n", encoding="utf-8")

    # Budget / audit reports.
    section_sizes = {str(t): s for t, _, s, _, _ in placed}
    image_payload = len(enc.data)
    budget = {
        "source": {"repo": SRC_REPO, "commit": SRC_COMMIT, "sha256_manifest": source_sha},
        "pack": {"bytes": total_size, "sha256": hashlib.sha256(out).hexdigest(),
                 "sections": section_sizes},
        "counts": {
            "nodes": len(nodes_json), "frames": frame_total, "text_frames": text_frames,
            "op_markers": op_total, "choice_nodes": choice_nodes, "choice_options": choice_total,
            "chapters": len(graph["chapters"]), "after_stories": len(graph["afterStories"]),
            "endings": len(MAIN_ENDINGS), "gallery_groups": len(gallery),
            "gallery_variants": gallery_variants, "assets": max_asset + 1,
            "images": len(enc.records), "text_blocks": len(blocks_meta),
            "flags": len(flag_names), "base_flags": len(base_names),
            "progress_keys": len(PROGRESS_KEYS),
        },
        "image_budget": {
            "background_quality": BG_QUALITY, "background_max": list(BG_MAX),
            "portrait_quality": PORTRAIT_QUALITY, "portrait_height": PORTRAIT_H,
            "atlas_quality": ATLAS_QUALITY, "atlas_size": ATLAS_SIZE,
            "encoded_image_bytes": len(enc.data),
            "encoded_image_count": len(enc.records),
            "story_image_count": 1997,
            "op_image_count": int(media_manifest["op"]["frames"]) if media_manifest else 0,
            "audio_bytes": len(audio_data),
            "audio_tracks": 2 if media_manifest else 0,
            "gallery_thumb_reuse": True,
            "mask_bits": 2,
            "portrait_subsampling": "4:4:4",
            "title_quality": 18,
            "transparent_edge_fill": "nearest opaque RGB",
            "opening_video": ("224x126 q15 3fps 95.460s" if media_manifest else "not included"),
            "audio": ("title/opening 4kHz IMA" if media_manifest else "not included"),
        },
        "source_shards": {
            "count": len(list((src / "src/common/story").glob("*/*.txt"))),
            "bytes": sum(p.stat().st_size for p in (src / "src/common/story").glob("*/*.txt")),
        },
        "text_data": len(text_data),
        "strings": len(strings.buf),
    }
    OUT_BUDGET.parent.mkdir(parents=True, exist_ok=True)
    OUT_BUDGET.write_text(json.dumps(budget, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    audit = {
        "source": budget["source"],
        "checks": {
            "node_ids_contiguous": True,
            "all_node_targets": True,
            "all_choice_targets": True,
            "all_complete_keys": sorted(all_complete),
            "all_effect_keys": sorted(all_effects),
            "all_condition_keys": sorted(all_conditions),
            "unreachable_condition_keys": sorted(set(all_conditions) - set(flag_rules)),
            "all_asset_refs": len(all_asset_refs),
            "missing_asset_refs": 0,
            "gallery_variants_mapped": gallery_variants,
            "text_blocks_max_raw": max(b["raw"] for b in blocks_meta),
            "text_blocks_max_comp": max(b["comp"] for b in blocks_meta),
            "unknown_assets": [],
            "op_markers": fake_ops,
        },
        "frame_layout": {
            "stage": [STAGE_W, STAGE_H], "display": [320, 180],
            "portrait_height": PORTRAIT_H, "atlas_size": ATLAS_SIZE,
        },
    }
    OUT_AUDIT.parent.mkdir(parents=True, exist_ok=True)
    OUT_AUDIT.write_text(json.dumps(audit, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    flag_report = {
        "base_names": base_names,
        "derived_names": derived_names,
        "flag_names": flag_names,
    }
    (OUT_AUDIT.parent / "flag_ids.json").write_text(
        json.dumps(flag_report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    when_report = [None] * len(when_cache)
    for key, idx in when_cache.items():
        when_report[idx] = {"expr": key[0], "negate": bool(key[1])}
    (OUT_AUDIT.parent / "whens.json").write_text(
        json.dumps(when_report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (OUT_AUDIT.parent / "progress_bits.json").write_text(
        json.dumps(PROGRESS_KEYS, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    OUT_MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    OUT_MANIFEST.write_text(json.dumps({
        "pack": str(out_pack), "bytes": total_size,
        "sha256": hashlib.sha256(out).hexdigest(),
        "source_sha256": source_sha,
        "source_commit": SRC_COMMIT,
        "counts": {**budget["counts"], "story_images": 1997,
                   "op_images": int(media_manifest["op"]["frames"]) if media_manifest else 0},
        "media": {"manifest_sha256": hashlib.sha256(media_path.read_bytes()).hexdigest()
                       if media_manifest else "",
                  "audio_bytes": len(audio_data),
                  "op_frames": int(media_manifest["op"]["frames"]) if media_manifest else 0,
                  "op_jpeg_bytes": int(media_manifest["op"]["jpeg_bytes"]) if media_manifest else 0},
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"pack_bytes": total_size, "images": len(enc.records),
                      "image_bytes": image_payload, "text_bytes": len(text_data),
                      "sha256": hashlib.sha256(out).hexdigest()}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
