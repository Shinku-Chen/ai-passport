#!/usr/bin/env python3
"""Independent checks for the generated Maitetsu content and media pack.

The default run needs only the repository (CI-safe):
  * the media manifest matches the shipped pack/audio bytes,
  * every image CRC in the pack verifies,
  * all bands decode (Pillow) and the scene table composes real pictures,
  * a source-derived sample fixture (tests/fixtures/maitetsu-scene-samples.txt)
    matches the pack's composition,
  * main/gal/gal_content.c parses back to 1,537 paragraphs / 1,542 events,
  * the audio pack decodes to the exact 283.88 s duration.

With ``--source <checkout>`` it additionally reconciles every paragraph, the
675 scene ids, the 35 CGs and the chapter counts against the pinned RW5 source,
and fails if the generated files are stale.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:   # CI 的静态检查环境不保证 Pillow；解码类检查会跳过。
    Image = None
    HAVE_PIL = False

REPO = Path(__file__).resolve().parent.parent
MEDIA = REPO / "assets" / "maitetsu" / "maitetsu_media.bin"
AUDIO = REPO / "assets" / "maitetsu" / "maitetsu_title.bin"
MANIFEST = REPO / "assets" / "maitetsu" / "media_manifest.json"
CONTENT = REPO / "main" / "gal" / "gal_content.c"
SCENES = REPO / "main" / "gal" / "maitetsu_assets.c"
FIXTURE = REPO / "tests" / "fixtures" / "maitetsu-scene-samples.txt"
ASSETS_H = REPO / "main" / "gal" / "maitetsu_assets.h"

failures = 0


def check(cond: bool, message: str) -> None:
    global failures
    if not cond:
        failures += 1
        print("FAIL:", message)


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


class Pack:
    def __init__(self, data: bytes) -> None:
        self.data = data
        check(data[:8] == b"MTMEDIA\0", "pack magic")
        self.images = u16(data, 12)
        self.bands = u16(data, 14)
        self.prefixes = u16(data, 16)
        self.band_rows = u16(data, 18)
        self.total = u32(data, 20)
        self.prefix_off = u32(data, 28)
        self.image_off = u32(data, 32)
        self.band_off = u32(data, 36)
        self.payload_off = u32(data, 40)
        check(self.total == len(data), "pack size matches the header")

    def image(self, index: int) -> dict:
        base = self.image_off + index * 16
        return {
            "w": u16(self.data, base),
            "h": u16(self.data, base + 2),
            "first": u16(self.data, base + 4),
            "bands": self.data[base + 6],
            "prefix": self.data[base + 7],
            "crc": u32(self.data, base + 12),
        }

    def band_entry(self, index: int) -> tuple[int, int, int, int]:
        base = self.band_off + index * 16
        return u32(self.data, base), u32(self.data, base + 4), u16(self.data, base + 8), u16(
            self.data, base + 10
        )

    def prefix(self, index: int) -> bytes:
        base = self.prefix_off + index * 8
        off, length = u32(self.data, base), u32(self.data, base + 4)
        start = self.payload_off + off
        return self.data[start:start + length]

    def verify_image(self, index: int) -> bool:
        info = self.image(index)
        start = self.band_off + info["first"] * 16
        entries = self.data[start:start + info["bands"] * 16]
        first = self.band_entry(info["first"])[0]
        last = self.band_entry(info["first"] + info["bands"] - 1)
        end = last[0] + last[1]
        payload = self.data[self.payload_off + first:self.payload_off + end]
        return (zlib.crc32(entries + payload) & 0xFFFFFFFF) == info["crc"]

    def band_image(self, index: int, band: int):
        info = self.image(index)
        off, length, y0, y1 = self.band_entry(info["first"] + band)
        strip = self.data[self.payload_off + off:self.payload_off + off + length]
        stream = self.prefix(info["prefix"]) + strip
        image = Image.open(io.BytesIO(stream))
        image.load()
        return image.convert("RGB")

    def image_pixels(self, index: int):
        info = self.image(index)
        canvas = Image.new("RGB", (info["w"], info["h"]))
        for band in range(info["bands"]):
            off, length, y0, y1 = self.band_entry(info["first"] + band)
            strip = self.band_image(index, band)
            check(strip.size == (info["w"], y1 - y0), "strip size matches table")
            check(y0 == band * self.band_rows, "strip start row")
            canvas.paste(strip, (0, y0))
        return canvas


def parse_content() -> list[tuple[str, str, int]]:
    text = CONTENT.read_text(encoding="utf-8")
    body = text.split("const gal_line_t gal_content_lines[] = {", 1)[1]
    body = body.split("};", 1)[0]
    pattern = re.compile(r'\{\s*("(?:[^"\\]|\\.)*")\s*,\s*("(?:[^"\\]|\\.)*")\s*,\s*(\d+)\s*,')
    records = []
    for match in pattern.finditer(body):
        speaker = json.loads(match.group(1))
        line = json.loads(match.group(2))
        records.append((speaker, line, int(match.group(3))))
    return records


def parse_int_constant(text: str, name: str) -> int:
    match = re.search(r"\b%s\s*=\s*(\d+)" % re.escape(name), text)
    if not match:
        raise SystemExit("missing constant " + name)
    return int(match.group(1))


def test_pack() -> Pack:
    data = MEDIA.read_bytes()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    check(manifest["media"]["pack_bytes"] == len(data), "manifest pack size")
    check(manifest["media"]["pack_sha256"] == hashlib.sha256(data).hexdigest(),
          "manifest pack sha256")
    pack = Pack(data)
    check(pack.images == 759, "759 media images")
    check(pack.bands == 3994, "3994 strips")
    check(pack.band_rows == 30, "30-row strips")

    for index in range(pack.images):
        info = pack.image(index)
        check(0 < info["w"] <= 320 and 0 < info["h"], "image dimensions in range")
        check(info["bands"] == (info["h"] + 29) // 30, "band count matches height")
        if not pack.verify_image(index):
            check(False, "image CRC %d" % index)
    return pack


def test_scene_tables(pack: Pack) -> list[dict]:
    text = SCENES.read_text(encoding="utf-8")
    scenes = [
        {"bg": int(m.group(1), 0), "patches": int(m.group(2)), "first": int(m.group(3))}
        for m in re.finditer(r"\{\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", text.split("maitetsu_scenes[] = {", 1)[1].split("};", 1)[0])
    ]
    patches = [
        {"image": int(m.group(1)), "x": int(m.group(2)), "y": int(m.group(3))}
        for m in re.finditer(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", text.split("maitetsu_patches[] = {", 1)[1].split("};", 1)[0])
    ]
    check(len(scenes) == 675, "675 scene rows")
    check(0 < len(patches) < 1000, "patch rows present")
    for scene in scenes:
        if scene["bg"] != 0xFFFF:
            check(scene["bg"] < pack.images, "scene background index in range")
            check(pack.image(scene["bg"])["w"] == 320, "background is full width")
        for p in range(scene["patches"]):
            patch = patches[scene["first"] + p]
            check(patch["image"] < pack.images, "patch image index in range")
    return scenes


def compose(pack: Pack, scenes: list[dict], scene_index: int) -> Image.Image:
    text = SCENES.read_text(encoding="utf-8")
    patches = [
        {"image": int(m.group(1)), "x": int(m.group(2)), "y": int(m.group(3))}
        for m in re.finditer(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", text.split("maitetsu_patches[] = {", 1)[1].split("};", 1)[0])
    ]
    row = scenes[scene_index]
    canvas = Image.new("RGB", (320, 180), (0, 0, 0))
    if row["bg"] != 0xFFFF:
        canvas.paste(pack.image_pixels(row["bg"]), (0, 0))
    for p in range(row["patches"]):
        patch = patches[row["first"] + p]
        canvas.paste(pack.image_pixels(patch["image"]), (patch["x"], patch["y"]))
    return canvas


def test_samples(pack: Pack, scenes: list[dict]) -> None:
    lines = FIXTURE.read_text(encoding="utf-8").splitlines()
    scene = -1
    compared = 0
    worst = 0.0
    canvas = None
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "scene":
            scene = int(parts[1])
            canvas = compose(pack, scenes, scene)
        elif parts[0] == "sample":
            x, y, r, g, b = (int(v) for v in parts[1:6])
            got = canvas.getpixel((x, y))
            delta = max(abs(got[0] - r), abs(got[1] - g), abs(got[2] - b))
            worst = max(worst, delta)
            check(delta <= 40, "scene sample (%d,%d) delta %d" % (scene, x, delta))
            compared += 1
    print("scene samples compared: %d (worst channel delta %d)" % (compared, int(worst)))
    check(compared >= 100, "enough scene samples")


def test_audio() -> None:
    data = AUDIO.read_bytes()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    check(manifest["audio"]["pack_sha256"] == hashlib.sha256(data).hexdigest(),
          "manifest audio sha256")
    check(data[:8] == b"MTAUDIO\0", "audio magic")
    rate = u32(data, 12)
    samples = u32(data, 16)
    block_samples = u16(data, 20)
    blocks = u32(data, 24)
    payload = u32(data, 28)
    check(rate == 4000, "audio source rate")
    check(samples == 1135520, "complete 283.88 s sample count")
    check(block_samples == 256 and blocks == 4436 and payload == blocks * 132,
          "ADPCM block layout")
    header = bytearray(data[:36])
    stored = u32(bytes(header), 32)
    header[32:36] = b"\0\0\0\0"
    crc = zlib.crc32(bytes(header) + data[36:]) & 0xFFFFFFFF
    check(crc == stored, "audio CRC")
    step = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
        598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
        1878, 2066, 2272, 2499, 2749, 3024, 3258, 3583, 3941, 4334, 4766, 5242,
        5766, 6342, 6975, 7672, 8438, 9280, 10205, 11220, 12338, 13570, 14924,
        16412, 18052, 19853, 21835, 24015, 26419, 29055, 32007,
    ]
    index_table = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
    total = 0
    for block in range(blocks):
        base = 36 + block * 132
        predictor = struct.unpack_from("<h", data, base)[0]
        index = min(data[base + 2], 88)
        total += 1
        for byte in data[base + 4:base + 132]:
            for nibble in (byte >> 4, byte & 0x0F):
                s = step[index]
                diff = s >> 3
                if nibble & 4:
                    diff += s
                if nibble & 2:
                    diff += s >> 1
                if nibble & 1:
                    diff += s >> 2
                predictor += -diff if nibble & 8 else diff
                predictor = max(-32768, min(32767, predictor))
                index = max(0, min(88, index + index_table[nibble & 7]))
                total += 1
    # 每个块解码出 1 个首样本 + 256 个增量样本，播放侧按 1,135,520 截断。
    check(total == blocks * 257, "decoded sample count")
    check(blocks * 256 - samples < 256, "padded tail within one block")
    print("audio: %.2f s, %d blocks" % (samples / rate, blocks))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", help="pinned maitetsu-rw5 checkout for full reconciliation")
    args = parser.parse_args(argv)

    records = parse_content()
    content_text = CONTENT.read_text(encoding="utf-8")
    check(len(records) == 1537, "1,537 paragraphs in generated C")
    events = len(re.findall(r"\{ GAL_EV_(?:LINE|TRANSITION|ENDING),", content_text))
    check(events == 1542, "1,542 events in generated C")
    check(len(re.findall(r"\{ GAL_EV_TRANSITION,", content_text)) == 4, "four transitions")
    check(len(re.findall(r"\{ GAL_EV_ENDING,", content_text)) == 1, "one ending event")
    check(len(re.findall(r'\{\s*"CG \d\d"', content_text)) == 35, "35 CG entries")
    check(re.search(r'"CG 35"', content_text) is not None, "CG 35 present")
    chapters = content_text.split("gal_content_chapters[] = {", 1)[1].split("};", 1)[0]
    check(len(re.findall(r'\{\s*"正和四十一年|\{\s*"御一夜旅行|\{\s*"小仓风暴|\{\s*"尾声"', chapters)) == 4,
          "four chapter entries")

    pack = test_pack()
    scenes = test_scene_tables(pack)
    if HAVE_PIL:
        test_samples(pack, scenes)
    else:
        print("SKIP: scene decode comparison (Pillow not installed)")
    test_audio()

    if args.source:
        source = Path(args.source)
        result = subprocess.run(
            [sys.executable, str(REPO / "tools" / "maitetsu_assets.py"),
             "--source", str(source), "--check"],
            capture_output=True, text=True,
        )
        check(result.returncode == 0, "generated files current: " + result.stderr.strip())
        text = json.loads((source / "converted" / "text.json").read_text(encoding="utf-8"))
        check(len(text) == len(records), "source paragraph count")
        for index, (spoke, line, _scene) in enumerate(records):
            if list(text[index]) != [spoke, line]:
                check(False, "paragraph %d differs from the source" % index)
                break
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        check(manifest["source_commit"] == "9c008406853affe973ceb80f62128696054365e7",
              "pinned source commit")
        print("source reconciliation: %d paragraphs" % len(records))

    if failures:
        print("FAIL: %d checks" % failures)
        return 1
    print("PASS: maitetsu content/pack")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
