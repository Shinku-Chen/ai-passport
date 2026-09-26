#!/usr/bin/env python3
"""Pack the galgame source material into the read-only flash partition image.

The source material (background art, character sprites and chapter scripts) is
third-party content, tracked at `assets/gal-source/common`; point `--source` at
another copy of the upstream project's `src/common` directory to pack that one.

When the source directory is absent the script emits a small placeholder pack so
that a clone without the material can still configure and build the firmware.

Outputs (into --out):
    gal_assets.bin      raw image for the `assets` flash partition
    gal_assets.json     size report and per-image accounting
    gal_charset.txt     every codepoint the scripts use, for font subsetting

Usage:
    python tools/gal/pack_assets.py --source assets/gal-source/common --out build/gal
    python tools/gal/pack_assets.py --out build/gal           # placeholder pack
    python tools/gal/pack_assets.py --preview 3 --preview-out scene3.png
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gal_format as gf  # noqa: E402

# --- Screen layout -----------------------------------------------------------
# Baked into the packed art and mirrored by main/gal/gal_layout.h. The source is
# 336x480 and the panel is 240x320, so a uniform width scale keeps the art
# undistorted and the extra 23 rows are dropped off the bottom.

SRC_W, SRC_H = 336, 480
SCREEN_W, SCREEN_H = 240, 320
SCALE = SCREEN_W / SRC_W

# Character sprite origin; must equal GAL_SPRITE_X / GAL_SPRITE_Y in gal_layout.h
# because the trimmed sprite offsets stored in the pack are relative to it.
SPRITE_X, SPRITE_Y = 42, 56

# --- Upstream defects --------------------------------------------------------
# The upstream scripts reference eight images that its own repository does not
# contain, five of them obvious typos. Left alone the original renderer draws a
# blank frame. Map each onto the closest existing asset instead.
MISSING_IMAGE_FALLBACKS = {
    "line2": "line21",                  # truncated form of line21
    "zev_ask_c03_01": "ev_ask_c03_01",  # stray leading "z"
    "zev_ask_c03_11": "ev_ask_c03_11",  # stray leading "z"
    "ev_ask_c03_03": "ev_ask_c03_01",   # no c03_03 exists
    "ev_ask_c01_06": "ev_ask_c01_03",   # no c01_06 exists
    "a0014h": "a0015h",                 # a0014h absent from fg/
    "ask_z2a0100": "ask_z1a0100",       # z2a/z2b body variants absent
    "ask_z2b0100": "ask_z1b0100",
}

SOLID_RGB565 = {
    "bg/white.png": 0xFFFF,
    "bg/black.png": 0x0000,
}

# UI images loaded by name at runtime. ("bg", name) or (None, name).
#
# The upstream dialogue panel artwork is deliberately not packed: the reader
# styles its panel in code (see main/gal/gal_layout.h), because that artwork is a
# near-white plate carrying a repeating ornament that reads as a busy grey band
# over the scene art. Not packing it also drops 37 KiB from the partition.
#
# These are pack names, which are also the paths below the source root and -- this
# is the part that matters -- the names the firmware looks up in
# main/gal/gal_app.c. A lookup the pack does not answer is a black screen rather
# than a missing asset, so the two sides are kept identical and compared by
# tests/test_gal_asset_names.py.
UI_IMAGES = ("bg/index_bg.png",)


# --- Source discovery --------------------------------------------------------


class Sources:
    """Uniform view over a real or absent source tree."""

    def __init__(self, root: pathlib.Path | None):
        self.root = root
        self.placeholder = root is None or not root.is_dir()

    def background(self, filename: str) -> pathlib.Path | None:
        if self.placeholder:
            return None
        path = self.root / "bg" / filename
        return path if path.is_file() else None

    def sprite(self, stem: str) -> pathlib.Path | None:
        if self.placeholder:
            return None
        path = self.root / "fg" / (stem + ".png")
        return path if path.is_file() else None

    def loose(self, filename: str) -> pathlib.Path | None:
        if self.placeholder:
            return None
        path = self.root / filename
        return path if path.is_file() else None

    def scripts(self) -> list[pathlib.Path]:
        if self.placeholder:
            return []
        found = [p for p in self.root.glob("*.txt") if p.stem.isdigit()]
        return sorted(found, key=lambda p: int(p.stem))


# --- Script model ------------------------------------------------------------


class Dialogue:
    __slots__ = ("text", "name", "body", "face", "is_end", "end_text")

    def __init__(self, raw: dict):
        self.text = raw.get("text") or ""
        self.name = raw.get("character") or ""
        self.body = raw.get("body")
        self.face = raw.get("face")
        self.is_end = "END" in raw
        self.end_text = raw.get("END") or ""

    def display_text(self) -> str:
        return self.end_text if self.is_end else self.text

    def display_name(self) -> str:
        return self.end_text if self.is_end else self.name


class Scene:
    __slots__ = ("background", "overlay", "overlay_x", "overlay_y", "dialogues")

    def __init__(self, raw: dict):
        self.background = raw.get("background")
        self.overlay = raw.get("Img")
        self.overlay_x = int(raw.get("ImgLeft") or 0)
        self.overlay_y = int(raw.get("ImgTop") or 0)
        self.dialogues = [Dialogue(d) for d in raw.get("dialogues", [])]


def load_chapters(sources: Sources) -> list[list[Scene]]:
    chapters = []
    for path in sources.scripts():
        raw = json.loads(path.read_text(encoding="utf-8"))
        chapters.append([Scene(scene) for scene in raw])
    return chapters


def collect_assets(sources: Sources, chapters: list[list[Scene]]):
    """Return (unique [(pack_name, path)], aliases, applied_fallbacks).

    A reference that had to be substituted keeps its own name in the scripts but
    resolves to the asset it borrows, so a substituted background is stored once
    rather than duplicated under a second name.
    """
    backgrounds: set[str] = set()
    sprites: set[str] = set()
    for chapter in chapters:
        for scene in chapter:
            if scene.background:
                backgrounds.add(pathlib.Path(scene.background).stem)
            if scene.overlay:
                backgrounds.add(pathlib.Path(scene.overlay).stem)
            for line in scene.dialogues:
                if line.body:
                    sprites.add(line.body)
                if line.face:
                    sprites.add(line.face)

    applied: dict[str, str] = {}
    aliases: dict[str, str] = {}
    for kind, stems, lookup in (("background", backgrounds, sources.background),
                               ("sprite", sprites, sources.sprite)):
        prefix = "bg" if kind == "background" else "fg"
        suffix = ".png" if kind == "background" else ""
        for stem in sorted(stems):
            if lookup(stem + suffix) is not None:
                continue
            replacement = MISSING_IMAGE_FALLBACKS.get(stem)
            if replacement is None or lookup(replacement + suffix) is None:
                raise SystemExit(f"missing {kind} {stem!r} has no usable fallback")
            applied[stem] = replacement
            aliases[f"{prefix}/{stem}.png"] = f"{prefix}/{replacement}.png"

    images: list[tuple[str, pathlib.Path]] = []
    seen: set[pathlib.Path] = set()

    def add(name: str, path: pathlib.Path) -> None:
        if path in seen:
            return
        seen.add(path)
        images.append((name, path))

    for stem in sorted(backgrounds):
        add(f"bg/{stem}.png", sources.background(applied.get(stem, stem) + ".png"))
    for stem in sorted(sprites):
        add(f"fg/{stem}.png", sources.sprite(applied.get(stem, stem)))
    for name in UI_IMAGES:
        path = sources.loose(name)
        if path is not None:
            add(name, path)
    return images, aliases, applied


# --- Image conversion --------------------------------------------------------


def palette_entries(quantised, mode: str):
    """Read a PIL palette as exactly GAL_PALETTE_ENTRIES RGBA tuples.

    PIL truncates the returned palette to the colours actually in use, but LVGL
    always indexes a 256-entry table, so unused slots are padded. Indices beyond
    the real palette are never referenced by the pixel data.
    """
    raw = quantised.getpalette(mode) or []
    step = 4 if mode == "RGBA" else 3
    entries = []
    for index in range(gf.PALETTE_ENTRIES):
        base = index * step
        if base + step <= len(raw):
            r, g, b = raw[base], raw[base + 1], raw[base + 2]
            a = raw[base + 3] if step == 4 else 255
        else:
            r = g = b = 0
            a = 255
        entries.append((r, g, b, a))
    return entries


def require_pillow():
    try:
        from PIL import Image  # noqa: F401
    except ImportError as exc:  # pragma: no cover - depends on the host
        raise SystemExit(
            "Pillow is required to pack the real artwork "
            "(pip install Pillow); the placeholder pack does not need it") from exc


def quantise_rgba(image, trim: bool = False):
    """Quantise a PIL RGBA image to 256 entries, preserving alpha.

    A median cut with error diffusion reads better on the flat, fully opaque
    backgrounds; the octree quantiser is the only one that keeps per-pixel alpha
    on the sprite sheets, whose edges are antialiased.
    """
    from PIL import Image

    alpha = image.getchannel("A")
    opaque = alpha.getextrema() == (255, 255)

    box = (0, 0, image.width, image.height)
    if trim:
        # getbbox() reports the bounding box of the non-zero pixels, which is
        # exactly the alpha extent; sprites carry ~90% empty margin.
        found = alpha.getbbox()
        if found is None:
            return None
        box = found
        image = image.crop(box)

    if opaque:
        quantised = image.convert("RGB").quantize(
            colors=gf.PALETTE_ENTRIES, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
        return quantised, palette_entries(quantised, "RGB"), False, box

    quantised = image.quantize(colors=gf.PALETTE_ENTRIES, method=Image.FASTOCTREE)
    return quantised, palette_entries(quantised, "RGBA"), True, box


def encode_lvgl_i8(quantised, entries) -> bytes:
    """LVGL indexed payload: lv_color32_t[256] palette, then one index per pixel."""
    out = bytearray()
    for r, g, b, a in entries:
        out += bytes((b, g, r, a))  # little-endian lv_color32_t
    out += quantised.tobytes()
    return bytes(out)


def solid_payload(colour: int) -> bytes:
    return struct.pack("<H", colour) + b"\x00\x00"


# --- Chapter packing ---------------------------------------------------------


def pack_chapter(index: int, scenes: list[Scene], image_ids: dict[str, int],
                 aliases: dict[str, str], is_finale: bool) -> bytes:
    text_pool = bytearray()
    name_pool = bytearray()
    text_offsets: dict[str, int] = {}
    name_offsets: dict[str, int] = {}

    def intern(pool: bytearray, offsets: dict[str, int], value: str) -> tuple[int, int]:
        if value not in offsets:
            offsets[value] = len(pool)
            pool += value.encode("utf-8") + b"\x00"
        return offsets[value], len(value.encode("utf-8"))

    scene_records = bytearray()
    dialogue_records = bytearray()
    cursor = 0

    for scene in scenes:
        def image_id(prefix: str, stem: str | None) -> int:
            if not stem:
                return gf.IMG_NONE
            name = f"{prefix}/{stem}.png"
            return image_ids.get(aliases.get(name, name), gf.IMG_NONE)

        background = image_id("bg", pathlib.Path(scene.background).stem) \
            if scene.background else gf.IMG_NONE
        overlay = image_id("bg", pathlib.Path(scene.overlay).stem) \
            if scene.overlay else gf.IMG_NONE

        first = cursor
        for line in scene.dialogues:
            text = line.display_text()
            name = line.display_name()
            text_off, text_len = intern(text_pool, text_offsets, text)
            name_off, name_len = intern(name_pool, name_offsets, name)
            dialogue_records += struct.pack(
                gf.DLG_REC_STRUCT, text_off, text_len, name_off, name_len,
                image_id("fg", line.body), image_id("fg", line.face),
                gf.DLG_FLAG_END if line.is_end else 0)
            cursor += 1
        scene_records += struct.pack(gf.SCENE_REC_STRUCT, background, overlay,
                                     scene.overlay_x, scene.overlay_y, first,
                                     len(scene.dialogues), 0)

    scenes_off = gf.CHAPTER_HEADER_SIZE
    dialogues_off = scenes_off + len(scene_records)
    text_pool_off = dialogues_off + len(dialogue_records)
    names_off = text_pool_off + len(text_pool)
    header = struct.pack(gf.CHAPTER_HEADER_STRUCT, gf.CHAPTER_MAGIC, len(scenes),
                         cursor, scenes_off, dialogues_off, text_pool_off,
                         len(text_pool), names_off, len(name_pool), index,
                         gf.CHAPTER_FLAG_FINALE if is_finale else 0, 0)
    return bytes(header) + bytes(scene_records) + bytes(dialogue_records) + \
        bytes(text_pool) + bytes(name_pool)


# --- Placeholder -------------------------------------------------------------
# The placeholder pack is built without Pillow: CI has neither the source artwork
# nor the image library, and a fresh clone must still configure and build. Solid
# fills need no decoding, so the whole pack can be emitted from plain bytes.

PLACEHOLDER_TEXT = "\u5360\u4f4d\u5267\u672c\u3002\u672a\u627e\u5230\u539f\u7248\u7d20\u6750\u3002"
PLACEHOLDER_DIALOGUE = "\u6b64\u56fa\u4ef6\u672a\u5305\u542b\u539f\u7248\u7d20\u6750\u3002"
PLACEHOLDER_IMAGES = (
    ("bg/index_bg.png", 0x1084),
    ("bg/black.png", 0x0000),
    ("bg/white.png", 0xFFFF),
    ("bg/placeholder.png", 0x18E4),
    ("fg/placeholder.png", 0x18E4),
    ("text_bg.png", 0x18E4),
)


def placeholder_chapters() -> list[list[Scene]]:
    return [[Scene({"background": "placeholder.png",
                    "dialogues": [{"character": "", "text": PLACEHOLDER_TEXT},
                                   {"character": "", "text": PLACEHOLDER_DIALOGUE}]})]]


# --- Assembly ----------------------------------------------------------------


def align_up(value: int, alignment: int = gf.ALIGN) -> int:
    return (value + alignment - 1) // alignment * alignment


def build(args) -> int:
    sources = Sources(pathlib.Path(args.source) if args.source else None)
    out_dir = pathlib.Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    if sources.placeholder:
        print(f"note: no source material under {args.source!r}; building a placeholder pack")

    chapters = load_chapters(sources)
    images, aliases, fallbacks = collect_assets(sources, chapters) if chapters else ([], {}, {})
    if not chapters:
        chapters = placeholder_chapters()
        images = []

    payloads: list[tuple[str, int, int, int, int, int, bytes]] = []
    # name, kind, width, height, offset_x, offset_y, payload

    def add(name: str, quantised, entries, has_alpha: bool, offset=(0, 0)) -> None:
        kind = gf.KIND_ALPHA if has_alpha else gf.KIND_OPAQUE
        payloads.append((name, kind, quantised.width, quantised.height,
                         offset[0], offset[1], encode_lvgl_i8(quantised, entries)))

    if sources.placeholder:
        for name, color in PLACEHOLDER_IMAGES:
            payloads.append((name, gf.KIND_SOLID, 0, 0, 0, 0, solid_payload(color)))
    else:
        require_pillow()
        from PIL import Image

        for name, path in images:
            if name in SOLID_RGB565:
                payloads.append((name, gf.KIND_SOLID, 0, 0, 0, 0,
                                 solid_payload(SOLID_RGB565[name])))
                continue

            source = Image.open(path).convert("RGBA")
            if name.startswith("fg/"):
                scaled = source.resize((round(source.width * SCALE),
                                        round(source.height * SCALE)), Image.LANCZOS)
                # Only the part of a sprite that can reach the panel is worth
                # storing: character art is 219x854 and the reader shows the top
                # ~264 rows above the dialogue panel. Trimming without this crop
                # would pack the whole figure at more than twice the Flash cost.
                scaled = scaled.crop((0, 0, min(scaled.width, SCREEN_W - SPRITE_X),
                                      min(scaled.height, SCREEN_H - SPRITE_Y)))
                result = quantise_rgba(scaled, trim=True)
                if result is None:
                    raise SystemExit(f"sprite {name} is fully transparent")
                quantised, entries, has_alpha, box = result
                # The trim removed (box[0], box[1]); restore it on top of the
                # shared sprite origin so the patch lands where the source put it.
                add(name, quantised, entries, has_alpha,
                    offset=(SPRITE_X + box[0], SPRITE_Y + box[1]))
            else:
                scaled = source.resize((SCREEN_W, round(source.height * SCALE)),
                                       Image.LANCZOS)
                scaled = scaled.crop((0, 0, SCREEN_W, min(scaled.height, SCREEN_H)))
                quantised, entries, has_alpha, _ = quantise_rgba(scaled)
                add(name, quantised, entries, has_alpha, offset=(0, 0))

    # --- names
    name_blob = bytearray()
    name_offsets: dict[str, int] = {}
    for name, *_ in payloads:
        name_offsets[name] = len(name_blob)
        name_blob += name.encode("utf-8") + b"\x00"

    image_ids = {name: i for i, (name, *_) in enumerate(payloads)}
    chapter_blobs = [pack_chapter(i, scenes, image_ids, aliases, i == len(chapters) - 1)
                     for i, scenes in enumerate(chapters)]

    image_index_off = gf.HEADER_SIZE
    name_blob_off = image_index_off + gf.IMAGE_ENTRY_SIZE * len(payloads)
    # Every record the firmware casts to a struct must start 4-byte aligned, and
    # the name blob has an arbitrary length, so the chapter index is padded onto
    # the next boundary.
    script_index_off = align_up(name_blob_off + len(name_blob))
    data_off = align_up(script_index_off + gf.CHAPTER_ENTRY_SIZE * len(chapter_blobs))

    image_entries = bytearray()
    image_payloads = bytearray()
    image_offsets = []
    cursor = data_off
    for name, kind, width, height, off_x, off_y, blob in payloads:
        image_offsets.append(cursor)
        image_entries += struct.pack(gf.IMAGE_ENTRY_STRUCT, width, height, off_x,
                                     off_y, kind, 0, name_offsets[name], cursor,
                                     len(blob))
        image_payloads += blob
        cursor = align_up(cursor + len(blob))

    chapter_entries = bytearray()
    chapter_payloads = bytearray()
    for blob in chapter_blobs:
        chapter_entries += struct.pack(gf.CHAPTER_ENTRY_STRUCT, cursor, len(blob))
        chapter_payloads += blob
        cursor = align_up(cursor + len(blob))

    max_text_len = max(len(line.display_text().encode("utf-8"))
                       for scenes in chapters for scene in scenes
                       for line in scene.dialogues)

    header = struct.pack(gf.HEADER_STRUCT, gf.MAGIC, gf.VERSION, gf.HEADER_SIZE,
                         len(payloads), len(chapter_blobs), image_index_off,
                         name_blob_off, len(name_blob), script_index_off,
                         max_text_len, 0)

    image = bytearray()
    image += header
    image += image_entries
    image += name_blob
    image += b"\x00" * (script_index_off - len(image))
    image += chapter_entries
    image += b"\x00" * (data_off - len(image))
    for index, blob in enumerate(payloads):
        image += b"\x00" * (image_offsets[index] - len(image))
        image += blob[6]
    for index, blob in enumerate(chapter_blobs):
        offset = struct.unpack_from(gf.CHAPTER_ENTRY_STRUCT, chapter_entries,
                                    index * gf.CHAPTER_ENTRY_SIZE)[0]
        image += b"\x00" * (offset - len(image))
        image += blob

    if args.max_bytes and len(image) > args.max_bytes:
        # Fail the build rather than ship a truncated partition image.
        raise SystemExit(
            f"pack is {len(image)} bytes but the assets partition holds only "
            f"{args.max_bytes}; enlarge the partition in partitions.csv or trim "
            f"the source material")

    (out_dir / "gal_assets.bin").write_bytes(bytes(image))

    charset = set(PLACEHOLDER_TEXT)
    for scenes in chapters:
        for scene in scenes:
            for line in scene.dialogues:
                charset.update(line.display_text())
                charset.update(line.display_name())
    (out_dir / "gal_charset.txt").write_text("".join(sorted(charset)), encoding="utf-8")

    report = {
        "placeholder": sources.placeholder,
        "chapters": len(chapter_blobs),
        "images": len(payloads),
        "data_off": data_off,
        "total_bytes": len(image),
        "image_table_bytes": len(image_entries),
        "name_blob_bytes": len(name_blob),
        "chapter_index_bytes": len(chapter_entries),
        "image_payload_bytes": len(image_payloads),
        "chapter_payload_bytes": len(chapter_payloads),
        "max_text_len": max_text_len,
        "unique_codepoints": len(charset),
        "fallbacks_applied": fallbacks,
        "aliases": aliases,
    }
    (out_dir / "gal_assets.json").write_text(json.dumps(report, indent=2) + "\n",
                                             encoding="utf-8")

    print(f"packed {report['images']} images, {report['chapters']} chapters -> "
          f"{report['total_bytes'] / 1048576:.2f} MiB")
    print(f"  images {report['image_payload_bytes'] / 1048576:.2f} MiB   "
          f"script {report['chapter_payload_bytes'] / 1024:.0f} KiB   "
          f"charset {report['unique_codepoints']} codepoints   "
          f"max text {max_text_len} B")
    if fallbacks:
        print(f"  substituted {len(fallbacks)} missing upstream asset(s): "
              + ", ".join(f"{k}->{v}" for k, v in sorted(fallbacks.items())))

    if args.preview is not None:
        render_preview(args, chapters, payloads)
    return 0


def read_layout() -> dict[str, int]:
    """Integer geometry constants from main/gal/gal_layout.h.

    The panel and the speaker plate are styled at runtime, so the preview has to
    read the firmware's own numbers. Duplicating them here is exactly how a
    preview quietly stops matching the device.
    """
    path = pathlib.Path(__file__).resolve().parents[2] / "main" / "gal" / "gal_layout.h"
    values: dict[str, int] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("/*")[0]
        match = re.match(r"\s*#define\s+(GAL_[A-Z0-9_]+)\s+(.+?)\s*$", line)
        if not match:
            continue
        name, expression = match.group(1), match.group(2)
        for key, value in values.items():
            expression = re.sub(rf"\b{key}\b", str(value), expression)
        if re.fullmatch(r"[-+*/() 0-9]+", expression):
            try:
                values[name] = int(eval(expression))  # noqa: S307 - digits and operators only
            except (SyntaxError, ZeroDivisionError):
                pass
    required = ("GAL_PANEL_MARGIN_X", "GAL_PANEL_TOP_SMALL", "GAL_PANEL_TOP_LARGE",
                "GAL_PANEL_WIDTH", "GAL_PANEL_H_SMALL", "GAL_PANEL_H_LARGE",
                "GAL_PANEL_RULE_WIDTH", "GAL_NAME_Y_SMALL", "GAL_NAME_Y_LARGE",
                "GAL_NAME_X", "GAL_NAME_HEIGHT", "GAL_NAME_PAD_X", "GAL_NAME_PAD_Y",
                "GAL_NAME_RADIUS")
    missing = [name for name in required if name not in values]
    if missing:
        raise SystemExit(f"could not read {', '.join(missing)} from {path}")
    return values


def render_preview(args, chapters, payloads) -> None:
    """Composite one chapter's first illustrated scene at screen resolution."""
    from PIL import Image, ImageDraw

    index = max(0, min(args.preview, len(chapters) - 1))
    by_name = {name: (kind, w, h, ox, oy, blob) for name, kind, w, h, ox, oy, blob in payloads}
    scene = None
    for candidate in chapters[index]:
        if any(line.body for line in candidate.dialogues):
            scene = candidate
            break
    if scene is None:
        scene = chapters[index][0]

    canvas = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 255))

    def compose(name, offset=None):
        entry = by_name.get(name)
        if entry is None:
            return
        kind, width, height, ox, oy, blob = entry
        position = offset if offset is not None else (ox, oy)
        if kind == gf.KIND_SOLID:
            colour = struct.unpack("<H", blob[:2])[0]
            canvas.alpha_composite(Image.new("RGBA", (SCREEN_W, SCREEN_H), (
                ((colour >> 11) & 0x1F) * 255 // 31, ((colour >> 5) & 0x3F) * 255 // 63,
                (colour & 0x1F) * 255 // 31, 255)))
            return
        palette = []
        for i in range(gf.PALETTE_ENTRIES):
            b, g, r, a = blob[i * 4:i * 4 + 4]
            palette += [r, g, b, a]
        patch = Image.new("P", (width, height))
        patch.putpalette(palette, "RGBA")
        patch.frombytes(blob[gf.PALETTE_BYTES:])
        canvas.alpha_composite(patch.convert("RGBA"), position)

    if scene.background:
        compose(f"bg/{pathlib.Path(scene.background).stem}.png")
    for line in scene.dialogues:
        if line.body:
            compose(f"fg/{line.body}.png")
        if line.face:
            compose(f"fg/{line.face}.png")
    if scene.overlay:
        compose(f"bg/{pathlib.Path(scene.overlay).stem}.png")

    # The dialogue panel and the speaker plate are drawn by LVGL on the device;
    # reproduce them here from the firmware's own geometry so the preview shows
    # the composition the reader will actually present.
    layout = read_layout()
    # The panel is sized from the line height, so render the size the caller asked
    # for; four lines always fit whole in either.
    size = "LARGE" if args.preview_large else "SMALL"
    top = layout[f"GAL_PANEL_TOP_{size}"]
    panel_h = layout[f"GAL_PANEL_H_{size}"]
    name_y = layout[f"GAL_NAME_Y_{size}"]
    panel = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(panel)
    left = layout["GAL_PANEL_MARGIN_X"]
    rule = layout["GAL_PANEL_RULE_WIDTH"]
    # Opaque plate with a single accent rule along its top edge, like the Saya port.
    draw.rectangle((left, top, left + layout["GAL_PANEL_WIDTH"] - 1, top + panel_h - 1),
                   fill=(0x10, 0x13, 0x17, 255))
    draw.rectangle((left, top, left + layout["GAL_PANEL_WIDTH"] - 1, top + rule - 1),
                   fill=(0x35, 0x60, 0x4A, 255))
    # Speaker plate: half-transparent black behind the name text.
    draw.rounded_rectangle((layout["GAL_NAME_X"], name_y,
                            layout["GAL_NAME_X"] + 78,
                            name_y + layout["GAL_NAME_HEIGHT"] - 1),
                           radius=layout["GAL_NAME_RADIUS"], fill=(0, 0, 0, 128))
    canvas.alpha_composite(panel)

    out = pathlib.Path(args.preview_out)
    canvas.convert("RGB").save(out)
    print(f"preview of chapter {index + 1} written to {out}")


def main() -> int:
    parser = argparse.ArgumentParser(description="pack galgame assets")
    parser.add_argument("--source", default="assets/gal-source/common",
                        help="upstream src/common directory (may be absent)")
    parser.add_argument("--out", default="build/gal", help="output directory")
    parser.add_argument("--max-bytes", type=int, default=0,
                        help="fail if the pack exceeds this size (the flash partition)")
    parser.add_argument("--preview", type=int, default=None,
                        help="composite chapter N (1-based) for layout checks")
    parser.add_argument("--preview-large", action="store_true",
                        help="draw the panel at the 20 px text size instead of 16 px")
    parser.add_argument("--preview-out", default="preview.png")
    return build(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
