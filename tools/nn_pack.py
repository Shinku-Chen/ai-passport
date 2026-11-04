#!/usr/bin/env python3
"""Build the NianNiang (男娘恋爱物语) resource pack for the AI Passport port.

Source project: https://github.com/hrk666666/NianNiang-MiBand-10 (HEAD b963df9)
  A Xiaomi Vela / aiot quick-app port of the Ren'Py short visual novel
  《男娘恋爱物语》. The upstream art is already re-composited for a 336x480
  portrait watch screen with `resize()` (no crop), so the original aspect ratio
  is unknown. This pack therefore ships *fit* crops (no crop, no extra stretch)
  and records that fact in the manifest.

The pack is a single little-endian binary read straight out of flash: the
firmware embeds it in the application image (`.rodata`, memory mapped by the
instruction cache), so there is no decompression, no partition lookup and no
per-record JSON parsing at runtime.

  header  : magic "NNPK01\\0\\0", version u32, total size u32, section count u32
  section : { type u32, offset u32, count u32, size u32 }

  SEC_TEXT     ( 0) UTF-8 string blob (dialogue, speakers, choices, endings, titles)
  SEC_META     ( 1) UTF-8 key=value lines (source repo/commit, conversion params)
  SEC_IMAGE    (16) { off u32, jpeg_len u32, mask_len u32, w u16, h u16,
                      kind u8, pad u8, pad u16 }   (20-byte record)
                                                   kind 0 = background, 1 = sprite,
                                                    2 = CG thumbnail, 3 = event CG,
                                                    4 = title art
  SEC_SCENE    (17) { first_op u16, op_count u16, name_off u32, name_len u16, pad u16 }
  SEC_OP       (18) 12-byte fixed records { type u8, arg u8, a u16, b u32, c u32 }
  SEC_SPEAKER  (19) { name_off u32, name_len u16, pad u16 }
  SEC_CHOICE   (20) { text_off u32, text_len u16, scene u16, op u16, flags u8, pad u8 }
  SEC_ENDING   (21) { name_off u32, text_off u32, name_len u16, text_len u16,
                      first_scene u16, pad u16 }  (16-byte record)

Text/script are stored uncompressed: the whole script is ~80 KB, and an
uncompressed fixed-record layout keeps the device side free of any decode
buffer (no zlib, no per-block window).

Image conversion (fixed landscape screen 320x240):
  background / CG  336x480 JPEG -> height 240 (width 168) -> JPEG, drawn centred
                   on a black canvas (fit; no crop, no extra stretch)
  event thumbnails 84x120 JPEG for the CG gallery grid
  sprite           240x480 RGBA PNG -> height 240 (width 120) -> crop to the
                   alpha bounding box -> RGB JPEG + 1bpp alpha plane,
                   bottom anchored
  title art        336x480 JPEG -> same fit as a background

Usage:
  python tools/nn_pack.py --source <NianNiang checkout> \
      --out assets/nn/nn_pack.bin \
      --manifest build/nn_manifest.json \
      --charset assets/nn/nn_charset.txt
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import struct
import sys
from typing import Dict, List, Optional, Tuple

try:
    from PIL import Image
except ImportError:  # pragma: no cover - tool dependency
    sys.exit("需要 Pillow: python -m pip install pillow")

MAGIC = b"NNPK01\0\0"
VERSION = 1
GEN_VERSION = "nn_pack/1"

SCREEN_W, SCREEN_H = 320, 240
ART_H = 240                    # fit height for backgrounds / CGs
BG_QUALITY = 75
THUMB_QUALITY = 70
THUMB_H = 120
SPRITE_QUALITY = 80
TITLE_SOURCE = "title_bg.jpg"

SEC_TEXT, SEC_META = 0, 1
SEC_IMAGE, SEC_SCENE, SEC_OP, SEC_SPEAKER, SEC_CHOICE, SEC_ENDING = 16, 17, 18, 19, 20, 21

SEC_NAMES = {
    SEC_TEXT: "text", SEC_META: "meta", SEC_IMAGE: "image", SEC_SCENE: "scene",
    SEC_OP: "op", SEC_SPEAKER: "speaker", SEC_CHOICE: "choice", SEC_ENDING: "ending",
}

# ---- op types (must match main/nn_pack.h) -----------------------------------
OP_SAY, OP_BG, OP_EV, OP_SPRITE, OP_CHAPTER, OP_JUMP, OP_CHOICE, OP_END, OP_FX = range(9)
OP_NAMES = ["say", "bg", "ev", "sprite", "chapter", "jump", "choice", "end", "fx"]

NONE16 = 0xFFFF
NONE8 = 0xFF
CHOICE_CONTINUE = 1 << 0       # "next op in the same scene" (source had an empty jump)

# Ending ids: fixed by the source sentinels, names from the upstream engine.
ENDING_NAMES = {
    "gameend_badend1": (0, "报警结局", "BAD END 1"),
    "gameend_badend2": (1, "关系破裂", "BAD END 2"),
    "gameend_normalend": (2, "室友结局", "NORMAL END"),
    "gameend_trueend": (3, "真结局", "TRUE END"),
}
ENDING_TEXT = {
    0: "你按下了报警键。警灯映在体育馆的玻璃上，你们的夏天停在了这一天。",
    1: "你说出的那句话把她推远。之后的走廊里，你们再没有并肩走过。",
    2: "你们还是室友。日子照旧，只是有些话谁也没有再提起。",
    3: "她笑着把伞往你这边倾了倾。梅雨季结束的那天，你们一起走出了校门。",
}
ENDING_ORDER = ["gameend_badend1", "gameend_badend2", "gameend_normalend", "gameend_trueend"]


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


class Strings:
    """UTF-8 blob with byte-exact dedup and (offset, length) addressing.

    每条字符串在 blob 里都以 NUL 结尾：固件要把指针直接交给 LVGL 的
    lv_label_set_text()，它要求 C 字符串。多花的每串 1 字节换来“不做运行时拷贝”。
    """

    def __init__(self) -> None:
        self.buf = bytearray()
        self.index: Dict[bytes, Tuple[int, int]] = {}

    def add(self, text: str) -> Tuple[int, int]:
        raw = text.encode("utf-8")
        if raw in self.index:
            return self.index[raw]
        off = len(self.buf)
        self.buf += raw + bytes([0])
        self.index[raw] = (off, len(raw))
        return off, len(raw)


def load_scene(path: str) -> List[list]:
    with open(path, "rb") as fh:
        return json.loads(fh.read().decode("utf-8"))


def bbox_of(alpha) -> Optional[Tuple[int, int, int, int]]:
    return alpha.getbbox()


def sprite_to_jpeg_mask(im: "Image.Image") -> Tuple[bytes, bytes, int, int]:
    """RGBA (already scaled) -> cropped RGB JPEG + 1bpp alpha rows.

    The firmware decodes the JPEG straight onto the scene canvas and masks each
    pixel from the 1bpp plane, so the sprite needs neither a decode buffer nor a
    second image layer. 1bpp alpha is what the published Saya no Uta port uses
    for exactly this screen; 4bpp masks would cost ~3x the flash for edges that
    are hard to see on a 120 px-wide sprite.
    """
    rgba = im.convert("RGBA")
    box = bbox_of(rgba.getchannel("A"))
    if box is None:
        raise SystemExit("立绘全透明，无法打包")
    w, h = box[2] - box[0], box[3] - box[1]
    crop = rgba.crop(box)
    alpha = crop.getchannel("A")
    px = alpha.load()
    mask = bytearray()
    for y in range(h):
        acc = 0
        n = 0
        for x in range(w):
            acc = (acc << 1) | (1 if px[x, y] >= 128 else 0)
            n += 1
            if n == 8:
                mask.append(acc)
                acc = 0
                n = 0
        if n:
            mask.append(acc << (8 - n))
    # keep the RGB plane free of the cut-away background so JPEG ringing does not
    # bleed colours through the mask edge
    bg = Image.new("RGBA", crop.size, (0, 0, 0, 255))
    rgb = Image.composite(crop, bg, alpha)
    return jpeg_bytes(rgb, SPRITE_QUALITY), bytes(mask), w, h


def jpeg_bytes(im: "Image.Image", quality: int) -> bytes:
    buf = io.BytesIO()
    im.convert("RGB").save(buf, "JPEG", quality=quality, optimize=True)
    return buf.getvalue()


def fit_image(im: "Image.Image", target_h: int) -> "Image.Image":
    """Scale to `target_h` high, keep aspect (no crop, no extra stretch)."""
    w, h = im.size
    nw = max(1, round(w * target_h / h))
    return im.resize((nw, target_h), Image.LANCZOS)


class Packer:
    def __init__(self, src_root: str) -> None:
        self.src = os.path.join(src_root, "src", "common")
        if not os.path.isdir(self.src):
            raise SystemExit(f"源目录里没有 src/common: {src_root}")
        self.strings = Strings()
        self.images: List[Dict[str, object]] = []
        self.image_id: Dict[str, int] = {}
        self.sprites: List[Dict[str, object]] = []
        self.sprite_id: Dict[str, int] = {}
        self.scenes: List[Dict[str, object]] = []
        self.ops: List[Tuple[int, int, int, int, int]] = []
        self.speakers: List[Tuple[int, int]] = []
        self.speaker_id: Dict[str, int] = {}
        self.choices: List[Dict[str, object]] = []
        self.endings: List[Dict[str, object]] = []
        self.missing: List[str] = []
        self.used_bg: set = set()
        self.used_ev: set = set()
        self.used_ch: set = set()
        self.stats = {"say": 0, "choices": 0, "options": 0, "ends": 0,
                      "bg": 0, "ev": 0, "sprite": 0, "chapter": 0, "jump": 0, "fx": 0}

    # ---------------------------------------------------------------- images
    def add_image(self, key: str, relpath: str, kind: int, data: bytes, w: int, h: int,
                  aux: bytes = b"") -> int:
        idx = len(self.images)
        self.images.append({"key": key, "path": relpath, "kind": kind, "w": w, "h": h,
                            "len": len(data), "data": data, "aux": aux})
        self.image_id[key] = idx
        return idx

    def build_images(self) -> None:
        bgdir = os.path.join(self.src, "bg")
        evdir = os.path.join(self.src, "ev")
        chdir = os.path.join(self.src, "ch")

        for name in sorted(os.listdir(bgdir)):
            if not name.lower().endswith((".jpg", ".jpeg", ".png")):
                continue
            key = os.path.splitext(name)[0]
            with Image.open(os.path.join(bgdir, name)) as im:
                art = fit_image(im, ART_H)
            data = jpeg_bytes(art, BG_QUALITY)
            self.add_image(key, f"bg/{name}", 0, data, art.size[0], art.size[1])   # 0 = background

        for name in sorted(os.listdir(evdir)):
            if not name.lower().endswith((".jpg", ".jpeg", ".png")):
                continue
            key = os.path.splitext(name)[0]
            with Image.open(os.path.join(evdir, name)) as im:
                art = fit_image(im, ART_H)
                thumb = fit_image(im, THUMB_H)
            self.add_image(key, f"ev/{name}", 3, jpeg_bytes(art, BG_QUALITY),
                           art.size[0], art.size[1])   # 3 = event CG
            self.add_image("thumb:" + key, f"ev/{name}", 2,
                           jpeg_bytes(thumb, THUMB_QUALITY), thumb.size[0], thumb.size[1])

        title_path = os.path.join(self.src, TITLE_SOURCE)
        if os.path.exists(title_path):
            with Image.open(title_path) as im:
                art = fit_image(im, ART_H)
            self.add_image("title", TITLE_SOURCE, 4, jpeg_bytes(art, BG_QUALITY),
                           art.size[0], art.size[1])   # 4 = title art

        for name in sorted(os.listdir(chdir)):
            if not name.lower().endswith(".png"):
                continue
            key = "ch/" + os.path.splitext(name)[0]
            with Image.open(os.path.join(chdir, name)) as im:
                scaled = fit_image(im.convert("RGBA"), ART_H)
            jpeg, mask, w, h = sprite_to_jpeg_mask(scaled)
            img_id = self.add_image(key, f"ch/{name}", 1, jpeg, w, h, aux=mask)
            self.sprites.append({"key": key, "path": f"ch/{name}", "w": w, "h": h,
                                 "len": len(jpeg) + len(mask), "image": img_id})
            self.sprite_id[key] = img_id

    # ---------------------------------------------------------------- script
    def speaker(self, name: str) -> int:
        if not name:
            return NONE16
        key = name.strip()
        if not key:
            return NONE16
        if key in self.speaker_id:
            return self.speaker_id[key]
        off, ln = self.strings.add(key)
        idx = len(self.speakers)
        self.speakers.append((off, ln))
        self.speaker_id[key] = idx
        return idx

    def text(self, s: str) -> Tuple[int, int]:
        return self.strings.add(s)

    def emit(self, op: int, arg: int = 0, a: int = 0, b: int = 0, c: int = 0) -> int:
        self.ops.append((op, arg & 0xFF, a & 0xFFFF, b & 0xFFFFFFFF, c & 0xFFFFFFFF))
        return len(self.ops) - 1

    def compile(self) -> None:
        scn_dir = os.path.join(self.src, "scn")
        files = sorted(n for n in os.listdir(scn_dir) if n.startswith("chunk") and n.endswith(".txt"))
        if not files:
            raise SystemExit("没有剧本分块")
        # two passes: first assign every scene its op range + label table,
        # then resolve jumps/choices that point at labels of other scenes.
        parsed = []
        for fname in files:
            parsed.append((fname, load_scene(os.path.join(scn_dir, fname))))

        scene_index = {fname: i for i, (fname, _) in enumerate(parsed)}
        # pass 1: emit ops, record labels as (scene, op_index_after_label)
        labels: Dict[Tuple[str, str], Tuple[int, int]] = {}
        pending: List[Dict[str, object]] = []
        for si, (fname, nodes) in enumerate(parsed):
            first = len(self.ops)
            for node in nodes:
                t = node[0]
                if t == 0:
                    labels[(fname, node[1])] = (si, len(self.ops))
                elif t == 1:
                    off, ln = self.text(node[1])
                    self.emit(OP_CHAPTER, 0, 0, off, ln)
                    self.stats["chapter"] += 1
                elif t == 2:
                    key = node[1]
                    self.used_bg.add(key)
                    if key not in self.image_id:
                        self.missing.append("bg/" + key)
                        continue
                    self.emit(OP_BG, 0, self.image_id[key])
                    self.stats["bg"] += 1
                elif t == 3:
                    spk = self.speaker(node[1])
                    off, ln = self.text(node[2])
                    self.emit(OP_SAY, 0, spk, off, ln)
                    self.stats["say"] += 1
                elif t == 4:
                    pending.append({"scene": si, "op_emit": len(self.ops), "options": node[1]})
                    self.emit(OP_CHOICE, 0, 0, len(node[1]))   # patched below
                    self.stats["choices"] += 1
                    self.stats["options"] += len(node[1])
                elif t == 5:
                    key = node[1]
                    self.used_ev.add(key)
                    if key not in self.image_id:
                        self.missing.append("ev/" + key)
                        continue
                    self.emit(OP_EV, 0, self.image_id[key])
                    self.stats["ev"] += 1
                elif t == 6:
                    pending.append({"scene": si, "op_emit": len(self.ops), "target": node[1]})
                    self.emit(OP_JUMP, 0, 0, 0)
                    self.stats["jump"] += 1
                elif t == 7:
                    key = node[1]
                    if key == "*":
                        self.emit(OP_SPRITE, 0, NONE16)
                        self.stats["sprite"] += 1
                        continue
                    sid = self.sprite_id.get("ch/" + key)
                    if sid is None:
                        self.missing.append("ch/" + key)
                        continue
                    self.used_ch.add(key)
                    self.emit(OP_SPRITE, 0, sid)   # a = image id (kind 1)
                    self.stats["sprite"] += 1
                elif t == 9:
                    self.emit(OP_FX, 0, 0)     # blackout only in this title
                    self.stats["fx"] += 1
            self.scenes.append({"first_op": first, "op_count": len(self.ops) - first,
                                "name": os.path.splitext(fname)[0]})

        def rel_op(scene: int, global_op: int) -> int:
            """Global op index -> index inside `scene` (the pack stores it that way)."""
            return global_op - self.scenes[scene]["first_op"]

        def resolve(scene_name: str, target: str) -> Optional[Tuple[int, int]]:
            """`013@end_1_4_1` -> (scene index, scene-relative op index)."""
            if "@" in target:
                scn, lab = target.split("@", 1)
                fname = f"chunk{scn}.txt"
                si = scene_index.get(fname)
                if si is None:
                    return None
                hit = labels.get((fname, lab))
            else:
                si = scene_index.get(os.path.basename(scene_name))
                hit = labels.get((os.path.basename(scene_name), target))
            if hit is None or si is None:
                return None
            return hit[0], rel_op(hit[0], hit[1])

        # pass 2: patch jumps + choice tables
        fname_by_scene = [f for f, _ in parsed]
        for item in pending:
            scene = item["scene"]
            if "target" in item:
                tgt = str(item["target"])
                if tgt.startswith("*gameend"):
                    key = tgt.lstrip("*")
                    if key not in ENDING_NAMES:
                        raise SystemExit(f"未知结局哨兵: {tgt}")
                    self.emit_patch_end(item["op_emit"], ENDING_NAMES[key][0])
                    self.stats["ends"] += 1
                    continue
                hit = resolve(fname_by_scene[scene], tgt)
                if hit is None:
                    raise SystemExit(f"跳转目标找不到: {fname_by_scene[scene]} -> {tgt}")
                tsi, top = hit
                self.ops[item["op_emit"]] = (OP_JUMP, 0, tsi, top, 0)
            else:
                first_opt = len(self.choices)
                for opt in item["options"]:
                    label_text, jump = opt[0], opt[1]
                    off, ln = self.text(label_text)
                    if jump:
                        hit = resolve(fname_by_scene[scene], jump)
                        if hit is None:
                            raise SystemExit(f"选项跳转找不到: {jump}")
                        tsi, top = hit
                        flags = 0
                    else:
                        tsi, top, flags = scene, rel_op(scene, item["op_emit"] + 1), CHOICE_CONTINUE
                    self.choices.append({"text_off": off, "text_len": ln, "scene": tsi,
                                         "op": top, "flags": flags})
                self.ops[item["op_emit"]] = (OP_CHOICE, 0, first_opt, len(item["options"]), 0)

        # endings table (ordered, always 4)
        for key in ENDING_ORDER:
            eid, zh, tag = ENDING_NAMES[key]
            off, ln = self.strings.add(zh)
            toff, tlen = self.strings.add(ENDING_TEXT[eid])
            self.endings.append({"id": eid, "name_off": off, "name_len": ln,
                                 "text_off": toff, "text_len": tlen, "tag": tag})

        self.scene_names = [s["name"] for s in self.scenes]
        for s in self.scenes:
            off, ln = self.strings.add(s["name"])
            s["name_off"], s["name_len"] = off, ln

    def emit_patch_end(self, idx: int, ending_id: int) -> None:
        self.ops[idx] = (OP_END, 0, ending_id, 0, 0)

    # ---------------------------------------------------------------- output
    def build(self) -> bytes:
        self.build_images()
        self.compile()

        blobs: List[Tuple[int, bytes, int]] = []   # type, payload, count
        blobs.append((SEC_TEXT, bytes(self.strings.buf), len(self.strings.buf)))

        image_tab = bytearray()
        image_data = bytearray()
        base = 0
        for img in self.images:
            image_tab += struct.pack("<IIIHHBBH", base, img["len"], len(img["aux"]),
                                     img["w"], img["h"], img["kind"], 0, 0)
            image_data += img["data"] + img["aux"]
            base += img["len"] + len(img["aux"])

        ops = bytearray()
        for op, arg, a, b, c in self.ops:
            ops += struct.pack("<BBHII", op, arg, a, b, c)

        meta = (f"gen={GEN_VERSION}\n"
                f"source_repo=https://github.com/hrk666666/NianNiang-MiBand-10\n"
                f"ops={len(self.ops)}\nscenes={len(self.scenes)}\n").encode("utf-8")

        sections = [
            (SEC_TEXT, bytes(self.strings.buf), len(self.strings.buf), 0),
            (SEC_META, meta, len(meta), 0),
            (SEC_IMAGE, bytes(image_tab), len(self.images), 0),
            (SEC_SCENE, b"".join(struct.pack("<HHIHH", s["first_op"], s["op_count"],
                                             s["name_off"], s["name_len"], 0)
                                 for s in self.scenes), len(self.scenes), 0),
            (SEC_OP, bytes(ops), len(self.ops), 0),
            (SEC_SPEAKER, b"".join(struct.pack("<IHH", off, ln, 0) for off, ln in self.speakers),
             len(self.speakers), 0),
            (SEC_CHOICE, b"".join(struct.pack("<IHHHBB", c["text_off"], c["text_len"],
                                              c["scene"], c["op"], c["flags"], 0)
                                  for c in self.choices), len(self.choices), 0),
            (SEC_ENDING, b"".join(struct.pack("<IIHHHH", e["name_off"], e["text_off"],
                                              e["name_len"], e["text_len"], 0, 0)
                                  for e in self.endings), len(self.endings), 0),
        ]
        # image payload rides inside the IMAGE section right after its table
        patched = []
        for stype, payload, count, _ in sections:
            if stype == SEC_IMAGE:
                payload = payload + bytes(image_data)
            patched.append((stype, payload, count))
        header_size = 20 + 16 * len(patched)
        body = bytearray()
        table = bytearray()
        for stype, payload, count in patched:
            off = header_size + len(body)
            table += struct.pack("<IIII", stype, off, count, len(payload))
            body += payload
            while len(body) % 4:
                body.append(0)
        total = header_size + len(body)
        out = bytearray()
        out += MAGIC
        out += struct.pack("<III", VERSION, total, len(patched))
        out += table
        out += body
        assert len(out) == total, (len(out), total)
        return bytes(out)

    def manifest(self, pack: bytes) -> Dict[str, object]:
        by_kind = {}
        for img in self.images:
            by_kind.setdefault(img["kind"], [0, 0])
            by_kind[img["kind"]][0] += 1
            by_kind[img["kind"]][1] += img["len"]
        return {
            "generator": GEN_VERSION,
            "source": {
                "repo": "https://github.com/hrk666666/NianNiang-MiBand-10",
                "commit": "b963df9245d885fe66b8e2e9c66588df5b616dc0",
                "scenes": len(self.scenes),
            },
            "counts": {
                "scenes": len(self.scenes),
                "ops": len(self.ops),
                "say": self.stats["say"],
                "choices": self.stats["choices"],
                "options": self.stats["options"],
                "endings": len(self.endings),
                "speakers": len(self.speakers),
                "images_bg": sum(1 for i in self.images if i["kind"] == 0),
                "images_cg": sum(1 for i in self.images if i["kind"] == 3),
                "images_thumb": sum(1 for i in self.images if i["kind"] == 2),
                "images_title": sum(1 for i in self.images if i["kind"] == 4),
                "sprites": sum(1 for i in self.images if i["kind"] == 1),
            },
            "bytes": {
                "pack": len(pack),
                "text": len(self.strings.buf),
                "ops": len(self.ops) * 12,
                "bg": by_kind.get(0, [0, 0])[1],
                "cg": by_kind.get(3, [0, 0])[1],
                "title": by_kind.get(4, [0, 0])[1],
                "thumbs": by_kind.get(2, [0, 0])[1],
                "sprites": by_kind.get(1, [0, 0])[1],
            },
            "assets": {
                "bg_used": sorted(self.used_bg),
                "cg_used": sorted(self.used_ev),
                "sprites_used": sorted(self.used_ch),
                "missing": sorted(self.missing),
            },
            "geometry": {
                "screen": [SCREEN_W, SCREEN_H],
                "fit_height": ART_H,
                "fit_width": 168,
                "note": "source art is 336x480 (0.700) produced by resize(); "
                        "packed as fit (no crop, no further stretch); "
                        "original aspect ratio unknown",
            },
            "sha256": hashlib.sha256(pack).hexdigest(),
        }


def find_source(root: str) -> str:
    """Accept either the project root or the directory that contains src/common."""
    for cand in (root, os.path.join(root, "raw", "src")):
        if os.path.isdir(os.path.join(cand, "src", "common")):
            return cand
    raise SystemExit(f"找不到 src/common（给了 {root}）")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True,
                    help="NianNiang-MiBand-10 checkout (or its raw/src clone)")
    ap.add_argument("--out", default="assets/nn/nn_pack.bin")
    ap.add_argument("--manifest", default="build/nn_manifest.json")
    ap.add_argument("--charset", default="assets/nn/nn_charset.txt")
    args = ap.parse_args()

    src = find_source(args.source)
    packer = Packer(src)
    pack = packer.build()
    manifest = packer.manifest(pack)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(pack)
    os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
    with open(args.manifest, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, ensure_ascii=False, indent=1)

    text = packer.strings.buf.decode("utf-8")
    chars = sorted({c for c in text if c not in "\r\n\t"})
    with open(args.charset, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# 男娘恋爱物语：剧本/选项/结局/章节标题用到的全部字符（tools/nn_pack.py 生成）\n")
        fh.write("".join(chars) + "\n")

    m = manifest
    log(f"pack {args.out}: {m['bytes']['pack']} B  sha256 {m['sha256'][:16]}")
    log(f"  scenes={m['counts']['scenes']} ops={m['counts']['ops']} say={m['counts']['say']} "
        f"choices={m['counts']['choices']} options={m['counts']['options']} "
        f"endings={m['counts']['endings']}")
    log(f"  bg={m['counts']['images_bg']} cg={m['counts']['images_cg']} "
        f"thumbs={m['counts']['images_thumb']} sprites={m['counts']['sprites']}")
    log(f"  text={m['bytes']['text']} B ops={m['bytes']['ops']} B "
        f"bg={m['bytes']['bg']} B cg={m['bytes']['cg']} B title={m['bytes']['title']} B "
        f"thumbs={m['bytes']['thumbs']} B sprites={m['bytes']['sprites']} B")
    if m["assets"]["missing"]:
        log(f"  MISSING: {m['assets']['missing']}")
        return 1
    log(f"  charset={len(chars)} 字符 -> {args.charset}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
