#!/usr/bin/env python3
"""Independent source audit for the Steins;Gate 0 (RW5) reader.

This tool deliberately does NOT import the packer.  It parses the RW5 source
snapshot (graph.txt + story chunks + assets.js + gallery.txt + phone data) with
its own code, then optionally re-parses the generated pack with a second,
independent reader and compares every dialogue op, name, choice target,
condition, entry, ending, image mapping and gallery entry.

Usage:
  python tools/steins0_audit.py <steins0-rw5 checkout> [--pack assets/steins0/pack.bin]
      [--json build/steins0_source_audit.json] [--dump build/steins0_ops.jsonl]

Exit code 0 = every check passed; 1 = at least one mismatch; 2 = bad input.
Only the checkout is read; outputs go below the current repository.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib
from pathlib import Path

# ---------------------------------------------------------------------------
# Pack format v1 (documented independently of tools/steins0_pack.py).
#
# header 20 B: magic[8] "SG0PK001", version u16, header_size u16,
#              section_count u16, reserved u16, total_size u32
# section 16 B: type u32, offset u32, count u32, size u32
#   0 TEXT    named strings, addressed by STRT
#   1 STRT    {off u32, len u16, pad u16} = 8 B
#   2 NODE    {chunk_first u16, chunk_count u16, choice u16, condition u16,
#              ending i8, flags u8, break_first u16, break_count u8,
#              chapter_first u16, chapter_count u8, event_first u16,
#              event_count u16, start_first u16, start_count u8, name u16} = 24 B
#   3 CHUNK   {zoff u32, zlen u32, raw_len u16, frames u8, pad u8} = 12 B
#   4 FRAME   {text_off u16, text_len u16, view u16, speaker u8, group u8,
#              date u16, flags u8, pad u8} = 12 B
#   5 VIEW    {layer_first u16, layer_count u8, pad u8} = 4 B
#   6 LAYER   {asset u16, x i16, y i16, w u16, h u16, opacity u16} = 12 B
#   7 CHOICE  {kind u8, critical u8, heading u16, category u16, count u8,
#              pad u8, to[2] u8, action[2] u8, text[2] u16} = 18 B
#   8 COND    {flag u8, yes u8, no u8, pad u8} = 4 B
#   9 ENDING  {name u16, ending_id u8, pad u8} = 4 B
#  10 ENDLIST u16 string index per clear-list entry
#  11 BREAK   u16 node-local cursor
#  12 CHGRP   {cursor u16, group u8, pad u8} = 4 B
#  13 EVENT   {at u16, type u8, flags u8, a u16, b u16, c u16, d u16} = 12 B
#  14 START   {node u8, group u8, script u16, index u16, date u16, name u16,
#              pad u16} = 12 B
#  15 ASSETID {part_first u16, part_count u8, kind u8, name u16, pad u16} = 8 B
#  16 PART    {off u32, len u32, mask_off u32, mask_len u32, w u16, h u16,
#              ax i16, ay i16, ow u16, oh u16, flags u8, pad u8} = 30 B
#  17 BLOB    image/mask payloads, 4-byte aligned
#  18 GG      {label u16, first u16, count u8, pad u8} = 8 B
#  19 GE      {asset u16, pad u16} = 4 B
#  20 META    key=value UTF-8 text
#  21 SPK     u16 string index per speaker slot (FRAME.speaker indexes this)
#  22 DATE    u32 source story date per slot (FRAME.date indexes this)
#  23 LAYERREF u16 layer index per view layer slot (VIEW.ref_first + index)
#  24 TITLE   u16 string index per chapter group (node.chapters/group)
#  25 RMSG ... 31 CARD: RINE catalog/replies/contacts/bases and chapter cards
# ---------------------------------------------------------------------------

PACK_MAGIC = b"SG0PK001"
SEC_TEXT, SEC_STRT, SEC_NODE, SEC_CHUNK, SEC_FRAME, SEC_VIEW, SEC_LAYER = range(7)
SEC_CHOICE, SEC_COND, SEC_ENDING, SEC_ENDLIST, SEC_BREAK, SEC_CHGRP = range(7, 13)
SEC_EVENT, SEC_START, SEC_ASSETID, SEC_PART, SEC_BLOB, SEC_GG, SEC_GE = range(13, 20)
SEC_META = 20
SEC_SPK = 21
SEC_DATE = 22
SEC_LAYERREF = 23
SEC_TITLE = 24
SEC_RMSG = 25
SEC_RREPLY = 26
SEC_RCHOICE = 27
SEC_RSEQ = 28
SEC_RCONTACT = 29
SEC_RBASE = 30
SEC_CARD = 31
SECTION_COUNT = 32
U16 = struct.Struct("<H")
I16 = struct.Struct("<h")
U32 = struct.Struct("<I")


def die(msg: str, code: int = 2):
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(code)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def u16(b, off):
    return U16.unpack_from(b, off)[0]


def i16(b, off):
    return I16.unpack_from(b, off)[0]


def u32(b, off):
    return U32.unpack_from(b, off)[0]


# ---------------------------------------------------------------------------
# Source parsing
# ---------------------------------------------------------------------------

def parse_js_object(text: str, marker: str) -> dict:
    """Extract the object literal that starts right after `marker`."""
    start = text.index(marker) + len(marker)
    while text[start] != "{":
        start += 1
    depth = 0
    instr = False
    esc = False
    for pos in range(start, len(text)):
        ch = text[pos]
        if instr:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                instr = False
            continue
        if ch == '"':
            instr = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return json.loads(text[start:pos + 1])
    die(f"unbalanced object after {marker}")


class Source:
    def __init__(self, root: Path):
        self.root = root
        self.common = root / "src" / "common"
        self.graph_blob = (self.common / "graph.txt").read_bytes()
        self.graph = json.loads(self.graph_blob.decode("utf-8"))
        assets_js = (self.common / "assets.js").read_bytes().decode("utf-8")
        self.assets = parse_js_object(assets_js, "const assets=")
        self.parts = parse_js_object(assets_js, "const parts=")
        self.gallery = json.loads((self.common / "gallery.txt").read_bytes().decode("utf-8"))
        self.glyph_widths, self.ascii_widths, self.unicode_widths = parse_widths(
            (self.common / "glyph-widths.js").read_bytes().decode("utf-8"))
        self.ttf = (self.common / "sg-original.ttf").read_bytes()
        self.cmap = ttf_union_cmap(self.ttf)
        self.phone = self.graph.get("phone", {})
        self.chunks: dict[str, dict] = {}
        self.node_chunks: dict[str, list[str]] = {}

    def chunk(self, rel: str) -> dict:
        d = self.chunks.get(rel)
        if d is None:
            raw = (self.common / "story" / rel).read_bytes()
            d = json.loads(raw.decode("utf-8"))
            self.chunks[rel] = d
        return d

    def node_order(self):
        starts = self.graph["starts"]
        order = []
        seen = set()

        def visit(node_id):
            if node_id in seen:
                return
            seen.add(node_id)
            order.append(node_id)
            node = self.graph["nodes"][node_id]
            if node.get("choice"):
                for item in node["choice"]["items"]:
                    visit(item["to"])
            if node.get("condition"):
                visit(node["condition"]["yes"])
                visit(node["condition"]["no"])

        visit(self.graph["initial"])
        for key in starts:
            visit(starts[key]["node"])
        for node_id in self.graph["nodes"]:
            visit(node_id)
        return order


def parse_widths(text: str):
    def parse(name):
        marker = f"export const {name}="
        start = text.index(marker) + len(marker)
        end = text.index("};", start) + 1
        return json.loads(text[start:end])

    return parse("glyphWidths"), parse("asciiWidths"), parse("unicodeWidths")


def ttf_union_cmap(data: bytes) -> set[int]:
    """Minimal TTF/OTF cmap reader (formats 4 and 12), no fontTools."""
    be16 = lambda off: struct.unpack_from(">H", data, off)[0]
    be32 = lambda off: struct.unpack_from(">I", data, off)[0]
    num_tables = be16(4)
    cmap_off = None
    for i in range(num_tables):
        rec = 12 + i * 16
        tag = data[rec:rec + 4]
        if tag == b"cmap":
            cmap_off = be32(rec + 8)
            break
    if cmap_off is None:
        die("sg-original.ttf has no cmap")
    count = be16(cmap_off + 2)
    out: set[int] = set()
    for i in range(count):
        rec = cmap_off + 4 + i * 8
        sub = cmap_off + be32(rec + 4)
        fmt = be16(sub)
        if fmt == 4:
            segx2 = be16(sub + 6)
            ends = sub + 14
            starts = ends + segx2 + 2
            deltas = starts + segx2
            ranges = deltas + segx2
            for s in range(segx2 // 2):
                end = be16(ends + s * 2)
                start = be16(starts + s * 2)
                delta = be16(deltas + s * 2)
                ro = be16(ranges + s * 2)
                for cp in range(start, min(end, 0xFFFF) + 1):
                    if cp == 0xFFFF:
                        continue
                    if ro == 0:
                        glyph = (cp + delta) & 0xFFFF
                    else:
                        addr = ranges + s * 2 + ro + (cp - start) * 2
                        if addr + 2 > len(data):
                            continue
                        glyph = be16(addr)
                        if glyph:
                            glyph = (glyph + delta) & 0xFFFF
                    if glyph:
                        out.add(cp)
        elif fmt == 12:
            ngroups = be32(sub + 12)
            for g in range(ngroups):
                go = sub + 16 + g * 12
                start = be32(go)
                end = be32(go + 4)
                gid = be32(go + 8)
                if start == 0xFFFFFFFF or end > 0x10FFFF:
                    continue
                for cp in range(start, end + 1):
                    if cp != 0:
                        out.add(cp)
    return out


# ---------------------------------------------------------------------------
# Independent pack reader
# ---------------------------------------------------------------------------

class Pack:
    def __init__(self, blob: bytes):
        if len(blob) < 20 or blob[:8] != PACK_MAGIC:
            die("pack magic mismatch")
        self.blob = blob
        self.version = u16(blob, 8)
        self.header_size = u16(blob, 10)
        self.section_count = u16(blob, 12)
        self.total_size = u32(blob, 16)
        if self.version != 1 or self.header_size != 20:
            die(f"unsupported pack version {self.version}/{self.header_size}")
        if self.total_size != len(blob):
            die(f"pack total_size {self.total_size} != file size {len(blob)}")
        if self.section_count != SECTION_COUNT:
            die(f"section_count {self.section_count} != {SECTION_COUNT}")
        self.sec = {}
        for i in range(self.section_count):
            off = 20 + i * 16
            sec_type, s_off, s_count, s_size = (u32(blob, off), u32(blob, off + 4),
                                                u32(blob, off + 8), u32(blob, off + 12))
            if sec_type in self.sec:
                die(f"duplicate section type {sec_type}")
            if s_off + s_size > len(blob):
                die(f"section {sec_type} out of range")
            self.sec[sec_type] = (s_off, s_count, s_size)
        for t in range(SECTION_COUNT):
            if t not in self.sec:
                die(f"missing section {t}")
        self.meta = blob[self.sec[SEC_META][0]:
                         self.sec[SEC_META][0] + self.sec[SEC_META][2]].decode("utf-8")
        self.inflated = 0

    def entry(self, sec, index, size):
        off, count, total = self.sec[sec]
        if index >= count:
            die(f"section {sec} index {index} >= {count}")
        return off + index * size

    def text(self, sid):
        off, count, _size = self.sec[SEC_STRT]
        if sid >= count:
            die(f"string index {sid} >= {count}")
        rec = off + sid * 8
        toff = u32(self.blob, rec)
        tlen = u16(self.blob, rec + 4)
        base = self.sec[SEC_TEXT][0]
        if toff + tlen > self.sec[SEC_TEXT][2]:
            die(f"string {sid} out of range")
        return self.blob[base + toff:base + toff + tlen].decode("utf-8")

    def node(self, idx):
        off = self.entry(SEC_NODE, idx, 24)
        return {
            "chunk_first": u16(self.blob, off),
            "chunk_count": u16(self.blob, off + 2),
            "choice": u16(self.blob, off + 4),
            "condition": u16(self.blob, off + 6),
            "ending": struct.unpack_from("<b", self.blob, off + 8)[0],
            "flags": self.blob[off + 9],
            "break_first": u16(self.blob, off + 10),
            "break_count": self.blob[off + 12],
            "chapter_first": u16(self.blob, off + 13),
            "chapter_count": self.blob[off + 15],
            "event_first": u16(self.blob, off + 16),
            "event_count": u16(self.blob, off + 18),
            "start_first": u16(self.blob, off + 20),
            "start_count": self.blob[off + 22],
        }

    def chunk(self, idx):
        off = self.entry(SEC_CHUNK, idx, 12)
        return {
            "zoff": u32(self.blob, off),
            "zlen": u32(self.blob, off + 4),
            "raw_len": u16(self.blob, off + 8),
            "frames": self.blob[off + 10],
        }

    def chunk_blob(self, idx):
        ch = self.chunk(idx)
        base = self.sec[SEC_BLOB][0]
        z = self.blob[base + ch["zoff"]:base + ch["zoff"] + ch["zlen"]]
        raw = zlib.decompress(z, bufsize=ch["raw_len"])
        if len(raw) != ch["raw_len"]:
            die(f"chunk {idx} raw length {len(raw)} != {ch['raw_len']}")
        return raw

    def frame(self, off):
        return {
            "text_off": u16(self.blob, off),
            "text_len": u16(self.blob, off + 2),
            "view": u16(self.blob, off + 4),
            "speaker": self.blob[off + 6],
            "group": self.blob[off + 7],
            "date": u16(self.blob, off + 8),
            "flags": self.blob[off + 10],
        }

    def view(self, idx):
        off = self.entry(SEC_VIEW, idx, 4)
        return u16(self.blob, off), self.blob[off + 2]

    def layer_ref(self, idx):
        off, count, _ = self.sec[SEC_LAYERREF]
        if idx >= count:
            die(f"layer ref {idx} >= {count}")
        return u16(self.blob, off + idx * 2)

    def layer(self, idx):
        off = self.entry(SEC_LAYER, idx, 12)
        return {
            "asset": u16(self.blob, off),
            "x": i16(self.blob, off + 2),
            "y": i16(self.blob, off + 4),
            "w": u16(self.blob, off + 6),
            "h": u16(self.blob, off + 8),
            "opacity": u16(self.blob, off + 10),
        }

    def choice(self, idx):
        off = self.entry(SEC_CHOICE, idx, 18)
        return {
            "kind": self.blob[off],
            "critical": self.blob[off + 1],
            "heading": self.text(u16(self.blob, off + 2)),
            "category": self.text(u16(self.blob, off + 4)),
            "items": [{
                "to": self.blob[off + 8 + i],
                "action": self.blob[off + 10 + i],
                "text": self.text(u16(self.blob, off + 12 + i * 2)),
            } for i in range(self.blob[off + 6])],
        }

    def event(self, idx):
        off = self.entry(SEC_EVENT, idx, 16)
        return {
            "at": u16(self.blob, off),
            "type": self.blob[off + 2],
            "flags": self.blob[off + 3],
            "a": u16(self.blob, off + 4),
            "b": u16(self.blob, off + 6),
            "c": u16(self.blob, off + 8),
            "d": u16(self.blob, off + 10),
            "data": u16(self.blob, off + 12),
        }

    def condition(self, idx):
        off = self.entry(SEC_COND, idx, 4)
        return {"flag": self.blob[off], "yes": self.blob[off + 1], "no": self.blob[off + 2]}

    def ending(self, idx):
        off = self.entry(SEC_ENDING, idx, 4)
        return {"name": self.text(u16(self.blob, off)),
                "ending_id": self.blob[off + 2]}

    def start(self, idx):
        off = self.entry(SEC_START, idx, 12)
        return {
            "node": self.blob[off], "group": self.blob[off + 1],
            "script": u16(self.blob, off + 2), "index": u16(self.blob, off + 4),
            "date": u16(self.blob, off + 6), "name": self.text(u16(self.blob, off + 8)),
        }

    def assetid(self, idx):
        off = self.entry(SEC_ASSETID, idx, 8)
        return {"part_first": u16(self.blob, off), "part_count": self.blob[off + 2],
                "kind": self.blob[off + 3], "name": self.text(u16(self.blob, off + 4))}

    def part(self, idx):
        off = self.entry(SEC_PART, idx, 30)
        return {
            "off": u32(self.blob, off), "len": u32(self.blob, off + 4),
            "mask_off": u32(self.blob, off + 8), "mask_len": u32(self.blob, off + 12),
            "w": u16(self.blob, off + 16), "h": u16(self.blob, off + 18),
            "ax": i16(self.blob, off + 20), "ay": i16(self.blob, off + 22),
            "ow": u16(self.blob, off + 24), "oh": u16(self.blob, off + 26),
            "flags": self.blob[off + 28],
        }

    def gallery_group(self, idx):
        off = self.entry(SEC_GG, idx, 8)
        return {"label": self.text(u16(self.blob, off)), "first": u16(self.blob, off + 2),
                "count": self.blob[off + 4]}

    def gallery_entry(self, idx):
        off = self.entry(SEC_GE, idx, 4)
        return u16(self.blob, off)

    def speaker_name(self, idx):
        off, count, _ = self.sec[SEC_SPK]
        if idx >= count:
            die(f"speaker {idx} >= {count}")
        return self.text(u16(self.blob, off + idx * 2))

    def date_value(self, idx):
        off, count, _ = self.sec[SEC_DATE]
        if idx >= count:
            die(f"date {idx} >= {count}")
        return u32(self.blob, off + idx * 4)

    def title(self, group):
        off, count, _ = self.sec[SEC_TITLE]
        if group >= count:
            die(f"title group {group} >= {count}")
        return self.text(u16(self.blob, off + group * 2))

    def meta_dict(self):
        out = {}
        for line in self.meta.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
        return out


# ---------------------------------------------------------------------------
# Audit
# ---------------------------------------------------------------------------

class Audit:
    def __init__(self, source: Source):
        self.src = source
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.report = {}

    def err(self, msg):
        if len(self.errors) < 200:
            self.errors.append(msg)

    def run(self):
        src = self.src
        graph = src.graph
        if graph.get("format") != 3:
            self.err(f"graph.format {graph.get('format')} != 3")
        nodes = graph["nodes"]
        order = src.node_order()

        # --- structural checks over every chunk and frame
        frames = 0
        strings = 0
        codepoints = 0
        pua_ids: set[int] = set()
        speaker_names: dict[str, int] = {}
        dates: set[int] = set()
        groups: set[int] = set()
        view_keys: set[str] = set()
        layer_keys: set[str] = set()
        asset_ids_used: set[str] = set()
        chunk_refs = 0
        unique_chunks = set()
        node_stats = {}
        for node_id in order:
            node = nodes[node_id]
            count = 0
            nframes = 0
            for part in node["chunks"]:
                rel = part["file"]
                chunk_refs += 1
                unique_chunks.add(rel)
                try:
                    data = src.chunk(rel)
                except (OSError, UnicodeDecodeError) as exc:
                    self.err(f"{node_id}: cannot read chunk {rel}: {exc}")
                    continue
                chunk_frames = data.get("frames")
                views = data.get("views")
                if not isinstance(chunk_frames, list) or not isinstance(views, list):
                    self.err(f"{node_id}:{rel}: frames/views missing")
                    continue
                if len(chunk_frames) != part["count"]:
                    self.err(f"{node_id}:{rel}: {len(chunk_frames)} frames != graph count {part['count']}")
                if count + len(chunk_frames) > node["count"]:
                    self.err(f"{node_id}:{rel}: frames exceed node count")
                for row in chunk_frames:
                    if not isinstance(row, list) or len(row) != 9:
                        self.err(f"{node_id}:{rel}: bad frame shape {row!r}")
                        continue
                    speaker, text, view, date, group, _site, _script, _contacts, _world = row
                    if not isinstance(text, str) or not text:
                        self.err(f"{node_id}:{rel}: empty text at frame {count}")
                    if not isinstance(view, int) or view < 0 or view >= len(views):
                        self.err(f"{node_id}:{rel}: view {view} out of range at frame {count}")
                    else:
                        for layer in views[view]:
                            if len(layer) != 7:
                                self.err(f"{node_id}:{rel}: bad layer shape {layer!r}")
                                continue
                            asset, lx, ly, lw, lh, opacity, z = layer
                            if asset not in src.assets:
                                self.err(f"{node_id}:{rel}: unknown asset {asset!r}")
                            else:
                                asset_ids_used.add(asset)
                            if lw <= 0 or lh <= 0:
                                self.err(f"{node_id}:{rel}: non-positive layer box {layer!r}")
                            layer_keys.add(json.dumps(layer, sort_keys=True))
                        view_keys.add(json.dumps(views[view], sort_keys=True))
                    if speaker not in speaker_names:
                        speaker_names[speaker] = len(speaker_names)
                    if not isinstance(date, int) or date < 0:
                        self.err(f"{node_id}:{rel}: bad date {date!r}")
                    dates.add(date)
                    groups.add(group)
                    frames += 1
                    nframes += 1
                    strings += 1
                    codepoints += len(text)
                    for ch in text:
                        cp = ord(ch)
                        if cp in (9, 10, 13):
                            continue
                        if 0xE000 <= cp <= 0xF8FF:
                            pua_ids.add(cp - 0xE000)
                        elif cp not in src.cmap:
                            self.err(f"U+{cp:04X} missing from sg-original.ttf cmap ({rel})")
                count += len(chunk_frames)
            if count != node["count"]:
                self.err(f"{node_id}: chunk frame total {count} != node.count {node['count']}")
            node_stats[node_id] = {"count": node["count"], "frames_seen": nframes,
                                   "chunks": len(node["chunks"])}

        # --- unique-chunk statistics (shared chunk files are stored once)
        u_frames = 0
        u_codepoints = 0
        u_codepoints_all = 0
        u_units = 0
        u_pua = set()
        u_speakers = set()
        u_dates = set()
        for rel in sorted(unique_chunks):
            data = src.chunk(rel)
            for row in data["frames"]:
                u_frames += 1
                u_speakers.add(row[0])
                u_dates.add(row[3])
                u_codepoints += len(row[1])
                u_codepoints_all += len(row[1]) + len(row[0])
                if row[0]:
                    u_units += 1
                if row[1]:
                    u_units += 1
                for cell in (row[0], row[1]):
                    for ch in cell:
                        cp = ord(ch)
                        if 0xE000 <= cp <= 0xF8FF:
                            u_pua.add(cp - 0xE000)

        # --- graph edges
        for node_id, node in nodes.items():
            choice = node.get("choice")
            if choice:
                if not isinstance(choice.get("items"), list) or len(choice["items"]) != 2:
                    self.err(f"{node_id}: choice must have exactly 2 items")
                for item in choice.get("items", []):
                    if item.get("to") not in nodes:
                        self.err(f"{node_id}: choice target {item.get('to')} unknown")
                    for key in ("text", "action", "to"):
                        if key not in item:
                            self.err(f"{node_id}: choice item missing {key}")
            cond = node.get("condition")
            if cond:
                for key in ("yes", "no"):
                    if cond.get(key) not in nodes:
                        self.err(f"{node_id}: condition {key} target {cond.get(key)} unknown")
                if "flag" not in cond:
                    self.err(f"{node_id}: condition without flag")
            if "endingId" in node:
                if not isinstance(node["endingId"], int) or not 0 <= node["endingId"] <= 5:
                    self.err(f"{node_id}: bad endingId {node['endingId']}")
                if not node.get("ending"):
                    self.err(f"{node_id}: endingId without name")
            if "ending" in node and "endingId" not in node:
                self.err(f"{node_id}: ending name without id")
            if not choice and not cond and "endingId" not in node:
                self.err(f"{node_id}: no way to leave the node")
            for at, group in node.get("chapters", []):
                if not isinstance(at, int) or at < 0 or at > node["count"]:
                    self.err(f"{node_id}: bad chapter cursor {at}")
                if group not in groups:
                    self.err(f"{node_id}: chapter group {group} not in titles")
            for at in node.get("breaks", []):
                if not isinstance(at, int) or at < 0 or at > node["count"]:
                    self.err(f"{node_id}: bad break cursor {at}")
            for ev in node.get("events", []):
                if not isinstance(ev.get("at"), int) or not 0 <= ev["at"] <= node["count"]:
                    self.err(f"{node_id}: bad event cursor {ev.get('at')}")
                if ev.get("type") == "message" and "id" not in ev:
                    self.err(f"{node_id}: message event without id")

        # --- starts
        starts = graph.get("starts", {})
        for key, item in starts.items():
            if item.get("node") not in nodes:
                self.err(f"start {key}: unknown node {item.get('node')}")
                continue
            node = nodes[item["node"]]
            if not isinstance(item.get("index"), int) or not 0 <= item["index"] < node["count"]:
                self.err(f"start {key}: index {item.get('index')} outside node count {node['count']}")
            if not isinstance(item.get("script"), int):
                self.err(f"start {key}: bad script {item.get('script')}")
            if item.get("group") not in groups:
                self.err(f"start {key}: group {item.get('group')} not in titles")
        # chapter starts must be covered by the node chapter table
        if starts:
            chapter_cursors = {}
            for node_id in {x["node"] for x in starts.values()}:
                chapter_cursors[node_id] = {at: group for at, group in nodes[node_id].get("chapters", [])}
            for key, item in starts.items():
                table = chapter_cursors.get(item["node"], {})
                if item["index"] not in table:
                    self.warnings.append(
                        f"start {key} index {item['index']} has no exact chapter/break entry")

        # --- endings
        endings = graph.get("endings")
        if not isinstance(endings, list) or len(endings) != 6:
            self.err(f"graph.endings count {len(endings) if isinstance(endings, list) else '?'} != 6")
        node_endings = {}
        for node_id, node in nodes.items():
            if "endingId" in node:
                node_endings.setdefault(node["endingId"], []).append(node_id)

        # --- gallery
        gallery_assets = set()
        gallery_groups = 0
        gallery_entries = 0
        for group in src.gallery:
            gallery_groups += 1
            if "image" not in group or "images" not in group or "label" not in group:
                self.err(f"gallery group missing fields: {group!r}")
                continue
            for asset in [group["image"]] + list(group["images"]):
                if asset not in src.assets:
                    self.err(f"gallery {group['label']}: unknown asset {asset!r}")
                gallery_assets.add(asset)
            gallery_entries += len(group["images"])

        # --- reachability from every entry (story plus chapter entry)
        reachable = set()
        stack = [graph["initial"]] + [x["node"] for x in starts.values()]
        while stack:
            node_id = stack.pop()
            if node_id in reachable or node_id not in nodes:
                continue
            reachable.add(node_id)
            node = nodes[node_id]
            if node.get("choice"):
                stack.extend(item["to"] for item in node["choice"]["items"])
            if node.get("condition"):
                stack.extend((node["condition"]["yes"], node["condition"]["no"]))
        unreachable = sorted(set(nodes) - reachable)
        for node_id in unreachable:
            self.err(f"node {node_id} is unreachable from all story/chapter entries")
        unreachable_endings = [eid for eid in range(6) if eid not in node_endings]
        for eid in unreachable_endings:
            self.err(f"ending {eid} has no ending node")

        # --- content digests (canonical op list)
        digest = hashlib.sha256()
        digest.update(struct.pack("<I", len(nodes)))
        for node_id in sorted(nodes, key=lambda x: int(x[1:])):
            node = nodes[node_id]
            digest.update(node_id.encode())
            digest.update(struct.pack("<I", node["count"]))
            for part in node["chunks"]:
                data = src.chunk(part["file"])
                for row in data["frames"]:
                    speaker, text, view = row[0], row[1], row[2]
                    layers = data["views"][view]
                    digest.update(speaker.encode("utf-8"))
                    digest.update(b"\x00")
                    digest.update(text.encode("utf-8"))
                    digest.update(b"\x00")
                    digest.update(json.dumps(layers, sort_keys=True).encode())
                    digest.update(struct.pack("<ii", row[3], row[4]))
            if node.get("choice"):
                digest.update(json.dumps(node["choice"], ensure_ascii=False, sort_keys=True).encode())
            if node.get("condition"):
                digest.update(json.dumps(node["condition"], ensure_ascii=False, sort_keys=True).encode())
            if "endingId" in node:
                digest.update(f"{node['ending']}:{node['endingId']}".encode())
        for group in src.gallery:
            digest.update(json.dumps(group, ensure_ascii=False, sort_keys=True).encode())
        for key in sorted(starts):
            digest.update(key.encode())
            digest.update(json.dumps(starts[key], ensure_ascii=False, sort_keys=True).encode())
        source_digest = digest.hexdigest()

        self.report["source"] = {
            "path": str(src.root),
            "graph_sha256": sha256(src.graph_blob),
            "assets_sha256": sha256((src.common / "assets.js").read_bytes()),
            "gallery_sha256": sha256((src.common / "gallery.txt").read_bytes()),
            "font_sha256": sha256(src.ttf),
            "content_digest": source_digest,
        }
        self.report["counts"] = {
            "nodes": len(nodes),
            "chunks_referenced": chunk_refs,
            "chunks_unique": len(unique_chunks),
            "frames": frames,
            "text_strings": strings,
            "codepoints": codepoints,
            "frames_unique": u_frames,
            "text_strings_unique": u_frames,
            "codepoints_unique": u_codepoints,
            "codepoints_all_unique": u_codepoints_all,
            "units_unique": u_units,
            "distinct_speakers": len(speaker_names),
            "distinct_speakers_unique": len(u_speakers),
            "dates_unique": len(u_dates),
            "pua_glyphs": len(pua_ids),
            "pua_glyphs_unique": len(u_pua),
            "pua_max_id": max(pua_ids) if pua_ids else -1,
            "dates": len(dates),
            "groups": len(groups),
            "views_distinct": len(view_keys),
            "layers_distinct": len(layer_keys),
            "gallery_groups": gallery_groups,
            "gallery_entries": gallery_entries,
            "starts": len(starts),
            "choices": sum(1 for n in nodes.values() if n.get("choice")),
            "conditions": sum(1 for n in nodes.values() if n.get("condition")),
            "endings": len(endings),
            "asset_ids_used_by_story": len(asset_ids_used),
            "asset_ids_total": len(src.assets),
            "asset_files_total": len(set(src.assets.values())),
            "events": sum(len(n.get("events", [])) for n in nodes.values()),
        }
        self.report["content"] = {
            "titles": graph.get("titles", []),
            "endings": endings,
            "initial": graph.get("initial"),
            "starts": [{"key": k, **v} for k, v in starts.items()],
            "endings_by_node": {str(k): v for k, v in node_endings.items()},
            "choices": [
                {"node": node_id, **nodes[node_id]["choice"]}
                for node_id in nodes if nodes[node_id].get("choice")
            ],
            "conditions": [
                {"node": node_id, **nodes[node_id]["condition"]}
                for node_id in nodes if nodes[node_id].get("condition")
            ],
            "node_stats": node_stats,
            "speakers": sorted(speaker_names),
        }
        self.report["assets"] = {
            "ids": len(src.assets),
            "files": len(set(src.assets.values())),
            "parts_entries": len(src.parts),
            "parts_two": sum(1 for v in src.parts.values() if len(v) == 2),
            "parts_one": sum(1 for v in src.parts.values() if len(v) == 1),
        }
        self.report["errors"] = self.errors
        self.report["warnings"] = self.warnings
        return self.report

    # -- pack comparison ---------------------------------------------------
    def compare_pack(self, pack_path: Path):
        src = self.src
        blob = pack_path.read_bytes()
        pack = Pack(blob)
        errors = []

        def eq(what, got, want):
            if got != want:
                errors.append(f"{what}: pack {got!r} != source {want!r}")

        meta = pack.meta_dict()
        self.report["pack"] = {
            "path": str(pack_path), "bytes": len(blob),
            "sha256": sha256(blob), "meta": meta,
            "sections": {
                str(t): {"offset": v[0], "count": v[1], "bytes": v[2]}
                for t, v in sorted(pack.sec.items())
            },
        }
        nodes = src.graph["nodes"]
        order = src.node_order()
        if pack.sec[SEC_NODE][1] != len(nodes):
            errors.append(f"node count {pack.sec[SEC_NODE][1]} != {len(nodes)}")
        node_index = {name: int(name[1:]) for name in nodes}

        # frames + text, chunk by chunk
        src_frames = 0
        pack_frames = 0
        chunk_index = 0
        for node_id in order:
            node = nodes[node_id]
            pnode = pack.node(node_index[node_id])
            eq(f"{node_id}.chunk_count", pnode["chunk_count"], len(node["chunks"]))
            source_events = node.get("events", [])
            eq(f"{node_id}.event_count", pnode["event_count"], len(source_events))
            for ei, ev in enumerate(source_events):
                if ei >= pnode["event_count"]:
                    break
                pe = pack.event(pnode["event_first"] + ei)
                kind = {"closeChoice": 0, "message": 1, "rineReset": 2,
                        "rineReply": 3, "expireRine": 4}.get(ev.get("type"), -1)
                eq(f"{node_id}.event[{ei}].type", pe["type"], kind)
                eq(f"{node_id}.event[{ei}].at", pe["at"], ev.get("at", 0))
                if kind == 1:
                    flags = (1 if ev.get("outgoing") else 0) | (2 if ev.get("stamp") else 0)
                    eq(f"{node_id}.event[{ei}].flags", pe["flags"], flags)
                    eq(f"{node_id}.event[{ei}].base", pe["c"], int(ev.get("base", 0)))
                elif kind == 2:
                    eq(f"{node_id}.event[{ei}].flags", pe["flags"],
                       1 if ev.get("scope") == "display" else 0)
                elif kind == 4:
                    eq(f"{node_id}.event[{ei}].base", pe["c"], int(ev.get("base", 0)))
                    # expireRine 只定义了 bit1=readMessages present；未知位
                    # (例如 0x80) 会被 C reader 拒绝，审计不能放过。
                    eq(f"{node_id}.event[{ei}].unknown_flags",
                       pe["flags"] & ~2, 0)
                    present = "readMessages" in ev
                    count = len(ev.get("readMessages", []))
                    eq(f"{node_id}.event[{ei}].count", pe["b"], count)
                    if not present:
                        # Missing is falsy: no presence bit, no fallback.
                        eq(f"{node_id}.event[{ei}].read_messages_present",
                           bool(pe["flags"] & 2), False)
                    elif count == 0:
                        # A present empty array is truthy and must not collapse
                        # into the legacy b=0 encoding.
                        eq(f"{node_id}.event[{ei}].read_messages_present",
                           bool(pe["flags"] & 2), True)
                    # Non-empty legacy packs (flags=0) and new packs (flags=2)
                    # are both semantically complete because b>0 already marks
                    # the fallback; the reader accepts both encodings.
                else:
                    eq(f"{node_id}.event[{ei}].flags", pe["flags"], 0)
            pchunks = range(pnode["chunk_first"], pnode["chunk_first"] + pnode["chunk_count"])
            for part, pci in zip(node["chunks"], pchunks):
                data = src.chunk(part["file"])
                raw = pack.chunk_blob(pci)
                ch = pack.chunk(pci)
                n = ch["frames"]
                if n != len(data["frames"]):
                    errors.append(f"{node_id}:{part['file']}: frames {n} != {len(data['frames'])}")
                    continue
                text_base = 12 * n
                frame_ok = True
                for i, row in enumerate(data["frames"]):
                    text_off, text_len, view, speaker, group, date, flags =                         struct.unpack_from("<HHHBBHBx", raw, i * 12)
                    fr = {"text_off": text_off, "text_len": text_len, "view": view,
                          "speaker": speaker, "group": group, "date": date,
                          "flags": flags}
                    speaker, text, view = row[0], row[1], row[2]
                    date, group = row[3], row[4]
                    want_text = text.encode("utf-8")
                    got_text = raw[text_base + fr["text_off"]:
                                   text_base + fr["text_off"] + fr["text_len"]]
                    if got_text != want_text:
                        errors.append(f"{node_id}:{part['file']}:{i}: text mismatch")
                        frame_ok = False
                    if pack.speaker_name(fr["speaker"]) != speaker:
                        errors.append(f"{node_id}:{part['file']}:{i}: speaker mismatch")
                        frame_ok = False
                    # view check: compare layer list to the source view
                    pview = fr["view"]
                    vfirst, vcount = pack.view(pview)
                    got_layers = [pack.layer(pack.layer_ref(vfirst + k))
                                  for k in range(vcount)]
                    want_layers = [{
                        "asset": pack_asset_index(pack, l[0]),
                        "x": l[1], "y": l[2], "w": l[3], "h": l[4],
                        "opacity": l[5],
                    } for l in data["views"][view]]
                    if got_layers != want_layers:
                        errors.append(f"{node_id}:{part['file']}:{i}: view mismatch")
                        frame_ok = False
                    if fr["group"] != group:
                        errors.append(f"{node_id}:{part['file']}:{i}: group mismatch")
                        frame_ok = False
                    if pack.date_value(fr["date"]) != date:
                        errors.append(f"{node_id}:{part['file']}:{i}: date mismatch")
                        frame_ok = False
                    src_frames += 1
                    pack_frames += 1
                    if not frame_ok:
                        break
                chunk_index += 1
            # node ends
            if node.get("choice"):
                pchoice = pack.choice(pnode["choice"])
                want = node["choice"]
                for i, item in enumerate(want["items"]):
                    if i >= len(pchoice["items"]):
                        errors.append(f"{node_id}: pack choice missing item {i}")
                        continue
                    got = pchoice["items"][i]
                    eq(f"{node_id}.choice[{i}].text", got["text"], item["text"])
                    eq(f"{node_id}.choice[{i}].action", got["action"],
                       0 if item["action"] == "accept" else 1)
                    eq(f"{node_id}.choice[{i}].to", got["to"], node_index[item["to"]])
                eq(f"{node_id}.choice.heading", pchoice["heading"], want["heading"])
                eq(f"{node_id}.choice.category", pchoice["category"], want["category"])
                eq(f"{node_id}.choice.kind", pchoice["kind"], 0 if want["kind"] == "call" else 1)
                eq(f"{node_id}.choice.critical", bool(pchoice["critical"]), bool(want["critical"]))
            if node.get("condition"):
                pc = pack.condition(pnode["condition"])
                eq(f"{node_id}.cond.yes", pc["yes"], node_index[node["condition"]["yes"]])
                eq(f"{node_id}.cond.no", pc["no"], node_index[node["condition"]["no"]])
            if "endingId" in node:
                pe = pack.ending(pnode["ending"]) if pnode["ending"] >= 0 else None
                if pe is None:
                    errors.append(f"{node_id}: pack has no ending record")
                else:
                    eq(f"{node_id}.ending.name", pe["name"], node["ending"])
                    eq(f"{node_id}.ending.id", pe["ending_id"], node["endingId"])
            eq(f"{node_id}.break_count", pnode["break_count"], len(node.get("breaks", [])))
            eq(f"{node_id}.chapter_count", pnode["chapter_count"], len(node.get("chapters", [])))

        if pack.sec[SEC_TITLE][1] != len(src.graph.get("titles", [])):
            errors.append(f"title count {pack.sec[SEC_TITLE][1]} != "
                          f"{len(src.graph.get('titles', []))}")
        for i, title in enumerate(src.graph.get("titles", [])):
            eq(f"title[{i}]", pack.title(i), title)

        # starts
        starts = src.graph["starts"]
        if pack.sec[SEC_START][1] != len(starts):
            errors.append(f"start count {pack.sec[SEC_START][1]} != {len(starts)}")
        for i, (key, item) in enumerate(starts.items()):
            ps = pack.start(i)
            eq(f"start[{key}].node", ps["node"], node_index[item["node"]])
            eq(f"start[{key}].index", ps["index"], item["index"])
            eq(f"start[{key}].script", ps["script"], item["script"])
            eq(f"start[{key}].group", ps["group"], item["group"])
            eq(f"start[{key}].name", ps["name"], item.get("name", ""))

        # gallery
        if pack.sec[SEC_GG][1] != len(src.gallery):
            errors.append(f"gallery groups {pack.sec[SEC_GG][1]} != {len(src.gallery)}")
        for gi, group in enumerate(src.gallery):
            pg = pack.gallery_group(gi)
            eq(f"gallery[{gi}].label", pg["label"], group["label"])
            eq(f"gallery[{gi}].count", pg["count"], len(group["images"]))
            for k, asset in enumerate(group["images"]):
                pa = pack.gallery_entry(pg["first"] + k)
                eq(f"gallery[{gi}][{k}].asset", pack_asset_index(pack, asset), pa)

        # asset mapping
        if pack.sec[SEC_ASSETID][1] < len(src.assets):
            errors.append(f"asset ids {pack.sec[SEC_ASSETID][1]} < {len(src.assets)}")
        file_parts = {}
        for asset, files in src.assets.items():
            pa = pack.assetid(pack_asset_index(pack, asset))
            eq(f"asset[{asset}].name", pa["name"], asset)
            want_parts = src.parts.get(asset, [files])
            if pa["part_count"] != len(want_parts):
                errors.append(f"asset {asset}: parts {pa['part_count']} != {len(want_parts)}")
                continue
            for k, rel in enumerate(want_parts):
                pp = pack.part(pa["part_first"] + k)
                key = (rel, pp["w"], pp["h"], pp["ax"], pp["ay"], pp["ow"], pp["oh"])
                file_parts.setdefault(rel, set()).add(key)

        # image dimensions from the source files (Pillow only for this check)
        try:
            from PIL import Image
        except ImportError:
            Image = None
        if Image is not None:
            for rel, keys in file_parts.items():
                try:
                    im = Image.open(src.common / "img" / rel)
                except OSError as exc:
                    errors.append(f"image {rel}: {exc}")
                    continue
                w, h = im.size
                for (_rel, pw, ph, ax, ay, ow, oh) in keys:
                    if ow != w or oh != h:
                        errors.append(f"{rel}: pack original size {ow}x{oh} != {w}x{h}")
                    if ax < 0 or ay < 0 or ax + pw > w or ay + ph > h:
                        errors.append(f"{rel}: patch {pw}x{ph}+{ax}+{ay} outside {w}x{h}")
                    if pw <= 0 or ph <= 0:
                        errors.append(f"{rel}: empty patch")

        # metadata counts
        meta_counts = {k: int(v) for k, v in meta.items()
                       if k.startswith("count_") and v.isdigit()}
        for key, value in meta_counts.items():
            field = key[len("count_"):]
            if field in self.report["counts"]:
                if field == "dates":
                    if value < self.report["counts"][field]:
                        errors.append(f"meta.{key}: pack {value} < source "
                                      f"{self.report['counts'][field]}")
                else:
                    eq(f"meta.{key}", value, self.report["counts"][field])

        self.report["pack"]["compare_errors"] = errors
        self.report["pack"]["frames_compared"] = src_frames
        self.report["pack"]["pack_frames"] = pack_frames
        return errors


_ASSET_INDEX_CACHE = {}


def pack_asset_index(pack, asset):
    key = id(pack)
    cache = _ASSET_INDEX_CACHE.get(key)
    if cache is None:
        cache = {}
        for i in range(pack.sec[SEC_ASSETID][1]):
            info = pack.assetid(i)
            cache.setdefault(info["name"], i)
        _ASSET_INDEX_CACHE[key] = cache
    if asset not in cache:
        die(f"pack has no asset named {asset}")
    return cache[asset]


# ---------------------------------------------------------------------------

def dump_ops(source: Source, path: Path):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for node_id in source.node_order():
            node = source.graph["nodes"][node_id]
            cursor = 0
            for part in node["chunks"]:
                data = source.chunk(part["file"])
                for row in data["frames"]:
                    fh.write(json.dumps({
                        "node": node_id, "cursor": cursor, "speaker": row[0],
                        "text": row[1], "view": row[2], "date": row[3], "group": row[4],
                        "source": row[5],
                    }, ensure_ascii=False) + "\n")
                    cursor += 1
            if node.get("choice"):
                fh.write(json.dumps({"node": node_id, "choice": node["choice"]},
                                    ensure_ascii=False) + "\n")
            if node.get("condition"):
                fh.write(json.dumps({"node": node_id, "condition": node["condition"]},
                                    ensure_ascii=False) + "\n")
            if "endingId" in node:
                fh.write(json.dumps({"node": node_id, "ending": node["ending"],
                                     "endingId": node["endingId"]}, ensure_ascii=False) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", help="steins0-rw5 checkout")
    ap.add_argument("--pack", help="generated pack to cross-check")
    ap.add_argument("--json", default="", help="write the audit report here")
    ap.add_argument("--dump", default="", help="write every source op as JSONL")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = Path(args.source)
    if not (root / "src" / "common" / "graph.txt").exists():
        die(f"{root} is not a steins0-rw5 checkout")
    source = Source(root)
    audit = Audit(source)
    report = audit.run()
    pack_errors = []
    if args.pack:
        pack_errors = audit.compare_pack(Path(args.pack))
    if args.dump:
        Path(args.dump).parent.mkdir(parents=True, exist_ok=True)
        dump_ops(source, Path(args.dump))
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n",
                                   encoding="utf-8")
    counts = report["counts"]
    if not args.quiet:
        print("source:", root)
        print("graph ", report["source"]["graph_sha256"][:16],
              "digest", report["source"]["content_digest"][:16])
        print(f"nodes {counts['nodes']} chunks {counts['chunks_unique']} "
              f"({counts['chunks_referenced']} refs) frames {counts['frames']} "
              f"strings {counts['text_strings']}")
        print(f"codepoints {counts['codepoints']} pua {counts['pua_glyphs']} "
              f"speakers {counts['distinct_speakers']} dates {counts['dates']} "
              f"groups {counts['groups']}")
        print(f"choices {counts['choices']} conditions {counts['conditions']} "
              f"endings {counts['endings']} starts {counts['starts']} "
              f"events {counts['events']}")
        print(f"views {counts['views_distinct']} layers {counts['layers_distinct']} "
              f"gallery {counts['gallery_groups']}/{counts['gallery_entries']} "
              f"assets {counts['asset_ids_total']}/{counts['asset_files_total']}")
        if args.pack:
            print(f"pack {report['pack']['bytes']} B {report['pack']['sha256'][:16]} "
                  f"frames compared {report['pack']['frames_compared']}")
    if report["errors"]:
        for msg in report["errors"][:30]:
            print("SOURCE ERROR:", msg, file=sys.stderr)
        print(f"source audit FAIL ({len(report['errors'])} errors)", file=sys.stderr)
        return 1
    if pack_errors:
        for msg in pack_errors[:30]:
            print("PACK ERROR:", msg, file=sys.stderr)
        print(f"pack compare FAIL ({len(pack_errors)} errors)", file=sys.stderr)
        return 1
    if not args.quiet:
        print("source audit PASS" + (" / pack compare PASS" if args.pack else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
