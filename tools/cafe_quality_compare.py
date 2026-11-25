#!/usr/bin/env python3
"""Source-art vs 224x126 pack comparison for title (q18)/daily (q10)/CG (q10).

Source side uses the watch source images (432x243 stage, 178x243 portrait
bases, 256x256 face atlases) composited with the same frame layout as the pack.
Target side is the actual 320x240 LVGL UI smoke screenshot (top art area).
"""
from __future__ import annotations

import json
import os
import struct
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps
import io

ROOT = Path(__file__).resolve().parent.parent
SRC = Path("C:/tmp/ai-passport-six-20261123-79f0/sources/cafestella-rw5")
PACK = ROOT / "assets/cafe/cafe.pak"
SHOTS = Path(os.environ.get("CAFE_UI_OUT",
    "reports/cafestella-media-20261123-1130/ui-smoke"))

blob = PACK.read_bytes()
header = struct.unpack_from("<IIII", blob, 8)
secs = {}
for i in range(header[3]):
    t, off, size, count, crc, _ = struct.unpack_from("<IIIIII", blob, 128 + i * 24)
    secs[t] = (off, size, count)

def frame(node, index):
    boff, bsize, bcount = secs[16]
    toff, tsize, tcount = secs[17]
    for bi in range(bcount):
        nid, first, count, raw_len, off, comp_len, crc = struct.unpack_from(
            "<HHHHIII", blob, boff + bi * 20)
        if nid != node or not (first <= index < first + count):
            continue
        raw = zlib.decompress(blob[toff + off:toff + off + comp_len])
        p = 0
        for j in range(count):
            kind = raw[p]
            sl, tl, bg, sp = struct.unpack_from("<HHHB", raw, p + 1)
            p += 8
            sprites = []
            for _ in range(sp):
                aid, left, top, op, _pad = struct.unpack_from("<HhhBB", raw, p)
                p += 8
                sprites.append((aid, left, top, op))
            p += sl + 1 + tl + 1
            if first + j == index:
                return bg, sprites
    raise KeyError((node, index))

IM = json.loads((SRC / "converted/image-map.json").read_text("utf-8"))

def source_path(info):
    return SRC / ("src" + info["src"])

def stage_image(bg_asset, sprites):
    canvas = Image.new("RGBA", (432, 243), (0, 0, 0, 255))
    info = IM[f"a{bg_asset}"]
    bg = Image.open(source_path(info)).convert("RGBA")
    bg = ImageOps.contain(bg, (432, 243), Image.Resampling.LANCZOS)
    canvas.alpha_composite(bg, ((432 - bg.width) // 2, (243 - bg.height) // 2))
    for aid, left, top, op in sprites:
        inf = IM[f"a{aid}"]
        base = Image.open(source_path(inf)).convert("RGBA")
        bx = round(left * 432 / 224)
        by = round(top * 243 / 126)
        if op < 255:
            a = base.getchannel("A").point(lambda n: n * op // 255)
            base.putalpha(a)
        canvas.alpha_composite(base, (bx, by))
        if inf.get("patch"):
            x, y, w, h = inf["patchRect"]
            ax, ay = inf["patchAtlas"][2], inf["patchAtlas"][3]
            atlas = Image.open(SRC / ("src" + inf["patch"])).convert("RGBA")
            patch = atlas.crop((ax, ay, ax + w, ay + h))
            if op < 255:
                a = patch.getchannel("A").point(lambda n: n * op // 255)
                patch.putalpha(a)
            canvas.alpha_composite(patch, (bx + x, by + y))
    return canvas.convert("RGB").resize((320, 180), Image.Resampling.LANCZOS)

def pack_jpeg(img_id):
    ioff, isize, icount = secs[18]
    doff, dsize, dcount = secs[19]
    rec = struct.unpack_from("<HBBHHIIIIIII", blob, ioff + img_id * 36)
    off, ln = rec[5], rec[6]
    poff, psize, pcount = secs.get(27, (0, 0, 0))
    xoff, xsize, xcount = secs.get(28, (0, 0, 0))
    if not poff:
        return blob[doff + off:doff + off + ln]
    pool = blob[poff:poff + psize]
    count = struct.unpack_from("<I", pool)[0]
    offs = [struct.unpack_from("<I", pool, 4 + 4 * i)[0] for i in range(count + 1)]
    desc = blob[xoff:xoff + xsize]
    d = struct.unpack_from("<I", desc, 4 + 4 * img_id)[0]
    parts = struct.unpack_from("<H", desc, d)[0]
    out = bytearray(); lit = 0
    for j in range(parts):
        v = struct.unpack_from("<H", desc, d + 2 + 2 * j)[0]
        if v & 0x8000:
            n = v & 0x7fff
            out += blob[doff + off + lit:doff + off + lit + n]
            lit += n
        else:
            out += pool[offs[v]:offs[v + 1]]
    if lit != ln:
        raise ValueError("JPEG literal length mismatch")
    return bytes(out)

def pack_image(img_id):
    pic = Image.open(io.BytesIO(pack_jpeg(img_id))).convert("RGB")
    return pic.resize((320, 180), Image.Resampling.LANCZOS)

def ui_image_id(index):
    uoff, usize, ucount = secs[22]
    return struct.unpack_from("<H", blob, uoff + index * 8 + 4)[0]

def make_pair(name, src_img, target_img, crop_h, pack_label="q10"):
    target = target_img.crop((0, 0, 320, crop_h))
    source = src_img.crop((0, 0, 320, crop_h))
    pair = Image.new("RGB", (640, crop_h + 28), (20, 20, 20))
    pair.paste(source, (0, 28))
    pair.paste(target, (320, 28))
    d = ImageDraw.Draw(pair)
    d.text((4, 6), "source (432x243 -> 320x180)", fill=(230, 230, 230))
    d.text((324, 6), f"pack {pack_label} 224x126 -> 320x180", fill=(230, 230, 230))
    pair.save(SHOTS / f"quality-{name}.png")
    # MAE on RGB, ignoring the label band
    import numpy as np
    a = np.asarray(source, dtype=np.int16)
    b = np.asarray(target, dtype=np.int16)
    mae = float(np.abs(a - b).mean())
    print(name, "mae=%.2f" % mae, "saved", SHOTS / f"quality-{name}.png")


# Title
title_src = Image.open(SRC / "src/common/ui/title-hero.jpg").convert("RGB")
title_src = ImageOps.contain(title_src, (320, 180), Image.Resampling.LANCZOS)
title_src = ImageOps.pad(title_src, (320, 180), color=(0, 0, 0))
make_pair("title", title_src, pack_image(ui_image_id(0)), 180, "q18")
# Daily multi-person n8[12]
bg, sprites = frame(8, 12)
make_pair("daily", stage_image(bg, sprites),
          Image.open(SHOTS / "02b_daily_art.png").convert("RGB"), 180)
# CG group 0 first variant
gallery = json.loads((SRC / "converted/gallery.json").read_text("utf-8"))
cg_src = Image.open(SRC / ("src" + gallery[0]["variants"][0]["src"])).convert("RGB")
cg_src = ImageOps.fit(cg_src, (320, 180), Image.Resampling.LANCZOS)
make_pair("cg", cg_src, Image.open(SHOTS / "13b_cg_art.png").convert("RGB"), 180)
