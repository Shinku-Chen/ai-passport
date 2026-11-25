#!/usr/bin/env python3
"""Build the offline Maitetsu Niiroku media pack and the generated C tables.

The input is a pinned, read-only checkout of ``xuanyide01/maitetsu-rw5``.
Nothing under the source tree is modified.  Outputs:

  assets/maitetsu/maitetsu_media.bin     banded JPEG scene/CG pack (RGB565 strips)
  assets/maitetsu/maitetsu_title.bin     IMA ADPCM title track (4 kHz source)
  assets/maitetsu/media_manifest.json    counts, quality, geometry and hashes
  main/gal/gal_content.c                 dialogue / events / gallery / chapters
  main/gal/maitetsu_assets.c/.h          scene, patch and media index tables

Every image is stored as independent 30-row JPEG strips whose quantisation and
Huffman tables live once per quality class.  A strip is 35 bytes of JPEG
scan headers plus entropy data, so the bounded 320x30 RGB565 strip buffer used
on the device never needs a whole-image allocation.

Run with ``--check`` to regenerate in memory and fail if the generated files
are stale.  The upstream pin and every input hash are recorded in the manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import struct
import sys
import wave
import zlib
from pathlib import Path

from PIL import Image

SOURCE_COMMIT = "9c008406853affe973ceb80f62128696054365e7"
SOURCE_REPO = "https://github.com/xuanyide01/maitetsu-rw5"

# Source art canvas: the watch app draws layers in a 432x286 box while the
# background art itself is letterboxed to 432x243 starting at y=21.  Every
# sprite layer stays inside that 432x243 band (validated below), so the visible
# composition is exactly 16:9.
SRC_ART_W = 432
SRC_ART_TOP = 21
SRC_ART_H = 243

# Target art viewport: 320x180 is exactly 16:9 and sits above the 60 px panel.
ART_W = 320
ART_H = 180
PANEL_H = 60
BAND_ROWS = 30
THUMB_W, THUMB_H = 64, 36
TITLE_H = ART_H + PANEL_H

# Per-class JPEG quality, tuned against the 8 MB flash app partition.
BG_QUALITY = 60
PATCH_QUALITY = 74
CG_QUALITY = 68
THUMB_QUALITY = 74
TITLE_QUALITY = 82

CHAPTERS = ["正和四十一年·小仓", "御一夜旅行", "小仓风暴", "尾声"]
CHAPTER_COUNTS = [354, 345, 368, 470]
CHAPTER_SUMMARIES = [
    "赤井的宿舍，二六第一次来访。",
    "御一夜的旅行与日常。",
    "小仓风暴逼近。",
    "尾声，两个人的未来。",
]

SCENE_NONE = 0xFFFF
GAL_UNLOCK_NONE = 0xFF


class BuildError(SystemExit):
    def __init__(self, message: str) -> None:
        super().__init__("maitetsu_assets: " + message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def clamp(value: int, low: int, high: int) -> int:
    return low if value < low else high if value > high else value


# --------------------------------------------------------------------------- source


def sprite_rect(layer: dict) -> tuple[int, int, int, int]:
    """Map a 432x286 layer rectangle onto the 320x180 art viewport."""
    x0 = round(layer["x"] * ART_W / SRC_ART_W)
    y0 = round((layer["y"] - SRC_ART_TOP) * ART_H / SRC_ART_H)
    x1 = round((layer["x"] + layer["w"]) * ART_W / SRC_ART_W)
    y1 = round((layer["y"] + layer["h"] - SRC_ART_TOP) * ART_H / SRC_ART_H)
    x0 = clamp(x0, 0, ART_W - 1)
    y0 = clamp(y0, 0, ART_H - 1)
    x1 = clamp(x1, x0 + 1, ART_W)
    y1 = clamp(y1, y0 + 1, ART_H)
    return x0, y0, x1 - x0, y1 - y0


class Source:
    """Read-only view of the pinned RW5 checkout with structural validation."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.story_dir = root / "src" / "common" / "story"
        self.image_dir = root / "src" / "common" / "images"
        self.ui_dir = root / "src" / "common" / "ui"
        self.lines: list[tuple[str, str, int]] = []
        self.scenes: list[dict] = []
        self.scene_index: dict[str, int] = {}
        self.gallery: list[dict] = []
        self.title_wav = root / "src" / "common" / "title-theme.wav"
        self.text_reference = root / "converted" / "text.json"
        self.flat_text = root / "tests" / "fixtures" / "original-text-reference.json"
        self.inputs: list[Path] = []
        self.chapter_first_line: list[int] = []
        self.chapter_first_event: list[int] = []

    def load(self) -> None:
        if not self.story_dir.is_dir():
            raise BuildError(f"story directory missing: {self.story_dir}")
        chunk_files: list[list[Path]] = []
        for chapter in range(1, 5):
            files = []
            for index in range(12):
                path = self.story_dir / f"{chapter}-{index}.txt"
                if not path.is_file():
                    break
                files.append(path)
            if not files:
                raise BuildError(f"chapter {chapter} has no chunks")
            chunk_files.append(files)
            self.inputs.extend(files)

        event_cursor = 0
        for chapter_index, files in enumerate(chunk_files):
            self.chapter_first_line.append(len(self.lines))
            self.chapter_first_event.append(event_cursor)
            count = 0
            event_cursor += 1   # chapter transition
            for path in files:
                chunk = json.loads(path.read_text(encoding="utf-8"))
                if chunk.get("v") != 2:
                    raise BuildError(f"unsupported chunk version in {path.name}")
                lines = chunk.get("lines")
                scenes = chunk.get("scenes")
                if not isinstance(lines, list) or not isinstance(scenes, list):
                    raise BuildError(f"{path.name} is missing lines/scenes arrays")
                for entry in lines:
                    if not isinstance(entry, list) or len(entry) != 3:
                        raise BuildError(f"{path.name} line is not [speaker, text, scene]")
                    speaker, text, scene_ref = entry
                    if not isinstance(speaker, str) or not isinstance(text, str):
                        raise BuildError(f"{path.name} speaker/text is not a string")
                    if not isinstance(scene_ref, int) or not 0 <= scene_ref < len(scenes):
                        raise BuildError(f"{path.name} line references an unknown scene")
                    local = scenes[scene_ref]
                    scene_id = local.get("id")
                    layers = local.get("layers")
                    if not isinstance(scene_id, str) or not scene_id:
                        raise BuildError(f"{path.name} scene has no id")
                    if not isinstance(layers, list) or len(layers) > 4:
                        raise BuildError(f"scene {scene_id} has an invalid layer list")
                    known = self.scene_index.get(scene_id)
                    if known is None:
                        known = len(self.scenes)
                        self.scene_index[scene_id] = known
                        self.scenes.append({"id": scene_id, "layers": layers})
                    elif self.scenes[known]["layers"] != layers:
                        raise BuildError(f"scene {scene_id} changed between chunks")
                    self.lines.append((speaker, text, known))
                    count += 1
                    event_cursor += 1
            if count != CHAPTER_COUNTS[chapter_index]:
                raise BuildError(
                    f"chapter {chapter_index + 1} has {count} paragraphs, expected "
                    f"{CHAPTER_COUNTS[chapter_index]}"
                )

        gallery_path = self.root / "src" / "common" / "gallery.txt"
        self.gallery = json.loads(gallery_path.read_text(encoding="utf-8"))
        self.inputs.append(gallery_path)
        self.inputs.extend(self.ui_dir / name for name in ("title.jpg", "title-logo.png"))
        self.inputs.append(self.title_wav)
        self.inputs.append(self.text_reference)

        self._validate()
        self._check_reference_text()

    # -- structural rules ---------------------------------------------------
    def _validate(self) -> None:
        if len(self.lines) != sum(CHAPTER_COUNTS):
            raise BuildError("paragraph total does not match the four chapter counts")
        if len(self.scenes) != 675:
            raise BuildError(f"expected 675 unique scenes, found {len(self.scenes)}")
        if len(self.gallery) != 35:
            raise BuildError(f"expected 35 gallery CGs, found {len(self.gallery)}")
        if len({item["group"] for item in self.gallery}) != 6:
            raise BuildError("expected 6 gallery groups")

        images: set[str] = set()
        for scene in self.scenes:
            layers = scene["layers"]
            backgrounds = 1 if layers and is_background(layers[0]) else 0
            for index, layer in enumerate(layers):
                name = layer["image"]
                if not isinstance(name, str):
                    raise BuildError("layer image is not a string")
                if not (self.image_dir / name).is_file():
                    raise BuildError(f"missing layer image {name}")
                images.add(name)
                for key in ("x", "y", "w", "h"):
                    if not isinstance(layer[key], int):
                        raise BuildError(f"layer {name} has a non-integer {key}")
                if layer["w"] <= 0 or layer["h"] <= 0:
                    raise BuildError(f"layer {name} has an empty rectangle")
                if index >= backgrounds and (
                    layer["x"] < 0
                    or layer["y"] < SRC_ART_TOP
                    or layer["x"] + layer["w"] > SRC_ART_W
                    or layer["y"] + layer["h"] > SRC_ART_TOP + SRC_ART_H
                ):
                    raise BuildError(f"scene {scene['id']} sprite leaves the art band")
        for item in self.gallery:
            if not (self.image_dir / item["image"]).is_file():
                raise BuildError(f"missing gallery image {item['image']}")
        self.inputs.extend(self.image_dir / name for name in sorted(images))
        self.inputs.extend(sorted({self.image_dir / item["image"] for item in self.gallery}))

    def _check_reference_text(self) -> None:
        reference = json.loads(self.text_reference.read_text(encoding="utf-8"))
        if len(reference) != len(self.lines):
            raise BuildError("converted/text.json does not cover every paragraph")
        for index, (pair, line) in enumerate(zip(reference, self.lines)):
            if list(pair) != [line[0], line[1]]:
                raise BuildError(f"converted/text.json differs at paragraph {index}")
        if self.flat_text.is_file():
            fixture = json.loads(self.flat_text.read_text(encoding="utf-8"))
            records = fixture.get("records") if isinstance(fixture, dict) else None
            if records is not None and records != [[line[0], line[1]] for line in self.lines]:
                raise BuildError("independent text fixture differs from the source")

    def scene_image(self, name: str) -> Image.Image:
        return Image.open(self.image_dir / name)

    def gallery_image(self, item: dict) -> Image.Image:
        return Image.open(self.image_dir / item["image"])

    def input_digest(self) -> str:
        digest = hashlib.sha256()
        for path in sorted(set(self.inputs)):
            digest.update(str(path.relative_to(self.root)).replace("\\", "/").encode())
            digest.update(b"\0")
            digest.update(sha256_file(path).encode())
            digest.update(b"\n")
        return digest.hexdigest()


