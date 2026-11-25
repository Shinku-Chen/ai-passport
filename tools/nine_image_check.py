#!/usr/bin/env python3
"""Reference image/composition check for the production C decoder.

Composite the packed images independently with Pillow (JPEG decode + palette /
zlib blocks) and compare the C dump. This is a real pixel comparison, not a
count of decode calls.
"""
from __future__ import annotations

import argparse
import io
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

from PIL import Image

W, H = 240, 135


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


class PackImages:
    def __init__(self, data: bytes):
        self.data = data
        self.count = u32(data, 20)
        self.table = u32(data, 24)

    def record(self, index: int):
        r = self.data[self.table + index * 32:self.table + (index + 1) * 32]
        return struct.unpack("<8I", r)

    def decode(self, index: int) -> Image.Image:
        kind, w, h, off, length, crc, tag, _ = self.record(index)
        payload = self.data[off:off + length]
        if kind == 0:
            return Image.open(io.BytesIO(payload)).convert("RGBA")
        palette_count, rows, blocks = struct.unpack_from("<III", payload, 0)
        palette = [tuple(payload[12 + i * 4:16 + i * 4]) for i in range(palette_count)]
        table = 12 + palette_count * 4
        raw = bytearray()
        for b in range(blocks):
            boff, blen = struct.unpack_from("<II", payload, table + b * 8)
            raw.extend(zlib.decompress(payload[boff - off:boff - off + blen]))
        image = Image.frombytes("P", (w, h), bytes(raw))
        flat = []
        for color in palette:
            flat.extend(color)
        image.putpalette(flat, rawmode="RGBA")
        return image.convert("RGBA")


class PackScript:
    def __init__(self, data: bytes):
        self.data = data
        self.visuals = (u32(data, 36 + 3 * 8), u32(data, 40 + 3 * 8))
        self.layers = (u32(data, 36 + 4 * 8), u32(data, 40 + 4 * 8))
        self.gallery = (u32(data, 36 + 11 * 8), u32(data, 40 + 11 * 8))
        self.strings = (u32(data, 132), u32(data, 136))

    def string(self, offset: int) -> str:
        start = self.strings[0] + offset
        end = self.data.index(0, start)
        return self.data[start:end].decode("utf-8")

    def visual(self, index: int):
        count, offset = self.visuals
        return struct.unpack_from("<II", self.data, offset + index * 8)

    def layer(self, index: int):
        count, offset = self.layers
        return struct.unpack_from("<6I", self.data, offset + index * 24)

    def gallery(self, index: int):
        count, offset = self.gallery
        return struct.unpack_from("<3I", self.data, offset + index * 12)


def rgba_layer(images: PackImages, index: int, width: int, height: int,
               opacity: int) -> Image.Image:
    image = images.decode(index).resize((width, height), Image.Resampling.NEAREST)
    if opacity < 255:
        alpha = image.getchannel("A").point(lambda v: v * opacity // 255)
        image.putalpha(alpha)
    return image


def compose(images: PackImages, script: PackScript, visual: int) -> Image.Image:
    first, count = script.visual(visual)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    for i in range(count):
        image, x, y, w, h, opacity = script.layer(first + i)
        layer = rgba_layer(images, image, w, h, opacity)
        canvas.paste(layer, (x, y), layer)
    return canvas


def compose_background_only(images: PackImages, script: PackScript, visual: int) -> Image.Image:
    first, count = script.visual(visual)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    if count:
        image, x, y, w, h, opacity = script.layer(first)
        layer = rgba_layer(images, image, w, h, opacity)
        canvas.paste(layer, (x, y), layer)
    return canvas


def dump_to_rgb(path: Path):
    raw = path.read_bytes()
    assert len(raw) == W * H * 2
    out = bytearray(W * H * 3)
    for i in range(W * H):
        value = raw[i * 2] | raw[i * 2 + 1] << 8
        out[i * 3] = ((value >> 11) & 31) * 255 // 31
        out[i * 3 + 1] = ((value >> 5) & 63) * 255 // 63
        out[i * 3 + 2] = (value & 31) * 255 // 31
    return out


def compare(actual: bytearray, reference: Image.Image):
    ref = reference.convert("RGB").tobytes()
    total = 0
    high = 0
    big = 0
    for i in range(0, len(actual), 3):
        a = actual[i:i + 3]
        b = ref[i:i + 3]
        for j in range(3):
            d = abs(a[j] - b[j])
            total += d
            if d > high:
                high = d
            if d > 40:
                big += 1
    mae = total / (W * H * 3)
    return mae, high, big


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack", type=Path, required=True)
    parser.add_argument("--dump", type=Path, required=True)
    args = parser.parse_args()
    script_data = (args.pack / "nine_script.bin").read_bytes()
    image_data = (args.pack / "nine_images.bin").read_bytes()
    script = PackScript(script_data)
    images = PackImages(image_data)

    cases = [(0, "visual"), (11, "visual")]
    _, gallery_off = script.gallery
    gallery_image = u32(script_data, gallery_off + 4)
    cases.append((gallery_image, "asset"))

    for index, mode in cases:
        if mode == "visual":
            reference = compose(images, script, index)
        else:
            reference = images.decode(index)
            reference = reference.resize((W, H), Image.Resampling.NEAREST)
        with tempfile.NamedTemporaryFile(suffix=".rgba", delete=False) as temp:
            dump_path = Path(temp.name)
        try:
            subprocess.run([str(args.dump), str(args.pack / "nine_script.bin"),
                            str(args.pack / "nine_images.bin"), str(index), mode,
                            str(dump_path)], check=True)
            actual = dump_to_rgb(dump_path)
        finally:
            dump_path.unlink(missing_ok=True)
        mae, high, big = compare(actual, reference)
        if mae > 6.0 or big > 200:
            print(f"FAIL {mode} {index}: MAE={mae:.2f} max={high} >40:{big}")
            return 1
        if mode == "visual" and index == 11:
            bg = compose_background_only(images, script, index)
            bg_mae, _, bg_big = compare(actual, bg)
            if bg_mae < 2.0 or bg_big < 500:
                print(f"FAIL visual {index}: sprite/patch layers not applied "
                      f"(bg MAE={bg_mae:.2f} big={bg_big})")
                return 1
        print(f"PASS {mode} {index}: MAE={mae:.2f} max={high} >40:{big}")
    print("nine image decode/composition reference: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
