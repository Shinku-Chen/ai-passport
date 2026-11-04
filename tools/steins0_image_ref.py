#!/usr/bin/env python3
"""Generate a small Pillow/TJpgDec cross-check fixture from the built pack.

The host image regression decodes the same JPEG bytes with the vendored TJpgDec
and compares sampled RGB565 values against this file (Pillow/libjpeg).  Keeping
the fixture in tests/fixtures makes the gate independent of Pillow and of the
unlicensed source checkout.

Usage:
  python tools/steins0_image_ref.py --pack assets/steins0/steins0_pack.bin \
      --out tests/fixtures/steins0_image_ref.txt
"""

from __future__ import annotations

import argparse
import hashlib
import io
import struct
import sys
from pathlib import Path

from PIL import Image

SEC_TEXT, SEC_STRT = 0, 1
SEC_ASSETID, SEC_PART, SEC_BLOB = 15, 16, 17

SAMPLES = ("bg0", "bg330", "bg101", "gal369", "gal376", "gal500", "color-1", "cb0c1ab3c7c10")
NL = chr(10)


def rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    blob = Path(args.pack).read_bytes()
    if blob[:8] != b"SG0PK001":
        sys.exit("bad pack magic")
    count = struct.unpack_from("<H", blob, 12)[0]
    sec = {}
    for i in range(count):
        sec_type, off, cnt, size = struct.unpack_from("<IIII", blob, 20 + i * 16)
        sec[sec_type] = (off, cnt, size)
    strt_off = sec[SEC_STRT][0]
    text_off = sec[SEC_TEXT][0]
    asset_off, asset_count = sec[SEC_ASSETID][0], sec[SEC_ASSETID][1]
    part_off = sec[SEC_PART][0]
    blob_off = sec[SEC_BLOB][0]

    def string(idx: int) -> str:
        off, length = struct.unpack_from("<IH", blob, strt_off + idx * 8)
        return blob[text_off + off:text_off + off + length].decode("utf-8")

    by_name = {}
    for i in range(asset_count):
        off = asset_off + i * 8
        part_first = struct.unpack_from("<H", blob, off)[0]
        by_name[string(struct.unpack_from("<H", blob, off + 4)[0])] = (i, part_first)

    lines = ["# pack_sha256 " + hashlib.sha256(blob).hexdigest()]
    for name in SAMPLES:
        if name not in by_name:
            continue
        _asset, part_first = by_name[name]
        off = part_off + part_first * 30
        poff, plen, mask_off, mask_len, w, h = struct.unpack_from("<IIIIHH", blob, off)
        jpeg = blob[blob_off + poff:blob_off + poff + plen]
        im = Image.open(io.BytesIO(jpeg)).convert("RGB")
        if im.size != (w, h):
            sys.exit(f"{name}: Pillow {im.size} != pack {w}x{h}")
        lines.append(f"asset {name} {w} {h}")
        row = []
        for gy in range(8):
            for gx in range(8):
                x = (gx * 2 + 1) * w // 16
                y = (gy * 2 + 1) * h // 16
                r, g, b = im.getpixel((min(x, w - 1), min(y, h - 1)))
                row.append(f"{x} {y} {rgb565(r, g, b)}")
                if len(row) == 8:
                    lines.append("sample " + " ".join(row))
                    row = []
        if row:
            lines.append("sample " + " ".join(row))
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(NL.join(lines) + NL, encoding="utf-8")
    print(f"wrote {args.out}: {len(SAMPLES)} sample groups")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