# --------------------------------------------------------------------------- jpeg


def encode_jpeg(image: Image.Image, quality: int) -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, format="JPEG", quality=quality, subsampling=2, optimize=False)
    return buffer.getvalue()


def jpeg_segments(data: bytes) -> tuple[list[tuple[int, bytes]], bytes]:
    """Split a baseline JPEG into header segments and entropy bytes."""
    if data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise BuildError("encoder did not produce a complete baseline JPEG")
    segments: list[tuple[int, bytes]] = []
    offset = 2
    while offset < len(data) - 1:
        if data[offset] != 0xFF:
            raise BuildError("malformed JPEG segment marker")
        marker = data[offset + 1]
        length = (data[offset + 2] << 8) | data[offset + 3]
        segments.append((marker, data[offset:offset + 2 + length]))
        offset += 2 + length
        if marker == 0xDA:
            return segments, data[offset:-2]
    raise BuildError("JPEG has no scan segment")


class JpegClass:
    """One shared DQT/DHT prefix plus a 35-byte scan header per strip."""

    def __init__(self, quality: int) -> None:
        self.quality = quality
        self.prefix = b""
        self.verified = 0

    def ensure_prefix(self, image: Image.Image) -> None:
        self._ensure_prefix(image)

    def _ensure_prefix(self, image: Image.Image) -> None:
        if self.prefix:
            return
        sample = image.crop((0, 0, image.width, min(BAND_ROWS, image.height)))
        segments, _ = jpeg_segments(encode_jpeg(sample, self.quality))
        prefix = bytearray(b"\xff\xd8")
        for marker, payload in segments:
            if marker in (0xDB, 0xC4):
                prefix += payload
        if len(prefix) == 2:
            raise BuildError("JPEG encoder emitted no quantisation/Huffman tables")
        self.prefix = bytes(prefix)

    def strips(self, image: Image.Image) -> list[bytes]:
        self._ensure_prefix(image)
        strips: list[bytes] = []
        for top in range(0, image.height, BAND_ROWS):
            strip = image.crop((0, top, image.width, min(image.height, top + BAND_ROWS)))
            jpeg = encode_jpeg(strip, self.quality)
            segments, entropy = jpeg_segments(jpeg)
            blob = bytearray()
            for marker, payload in segments:
                if marker in (0xC0, 0xC2, 0xDA):
                    blob += payload
            blob += entropy
            blob += b"\xff\xd9"
            # Prove the shared prefix reconstructs this strip bit-exactly.
            rebuilt = Image.open(io.BytesIO(self.prefix + bytes(blob)))
            rebuilt.load()
            expected = Image.open(io.BytesIO(jpeg))
            expected.load()
            if rebuilt.tobytes() != expected.tobytes():
                raise BuildError("shared JPEG prefix does not reconstruct a strip")
            self.verified += 1
            strips.append(bytes(blob))
        return strips


