#!/usr/bin/env python3
"""Packer image-regression tests: alpha overlap, odd JPEG, real pack metadata."""
from __future__ import annotations

import io
import json
import os
import struct
import sys
import zlib

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import anemoi_pack as pack  # noqa: E402

PACK = os.path.join(ROOT, "main", "anemoi_data", "anemoi_pack.bin")
META = os.path.join(ROOT, "main", "anemoi_data", "anemoi_pack.json")


def parse_block(payload: bytes):
    w, h, pal_count, rows, blocks, _flags = struct.unpack_from("<HHHHHH", payload, 0)
    raw = struct.unpack_from("<I", payload, 12)[0]
    off = 16
    palette = [struct.unpack_from("<H", payload, off + i * 2)[0] for i in range(pal_count)]
    off += pal_count * 2
    alpha = list(payload[off:off + pal_count])
    off += pal_count
    lengths = [struct.unpack_from("<I", payload, off + i * 4)[0] for i in range(blocks)]
    off += blocks * 4
    indices = []
    prev = bytes(w)
    for b in range(blocks):
        chunk = zlib.decompress(payload[off:off + lengths[b]])
        off += lengths[b]
        rp = 0
        for _ in range(min(rows, h - b * rows)):
            f = chunk[rp]
            row = bytearray(w)
            for x in range(w):
                a = row[x - 1] if x else 0
                up = prev[x]
                up_left = prev[x - 1] if x else 0
                v = chunk[rp + 1 + x]
                if f == 1:
                    v += a
                elif f == 2:
                    v += up
                elif f == 3:
                    v += (a + up) >> 1
                elif f == 4:
                    p = a + up - up_left
                    pa, pb, pc = abs(p - a), abs(p - up), abs(p - up_left)
                    v += a if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                row[x] = v & 0xFF
            indices.append(bytes(row))
            prev = row
            rp += w + 1
    assert off == len(payload)
    assert raw == (w + 1) * rows
    return w, h, palette, alpha, indices


def test_alpha_overlap():
    im = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    ImageDraw.Draw(im).rectangle((2, 2, 5, 5), fill=(0, 0, 0, 255))
    payload = pack.encode_block_sprite(im, 8, 8, colors=32)
    w, h, palette, alpha, indices = parse_block(payload)
    assert (w, h) == (8, 8)
    assert len(palette) <= 32
    assert alpha[0] == 0
    assert indices[0][0] == 0
    center = indices[4][4]
    assert alpha[center] == 255
    assert (palette[center] & 0x7E0) == 0
    assert sum(1 for row in indices for j in row if alpha[j] < 8) >= 40
    assert sum(1 for row in indices for j in row if alpha[j] >= 250) >= 8


def test_partial_alpha_survives_buckets():
    im = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((2, 2, 5, 5), fill=(0, 0, 0, 255))
    d.point((1, 1), fill=(255, 0, 0, 128))
    payload = pack.encode_block_sprite(im, 8, 8, colors=32)
    _w, _h, _palette, alpha, indices = parse_block(payload)
    assert indices[0][0] == 0 and alpha[0] == 0
    assert any(8 <= a < 248 for a in alpha), alpha
    assert any(a >= 248 for a in alpha), alpha


def test_odd_jpeg_has_no_padding():
    im = Image.new("RGB", (17, 9), (240, 20, 20))
    for x in range(9, 17):
        for y in range(9):
            im.putpixel((x, y), (20, 210, 40))
    data = pack.encode_jpeg(im, 95)
    with Image.open(io.BytesIO(data)) as got:
        assert got.size == (17, 9)


def _sections(data: bytes):
    assert data[:7] == b"ANEMPK1"
    version, header, count = struct.unpack_from("<HHH", data, 8)
    assert (version, header, count) == (1, 28, 24)
    return {struct.unpack_from("<I", data, 28 + i * 16)[0]:
            struct.unpack_from("<III", data, 32 + i * 16) for i in range(count)}


def test_real_pack_metadata():
    if not os.path.exists(PACK) or not os.path.exists(META):
        print("SKIP: real anemoi pack/manifest missing")
        return
    data = open(PACK, "rb").read()
    sec = _sections(data)
    image_off, image_count, image_size = sec[16]
    assert image_size == image_count * 16
    blob_off, _blob_count, blob_size = sec[17]
    blob = data[blob_off:blob_off + blob_size]
    jpeg_rows = 0
    title = None
    title_ids = {}
    block_rows = 0
    transparent_blocks = 0
    max_palette = 0
    for i in range(image_count):
        off, ln, w, h, kind, _flags, _res = struct.unpack_from(
            "<IIHHBBH", data, image_off + i * 16)
        if kind == 0:
            jpeg_rows += 1
            with Image.open(io.BytesIO(blob[off:off + ln])) as im:
                assert im.size == (w, h), (i, (w, h), im.size)
            if w == 151 and h == 180:
                title = i
        elif kind == 1:
            block_rows += 1
            payload = blob[off:off + ln]
            _w, _h, pal_count = struct.unpack_from("<HHH", payload, 0)
            max_palette = max(max_palette, pal_count)
            p = 16 + pal_count * 2
            alpha = payload[p:p + pal_count]
            if any(a < 8 for a in alpha):
                transparent_blocks += 1
    meta = json.load(open(META, encoding="utf-8"))
    for name in ("normal", "anemoi", "eternicle"):
        title_ids[name] = meta["title_images"][name]
        row = struct.unpack_from("<IIHHBBH", data, image_off + title_ids[name] * 16)
        assert row[4] == 0 and (row[2], row[3]) == (151, 180), (name, row)
    assert jpeg_rows >= 300
    assert title is not None and meta["title_image"] == title_ids["normal"]
    assert meta["title_image"] == title_ids["normal"]
    assert block_rows >= 1700
    assert transparent_blocks >= 1700
    assert 32 < max_palette <= 97
    assert meta["block_palette"]["max_entries"] == max_palette
    for row in meta["included_scenes"]:
        assert int(row) >= 0
    print("PASS real pack metadata: jpeg=%d block=%d transparent_blocks=%d max_palette=%d titles=%s"
          % (jpeg_rows, block_rows, transparent_blocks, max_palette, title_ids))


def main():
    test_alpha_overlap()
    test_partial_alpha_survives_buckets()
    test_odd_jpeg_has_no_padding()
    test_real_pack_metadata()
    print("test_anemoi_image_encoder: PASS")


if __name__ == "__main__":
    main()
