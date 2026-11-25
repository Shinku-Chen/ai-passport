#!/usr/bin/env python3
"""Independent source-to-pack audit for the 9-nine- sorairo reader.

This deliberately re-parses the binary pack instead of importing the packer. It
reconstructs every frame from the pinned graph/chunks and checks text, speaker,
visual layers, choices, conditions, effects, chapters, endings, gallery, media
and the source digest. Run:

    python tools/nine_audit.py --source <checkout> --pack assets/nine \
        --expect-commit 6a8907a5ae576f4377abe4b74f15bd9af13efe07
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
import zlib
from pathlib import Path

NONE = 0xFFFFFFFF
ART = (240, 135)
TITLE_CROP = (0, 80, 432, 323)
TITLE_SIZE = (240, 135)
STAGE = (432, 243)
STRIDES = (48, 24, 16, 8, 24, 8, 12, 12, 24, 12, 12, 12)
COND = {"==": 1, "!=": 2, "<": 3, "<=": 4, ">": 5, ">=": 6}


def fail(message: str) -> "None":
    raise AssertionError(message)


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


class Pack:
    def __init__(self, data: bytes, path: str) -> None:
        self.data = data
        self.path = path
        if len(data) < 256 or data[:8] != b"NINESC1\0":
            fail("script magic")
        self.version = u32(data, 8)
        self.size = u32(data, 12)
        self.crc = u32(data, 16)
        if self.version != 2 or self.size != len(data):
            fail("script version/size")
        zeroed = data[:16] + b"\0\0\0\0" + data[20:]
        if zlib.crc32(zeroed) & NONE != self.crc:
            fail("script CRC")
        self.tables = []
        for i in range(12):
            count = u32(data, 36 + i * 8)
            offset = u32(data, 40 + i * 8)
            self.tables.append((count, offset, STRIDES[i]))
        self.strings_offset = u32(data, 132)
        self.strings_size = u32(data, 136)
        self.episode = self.string(u32(data, 20))
        self.title = self.string(u32(data, 24))
        self.source_commit = self.string(u32(data, 28))
        self.start = u32(data, 32)
        self.source_digest = data[140:172].hex()
        if self.strings_offset + self.strings_size > len(data):
            fail("string table outside pack")
        self.counts = [t[0] for t in self.tables]

    def string(self, offset: int) -> str:
        start = self.strings_offset + offset
        end = self.data.index(0, start)
        return self.data[start:end].decode("utf-8")

    def row(self, table: int, index: int) -> bytes:
        count, offset, stride = self.tables[table]
        if index >= count:
            fail(f"table {table} index {index}")
        return self.data[offset + index * stride:offset + (index + 1) * stride]

    def node(self, index: int):
        r = self.row(0, index)
        return dict(first_frame=u32(r, 0), frame_count=u32(r, 4),
                    first_effect=u32(r, 8), effect_count=u32(r, 12),
                    first_next=u32(r, 16), next_count=u32(r, 20),
                    first_choice=u32(r, 24), choice_count=u32(r, 28),
                    title=u32(r, 32), chapter=u32(r, 36),
                    name=u32(r, 40), script=u32(r, 44))

    def frame(self, index: int):
        r = self.row(1, index)
        return dict(kind=u32(r, 0), visual=u32(r, 4), block=u32(r, 8),
                    offset=u32(r, 12), length=u32(r, 16), speaker=u32(r, 20))

    def block(self, index: int):
        r = self.row(2, index)
        return dict(offset=u32(r, 0), compressed=u32(r, 4), raw=u32(r, 8),
                    crc=u32(r, 12))

    def text_block(self, index: int) -> bytes:
        b = self.block(index)
        raw = zlib.decompress(self.data[b["offset"]:b["offset"] + b["compressed"]])
        if len(raw) != b["raw"] or zlib.crc32(raw) & NONE != b["crc"]:
            fail(f"text block {index} CRC")
        return raw

    def visual(self, index: int):
        r = self.row(3, index)
        return u32(r, 0), u32(r, 4)

    def layer(self, index: int):
        r = self.row(4, index)
        return dict(image=u32(r, 0), x=i32(r, 4), y=i32(r, 8), w=u32(r, 12),
                    h=u32(r, 16), opacity=u32(r, 20))

    def var(self, index: int):
        r = self.row(5, index)
        return self.string(u32(r, 0)), u32(r, 4)

    def cond(self, index: int):
        r = self.row(6, index)
        return u32(r, 0), u32(r, 4), i32(r, 8)

    def effect(self, index: int):
        r = self.row(7, index)
        return u32(r, 0), u32(r, 4), i32(r, 8)

    def edge(self, index: int):
        r = self.row(8, index)
        return dict(to=u32(r, 0), first_cond=u32(r, 4), cond_count=u32(r, 8),
                    first_effect=u32(r, 12), effect_count=u32(r, 16),
                    label=u32(r, 20))

    def chapter(self, index: int):
        r = self.row(9, index)
        return u32(r, 0), u32(r, 4), u32(r, 8)

    def ending(self, index: int):
        r = self.row(10, index)
        return u32(r, 0), u32(r, 4), u32(r, 8)

    def gallery(self, index: int):
        r = self.row(11, index)
        return u32(r, 0), u32(r, 4), u32(r, 8)


class Images:
    def __init__(self, data: bytes, script: Pack) -> None:
        self.data = data
        if len(data) < 128 or data[:8] != b"NINEIM1\0":
            fail("image magic")
        if u32(data, 8) != 2 or u32(data, 12) != len(data):
            fail("image version/size")
        zeroed = data[:16] + b"\0\0\0\0" + data[20:]
        if zlib.crc32(zeroed) & NONE != u32(data, 16):
            fail("image CRC")
        self.count = u32(data, 20)
        self.table = u32(data, 24)
        self.op_count = u32(data, 28)
        self.op_table = u32(data, 32)
        self.audio_count = u32(data, 36)
        self.audio_table = u32(data, 40)
        self.title_images = [u32(data, 44 + i * 4) for i in range(4)]
        self.records = []
        self.by_tag = {}
        for i in range(self.count):
            r = data[self.table + i * 32:self.table + (i + 1) * 32]
            kind, w, h, off, length, crc, tag, _ = struct.unpack("<8I", r)
            if off + length > len(data) or not w or not h:
                fail(f"image {i} bounds")
            payload_crc = self.verify(kind, data[off:off + length], off)
            if payload_crc != crc:
                fail(f"image {i} payload CRC")
            self.records.append(dict(kind=kind, w=w, h=h, off=off, length=length,
                                     crc=crc, tag=tag))
            if tag in self.by_tag:
                fail(f"duplicate image tag {tag:08x}")
            self.by_tag[tag] = i

    @staticmethod
    def verify(kind: int, payload: bytes, base: int) -> int:
        if kind == 0:
            if payload[:2] != b"\xff\xd8" or payload[-2:] != b"\xff\xd9":
                fail("JPEG markers")
            return zlib.crc32(payload) & NONE
        if kind != 1:
            fail("unknown image kind")
        palette_count, rows, blocks = struct.unpack_from("<III", payload, 0)
        if rows != 8 or not palette_count or palette_count > 256:
            fail("indexed header")
        table_size = 12 + palette_count * 4 + blocks * 8
        cursor = table_size
        raw = bytearray()
        for b in range(blocks):
            off, length = struct.unpack_from("<II", payload, 12 + palette_count * 4 + b * 8)
            rel = off - base
            if rel < table_size or rel + length > len(payload):
                fail("indexed block bounds")
            chunk = zlib.decompress(payload[rel:rel + length])
            raw.extend(chunk)
        return zlib.crc32(raw) & NONE

    def op(self, index: int):
        r = self.data[self.op_table + index * 12:self.op_table + (index + 1) * 12]
        return struct.unpack("<3I", r)

    def audio(self, index: int):
        r = self.data[self.audio_table + index * 32:self.audio_table + (index + 1) * 32]
        kind, name, off, length, duration, rate, channels, crc = struct.unpack("<8I", r)
        payload = self.data[off:off + length]
        if zlib.crc32(payload) & NONE != crc:
            fail("audio CRC")
        return dict(kind=kind, name=self.script_string(name), off=off, length=length,
                    duration=duration, rate=rate, channels=channels)

    def script_string(self, offset: int) -> str:
        raise RuntimeError("set after construction")


def crc32(text: str) -> int:
    return zlib.crc32(text.encode()) & NONE


def tag_for(rel: str, crop, sprite: bool, size, opaque: bool) -> int:
    return crc32(f"{rel}|{crop}|{sprite}|{size}|{opaque}")


def load_graph(source: Path):
    text = (source / "src/common/graph.js").read_text("utf-8").strip()
    prefix = "export const GRAPH = "
    assert text.startswith(prefix)
    text = text[len(prefix):].strip()
    if text.endswith(";"):
        text = text[:-1]
    return json.loads(text)


def rpn(when, variables: dict[str, int]):
    if not when:
        return []
    if when[0] == "all":
        out = []
        for item in when[1:]:
            out.extend(rpn(item, variables))
        out.append((7, 0, len(when) - 1))
        return out
    return [(COND[when[1]], variables[when[0]], int(when[2]))]


def expected_layers(background: str, sprites: list, image_tags: dict):
    out = []
    if background:
        out.append((tag_for(background.lstrip("/"), None, False, ART, False),
                    0, 0, ART[0], ART[1], 255))
    for sprite in sprites:
        if isinstance(sprite, list):
            src, width, left = sprite[:3]
            opacity = sprite[3] if len(sprite) > 3 and sprite[3] is not None else 1
            patch = sprite[4] if len(sprite) > 4 else None
            height = sprite[13] if len(sprite) > 13 and sprite[13] else 243
            top = 4 if height >= 200 else 243 - height
            item = dict(src=src, width=width, left=left, height=height, top=top,
                        opacity=opacity)
            if patch:
                for name, value in zip(
                        ("patch", "patchWidth", "patchHeight", "patchLeft", "patchTop",
                         "patchAtlasWidth", "patchAtlasHeight", "patchImageLeft",
                         "patchImageTop"), sprite[4:13]):
                    item[name] = value
        else:
            item = sprite
        dw = max(1, round(item["width"] * ART[0] / STAGE[0]))
        dh = max(1, round(item.get("height", 243) * ART[1] / STAGE[1]))
        dx = round(item["left"] * ART[0] / STAGE[0])
        dy = round(item.get("top", 4) * ART[1] / STAGE[1])
        alpha = round(item.get("opacity", 1) * 255)
        out.append((tag_for(item["src"].lstrip("/"), None, True, (dw, dh), False),
                    dx, dy, dw, dh, alpha))
        if item.get("patch"):
            ax = item["patchAtlasWidth"]
            ay = item["patchAtlasHeight"]
            px = -item["patchImageLeft"]
            py = -item["patchImageTop"]
            pw = item["patchWidth"]
            ph = item["patchHeight"]
            crop = (round(px * 256 / ax), round(py * 256 / ay),
                    round((px + pw) * 256 / ax), round((py + ph) * 256 / ay))
            ps = (max(1, round(pw * ART[0] / STAGE[0])),
                  max(1, round(ph * ART[1] / STAGE[1])))
            out.append((tag_for(item["patch"].lstrip("/"), crop, True, ps, False),
                        round(item["patchLeft"] * ART[0] / STAGE[0]),
                        round(item["patchTop"] * ART[1] / STAGE[1]), *ps, alpha))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--pack", type=Path, required=True)
    parser.add_argument("--expect-commit", default="")
    args = parser.parse_args()
    source = args.source / "nine-sorairo-rw5" if (args.source / "nine-sorairo-rw5").is_dir() else args.source
    script = (args.pack / "nine_script.bin").read_bytes()
    pack = Pack(script, str(args.pack / "nine_script.bin"))
    images = Images((args.pack / "nine_images.bin").read_bytes(), pack)
    images.script_string = lambda offset: pack.string(offset)
    graph = load_graph(source)
    converted = json.loads((source / "converted/graph.json").read_text("utf-8"))
    if args.expect_commit:
        import subprocess
        commit = subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip()
        if commit != args.expect_commit:
            fail(f"source commit {commit} != {args.expect_commit}")
        if pack.source_commit != commit:
            fail("pack source commit")

    # Variables: every effect/condition key plus the persistent key names.
    variables = {}
    for i in range(pack.counts[5]):
        name, flags = pack.var(i)
        variables[name] = i
    if len(variables) != 15:
        fail(f"variable count {len(variables)}")
    persistent = {"loop1", "clear_true", "titleBg", "loop_my", "loop_sr", "cf_sr",
                  "loop_hk", "cf_hk", "loop_na", "cf_na", "true"}
    for name in persistent:
        if name not in variables:
            fail(f"missing persistent key {name}")
    for i in range(pack.counts[5]):
        name, flags = pack.var(i)
        if bool(flags & 1) != (name in persistent):
            fail(f"persistent flag mismatch {name}")

    # Nodes/frames/text/visuals.
    nodes = list(graph["nodes"])
    if pack.counts[0] != len(nodes) or pack.start != 0:
        fail("node count/start")
    chapter_names = {c.get("title", c.get("id", "")): i
                     for i, c in enumerate(graph["chapters"])}
    total_text = 0
    total_frames = 0
    visual_ids = set()
    for nindex, node_name in enumerate(nodes):
        node = pack.node(nindex)
        src = graph["nodes"][node_name]
        if pack.string(node["name"]) != node_name:
            fail(f"node name {node_name}")
        if node["frame_count"] != src["count"]:
            fail(f"{node_name} frame count")
        if node["chapter"] != chapter_names.get(src["chapter"], NONE):
            fail(f"{node_name} chapter")
        if pack.string(node["title"]) != src["title"]:
            fail(f"{node_name} title")
        if node["choice_count"] != len(src.get("choices", [])):
            fail(f"{node_name} choice count")
        if node["next_count"] != len(src.get("nexts", [])):
            fail(f"{node_name} next count")
        # expected node effects: source effects + complete-ending effects
        expected_effects = [(0 if op == "=" else 1, variables[k], int(v))
                            for k, op, v in src.get("effects", [])]
        for end in src.get("complete", []):
            expected_effects.append((0, variables[end], 1))
            if end == "true":
                expected_effects.append((0, variables["clear_true"], 1))
        actual_effects = [pack.effect(node["first_effect"] + i)
                          for i in range(node["effect_count"])]
        if actual_effects != expected_effects:
            fail(f"{node_name} effects")
        # nexts
        for i, next_src in enumerate(src.get("nexts", [])):
            edge = pack.edge(node["first_next"] + i)
            target = NONE if next_src["to"] == "@end" else nodes.index(next_src["to"])
            if edge["to"] != target:
                fail(f"{node_name} next target {i}")
            actual = [pack.cond(edge["first_cond"] + j) for j in range(edge["cond_count"])]
            if actual != rpn(next_src.get("when"), variables):
                fail(f"{node_name} next condition {i}")
            if edge["effect_count"]:
                fail(f"{node_name} next effects")
            if edge["label"] != 0:
                fail(f"{node_name} next label")
        # choices
        for i, choice_src in enumerate(src.get("choices", [])):
            edge = pack.edge(node["first_choice"] + i)
            if edge["to"] != nodes.index(choice_src["to"]):
                fail(f"{node_name} choice target {i}")
            if pack.string(edge["label"]) != choice_src["text"]:
                fail(f"{node_name} choice label {i}")
            actual = [pack.cond(edge["first_cond"] + j) for j in range(edge["cond_count"])]
            if actual != rpn(choice_src.get("when"), variables):
                fail(f"{node_name} choice condition {i}")
            expected = [(0 if op == "=" else 1, variables[k], int(v))
                        for k, op, v in choice_src.get("effects", [])]
            actual = [pack.effect(edge["first_effect"] + j) for j in range(edge["effect_count"])]
            if actual != expected:
                fail(f"{node_name} choice effects {i}")
        # frames and chunk text
        frame_cursor = node["first_frame"]
        for chunk in src.get("chunks", []):
            rows = json.loads((source / "src/common/story" / chunk["file"]).read_text("utf-8"))
            background = ""
            sprites = []
            seen_art = False
            for row in rows:
                frame = pack.frame(frame_cursor)
                if row.get("kind") == "op":
                    if frame["kind"] != 1 or frame["visual"] != NONE:
                        fail(f"{node_name} OP frame")
                    total_frames += 1
                    frame_cursor += 1
                    continue
                if "art" in row:
                    background = row["art"].get("background", "")
                    sprites = row["art"].get("sprites") or []
                    seen_art = True
                    speaker = row.get("speaker", "")
                    line = row.get("text", "")
                else:
                    if not seen_art and ("b" in row or "p" in row):
                        seen_art = True
                    background = row.get("b", background)
                    sprites = row.get("p", sprites)
                    speaker = row.get("s", "")
                    line = row.get("t", "")
                if frame["kind"] != 0:
                    fail(f"{node_name} frame kind")
                block = pack.text_block(frame["block"])
                text = block[frame["offset"]:frame["offset"] + frame["length"]].decode("utf-8")
                if text != line or block[frame["offset"] + frame["length"]] != 0:
                    fail(f"{node_name} text at {frame_cursor}")
                if pack.string(frame["speaker"]) != speaker:
                    fail(f"{node_name} speaker at {frame_cursor}")
                visual_ids.add(frame["visual"])
                first, count = pack.visual(frame["visual"])
                actual = []
                for j in range(count):
                    layer = pack.layer(first + j)
                    actual.append((images.records[layer["image"]]["tag"], layer["x"], layer["y"],
                                   layer["w"], layer["h"], layer["opacity"]))
                expected = expected_layers(background, sprites, images)
                if actual != expected:
                    fail(f"{node_name} visual at {frame_cursor}")
                total_text += 1
                total_frames += 1
                frame_cursor += 1
        if frame_cursor != node["first_frame"] + node["frame_count"]:
            fail(f"{node_name} frame table")
    if total_frames != 10386 or total_text != 10385:
        fail(f"frame totals {total_frames}/{total_text}")
    if len(visual_ids) != 2119:
        fail(f"visual count {len(visual_ids)}")
    if pack.counts[4] != 8143:
        fail("layer count")

    # Chapters / endings / gallery.
    if pack.counts[9] != len(graph["chapters"]):
        fail("chapter count")
    for i, chapter in enumerate(graph["chapters"]):
        title, node, preview = pack.chapter(i)
        if pack.string(title) != chapter.get("title", "") or node != nodes.index(chapter["node"]):
            fail(f"chapter {i}")
        if pack.string(preview) != chapter.get("preview", ""):
            fail(f"chapter preview {i}")
    if pack.counts[10] != 2:
        fail("ending count")
    for i, ending in enumerate(graph["endings"]):
        name, key, desc = pack.ending(i)
        if pack.string(name) != ending["name"] or key != variables[ending["id"]]:
            fail(f"ending {i}")
        if pack.string(desc) != ending["description"]:
            fail(f"ending description {i}")
    groups = json.loads((source / "converted/gallery.json").read_text("utf-8"))
    expected_variants = [(v["name"], tag_for(v["src"].lstrip("/"), None, False, ART, False), g)
                         for g, group in enumerate(groups) for v in group["variants"]]
    if pack.counts[11] != len(expected_variants):
        fail("gallery count")
    for i, (name, tag, group) in enumerate(expected_variants):
        actual_name, image, actual_group = pack.gallery(i)
        if pack.string(actual_name) != name or actual_group != group:
            fail(f"gallery record {i}")
        if images.records[image]["tag"] != tag:
            fail(f"gallery image {i}")

    # Image/audio/opening/title package.
    if images.count != 853:
        fail("image count")
    title_tags = [tag_for(f"/common/ui/title-bg{i}.jpg".lstrip("/"), TITLE_CROP,
                          False, TITLE_SIZE, True) for i in (1, 2, 3)]
    for i, tag in enumerate(title_tags):
        if images.records[images.title_images[i]]["tag"] != tag:
            fail(f"title bg {i}")
    if images.records[images.title_images[3]]["tag"] != tag_for(
            "common/ui/title-logo.png", None, True, (51, 135), False):
        fail("title logo")
    timeline = json.loads((source / "converted/opening/timeline.json").read_text("utf-8"))
    picked = []
    seen = set()
    fps = 3
    for i in range(math.ceil(timeline["durationMs"] * fps / 1000)):
        index = min(len(timeline["frames"]) - 1, round(i * timeline["fps"] / fps))
        if index not in seen:
            picked.append(timeline["frames"][index])
            seen.add(index)
    if picked[-1] is not timeline["frames"][-1]:
        picked.append(timeline["frames"][-1])
    if images.op_count != len(picked) or len(picked) != 374:
        fail("opening frame count")
    last = 0
    for i, frame_src in enumerate(picked):
        image, timestamp, duration = images.op(i)
        end = picked[i + 1]["atMs"] if i + 1 < len(picked) else timeline["durationMs"]
        if timestamp != frame_src["atMs"] or duration != end - frame_src["atMs"]:
            fail(f"opening timing {i}")
        if images.records[image]["tag"] != tag_for(frame_src["src"].lstrip("/"), None, False, ART, False):
            fail(f"opening image {i}")
        last = timestamp + duration
    if last != timeline["durationMs"]:
        fail("opening duration")
    if images.audio_count != 2:
        fail("audio count")
    expected_audio = [("title", 85417), ("opening", 124680)]
    for i, (name, duration) in enumerate(expected_audio):
        audio = images.audio(i)
        if audio["name"] != name or audio["rate"] != 12000 or audio["channels"] != 1:
            fail(f"audio {i}")
        if abs(audio["duration"] - duration) > 1000:
            fail(f"audio duration {i}")
        if audio["length"] % 132:
            fail(f"audio block alignment {i}")

    # Source digest over the exact tree used by the packer.
    digest = hashlib.sha256()
    for directory in ("src/common", "converted"):
        for path in sorted((source / directory).rglob("*")):
            if path.is_file():
                rel = path.relative_to(source).as_posix()
                payload = path.read_bytes()
                digest.update(rel.encode() + b"\0" + struct.pack("<Q", len(payload)) + payload)
    if digest.hexdigest() != pack.source_digest:
        fail(f"source digest {digest.hexdigest()} != {pack.source_digest}")

    # Text symbols must cover every source frame/speaker and metadata string.
    symbols = set((args.pack / "nine_symbols.txt").read_text("utf-8"))
    needed = set()
    for node_name in nodes:
        src = graph["nodes"][node_name]
        needed.update(src["title"])
        needed.update(src["chapter"])
        for chunk in src.get("chunks", []):
            for row in json.loads((source / "src/common/story" / chunk["file"]).read_text("utf-8")):
                needed.update(row.get("t", ""))
                needed.update(row.get("s", ""))
    for chapter in graph["chapters"]:
        needed.update(chapter.get("title", ""))
        needed.update(chapter.get("preview", ""))
    for ending in graph["endings"]:
        needed.update(ending["name"] + ending["description"])
    for group in groups:
        for variant in group["variants"]:
            needed.update(variant["name"])
    needed -= {chr(13), chr(10), chr(9)}
    missing = needed - symbols
    if missing:
        fail(f"font symbols missing {len(missing)}: {sorted(missing)[:8]}")

    print("nine source/pack audit: PASS")
    print(f"  nodes={len(nodes)} frames={total_frames} text={total_text} "
          f"visuals={len(visual_ids)} layers={pack.counts[4]}")
    print(f"  chapters={pack.counts[9]} endings={pack.counts[10]} "
          f"choices={sum(len(n.get('choices', [])) for n in graph['nodes'].values())} "
          f"variables={pack.counts[5]}")
    print(f"  gallery={pack.counts[11]}/{len(groups)} images={images.count} "
          f"opening={images.op_count}/{len(timeline['frames'])} audio={images.audio_count}")
    print(f"  source={pack.source_commit} digest={pack.source_digest[:16]}… "
          f"script={len(script)} image={len(images.data)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