# --------------------------------------------------------------------------- pack


class Pack:
    """Serialise images, strips and shared prefixes into one binary blob."""

    def __init__(self) -> None:
        self.prefixes: list[bytes] = []
        self.images: list[dict] = []
        self.bands: list[tuple[bytes, int, int]] = []

    def add(self, image: Image.Image, encoder: JpegClass) -> int:
        if image.width <= 0 or image.width > ART_W or image.height <= 0:
            raise BuildError(f"image {image.width}x{image.height} does not fit the strip buffer")
        encoder.ensure_prefix(image)
        if encoder.prefix not in self.prefixes:
            self.prefixes.append(encoder.prefix)
        prefix_index = self.prefixes.index(encoder.prefix)
        first_band = len(self.bands)
        strips = encoder.strips(image)
        for index, blob in enumerate(strips):
            top = index * BAND_ROWS
            bottom = min(image.height, top + BAND_ROWS)
            self.bands.append((blob, top, bottom))
        self.images.append(
            {
                "w": image.width,
                "h": image.height,
                "prefix": prefix_index,
                "first_band": first_band,
                "bands": len(strips),
            }
        )
        return len(self.images) - 1

    def serialise(self) -> bytes:
        header_size = 48
        prefix_table = header_size
        image_table = prefix_table + 8 * len(self.prefixes)
        band_table = image_table + 16 * len(self.images)
        payload = band_table + 16 * len(self.bands)

        prefix_bytes = bytearray()
        for prefix in self.prefixes:
            prefix_bytes += prefix

        band_bytes = bytearray()
        band_entries: list[tuple[int, int, int, int]] = []
        for blob, top, bottom in self.bands:
            band_entries.append((len(band_bytes), len(blob), top, bottom))
            band_bytes += blob

        bias = len(prefix_bytes)
        band_table_bytes = bytearray()
        for offset, length, top, bottom in band_entries:
            band_table_bytes += struct.pack("<IIHH", offset + bias, length, top, bottom)
            band_table_bytes += bytes(4)

        self.entries = []
        image_bytes = bytearray()
        for image in self.images:
            first = image["first_band"]
            count = image["bands"]
            crc = zlib.crc32(bytes(band_table_bytes[first * 16:(first + count) * 16])) & 0xFFFFFFFF
            start_offset = band_entries[first][0]
            end_entry = band_entries[first + count - 1]
            end_offset = end_entry[0] + end_entry[1]
            crc = zlib.crc32(bytes(band_bytes[start_offset:end_offset]), crc) & 0xFFFFFFFF
            image_bytes += struct.pack(
                "<HHHBB", image["w"], image["h"], first, count, image["prefix"]
            )
            image_bytes += bytes(4)
            image_bytes += struct.pack("<I", crc)
            self.entries.append((image["w"], image["h"], first, count, image["prefix"], crc))

        total = payload + len(prefix_bytes) + len(band_bytes)
        out = bytearray()
        out += b"MTMEDIA\x00"
        out += struct.pack("<HHHHH", 1, header_size, len(self.images), len(self.bands),
                           len(self.prefixes))
        out += struct.pack("<H", BAND_ROWS)
        out += struct.pack("<IIII", total, 0, prefix_table, image_table)
        out += struct.pack("<II", band_table, payload)
        out += struct.pack("<HH", ART_W, ART_H)
        if len(out) != header_size:
            raise BuildError("pack header is not 48 bytes")

        prefix_entries = bytearray()
        cursor = 0
        for prefix in self.prefixes:
            prefix_entries += struct.pack("<II", cursor, len(prefix))
            cursor += len(prefix)

        out += prefix_entries
        out += image_bytes
        out += band_table_bytes
        if len(out) != payload:
            raise BuildError("pack table layout mismatch")
        # Header/table CRC covers every table byte with the CRC field zeroed.
        struct.pack_into("<I", out, 24, zlib.crc32(bytes(out)) & 0xFFFFFFFF)
        out += prefix_bytes
        out += band_bytes
        if len(out) != total:
            raise BuildError("pack size mismatch")
        return bytes(out)


