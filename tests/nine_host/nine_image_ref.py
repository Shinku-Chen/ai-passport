#!/usr/bin/env python3
"""Independent reference composites for the C image decoder test.

Parses the produced NINESC2/NINEIM1 packages without importing the packer,
decodes JPEG/indexed assets with Pillow, and composites the layer list with the
same integer rules as nine_image.c. JPEG layers get a small per-channel
tolerance because libjpeg and TJpgDec differ in YCbCr rounding; indexed layers
must match exactly.

Usage: python tests/nine_host/nine_image_ref.py <script.bin> <images.bin> <outdir>
"""
from __future__ import annotations
import math, struct, sys, zlib
from pathlib import Path
from PIL import Image

W, H = 256, 144


def rd32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def blend(canvas, x, y, src, opacity):
    d = canvas[y][x]
    if opacity == 255:
        canvas[y][x] = src
        return
    if not opacity:
        return
    inv = 255 - opacity
    r = (((src >> 11) & 31) * opacity + ((d >> 11) & 31) * inv + 127) // 255
    g = (((src >> 5) & 63) * opacity + ((d >> 5) & 63) * inv + 127) // 255
    b = ((src & 31) * opacity + (d & 31) * inv + 127) // 255
    canvas[y][x] = (r << 11) | (g << 5) | b


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


class Pack:
    def __init__(self, script, images):
        self.script = Path(script).read_bytes()
        self.images = Path(images).read_bytes()
        s = self.script
        self.visual_count = rd32(s, 13 * 4 + 8)
        self.visual_off = rd32(s, 14 * 4 + 8)
        self.layer_count = rd32(s, 15 * 4 + 8)
        self.layer_off = rd32(s, 16 * 4 + 8)
        self.frame_off = rd32(s, 10 * 4 + 8)
        self.node_off = rd32(s, 8 * 4 + 8)
        self.chapter_count = rd32(s, 25 * 4 + 8)
        self.chapter_off = rd32(s, 26 * 4 + 8)
        img = self.images
        self.image_count = rd32(img, 20)
        self.image_off = rd32(img, 24)
        self.title = [rd32(img, 40 + 4 * i) for i in range(3)]

    def image(self, index):
        p = self.image_off + index * 32
        kind, w, h, off, size, crc = struct.unpack_from('<6I', self.images, p)
        return kind, w, h, off, size

    def visual(self, index):
        first, count = struct.unpack_from('<2I', self.script, self.visual_off + index * 8)
        layers = []
        for i in range(count):
            layers.append(struct.unpack_from('<IiiIII', self.script,
                                             self.layer_off + (first + i) * 24))
        return layers

    def decode(self, index):
        kind, w, h, off, size = self.image(index)
        raw = self.images[off:off + size]
        if kind == 0:
            with Image.open(__import__('io').BytesIO(raw)) as im:
                im = im.convert('RGB')
                return [im.getpixel((x, y)) for y in range(h) for x in range(w)], w, h, None
        palette_count, rows, blocks = struct.unpack_from('<3I', raw, 0)
        palette = [tuple(raw[12 + i * 4:16 + i * 4]) for i in range(palette_count)]
        table = 12 + palette_count * 4
        out = [0] * (w * h)
        for block in range(blocks):
            boff, blen = struct.unpack_from('<2I', raw, table + block * 8)
            data = zlib.decompress(self.images[boff:boff + blen])
            y0 = block * rows
            nrows = min(rows, h - y0)
            for yy in range(nrows):
                for xx in range(w):
                    out[(y0 + yy) * w + xx] = palette[data[yy * w + xx]]
        return out, w, h, palette_count


def composite(pack, visual):
    layers = pack.visual(visual)
    canvas = [[0] * W for _ in range(H)]
    jpeg_layer = False
    for image, x, y, w, h, opacity in layers:
        # Pack geometry is the 320x180 stage; the display canvas is 256x144.
        x = math.trunc((x * 4 + (2 if x >= 0 else -2)) / 5)
        y = math.trunc((y * 4 + (2 if y >= 0 else -2)) / 5)
        w = max(1, (w * 4 + 2) // 5)
        h = max(1, (h * 4 + 2) // 5)
        pixels, sw, sh, _ = pack.decode(image)
        kind = pack.image(image)[0]
        jpeg_layer = jpeg_layer or kind == 0
        for dy in range(max(0, y), min(H, y + h)):
            sy = (dy - y) * sh // h
            row = canvas[dy]
            for dx in range(max(0, x), min(W, x + w)):
                sx = (dx - x) * sw // w
                px = pixels[sy * sw + sx]
                if kind == 0:
                    blend(canvas, dx, dy, rgb565(*px), opacity)
                else:
                    a = (px[3] * opacity + 127) // 255
                    blend(canvas, dx, dy, rgb565(px[0], px[1], px[2]), a)
    return canvas, jpeg_layer


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    pack = Pack(sys.argv[1], sys.argv[2])
    out = Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    # Sample: chapter-start visuals, evenly spread visuals, and the visuals with
    # the richest layer lists (multi-character + face patches).
    picks = set()
    nodes = []
    for i in range(rd32(pack.script, 7 * 4 + 8)):
        rec = struct.unpack_from('<12I', pack.script, pack.node_off + i * 48)
        nodes.append(rec)
    for i in range(pack.chapter_count):
        rec = struct.unpack_from('<3I', pack.script, pack.chapter_off + i * 12)
        node = rec[2]
        frames = nodes[node]
        if frames[1]:
            picks.add(struct.unpack_from('<I', pack.script, pack.frame_off + frames[0] * 24 + 4)[0])
    for v in range(0, pack.visual_count, max(1, pack.visual_count // 24)):
        picks.add(v)
    richest = sorted(range(pack.visual_count),
                     key=lambda v: len(pack.visual(v)), reverse=True)[:12]
    picks.update(richest)
    picks.add(pack.visual_count - 1)
    picks = [v for v in sorted(picks) if v < pack.visual_count]
    manifest = ['# visual tolerance']
    for v in picks:
        canvas, jpeg_layer = composite(pack, v)
        name = f'v{v:05d}.rgb'
        with open(out / name, 'wb') as f:
            for row in canvas:
                f.write(struct.pack('<' + 'H' * W, *row))
        manifest.append(f'{v} {name} {6 if jpeg_layer else 0}')
    (out / 'manifest.txt').write_text('\n'.join(manifest) + '\n', encoding='utf-8')
    print(f'{len(picks)} reference visuals -> {out}')


if __name__ == '__main__':
    main()
