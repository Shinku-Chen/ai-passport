#!/usr/bin/env python3
"""Inspect a packed galgame asset image, and optionally export an image from it.

Useful when the firmware shows something unexpected: this reads the pack with the
same layout the firmware uses, so a mismatch points at the packer or the C code
rather than at the display path.

Usage:
    python tools/gal/inspect_pack.py build/gal/gal_assets.bin
    python tools/gal/inspect_pack.py build/gal/gal_assets.bin --export bg/bg08a.png out.png
    python tools/gal/inspect_pack.py build/gal/gal_assets.bin --chapter 3
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gal_format as gf  # noqa: E402


class Pack:
    def __init__(self, path: pathlib.Path):
        self.data = path.read_bytes()
        fields = struct.unpack(gf.HEADER_STRUCT, self.data[:gf.HEADER_SIZE])
        (self.magic, self.version, self.header_size, self.image_count,
         self.chapter_count, self.image_index_off, self.name_blob_off,
         self.name_blob_len, self.script_index_off, self.max_text_len,
         _reserved) = fields
        if self.magic != gf.MAGIC:
            raise SystemExit(f"bad magic 0x{self.magic:08X} (expected 0x{gf.MAGIC:08X})")
        if self.version != gf.VERSION:
            raise SystemExit(f"version {self.version} != {gf.VERSION}")
        if self.header_size != gf.HEADER_SIZE:
            raise SystemExit(f"header size {self.header_size} != {gf.HEADER_SIZE}")

        # The firmware casts offsets to structs; misaligned ones must never ship.
        problems = []
        for label, value in (("image_index_off", self.image_index_off),
                             ("name_blob_off", self.name_blob_off),
                             ("script_index_off", self.script_index_off)):
            if value % gf.ALIGN:
                problems.append(f"{label}={value}")
        for index in range(self.chapter_count):
            data_off = self.chapter_offset(index)
            if data_off % gf.ALIGN:
                problems.append(f"chapter[{index}].data_off={data_off}")
        self.alignment_problems = problems

    def chapter_offset(self, index: int) -> int:
        base = self.script_index_off + index * gf.CHAPTER_ENTRY_SIZE
        return struct.unpack_from(gf.CHAPTER_ENTRY_STRUCT, self.data, base)[0]

    def name(self, offset: int) -> str:
        start = self.name_blob_off + offset
        end = self.data.index(b"\x00", start)
        return self.data[start:end].decode("utf-8")

    def images(self):
        for index in range(self.image_count):
            base = self.image_index_off + index * gf.IMAGE_ENTRY_SIZE
            entry = struct.unpack(gf.IMAGE_ENTRY_STRUCT,
                                  self.data[base:base + gf.IMAGE_ENTRY_SIZE])
            width, height, off_x, off_y, kind, _r, name_off, data_off, data_len = entry
            yield index, kind, width, height, off_x, off_y, self.name(name_off), \
                data_off, data_len

    def find(self, name: str):
        for entry in self.images():
            if entry[6] == name:
                return entry
        return None

    def chapter(self, index: int):
        base = self.script_index_off + index * gf.CHAPTER_ENTRY_SIZE
        data_off, data_len = struct.unpack(gf.CHAPTER_ENTRY_STRUCT,
                                           self.data[base:base + gf.CHAPTER_ENTRY_SIZE])
        blob = self.data[data_off:data_off + data_len]
        fields = struct.unpack(gf.CHAPTER_HEADER_STRUCT, blob[:gf.CHAPTER_HEADER_SIZE])
        (magic, scene_count, dialogue_count, scenes_off, dialogues_off, text_off,
         text_len, names_off, names_len, chapter_index, flags, _r) = fields
        if magic != gf.CHAPTER_MAGIC:
            raise SystemExit(f"chapter {index} bad magic 0x{magic:08X}")
        scenes = []
        for i in range(scene_count):
            base_i = scenes_off + i * gf.SCENE_REC_SIZE
            scenes.append(struct.unpack(gf.SCENE_REC_STRUCT,
                                        blob[base_i:base_i + gf.SCENE_REC_SIZE]))
        dialogues = []
        for i in range(dialogue_count):
            base_i = dialogues_off + i * gf.DLG_REC_SIZE
            dialogues.append(struct.unpack(gf.DLG_REC_STRUCT,
                                          blob[base_i:base_i + gf.DLG_REC_SIZE]))
        names = blob[names_off:names_off + names_len]
        text = blob[text_off:text_off + text_len]
        return dict(chapter_index=chapter_index, flags=flags, scenes=scenes,
                    dialogues=dialogues, names=names, text=text, blob=blob)

    def string(self, pool: bytes, offset: int) -> str:
        if offset >= len(pool):
            return f"<out of range {offset}/{len(pool)}>"
        end = pool.find(b"\x00", offset)
        if end < 0:
            return f"<unterminated at {offset}>"
        return pool[offset:end].decode("utf-8", errors="replace")

    def export(self, name: str, out: pathlib.Path) -> None:
        """Write one packed image back out as a PNG."""
        from PIL import Image

        entry = self.find(name)
        if entry is None:
            raise SystemExit(f"image {name!r} not in the pack")
        _index, kind, width, height, off_x, off_y, _n, data_off, data_len = entry
        if kind == gf.KIND_SOLID:
            colour = struct.unpack("<H", self.data[data_off:data_off + 2])[0]
            Image.new("RGB", (width or 8, height or 8), (
                ((colour >> 11) & 0x1F) * 255 // 31, ((colour >> 5) & 0x3F) * 255 // 63,
                (colour & 0x1F) * 255 // 31)).save(out)
            print(f"{name}: solid RGB565 0x{colour:04X} -> {out}")
            return
        blob = self.data[data_off:data_off + data_len]
        palette = []
        for i in range(gf.PALETTE_ENTRIES):
            b, g, r, a = blob[i * 4:i * 4 + 4]
            palette += [r, g, b, a]
        image = Image.new("P", (width, height))
        image.putpalette(palette, "RGBA")
        image.frombytes(blob[gf.PALETTE_BYTES:])
        image.convert("RGBA").save(out)
        print(f"{name}: {width}x{height} offset=({off_x},{off_y}) -> {out}")


def main() -> int:
    parser = argparse.ArgumentParser(description="inspect a galgame asset pack")
    parser.add_argument("pack")
    parser.add_argument("--export", nargs=2, metavar=("NAME", "PNG"))
    parser.add_argument("--chapter", type=int, help="dump chapter N (1-based)")
    parser.add_argument("--lines", type=int, default=6, help="lines to show per chapter")
    args = parser.parse_args()

    pack = Pack(pathlib.Path(args.pack))
    total = len(pack.data)
    print(f"pack {args.pack}: {total} bytes, {pack.image_count} images, "
          f"{pack.chapter_count} chapters, max text {pack.max_text_len} B")
    if pack.alignment_problems:
        print("  ALIGNMENT PROBLEMS: " + ", ".join(pack.alignment_problems))
        return 1
    print("  alignment: all struct offsets are 4-byte aligned")

    kinds: dict[int, int] = {}
    payload = 0
    for entry in pack.images():
        kinds[entry[1]] = kinds.get(entry[1], 0) + 1
        payload += entry[8]
    names = {gf.KIND_ALPHA: "alpha", gf.KIND_OPAQUE: "opaque", gf.KIND_SOLID: "solid"}
    print("  kinds: " + ", ".join(f"{names.get(k, k)}={v}" for k, v in sorted(kinds.items())))
    print(f"  image payload {payload} bytes ({payload / 1048576:.2f} MiB)")

    if args.export:
        pack.export(args.export[0], pathlib.Path(args.export[1]))
        return 0

    if args.chapter is not None:
        index = max(1, min(args.chapter, pack.chapter_count))
        chapter = pack.chapter(index - 1)
        print(f"chapter {index}: {len(chapter['scenes'])} scenes, "
              f"{len(chapter['dialogues'])} dialogues, flags 0x{chapter['flags']:04X}")
        for scene_i, scene in enumerate(chapter["scenes"]):
            background, overlay, ox, oy, first, count, _r = scene
            print(f"  scene {scene_i}: bg={background} overlay={overlay} "
                  f"({ox},{oy}) dialogues=[{first},{first + count})")
            for line_i in range(first, min(first + count, first + args.lines)):
                text_off, text_len, name_off, name_len, body, face, flags = \
                    chapter["dialogues"][line_i]
                speaker = pack.string(chapter["names"], name_off) if name_len else ""
                text = pack.string(chapter["text"], text_off)
                end = " [END]" if flags & gf.DLG_FLAG_END else ""
                print(f"    {line_i}: {speaker!r} body={body} face={face}{end} {text!r}")
            if count > args.lines:
                print(f"    ... {count - args.lines} more")
        return 0

    for entry in pack.images():
        index, kind, width, height, off_x, off_y, name, data_off, data_len = entry
        print(f"  [{index:3d}] {names.get(kind, kind):6s} {width:3d}x{height:<3d} "
              f"offset=({off_x:4d},{off_y:4d}) len={data_len:7d} {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