# --------------------------------------------------------------------------- audio

IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3258, 3583, 3941, 4334, 4766, 5242, 5766, 6342, 6975, 7672, 8438, 9280,
    10205, 11220, 12338, 13570, 14924, 16412, 18052, 19853, 21835, 24015, 26419, 29055,
    32007,
]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
BLOCK_SAMPLES = 256


def ima_step(sample: int, predictor: int, index: int) -> tuple[int, int, int]:
    step = IMA_STEP[index]
    diff = sample - predictor
    nibble = 8 if diff < 0 else 0
    if diff < 0:
        diff = -diff
    vpdiff = step >> 3
    if diff >= step:
        nibble |= 4
        diff -= step
        vpdiff += step
    step >>= 1
    if diff >= step:
        nibble |= 2
        diff -= step
        vpdiff += step
    step >>= 1
    if diff >= step:
        nibble |= 1
        vpdiff += step
    predictor = predictor - vpdiff if nibble & 8 else predictor + vpdiff
    predictor = clamp(predictor, -32768, 32767)
    index = clamp(index + IMA_INDEX[nibble & 7], 0, len(IMA_STEP) - 1)
    return nibble, predictor, index


def ima_apply(nibble: int, predictor: int, index: int) -> tuple[int, int]:
    step = IMA_STEP[index]
    vpdiff = step >> 3
    if nibble & 4:
        vpdiff += step
    if nibble & 2:
        vpdiff += step >> 1
    if nibble & 1:
        vpdiff += step >> 2
    predictor = predictor - vpdiff if nibble & 8 else predictor + vpdiff
    predictor = clamp(predictor, -32768, 32767)
    index = clamp(index + IMA_INDEX[nibble & 7], 0, len(IMA_STEP) - 1)
    return predictor, index


def ima_encode_block(samples: list[int]) -> bytes:
    """Encode one independent block: header sample + 255 deltas + one pad nibble."""
    best: tuple[int, str] | None = None
    for start_index in (0, 8, 16, 24, 32, 40, 48, 56, 64, 72):
        predictor = samples[0]
        index = start_index
        nibbles: list[int] = []
        error = 0
        for sample in samples[1:]:
            nibble, predictor, index = ima_step(sample, predictor, index)
            nibbles.append(nibble)
            error += (sample - predictor) ** 2
        if best is None or error < best[0]:
            best = (error, start_index, nibbles)
    assert best is not None
    _, start_index, nibbles = best
    while len(nibbles) < BLOCK_SAMPLES:
        nibbles.append(0)
    first = samples[0]
    payload = bytearray([first & 0xFF, (first >> 8) & 0xFF, start_index & 0xFF, 0])
    for index in range(0, len(nibbles), 2):
        payload.append(((nibbles[index] & 0xF) << 4) | (nibbles[index + 1] & 0xF))
    return bytes(payload)


def ima_decode_block(block: bytes) -> list[int]:
    first = block[0] | (block[1] << 8)
    if first >= 0x8000:
        first -= 0x10000
    index = clamp(block[2], 0, len(IMA_STEP) - 1)
    predictor = first
    out = [predictor]
    for byte in block[4:]:
        for nibble in (byte >> 4, byte & 0x0F):
            predictor, index = ima_apply(nibble, predictor, index)
            out.append(predictor)
    return out[:BLOCK_SAMPLES]


