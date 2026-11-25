#!/usr/bin/env python3
"""Wrap nine_script.bin + nine_images.bin into one flashable assets image.

The runtime maps this container from the `assets` data partition and validates
the CRC before touching any save state. The two inner files keep their own CRC
and identity, so the container is only a layout wrapper.
"""
from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

MAGIC = b"NINECNT\0"
VERSION = 1
HEADER = 64
CRC_OFFSET = 32


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def build(source: Path, out: Path) -> dict:
    script = (source / "nine_script.bin").read_bytes()
    images = (source / "nine_images.bin").read_bytes()
    if not script or not images:
        raise SystemExit("empty script/image pack")
    script_off = HEADER
    images_off = (script_off + len(script) + 63) & ~63
    total = images_off + len(images)
    blob = bytearray(total)
    blob[:8] = MAGIC
    struct.pack_into("<8I", blob, 8, VERSION, total, script_off, len(script),
                     images_off, len(images), 0, 0)
    blob[script_off:script_off + len(script)] = script
    blob[images_off:images_off + len(images)] = images
    struct.pack_into("<I", blob, CRC_OFFSET, crc32(blob))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    return {"container": str(out), "bytes": total, "script": len(script),
            "images": len(images), "imagesOffset": images_off,
            "crc32": f"{crc32(blob):08x}"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    result = build(args.source, args.out)
    print("nine_container:", result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
