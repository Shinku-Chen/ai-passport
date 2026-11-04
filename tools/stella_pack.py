#!/usr/bin/env python3
"""Build the STELPK01 resource pack for the Stella of the End reader.

Source checkout: xuanyide01/stella-rw5 (Xiaomi Vela port).  The pack is read
directly from flash by the firmware: no runtime JSON parsing, no PNG decoder.

Layout (little endian), header 20 B:
  magic "STELPK01", version u16, header_size u16, section_count u16,
  reserved u16, total_size u32
Sections are {type u32, offset u32, count u32, size u32} (16 B each):
  0 TEXT     UTF-8 blob, strings addressed by (offset, length)
  1 NAME     {off u32, len u16, pad u16}
  2 SCENE    {id u16, route u8, flags u8, name u16, next u16,
              first_op u32, op_count u16, pad u16,
              data_off u32, data_len u32, raw_len u32} = 28 B
  3 OPS      per-scene zlib streams (one independent stream per scene)
  4 GALLERY  groups {label u16, first u16, count u16, pad u16} = 8 B
             entries {asset u16, label u16} = 4 B
  5 ASSET    {off u32, len u32, w u16, h u16, mask_off u32, mask_len u32,
              kind u8, flags u8, ax i16, ay i16, ow u16, oh u16} = 32 B
  6 BLOB     image payloads, 4-byte aligned
  7 META     key=value text

Op record (24 B):
  type u8, body_count u8, bg u16, body[3] u16, speaker u16,
  text_off u32, text_len u16, arg u16, reserved u32
  type 0 text, 1 choice (arg = target op index), 2 end (text = label)

Usage:
  python tools/stella_pack.py --source <stella-rw5 checkout> \
      --out assets/stella/stella_pack.bin --report build/stella_pack_report.json
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import struct
import sys
import zlib
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("需要 Pillow: python -m pip install pillow")

MAGIC = b"STELPK01"
VERSION = 1
HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
NAME = struct.Struct("<IHH")
SCENE = struct.Struct("<HBBHHIHHIII")
OP = struct.Struct("<BBHHHHHIHIH")
ASSET = struct.Struct("<IIHHIIBBhhHHH")
GROUP = struct.Struct("<HHHH")
GENTRY = struct.Struct("<HH")
assert HEADER.size == 20 and SECTION.size == 16 and NAME.size == 8
assert SCENE.size == 28 and OP.size == 24 and ASSET.size == 32, (SCENE.size, OP.size, ASSET.size)

SEC_TEXT, SEC_NAME, SEC_SCENE, SEC_OPS, SEC_GALLERY, SEC_ASSET, SEC_BLOB, SEC_META = range(8)
KIND_BG, KIND_SPRITE, KIND_TITLE = 0, 1, 2

SCREEN_W, SCREEN_H = 320, 240
BG_W, BG_H = 320, 180
TITLE_W, TITLE_H = 320, 240
NONE = 0xFFFF

# route: 0 main, 1 after story, 2 legacy epilogue (not listed in chapter browse).
ROUTE_MAIN, ROUTE_AFTER, ROUTE_LEGACY = 0, 1, 2
SCENE_FLAG_MAIN_CLEAR = 1 << 0
SCENE_FLAG_AFTER_CLEAR = 1 << 1


def extract(path: Path, marker: str):
    s = path.read_text(encoding="utf-8")
    i = s.index(marker) + len(marker)
    while s[i] != "{":
        i += 1
    depth = 0
    instr = False
    esc = False
    for j in range(i, len(s)):
        c = s[j]
        if instr:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                instr = False
            continue
        if c == '"':
            instr = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return json.loads(s[i:j + 1])
    raise ValueError(f"unbalanced JSON in {path}")


class TextBlob:
    def __init__(self) -> None:
        self.buf = bytearray()
        self.offsets: dict[str, tuple[int, int]] = {}

    def add(self, s: str) -> tuple[int, int]:
        if s in self.offsets:
            return self.offsets[s]
        raw = s.encode("utf-8")
        off = len(self.buf)
        self.buf += raw
        self.offsets[s] = (off, len(raw))
        return off, len(raw)


class Packer:
    def __init__(self, source: Path, quality: int, portrait_quality: int) -> None:
        self.source = source
        self.quality = quality
        self.portrait_quality = portrait_quality
        self.text = TextBlob()
        self.names: list[tuple[int, int]] = []
        self.name_index: dict[str, int] = {}
        self.scenes: list[struct.Struct] = []
        self.ops_blobs: list[bytes] = []
        self.scene_meta: list[dict] = []
        self.assets: list[dict] = []
        self.asset_index: dict[str, int] = {}
        self.all_ids: set[str] = set()
        self.blob = bytearray()
        self.report = {
            "source": str(source),
            "quality": quality,
            "portrait_quality": portrait_quality,
            "texts": 0,
            "dialogue_ops": 0,
            "choice_ops": 0,
            "end_ops": 0,
            "scenes": 0,
            "gallery_groups": 0,
            "gallery_entries": 0,
            "assets": 0,
            "asset_bytes": 0,
            "dedup_hits": 0,
            "missing": [],
            "scenes_detail": [],
        }
        self._hash_to_asset: dict[str, int] = {}

    def name(self, s: str) -> int:
        if s not in self.name_index:
            off, ln = self.text.add(s)
            self.name_index[s] = len(self.names)
            self.names.append((off, ln))
        return self.name_index[s]

    def load(self) -> None:
        src = self.source
        self.catalog = extract(src / "src/common/catalog.js", "export const catalog=")
        self.assets_map = extract(src / "src/common/assets.js", "export const assets=")
        self.gallery = json.loads((src / "src/common/gallery.txt").read_text(encoding="utf-8"))
        self.portrait_dims = {}
        pl = (src / "src/common/portrait-layout.js").read_text(encoding="utf-8")
        start = pl.index("const dimensions=") + len("const dimensions=")
        end = pl.index(";", start)
        self.portrait_dims = json.loads(pl[start:end])
        self.ops_by_scene = {}
        self.report["story_files"] = {}
        for sid in sorted(self.catalog["scenes"], key=int):
            d = src / "src/common/story" / sid
            ops = []
            files = []
            if d.is_dir():
                for fn in sorted(d.iterdir()):
                    if fn.suffix == ".txt":
                        files.append(fn.name)
                        ops.extend(json.loads(fn.read_text(encoding="utf-8")))
            self.ops_by_scene[int(sid)] = ops
            self.report["story_files"][sid] = files

    def _image_path(self, asset_id: str) -> Path:
        fn = self.assets_map.get(asset_id)
        if not fn:
            raise SystemExit(f"assets.js 缺少 {asset_id}")
        return self.source / "src/common/images" / fn

    def _encode_bg(self, path: Path, w: int, h: int) -> bytes:
        with Image.open(path) as im:
            im = im.convert("RGB")
            if im.size != (w, h):
                im = im.resize((w, h), Image.Resampling.LANCZOS)
            out = io.BytesIO()
            im.save(out, "JPEG", quality=self.quality, optimize=True, subsampling=2)
            return out.getvalue()

    def _encode_portrait(self, path: Path) -> tuple[bytes, bytes, int, int, int, int]:
        with Image.open(path) as im:
            im = im.convert("RGBA")
            alpha = im.getchannel("A")
            bbox = alpha.getbbox()
            if not bbox:
                bbox = (0, 0, im.width, im.height)
            crop = im.crop(bbox)
            # Keep the source box offset; the firmware adds it back after scaling.
            ax, ay = bbox[0], bbox[1]
            ow, oh = im.width, im.height
            rgb = Image.new("RGB", crop.size, (0, 0, 0))
            rgb.paste(crop, mask=crop.getchannel("A"))
            out = io.BytesIO()
            rgb.save(out, "JPEG", quality=self.portrait_quality, optimize=True, subsampling=2)
            jpeg = out.getvalue()
            # 1 bpp mask, row packed, 1 = opaque.
            mw, mh = crop.size
            stride = (mw + 7) // 8
            mask = bytearray(stride * mh)
            a = crop.getchannel("A").tobytes()
            for y in range(mh):
                row = y * stride
                for x in range(mw):
                    if a[y * mw + x] >= 128:
                        mask[row + (x >> 3)] |= 0x80 >> (x & 7)
            return jpeg, bytes(mask), mw, mh, ax, ay, ow, oh

    def add_asset(self, asset_id: str) -> int:
        self.all_ids.add(asset_id)
        if asset_id in self.asset_index:
            return self.asset_index[asset_id]
        path = self._image_path(asset_id)
        prefix = "as_p_" if asset_id.startswith("as_p_") else "p_" if asset_id.startswith("p_") else "b_"
        if asset_id.startswith("as_b_"):
            prefix = "as_b_"
        if prefix in ("p_", "as_p_"):
            jpeg, mask, w, h, ax, ay, ow, oh = self._encode_portrait(path)
            kind = KIND_SPRITE
        else:
            jpeg = self._encode_bg(path, BG_W, BG_H)
            mask = b""
            w, h, ax, ay, ow, oh = BG_W, BG_H, 0, 0, BG_W, BG_H
            kind = KIND_BG
        digest = hashlib.sha256(jpeg + b"|" + mask).hexdigest()
        if digest in self._hash_to_asset:
            idx = self._hash_to_asset[digest]
            self.asset_index[asset_id] = idx
            self.report["dedup_hits"] += 1
            return idx
        # 4-byte align each payload.
        while len(self.blob) % 4:
            self.blob.append(0)
        off = len(self.blob)
        self.blob += jpeg
        while len(self.blob) % 4:
            self.blob.append(0)
        mask_off = len(self.blob) if mask else 0
        if mask:
            self.blob += mask
            while len(self.blob) % 4:
                self.blob.append(0)
        idx = len(self.assets)
        self.assets.append({
            "id": asset_id, "off": off, "len": len(jpeg), "w": w, "h": h,
            "mask_off": mask_off, "mask_len": len(mask), "kind": kind,
            "ax": ax, "ay": ay, "ow": ow, "oh": oh, "bytes": len(jpeg) + len(mask),
        })
        self.asset_index[asset_id] = idx
        self._hash_to_asset[digest] = idx
        return idx

    def add_title(self) -> int:
        path = self.source / "src/common/title_bg.png"
        jpeg = self._encode_bg(path, TITLE_W, TITLE_H)
        digest = hashlib.sha256(jpeg).hexdigest()
        while len(self.blob) % 4:
            self.blob.append(0)
        off = len(self.blob)
        self.blob += jpeg
        idx = len(self.assets)
        self.assets.append({
            "id": "__title__", "off": off, "len": len(jpeg), "w": TITLE_W, "h": TITLE_H,
            "mask_off": 0, "mask_len": 0, "kind": KIND_TITLE,
            "ax": 0, "ay": 0, "ow": TITLE_W, "oh": TITLE_H, "bytes": len(jpeg),
        })
        self.asset_index["__title__"] = idx
        self._hash_to_asset[digest] = idx
        return idx

    def build_scenes(self) -> None:
        cat = self.catalog
        scenes = cat["scenes"]
        for sid in sorted(self.ops_by_scene, key=int):
            ops = self.ops_by_scene[sid]
            recs = bytearray()
            next_scene = NONE
            op_count = 0
            for o in ops:
                typ = o.get("op")
                if typ == "text":
                    bg = self.add_asset(o["background"]) if o.get("background") else NONE
                    bodies = o.get("bodies") or []
                    if len(bodies) > 3:
                        raise SystemExit(f"scene {sid}: >3 bodies")
                    b = [self.add_asset(x) for x in bodies] + [NONE] * (3 - len(bodies))
                    speaker = self.name(o.get("speaker") or "") if o.get("speaker") else NONE
                    off, ln = self.text.add(o.get("text") or "")
                    recs += OP.pack(0, len(bodies), bg, b[0], b[1], b[2], speaker, off, ln, 0, 0)
                    self.report["dialogue_ops"] += 1
                    op_count += 1
                elif typ == "choice":
                    items = o.get("items") or []
                    if len(items) != 1:
                        raise SystemExit(f"scene {sid}: expected exactly one choice item, got {len(items)}")
                    it = items[0]
                    off, ln = self.text.add(it.get("text") or "")
                    recs += OP.pack(1, 0, NONE, NONE, NONE, NONE, NONE, off, ln, int(it["to"]), 0)
                    self.report["choice_ops"] += 1
                    op_count += 1
                elif typ == "jumpScene":
                    next_scene = int(o["scene"])
                elif typ == "end":
                    label = cat.get("endings", {}).get(str(sid), "阅读完毕")
                    off, ln = self.text.add(label)
                    recs += OP.pack(2, 0, NONE, NONE, NONE, NONE, NONE, off, ln, 0, 0)
                    self.report["end_ops"] += 1
                    op_count += 1
                else:
                    raise SystemExit(f"scene {sid}: unknown op {typ}")
            raw = bytes(recs)
            # 直接 mmap 读取：op 记录本身就是 24 字节定长，压缩只省 200 KB，
            # 却要在 30 KB 堆上再开 28 KB 解压缓冲；这里选择“用 Flash 换 RAM”。
            comp = raw
            self.ops_blobs.append(comp)
            self.scenes.append((int(sid), op_count, next_scene, len(raw), len(comp), sid))
            self.report["scenes"] += 1
            self.report["texts"] += len(ops)
            self.report["scenes_detail"].append({
                "id": int(sid), "ops": op_count, "raw": len(raw), "compressed": len(comp),
                "next": next_scene,
            })

    def include_all_assets(self) -> None:
        # 源仓库里的 p_/b_/as_* 都是作品素材；即使当前剧本/鉴赏表没有引用，
        # 也不能因为“看起来没用到”就静默丢掉。去重后只多出很少的字节。
        for asset_id in self.assets_map:
            self.add_asset(asset_id)

    def build_gallery(self) -> None:
        groups = []
        entries = []
        seen = {}
        for item in self.gallery:
            gid = item.get("group") or item.get("source")
            if gid not in seen:
                seen[gid] = {"label": item.get("label", gid), "first": len(entries), "count": 0}
                groups.append(seen[gid])
            g = seen[gid]
            try:
                aid = self.add_asset(item["image"])
            except KeyError:
                self.report["missing"].append(item.get("image"))
                aid = NONE
            label = self.name(g.get("label", gid))
            entries.append((aid, label))
            g["count"] += 1
        self.groups = groups
        self.gentries = entries
        self.report["gallery_groups"] = len(groups)
        self.report["gallery_entries"] = len(entries)

    def write(self, out_path: Path) -> None:
        # names, scenes, ops, gallery tables
        scene_tab = bytearray()
        ops_blob = bytearray()
        ops_section_off = 0
        for (sid, op_count, next_scene, raw_len, comp_len, _), comp in zip(self.scenes, self.ops_blobs):
            route = ROUTE_MAIN if sid <= 21 else ROUTE_LEGACY if sid == 108 else ROUTE_AFTER
            flags = SCENE_FLAG_MAIN_CLEAR if route == ROUTE_AFTER else 0
            if sid == 109:
                flags |= SCENE_FLAG_AFTER_CLEAR
            name_idx = self.name(self.catalog["scenes"][str(sid)]["name"])
            scene_tab += SCENE.pack(sid, route, flags, name_idx, next_scene, 0, op_count, 0,
                                    ops_section_off, comp_len, raw_len)
            ops_blob += comp
            while len(ops_blob) % 4:
                ops_blob.append(0)
            ops_section_off = len(ops_blob)
        # 场景名/结局/鉴赏标签都可能在场景循环里新增 name；必须最后再序列化
        # NAME 表，否则 section.len 与 count 不一致，后面的名字索引越界。
        name_tab = b"".join(NAME.pack(off, ln, 0) for off, ln in self.names)
        group_tab = b"".join(GROUP.pack(self.name(g["label"]), g["first"], g["count"], 0)
                             for g in self.groups)
        gentry_tab = b"".join(GENTRY.pack(a, l) for a, l in self.gentries)
        asset_tab = bytearray()
        for a in self.assets:
            asset_tab += ASSET.pack(a["off"], a["len"], a["w"], a["h"], a["mask_off"],
                                    a["mask_len"], a["kind"], 0, a["ax"], a["ay"],
                                    a["ow"], a["oh"], 0)
        meta = "\n".join([
            "generator=stella_pack/1",
            f"source={self.source.name}",
            f"quality={self.quality}",
            f"portrait_quality={self.portrait_quality}",
            f"scenes={len(self.scenes)}",
            f"dialogue_ops={self.report['dialogue_ops']}",
            f"choice_ops={self.report['choice_ops']}",
            f"end_ops={self.report['end_ops']}",
            f"gallery_groups={len(self.groups)}",
            f"gallery_entries={len(self.gentries)}",
            f"assets={len(self.assets)}",
            f"initial={self.catalog['initial']}",
            f"after_initial={self.catalog['afterInitial']}",
            f"flow_scene={self.catalog['flowScene']}",
        ] + [f"asset.{i}={a['id']}" for i, a in enumerate(self.assets)] +
            [f"asset_id.{aid}={idx}" for aid, idx in sorted(self.asset_index.items())]).encode("utf-8") + b"\n"

        sections = [
            (SEC_TEXT, bytes(self.text.buf), 0),
            (SEC_NAME, name_tab, len(self.names)),
            (SEC_SCENE, bytes(scene_tab), len(self.scenes)),
            (SEC_OPS, bytes(ops_blob), 0),
            (SEC_GALLERY, group_tab + gentry_tab, len(self.groups)),
            (SEC_ASSET, bytes(asset_tab), len(self.assets)),
            (SEC_BLOB, bytes(self.blob), 0),
            (SEC_META, meta, 0),
        ]
        header_size = HEADER.size + SECTION.size * len(sections)
        offset = header_size
        table = bytearray()
        for typ, payload, count in sections:
            table += SECTION.pack(typ, offset, count, len(payload))
            offset += len(payload)
            while offset % 4:
                offset += 1
        total = offset
        out = bytearray(MAGIC)
        out += struct.pack("<III", VERSION, total, len(sections))
        out += table
        for typ, payload, count in sections:
            out += payload
            while len(out) % 4:
                out.append(0)
        assert len(out) == total, (len(out), total)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(out)
        self.report["assets"] = len(self.assets)
        self.report["asset_ids"] = sorted(self.all_ids)
        self.report["pack_bytes"] = len(out)
        self.report["text_bytes"] = len(self.text.buf)
        self.report["name_count"] = len(self.names)
        self.report["asset_bytes"] = sum(a["bytes"] for a in self.assets)
        self.report["layers"] = {
            "text": len(self.text.buf),
            "names": len(name_tab),
            "scenes": len(scene_tab),
            "ops": len(ops_blob),
            "gallery": len(group_tab) + len(gentry_tab),
            "asset_index": len(asset_tab),
            "blob": len(self.blob),
            "meta": len(meta),
        }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--report", type=Path)
    ap.add_argument("--quality", type=int, default=33)
    ap.add_argument("--portrait-quality", type=int, default=54)
    args = ap.parse_args()
    packer = Packer(args.source, args.quality, args.portrait_quality)
    packer.load()
    packer.add_title()
    packer.build_scenes()
    packer.build_gallery()
    packer.include_all_assets()
    packer.write(args.out)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(packer.report, ensure_ascii=False, indent=2), encoding="utf-8")
    r = packer.report
    print(f"pack: {r['pack_bytes']:,} B")
    print(f"  scenes={r['scenes']} dialogue={r['dialogue_ops']} choice={r['choice_ops']} end={r['end_ops']}")
    print(f"  gallery {r['gallery_groups']} groups / {r['gallery_entries']} entries")
    print(f"  assets={r['assets']} bytes={r['asset_bytes']:,} dedup={r['dedup_hits']} text={r['text_bytes']:,}")
    for k, v in r["layers"].items():
        print(f"  {k:12s} {v:>10,} B")


if __name__ == "__main__":
    main()