def load_title_wav(path: Path) -> tuple[list[int], int]:
    with wave.open(str(path), "rb") as handle:
        if handle.getnchannels() != 1 or handle.getsampwidth() != 2:
            raise BuildError("title track is not mono 16-bit PCM")
        rate = handle.getframerate()
        frames = handle.readframes(handle.getnframes())
    if rate != 4000:
        raise BuildError(f"title track sample rate is {rate}, expected 4000")
    samples = list(struct.unpack("<%dh" % (len(frames) // 2), frames))
    if len(samples) != 1135520:
        raise BuildError(f"title track has {len(samples)} samples, expected 1135520")
    return samples, rate


def build_audio(samples: list[int], rate: int) -> tuple[bytes, dict]:
    blocks = []
    for offset in range(0, len(samples), BLOCK_SAMPLES):
        block = samples[offset:offset + BLOCK_SAMPLES]
        block = block + [block[-1]] * (BLOCK_SAMPLES - len(block))
        blocks.append(ima_encode_block(block))
    payload = b"".join(blocks)
    header = bytearray(b"MTAUDIO\x00")
    header += struct.pack("<BBH", 1, 1, 0)
    header += struct.pack("<II", rate, len(samples))
    header += struct.pack("<HH", BLOCK_SAMPLES, 0)
    header += struct.pack("<II", len(blocks), len(payload))
    header += struct.pack("<I", 0)
    if len(header) != 36:
        raise BuildError("audio header is not 36 bytes")
    body = bytearray(bytes(header) + payload)
    struct.pack_into("<I", body, 32, zlib.crc32(bytes(body)) & 0xFFFFFFFF)
    body = bytes(body)

    decoded: list[int] = []
    for block in blocks:
        decoded.extend(ima_decode_block(block)[:BLOCK_SAMPLES])
    decoded = decoded[:len(samples)]
    error = sum((a - b) ** 2 for a, b in zip(samples, decoded))
    signal = sum(a * a for a in samples)
    snr = float("inf") if error == 0 else 10.0 * math.log10(signal / error)
    report = {
        "source_rate": rate,
        "sample_count": len(samples),
        "seconds": round(len(samples) / rate, 3),
        "block_samples": BLOCK_SAMPLES,
        "block_count": len(blocks),
        "payload_bytes": len(payload),
        "pack_bytes": len(body),
        "pack_sha256": sha256_bytes(body),
        "snr_db": round(snr, 3),
        "duration_preserved": len(decoded) == len(samples),
        "pcm_bytes": len(samples) * 2,
    }
    return body, report


# --------------------------------------------------------------------------- media


def is_background(layer: dict) -> bool:
    """A full-width 432x243 image filling the art band (two scenes use 431 px)."""
    return (
        layer["x"] == 0
        and layer["y"] == SRC_ART_TOP
        and layer["h"] == SRC_ART_H
        and layer["w"] >= SRC_ART_W - 1
    )


def compose_background(cache: dict[str, Image.Image], source: Source, layer: dict) -> Image.Image:
    key = "bg:%s:%d:%d:%d:%d" % (
        layer["image"], layer["x"], layer["y"], layer["w"], layer["h"]
    )
    if key not in cache:
        canvas = Image.new("RGB", (ART_W, ART_H), (0, 0, 0))
        x0 = round(layer["x"] * ART_W / SRC_ART_W)
        x1 = round((layer["x"] + layer["w"]) * ART_W / SRC_ART_W)
        fitted = source.scene_image(layer["image"]).convert("RGB").resize(
            (max(1, x1 - x0), ART_H), Image.LANCZOS
        )
        canvas.paste(fitted, (x0, 0))
        cache[key] = canvas
    return cache[key].copy()


def compose_scene(source: Source, scene: dict, cache: dict[str, Image.Image]) -> Image.Image:
    layers = scene["layers"]
    if layers and is_background(layers[0]):
        canvas = compose_background(cache, source, layers[0])
        layers = layers[1:]
    else:
        canvas = Image.new("RGB", (ART_W, ART_H), (0, 0, 0))
    for layer in layers:
        x, y, w, h = sprite_rect(layer)
        sprite = source.scene_image(layer["image"]).convert("RGBA").resize((w, h), Image.LANCZOS)
        canvas.paste(sprite, (x, y), sprite)
    return canvas


def build_media(source: Source):
    backgrounds = JpegClass(BG_QUALITY)
    patches = JpegClass(PATCH_QUALITY)
    cgs = JpegClass(CG_QUALITY)
    thumbs = JpegClass(THUMB_QUALITY)
    title_class = JpegClass(TITLE_QUALITY)
    pack = Pack()
    base_cache: dict[str, Image.Image] = {}

    bg_index: dict[str, int] = {}
    patch_index: dict[str, int] = {}
    scene_rows: list[dict] = []
    patch_rows: list[dict] = []

    for scene in source.scenes:
        layers = scene["layers"]
        if not layers:
            scene_rows.append({"bg": SCENE_NONE, "patch_count": 0, "first_patch": len(patch_rows)})
            continue
        has_background = is_background(layers[0])
        if has_background:
            base = compose_background(base_cache, source, layers[0])
            key = "bg:%s:%d:%d:%d:%d" % (
                layers[0]["image"], layers[0]["x"], layers[0]["y"],
                layers[0]["w"], layers[0]["h"]
            )
            if key not in bg_index:
                bg_index[key] = pack.add(base, backgrounds)
            bg_media = bg_index[key]
            sprites = layers[1:]
        else:
            base = Image.new("RGB", (ART_W, ART_H), (0, 0, 0))
            bg_media = SCENE_NONE
            sprites = layers
        canvas = base.copy()
        first_patch = len(patch_rows)
        for layer in sprites:
            x, y, w, h = sprite_rect(layer)
            sprite = source.scene_image(layer["image"]).convert("RGBA").resize((w, h), Image.LANCZOS)
            patch_image = canvas.crop((x, y, x + w, y + h))
            patch_image.paste(sprite, (0, 0), sprite)
            key = hashlib.sha1(patch_image.tobytes() + struct.pack("<HHHH", x, y, w, h)).hexdigest()
            if key not in patch_index:
                patch_index[key] = pack.add(patch_image, patches)
            patch_rows.append({"image": patch_index[key], "x": x, "y": y})
            canvas.paste(sprite, (x, y), sprite)
        scene_rows.append({"bg": bg_media, "patch_count": len(sprites),
                           "first_patch": first_patch})

    cg_images: list[int] = []
    cg_thumbs: list[int] = []
    for item in source.gallery:
        image = source.gallery_image(item).convert("RGB")
        scale = min(ART_W / image.width, ART_H / image.height)
        size = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
        fitted = image.resize(size, Image.LANCZOS)
        canvas = Image.new("RGB", (ART_W, ART_H), (0, 0, 0))
        canvas.paste(fitted, ((ART_W - size[0]) // 2, (ART_H - size[1]) // 2))
        cg_images.append(pack.add(canvas, cgs))
        cg_thumbs.append(pack.add(image.resize((THUMB_W, THUMB_H), Image.LANCZOS), thumbs))

    # Title key visual: the opening source scene plus the Chinese logo over a
    # darker strip, so the landscape title page keeps its own composition.
    opening = next(scene for scene in source.scenes if scene["layers"])
    art = compose_scene(source, opening, base_cache)
    key_visual = Image.new("RGB", (ART_W, TITLE_H), (0, 0, 0))
    key_visual.paste(art, (0, 0))
    key_visual.paste(Image.new("RGB", (ART_W, PANEL_H), (0x0B, 0x10, 0x20)), (0, ART_H))
    logo = Image.open(source.ui_dir / "title-logo.png").convert("RGBA")
    logo_scale = min((ART_W - 24) / logo.width, 46 / logo.height)
    logo = logo.resize((round(logo.width * logo_scale), round(logo.height * logo_scale)),
                       Image.LANCZOS)
    key_visual.paste(logo, (12, ART_H + (PANEL_H - logo.height) // 2), logo)
    title_index = pack.add(key_visual, title_class)

    blob = pack.serialise()
    meta = {
        "format": "MTMEDIA1",
        "art": [ART_W, ART_H],
        "band_rows": BAND_ROWS,
        "quality": {
            "background": BG_QUALITY,
            "patch": PATCH_QUALITY,
            "cg": CG_QUALITY,
            "thumb": THUMB_QUALITY,
            "title": TITLE_QUALITY,
        },
        "images": len(pack.images),
        "bands": len(pack.bands),
        "prefixes": len(pack.prefixes),
        "pack_bytes": len(blob),
        "pack_sha256": sha256_bytes(blob),
        "background_images": len(bg_index),
        "patch_instances": len(patch_rows),
        "patch_images": len(patch_index),
        "cg_images": len(cg_images),
        "cg_thumbs": len(cg_thumbs),
        "title_image": title_index,
        "verified_strips": backgrounds.verified + patches.verified + cgs.verified
                            + thumbs.verified + title_class.verified,
    }
    return blob, meta, scene_rows, patch_rows, cg_images, cg_thumbs, title_index, pack.entries


# --------------------------------------------------------------------------- C output

def c_string(value: str) -> str:
    out = ['"']
    for char in value:
        if char in '"\\':
            out.append("\\" + char)
        elif char == "\n":
            out.append("\\n")
        elif ord(char) < 0x20:
            out.append("\\x%02x" % ord(char))
        else:
            out.append(char)
    out.append('"')
    return "".join(out)


def emit_content(source: Source, cg_images: list[int]) -> str:
    out: list[str] = []
    out.append("// main/gal/gal_content.c —— 生成文件，请勿手改。")
    out.append("// 由 tools/maitetsu_assets.py 从固定提交的 RW5 二六线素材生成：")
    out.append("// %s @ %s" % (SOURCE_REPO, SOURCE_COMMIT))
    out.append('#include "gal_content.h"')
    out.append("")
    out.append('#define GAL_CONTENT_TITLE    "爱上火车"')
    out.append('#define GAL_CONTENT_SUBTITLE "二六线 · 全四章"')
    out.append('#define GAL_CONTENT_LOGO_SUB "MAITETSU · NIIROKU"')
    out.append('#define GAL_CONTENT_VERSION  "v0.1.0"')
    out.append("")
    out.append("const gal_line_t gal_content_lines[] = {")
    for speaker, text, scene in source.lines:
        out.append("    { %s, %s, %d, GAL_UNLOCK_NONE, NULL, NULL },"
                   % (c_string(speaker), c_string(text), scene))
    out.append("};")
    out.append("const int gal_content_line_count =")
    out.append("    (int)(sizeof(gal_content_lines) / sizeof(gal_content_lines[0]));")
    out.append("")
    out.append("// 本作没有分支选项：保留一个空组，事件流里不出现 CHOICE。")
    out.append("const gal_choice_t gal_content_choices[] = { { \"\", 0, NULL } };")
    out.append("const int gal_content_choice_count = 0;")
    out.append("")
    out.append("const gal_transition_t gal_content_transitions[] = {")
    for index, name in enumerate(CHAPTERS):
        out.append("    { %s, \"CHAPTER %02d\", %s, %s },"
                   % (c_string(name), index + 1, c_string(name),
                      c_string("第 %d 章 · %d 段" % (index + 1, CHAPTER_COUNTS[index]))))
    out.append("};")
    out.append("const int gal_content_transition_count =")
    out.append("    (int)(sizeof(gal_content_transitions) / sizeof(gal_content_transitions[0]));")
    out.append("")
    out.append("const gal_ending_t gal_content_endings[] = {")
    out.append('    { "EPILOGUE", "繋いでいく未来",')
    out.append('      "二六线的四章已经读完。\\n"')
    out.append('      "感谢原作者与中文化作者。\\n"')
    out.append('      "本固件只包含这条线路，不代表原作全部路线。" },')
    out.append("};")
    out.append("const int gal_content_ending_count =")
    out.append("    (int)(sizeof(gal_content_endings) / sizeof(gal_content_endings[0]));")
    out.append("")
    out.append("const gal_event_t gal_content_events[] = {")
    line = 0
    for index in range(4):
        out.append("    { GAL_EV_TRANSITION, %d, GAL_UNLOCK_NONE }," % index)
        for _ in range(CHAPTER_COUNTS[index]):
            out.append("    { GAL_EV_LINE, %d, GAL_UNLOCK_NONE }," % line)
            line += 1
    out.append("    { GAL_EV_ENDING, 0, GAL_UNLOCK_NONE },")
    out.append("};")
    out.append("const int gal_content_event_count =")
    out.append("    (int)(sizeof(gal_content_events) / sizeof(gal_content_events[0]));")
    out.append("")

    groups: dict[str, list[tuple[int, dict]]] = {}
    for index, item in enumerate(source.gallery):
        groups.setdefault(item["group"], []).append((index, item))
    out.append("const gal_cg_t gal_content_cgs[] = {")
    for position, (media_index, item) in enumerate(zip(cg_images, source.gallery)):
        siblings = groups[item["group"]]
        variant = next(i for i, (index, _) in enumerate(siblings) if index == position) + 1
        caption = item["label"]
        if len(siblings) > 1:
            caption = "%s · 差分 %d/%d" % (caption, variant, len(siblings))
        out.append('    { "CG %02d", %s, GAL_EVENT_NONE, %d, true, NULL },'
                   % (position + 1, c_string(caption), media_index))
    out.append("};")
    out.append("const int gal_content_cg_count =")
    out.append("    (int)(sizeof(gal_content_cgs) / sizeof(gal_content_cgs[0]));")
    out.append("")
    out.append("const gal_chapter_t gal_content_chapters[] = {")
    for index, name in enumerate(CHAPTERS):
        out.append("    { %s, %s, %s, %d, GAL_UNLOCK_NONE, true },"
                   % (c_string(name), c_string(CHAPTER_SUMMARIES[index]),
                      c_string("全 %d 段" % CHAPTER_COUNTS[index]),
                      source.chapter_first_event[index]))
    out.append("};")
    out.append("const int gal_content_chapter_count =")
    out.append("    (int)(sizeof(gal_content_chapters) / sizeof(gal_content_chapters[0]));")
    out.append("")
    out.append("const gal_recall_t gal_content_recalls[] = {")
    recall_entries = []
    for group, items in groups.items():
        label = items[0][1]["label"]
        chapter = 3 if label.startswith("尾声") else 1
        recall_entries.append((label, chapter))
    for label, chapter in recall_entries:
        out.append("    { %s, %s, (uint8_t)0, %d },"
                   % (c_string(label), c_string(CHAPTERS[chapter]),
                      source.chapter_first_event[chapter]))
    out.append("};")
    out.append("const int gal_content_recall_count =")
    out.append("    (int)(sizeof(gal_content_recalls) / sizeof(gal_content_recalls[0]));")
    out.append("")
    out.append("const gal_track_t gal_content_tracks[] = {")
    out.append('    { "繋いでいく未来 (Piano)", "04:44", (uint8_t)0, NULL, (uint8_t)0, (uint8_t)0 },')
    out.append("};")
    out.append("const int gal_content_track_count =")
    out.append("    (int)(sizeof(gal_content_tracks) / sizeof(gal_content_tracks[0]));")
    out.append("")
    out.append("const gal_about_row_t gal_content_about_rows[] = {")
    rows = [
        ("Version", "v0.1.0"),
        ("Source", "xuanyide01/maitetsu-rw5"),
        ("Pin", SOURCE_COMMIT[:12]),
        ("Scope", "二六线四章 · 1537 段"),
        ("Routes", "仅本线，不含原作其他路线"),
        ("Rights", "Lose / HIKARI FIELD"),
    ]
    for key, value in rows:
        out.append("    { %s, %s }," % (c_string(key), c_string(value)))
    out.append("};")
    out.append("const int gal_content_about_row_count =")
    out.append("    (int)(sizeof(gal_content_about_rows) / sizeof(gal_content_about_rows[0]));")
    out.append("")
    out.append("const char *const gal_content_title_menu[] = {")
    out.append('    "开始游戏", "继续", "读取存档", "设置", "鉴赏室", "关于",')
    out.append("};")
    out.append("const int gal_content_title_menu_count =")
    out.append("    (int)(sizeof(gal_content_title_menu) / sizeof(gal_content_title_menu[0]));")
    out.append("")
    out.append("const char *const gal_content_quick_menu[] = {")
    out.append('    "返回故事", "保存", "读取", "历史", "设置", "返回标题",')
    out.append("};")
    out.append("const int gal_content_quick_menu_count =")
    out.append("    (int)(sizeof(gal_content_quick_menu) / sizeof(gal_content_quick_menu[0]));")
    out.append("")
    out.append("const char *const gal_content_gallery_menu[] = {")
    out.append('    "CG 鉴赏", "CG 回想", "剧情鉴赏", "音乐鉴赏",')
    out.append("};")
    out.append("const int gal_content_gallery_menu_count =")
    out.append("    (int)(sizeof(gal_content_gallery_menu) / sizeof(gal_content_gallery_menu[0]));")
    out.append("")
    out.append("const void *gal_content_title_image = NULL;   // 运行时指向媒体包")
    out.append("")
    return "\n".join(out)


def emit_scene_samples(source: Source, scene_rows: list[dict]) -> str:
    """Reference RGB samples from the source composition for the C decode test."""
    wanted = ["bec864fe561b53b0", "a18700e95f2a9f27"]
    picks: list[int] = []
    if scene_rows:
        picks.append(0)
    for name in wanted:
        index = source.scene_index.get(name)
        if index is not None:
            picks.append(index)
    for index, row in enumerate(scene_rows):
        if row["patch_count"] == 2:
            picks.append(index)
            break
    for index, row in enumerate(scene_rows):
        if row["patch_count"] == 1 and index not in picks:
            picks.append(index)
            break
    if len(scene_rows) > 300:
        picks.append(300)
    picks.append(len(scene_rows) - 1)
    order = sorted(set(picks))
    cache: dict[str, Image.Image] = {}
    lines = ["# generated by tools/maitetsu_assets.py: source composition samples"]
    for scene in order:
        row = scene_rows[scene]
        image = compose_scene(source, source.scenes[scene], cache)
        lines.append("scene %d" % scene)
        for x in (4, 80, 160, 240, 316):
            for y in (4, 45, 90, 135, 176):
                if x >= image.width or y >= image.height:
                    continue
                r, g, b = image.getpixel((x, y))
                lines.append("sample %d %d %d %d %d" % (x, y, r, g, b))
        lines.append("# patches %d bg %s" % (row["patch_count"], row["bg"]))
    kept = [line for line in lines if not line.startswith("#")]
    return "\n".join(kept) + "\n"


def emit_assets(scene_rows: list[dict], patch_rows: list[dict], cg_images: list[int],
                cg_thumbs: list[int], title_index: int,
                image_entries: list[tuple]) -> tuple[str, str]:
    header = [
        "// main/gal/maitetsu_assets.h —— 生成文件，请勿手改。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "#include \"maitetsu_pack.h\"",
        "",
        "#define MAITETSU_SCENE_NONE 0xFFFFu",
        "",
        "typedef struct {",
        "    uint16_t bg;          // 媒体包图像下标，MAITETSU_SCENE_NONE = 纯黑",
        "    uint8_t patch_count;  // 叠在背景上的补丁数",
        "    uint16_t first_patch; // 本场景第一个补丁的下标",
        "} maitetsu_scene_t;",
        "",
        "typedef struct {",
        "    uint16_t image;       // 已经与背景合成好的不透明补丁",
        "    uint16_t x;",
        "    uint16_t y;",
        "} maitetsu_patch_t;",
        "",
        "extern const maitetsu_scene_t maitetsu_scenes[];",
        "extern const int maitetsu_scene_count;",
        "extern const maitetsu_patch_t maitetsu_patches[];",
        "extern const int maitetsu_patch_count;",
        "extern const uint16_t maitetsu_cg_images[];",
        "extern const uint16_t maitetsu_cg_thumbs[];",
        "extern const int maitetsu_cg_count;",
        "extern const uint16_t maitetsu_title_media;",
        "",
        "// 稳定的按图描述（Flash 常量）：maitetsu_pack_image() 直接返回它的元素，",
        "// 生命周期是整个进程，不存在“指向同一临时对象”的别名。",
        "extern const maitetsu_image_t maitetsu_pack_images[];",
        "extern const int maitetsu_pack_images_count;",
        "",
    ]
    source = [
        "// main/gal/maitetsu_assets.c —— 生成文件，请勿手改。",
        '#include "maitetsu_assets.h"',
        "",
        "const maitetsu_image_t maitetsu_pack_images[] = {",
    ]
    for (w, h, first, count, prefix, crc) in image_entries:
        source.append("    { %d, %d, %d, %d, %d, 0x%08Xu }," % (w, h, first, count, prefix, crc))
    source += [
        "};",
        "const int maitetsu_pack_images_count =",
        "    (int)(sizeof(maitetsu_pack_images) / sizeof(maitetsu_pack_images[0]));",
        "",
        "const maitetsu_scene_t maitetsu_scenes[] = {",
    ]
    for row in scene_rows:
        source.append("    { %s, %d, %d },"
                      % ("0xFFFF" if row["bg"] == SCENE_NONE else row["bg"],
                         row["patch_count"], row["first_patch"]))
    source += [
        "};",
        "const int maitetsu_scene_count =",
        "    (int)(sizeof(maitetsu_scenes) / sizeof(maitetsu_scenes[0]));",
        "",
        "const maitetsu_patch_t maitetsu_patches[] = {",
    ]
    for row in patch_rows:
        source.append("    { %d, %d, %d }," % (row["image"], row["x"], row["y"]))
    source += [
        "};",
        "const int maitetsu_patch_count =",
        "    (int)(sizeof(maitetsu_patches) / sizeof(maitetsu_patches[0]));",
        "",
        "const uint16_t maitetsu_cg_images[] = {",
    ]
    for index in range(0, len(cg_images), 8):
        source.append("    " + ", ".join(str(v) for v in cg_images[index:index + 8]) + ",")
    source += ["};", "", "const uint16_t maitetsu_cg_thumbs[] = {"]
    for index in range(0, len(cg_thumbs), 8):
        source.append("    " + ", ".join(str(v) for v in cg_thumbs[index:index + 8]) + ",")
    source += [
        "};",
        "const int maitetsu_cg_count =",
        "    (int)(sizeof(maitetsu_cg_images) / sizeof(maitetsu_cg_images[0]));",
        "",
        "const uint16_t maitetsu_title_media = %d;" % title_index,
        "",
    ]
    return "\n".join(header), "\n".join(source)


# --------------------------------------------------------------------------- main


def write_or_check(path: Path, data: bytes, check: bool, changes: list[str]) -> None:
    if check:
        if not path.is_file() or path.read_bytes() != data:
            changes.append(str(path))
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == data:
        return
    path.write_bytes(data)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, help="pinned read-only maitetsu-rw5 checkout")
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent),
                        help="target repository (default: this checkout)")
    parser.add_argument("--check", action="store_true", help="fail if generated files are stale")
    parser.add_argument("--report", help="write a JSON build report here")
    args = parser.parse_args(argv)

    source = Source(Path(args.source))
    source.load()
    repo = Path(args.repo)

    blob, media_meta, scenes, patches, cg_images, cg_thumbs, title_index, image_entries =         build_media(source)
    wav_samples, rate = load_title_wav(source.title_wav)
    audio_blob, audio_meta = build_audio(wav_samples, rate)
    content_c = emit_content(source, cg_images).encode("utf-8")
    assets_h, assets_c = emit_assets(scenes, patches, cg_images, cg_thumbs, title_index,
                                     image_entries)
    samples_txt = emit_scene_samples(source, scenes).encode("utf-8")

    manifest = {
        "source_repo": SOURCE_REPO,
        "source_commit": SOURCE_COMMIT,
        "source_digest": source.input_digest(),
        "paragraphs": len(source.lines),
        "chapters": CHAPTER_COUNTS,
        "scenes": len(scenes),
        "gallery_cgs": len(cg_images),
        "gallery_groups": len({item["group"] for item in source.gallery}),
        "media": media_meta,
        "audio": audio_meta,
    }

    changes: list[str] = []
    write_or_check(repo / "assets" / "maitetsu" / "maitetsu_media.bin", blob, args.check, changes)
    write_or_check(repo / "assets" / "maitetsu" / "maitetsu_title.bin", audio_blob, args.check, changes)
    write_or_check(repo / "main" / "gal" / "gal_content.c", content_c, args.check, changes)
    write_or_check(repo / "main" / "gal" / "maitetsu_assets.c", assets_c.encode(), args.check, changes)
    write_or_check(repo / "main" / "gal" / "maitetsu_assets.h", assets_h.encode(), args.check, changes)
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode()
    write_or_check(repo / "assets" / "maitetsu" / "media_manifest.json", manifest_bytes,
                   args.check, changes)
    write_or_check(repo / "tests" / "fixtures" / "maitetsu-scene-samples.txt", samples_txt,
                   args.check, changes)

    if args.report:
        Path(args.report).write_text(
            json.dumps({"manifest": manifest, "changes": changes}, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )

    if args.check:
        if changes:
            print("maitetsu_assets: generated files are stale:", file=sys.stderr)
            for path in changes:
                print("  " + path, file=sys.stderr)
            return 1
        print("maitetsu_assets: generated files are current")
        return 0

    print(
        "maitetsu_assets: %d paragraphs, %d scenes, %d images, %d bands, "
        "pack %d bytes, audio %d bytes (%.2fs, SNR %.2f dB)"
        % (len(source.lines), len(scenes), media_meta["images"], media_meta["bands"],
           media_meta["pack_bytes"], audio_meta["pack_bytes"], audio_meta["seconds"],
           audio_meta["snr_db"])
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
