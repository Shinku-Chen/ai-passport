#!/usr/bin/env python3
"""Build the Summer Pockets REFLECTION BLUE reader pack from the RW5 source snapshot.

The input is the read-only converted source tree (the same layout as
`assessment-games/repos/summer-rw5`): `src/common/catalog.js`,
`src/common/assets.js`, `src/common/gallery.txt`, `converted-story/*.json`
and `src/common/images/*`.

The output is a single little-endian pack embedded in the firmware.  It has no
runtime JSON parser: every table is a fixed or varint record and all strings
live in one NUL-terminated UTF-8 blob.

    python tools/summer_pack.py --source <snapshot> \
        --out main/summer_data/summer_pack.bin --report build/summer_pack_report.json

Image policy (documented in docs/development/engineering/galgame-summer.md):

* scene images (backgrounds / CGs) are scaled to `--scene-max` preserving the
  full 16:9 frame, then re-encoded as baseline JPEG;
* portraits keep every source pose and expression, are scaled into
  `--portrait-box`, encoded as RGB JPEG, and carry an RLE 1bpp alpha mask;
* every referenced image id is packed; source ids that the RW5 runtime itself
  falls back to `bg_black` are mapped to the black entry (never invented).

The packer is deliberately independent from `tools/summer_audit.py`: the audit
re-reads the source and the produced pack with its own parser.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import struct
import sys
from dataclasses import dataclass

try:
    from PIL import Image
except ImportError:  # pragma: no cover - build-time dependency
    Image = None


MAGIC = b"SUMMPK01"
VERSION = 1
HEADER = struct.Struct("<8sHHHHI")  # magic, version, header_size, sections, flags, total
SECTION = struct.Struct("<HHIII")  # type, flags, offset, count, size

SEC_TEXT = 0
SEC_OPS = 1
SEC_OPIDX = 2
SEC_SCENE = 3
SEC_CHAPTER = 4
SEC_ROUTE = 5
SEC_GALLERY = 6
SEC_GALLERY_ITEM = 7
SEC_IMAGE = 8
SEC_IMAP = 9
SEC_EXPR = 10
SEC_EXPRIDX = 11
SEC_VARS = 12
SEC_CHOICE = 13
SEC_CHOICE_ITEM = 14
SEC_MAP = 15
SEC_MAP_ITEM = 16
SEC_MAPLABEL = 17
SEC_FLOWLABEL = 18
SEC_PROGRESS = 19
SEC_META = 20
SEC_SCENE_INDEX = 21
SEC_SPEAKER = 22
SEC_IMGDATA = 23
SEC_CHARTAB = 24
SECTION_COUNT = 25
HEADER_SIZE = HEADER.size + SECTION.size * SECTION_COUNT

# op kinds (must match main/summer_pack.h)
OP_TEXT = 0
OP_IMAGE = 1
OP_SET = 2
OP_ADD = 3
OP_SUB = 4
OP_IF = 5
OP_GOTO = 6
OP_CALL = 7
OP_CALLSCENE = 8
OP_JUMPSCENE = 9
OP_RETURN = 10
OP_END = 11
OP_NOP = 12
OP_CHOICE = 13
OP_MAP = 14
OP_CLEAR = 15

# image kinds
IMG_SCENE = 0
IMG_PORTRAIT = 1
IMG_SOLID = 2

# expression postfix ops (must match main/summer_pack.h)
EX_CONST = 0
EX_VAR = 1
EX_DATE = 2
EX_STRLIT = 3
EX_NATIVE = 4
EX_UNSUPPORTED = 5
EX_NOT = 6
EX_NEG = 7
EX_POS = 8
EX_BNOT = 9
EX_ADD = 10
EX_SUB = 11
EX_MUL = 12
EX_DIV = 13
EX_MOD = 14
EX_AND = 15
EX_OR = 16
EX_XOR = 17
EX_SHL = 18
EX_SHR = 19
EX_EQ = 20
EX_NE = 21
EX_LT = 22
EX_LE = 23
EX_GT = 24
EX_GE = 25
EX_LAND = 26
EX_LOR = 27
EX_STREQ = 28
EX_STRNE = 29
EX_VARKEY = 30


def die(msg: str) -> "NoReturn":
    print(f"summer_pack: ERROR: {msg}", file=sys.stderr)
    raise SystemExit(2)


def log(msg: str) -> None:
    print(f"summer_pack: {msg}", file=sys.stderr)


# ---------------------------------------------------------------- varints
def uvarint(value: int) -> bytes:
    if value < 0:
        raise ValueError("uvarint 只接受非负数")
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def zigzag(value: int) -> int:
    return (value << 1) ^ (value >> 31) if value >= 0 else ((-value) << 1) - 1


def svarint(value: int) -> bytes:
    return uvarint(zigzag(int(value)))


def visible_text(value, full_name=False):
    """Mirror RW5 src/common/names.js visibleText().

    The watch app never shows the raw U+061F (Arabic question mark) or the
    compatibility ideograph U+4C37; dialogue keeps the short 紬, while labels
    (scene/route/chapter names) use the full 紬文德斯.
    """
    text = str(value or "")
    text = text.replace("䌷", "紬")
    text = re.sub(r"紬[·・\s]?文德斯", "紬文德斯", text)
    text = text.replace("Tsumugi", "紬文德斯")
    text = text.replace("؟", "？")
    if full_name:
        text = re.sub(r"紬(?!文德斯)", "紬文德斯", text)
    return text


def fullwidth_digits(value):
    return "".join(chr(ord(c) + 0xFEE0) if "0" <= c <= "9" else c for c in str(value))


class TextPool:
    """Deduplicated strings stored as 12-bit codepoint indices.

    Code 0 terminates a string.  The codepoint table (index -> Unicode
    codepoint) is written as a separate pack section so the device can decode
    a string to UTF-8 without any decompressor.  Indices are assigned in first
    appearance order; 12 bits cover the 3.8k glyphs used by this title.
    """

    def __init__(self) -> None:
        self._offsets: dict[str, int] = {}
        self.codes: list[int] = []
        self._char_index: dict[int, int] = {}
        self.chars: list[int] = []

    def _code(self, ch: str) -> int:
        cp = ord(ch)
        if cp == 0:
            die("字符串里不允许 NUL")
        index = self._char_index.get(cp)
        if index is None:
            index = len(self.chars) + 1
            if index > 0xFFF:
                die(f"字符表超过 4095 项(U+{cp:04X})")
            self._char_index[cp] = index
            self.chars.append(cp)
        return index

    def add(self, value: str | None, full_name: bool = False) -> int:
        value = visible_text(value, full_name)
        if value in self._offsets:
            return self._offsets[value]
        off = len(self.codes)
        self._offsets[value] = off
        for ch in value:
            self.codes.append(self._code(ch))
        self.codes.append(0)
        return off

    def blob(self) -> bytes:
        out = bytearray()
        codes = self.codes
        for i in range(0, len(codes), 2):
            c0 = codes[i]
            c1 = codes[i + 1] if i + 1 < len(codes) else 0
            value = c0 | (c1 << 12)
            out.append(value & 0xFF)
            out.append((value >> 8) & 0xFF)
            out.append((value >> 16) & 0xFF)
        return bytes(out)

    def chartab(self) -> bytes:
        return b"".join(struct.pack("<H", cp) for cp in self.chars)

    def __len__(self) -> int:
        return len(self._offsets)


# ---------------------------------------------------------------- source
def load_js_object(path: str, prefix: str) -> dict:
    text = open(path, encoding="utf-8").read()
    match = re.search(prefix + r"\s*=\s*(\{.*\})\s*;", text, re.S)
    if not match:
        die(f"{path}: 找不到对象字面量")
    return json.loads(match.group(1))


def load_source(root: str):
    cat = load_js_object(os.path.join(root, "src/common/catalog.js"), "export const catalog")
    assets_text = open(os.path.join(root, "src/common/assets.js"), encoding="utf-8").read()
    match = re.search(r"const paths\s*=\s*(\{.*?\});", assets_text, re.S)
    if not match:
        die("assets.js: 找不到 paths")
    paths = json.loads(match.group(1))
    gallery = json.load(open(os.path.join(root, "src/common/gallery.txt"), encoding="utf-8"))
    scenes = {}
    for name in os.listdir(os.path.join(root, "converted-story")):
        if not name.endswith(".json"):
            continue
        scene_id = int(name[:-5])
        scenes[scene_id] = json.load(open(os.path.join(root, "converted-story", name), encoding="utf-8"))
    return cat, paths, gallery, scenes


# ---------------------------------------------------------------- expressions
class ExprCompiler:
    def __init__(self, text: TextPool):
        self.text = text
        self.programs: list[bytes] = []
        self.offsets: list[int] = []
        self._ids: dict[str, int] = {}
        self._blob = bytearray()
        self.uses_special = False

    def compile(self, expr) -> int:
        canonical = json.dumps(expr, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        existing = self._ids.get(canonical)
        if existing is not None:
            return existing
        expr_id = len(self.programs) + 1
        start = len(self._blob)
        self._emit(expr)
        self._blob = self._blob  # keep bytearray
        self.programs.append(bytes(self._blob[start:]))
        self.offsets.append(start)
        self._ids[canonical] = expr_id
        return expr_id

    def _emit(self, expr) -> None:
        if isinstance(expr, (int, bool)):
            self._blob += bytes([EX_CONST]) + svarint(int(expr))
            return
        if not isinstance(expr, list) or not expr:
            die(f"无法编译的条件: {expr!r}")
        op = expr[0]
        if op == "v":
            if isinstance(expr[1], int):
                var_id = int(expr[1])
                if var_id < 0 or var_id > 0xFFFFFFFF:
                    die(f"变量 id 越界: {var_id}")
                self._blob += bytes([EX_VAR]) + uvarint(var_id)
            else:
                self._emit(expr[1])
                self._blob += bytes([EX_VARKEY])
            return
        if op == "s":
            if expr[1] != 106:
                die(f"仅支持源日期字符串 s(106): {expr!r}")
            self._blob += bytes([EX_DATE])
            return
        if op == "string":
            self._blob += bytes([EX_STRLIT]) + uvarint(self.text.add(str(expr[1])))
            return
        if op == "native":
            self._blob += bytes([EX_NATIVE])
            return
        if op == "unsupported":
            self._blob += bytes([EX_UNSUPPORTED])
            return
        if op in ("!", "neg", "pos", "~"):
            self._emit(expr[1])
            self._blob += bytes([{"!": EX_NOT, "neg": EX_NEG, "pos": EX_POS, "~": EX_BNOT}[op]])
            return
        binary = {
            "+": EX_ADD, "-": EX_SUB, "*": EX_MUL, "/": EX_DIV, "%": EX_MOD,
            "&": EX_AND, "|": EX_OR, "^": EX_XOR, "<<": EX_SHL, ">>": EX_SHR,
            "==": EX_EQ, "!=": EX_NE, "<": EX_LT, "<=": EX_LE, ">": EX_GT, ">=": EX_GE,
        }
        if op in ("&&", "||"):
            # The source evaluates left to right with short-circuit; operands are
            # side-effect free, so a strict postfix chain is equivalent.
            for sub in expr[1:]:
                self._emit(sub)
            for _ in range(len(expr) - 2):
                self._blob += bytes([EX_LAND if op == "&&" else EX_LOR])
            return
        if op in binary:
            # String comparison is only used for the date (s 106 == "…").
            if op in ("==", "!="):
                left, right = expr[1], expr[2]
                if self._is_str(left) or self._is_str(right):
                    self._emit(left)
                    self._emit(right)
                    self._blob += bytes([EX_STREQ if op == "==" else EX_STRNE])
                    return
            self._emit(expr[1])
            self._emit(expr[2])
            self._blob += bytes([binary[op]])
            return
        die(f"未支持的条件操作: {op}")

    @staticmethod
    def _is_str(expr) -> bool:
        return isinstance(expr, list) and expr and expr[0] in ("s", "string")

    def finish(self) -> tuple[bytes, bytes]:
        # Offsets are relative to the expression blob; the final sentinel is the size.
        idx = bytearray()
        for off in self.offsets:
            idx += struct.pack("<I", off)
        idx += struct.pack("<I", len(self._blob))
        return bytes(self._blob), bytes(idx)


# ---------------------------------------------------------------- images
@dataclass
class ImageEntry:
    kind: int
    width: int
    height: int
    data: bytes
    mask: bytes = b""

    @property
    def size(self) -> int:
        return len(self.data) + len(self.mask)


def rle_mask(mask) -> bytes:
    """RLE a 1bpp alpha mask.

    Per row: uvarint(run count), then alternating run lengths starting with a
    transparent run.  A final transparent run is omitted.  Width is known from
    the image entry, so rows are self-delimiting.
    """
    w, h = mask.size
    px = mask.load()
    out = bytearray()
    for y in range(h):
        runs: list[int] = []
        cur = 0
        run = 0
        for x in range(w):
            v = 1 if px[x, y] > 127 else 0
            if v == cur:
                run += 1
            else:
                runs.append(run)
                cur = v
                run = 1
        runs.append(run)
        if runs and runs[-1] == 0:
            runs.pop()
        out += uvarint(len(runs))
        for r in runs:
            out += uvarint(r)
    return bytes(out)


def encode_scene(path: str, max_w: int, max_h: int, quality: int) -> ImageEntry:
    im = Image.open(path).convert("RGB")
    w, h = im.size
    scale = min(max_w / w, max_h / h, 1.0)
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    if (nw, nh) != (w, h):
        im = im.resize((nw, nh), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality, optimize=True, subsampling=2)
    return ImageEntry(IMG_SCENE, nw, nh, buf.getvalue())


def encode_portrait(path: str, box_w: int, box_h: int, quality: int) -> ImageEntry:
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    scale = min(box_w / w, box_h / h, 1.0)
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    if (nw, nh) != (w, h):
        im = im.resize((nw, nh), Image.LANCZOS)
    rgb = im.convert("RGB")
    buf = io.BytesIO()
    rgb.save(buf, "JPEG", quality=quality, optimize=True, subsampling=2)
    mask = im.getchannel("A").point(lambda v: 255 if v > 127 else 0)
    return ImageEntry(IMG_PORTRAIT, nw, nh, buf.getvalue(), rle_mask(mask))


# ---------------------------------------------------------------- pack build
class Builder:
    def __init__(self, args):
        self.args = args
        self.text = TextPool()
        self.expr = None
        self.sections: dict[int, tuple[int, bytes]] = {}

    def add(self, sec: int, data: bytes, count: int = 0) -> None:
        if sec in self.sections:
            die(f"重复写入段 {sec}")
        self.sections[sec] = (count, data)

    def build(self) -> bytes:
        header = HEADER.pack(MAGIC, VERSION, HEADER_SIZE, SECTION_COUNT, 0, 0)
        offset = HEADER_SIZE
        table = bytearray()
        payload = bytearray()
        for sec in range(SECTION_COUNT):
            count, data = self.sections.get(sec, (0, b""))
            # Align every section to 8 bytes: the C reader maps fixed records
            # (u16/u32 fields) directly, so a misaligned section would be UB on
            # RISC-V and would trap under host UBSan.
            pad = (-offset) % 8
            payload += bytes(pad)
            offset += pad
            table += SECTION.pack(sec, 0, offset, count, len(data))
            payload += data
            offset += len(data)
        total = HEADER_SIZE + len(payload)
        header = HEADER.pack(MAGIC, VERSION, HEADER_SIZE, SECTION_COUNT, 0, total)
        return header + bytes(table) + bytes(payload)


def build(args) -> int:
    if Image is None:
        die("生成资源包需要 Pillow")
    root = args.source
    cat, paths, gallery, scenes = load_source(root)

    # -------- scene order: numeric source ids (converted-story/*.json)
    scene_ids = sorted(scenes)
    if len(scene_ids) != cat["count"]:
        die(f"converted-story 场景数 {len(scene_ids)} != catalog.count {cat['count']}")
    catalog_scenes = cat["scenes"]
    missing = [sid for sid in scene_ids if str(sid) not in catalog_scenes]
    if missing:
        die(f"catalog 缺少场景: {missing[:8]}")
    extra = [int(k) for k in catalog_scenes if int(k) not in scenes]
    if extra:
        die(f"catalog 多出场景: {extra[:8]}")

    # -------- variable table (source variable id -> dense index)
    var_ids: set[int] = set()

    def scan_expr(expr):
        if isinstance(expr, list) and expr:
            op = expr[0]
            if op == "v" and isinstance(expr[1], int):
                var_ids.add(expr[1])
            for sub in expr[1:]:
                scan_expr(sub)

    for sid in scene_ids:
        for op in scenes[sid]:
            kind = op["op"]
            if kind in ("set", "add", "sub") and isinstance(op.get("variable"), int):
                var_ids.add(op["variable"])
            if kind in ("choice", "map") and isinstance(op.get("variable"), int):
                var_ids.add(op["variable"])
            if kind == "if":
                scan_expr(op.get("value"))
            if kind == "choice":
                for item in op.get("items", []):
                    scan_expr(item.get("condition", 1))
    var_ids.add(24006)
    var_index = {vid: i for i, vid in enumerate(sorted(var_ids))}
    special_24006 = var_index.get(24006, 0xFFFF)
    self_text = TextPool()
    compiler = ExprCompiler(self_text)

    # -------- ops / scene records
    op_blob = bytearray()
    block_offsets: list[int] = []
    scene_records = []
    choice_groups = []
    choice_items = []
    map_groups = []
    map_items = []
    speaker_pool: dict[str, int] = {}

    def speaker_id(name: str) -> int:
        # The source index.ux calls visibleText(f.speaker, true): dialogue keeps
        # the short form, but the speaker label expands 紬 to 紬文德斯.
        if name not in speaker_pool:
            speaker_pool[name] = len(speaker_pool)
        return speaker_pool[name]

    total_ops = 0
    for sid in scene_ids:
        meta = catalog_scenes[str(sid)]
        ops = scenes[sid]
        first_op = total_ops
        # block table: one offset per 32 ops
        first_block = len(block_offsets)
        scene_op_start = len(op_blob)
        op_count = 0
        # block offsets are global op counts, converted to byte offsets after
        # all scenes are emitted; store byte offsets now as we append.
        for index, op in enumerate(ops):
            if index % 32 == 0:
                block_offsets.append(len(op_blob))
            kind = op["op"]
            if kind == "text":
                text_off = self_text.add(op.get("text", ""))
                op_blob += bytes([OP_TEXT]) + uvarint(speaker_id(op.get("speaker", ""))) + uvarint(text_off)
            elif kind == "image":
                image_id = int(op["image"][1:]) if str(op.get("image", "")).startswith("i") else 0
                op_blob += bytes([OP_IMAGE]) + uvarint(image_id) + uvarint(int(op.get("layer", 0)))
            elif kind in ("set", "add", "sub"):
                var = op.get("variable")
                if not isinstance(var, int):
                    die(f"场景 {sid} 的 {kind} 变量不是整数: {var!r}")
                tag = 0
                value = op.get("value")
                if isinstance(value, list):
                    tag = 1
                    encoded = compiler.compile(value)
                    payload = uvarint(encoded)
                else:
                    payload = svarint(int(value or 0))
                op_blob += bytes([{"set": OP_SET, "add": OP_ADD, "sub": OP_SUB}[kind]])
                op_blob += uvarint(int(var)) + bytes([tag]) + payload
            elif kind == "if":
                expr = op.get("value")
                eid = compiler.compile(expr)
                op_blob += bytes([OP_IF]) + uvarint(eid) + uvarint(int(op["target"])) + bytes([1 if op.get("truth") else 0])
            elif kind == "goto":
                op_blob += bytes([OP_GOTO]) + uvarint(int(op["target"]))
            elif kind == "call":
                op_blob += bytes([OP_CALL]) + uvarint(int(op["target"]))
            elif kind in ("callScene", "jumpScene"):
                op_blob += bytes([OP_CALLSCENE if kind == "callScene" else OP_JUMPSCENE])
                op_blob += uvarint(int(op["scene"])) + uvarint(int(op["target"]))
            elif kind == "return":
                op_blob += bytes([OP_RETURN])
            elif kind == "end":
                op_blob += bytes([OP_END])
            elif kind == "nop":
                op_blob += bytes([OP_NOP])
            elif kind == "clear":
                op_blob += bytes([OP_CLEAR]) + uvarint(int(op.get("layer", 0)))
            elif kind == "choice":
                group = len(choice_groups)
                choice_groups.append({
                    "var": int(op["variable"]),
                    "first": len(choice_items),
                    "count": len(op["items"]),
                })
                for item in op["items"]:
                    cond = item.get("condition", 1)
                    eid = 0 if cond == 1 else compiler.compile(cond)
                    choice_items.append({
                        "text": self_text.add(str(item.get("text", ""))),
                        "value": int(item.get("value", 0)),
                        "cond": eid,
                    })
                op_blob += bytes([OP_CHOICE]) + uvarint(group)
            elif kind == "map":
                group = len(map_groups)
                map_groups.append({
                    "var": int(op["variable"]),
                    "first": len(map_items),
                    "count": len(op["items"]),
                })
                for item in op["items"]:
                    map_items.append(int(item.get("value", 0)))
                op_blob += bytes([OP_MAP]) + uvarint(group)
            else:
                die(f"场景 {sid} 未支持的 op: {kind}")
            op_count += 1
        total_ops += op_count
        if op_count != int(meta.get("length", op_count)):
            die(f"场景 {sid} 长度不符: 源 {op_count} catalog {meta.get('length')}")
        # block offsets for this scene are in block_offsets[first_block:]
        block_count = (op_count + 31) // 32
        source = str(meta.get("source", ""))
        # date exactly like sceneVariables(): MMDD suffix in the source name.
        # strings[106] is the source's raw value: full-width digits, set only
        # when the source name carries MMDD and never cleared on later scenes.
        date = ""
        m = re.search(r"(\d{2})(\d{2})(AM|PM|[a-z]?)$", source)
        if m and 1 <= int(m.group(1)) <= 12 and 1 <= int(m.group(2)) <= 31:
            date = fullwidth_digits(f"{int(m.group(1))}月{int(m.group(2))}日")
        date_label = str(meta.get("date", ""))
        route = int(meta.get("route", -1))
        phase = {"personal": 1, "final": 2}.get(str(meta.get("phase", "")), 0)
        flags = (1 if source.startswith("_") else 0) | (2 if re.match(r"^(?:RB)?90_", source) else 0)
        scene_records.append({
            "id": sid,
            "first_op": first_op,
            "op_count": op_count,
            "next": int(meta.get("next", 0) or 0),
            "route": route,
            "phase": phase,
            "flags": flags,
            "first_block": first_block,
            "block_count": block_count,
            "name": self_text.add(str(meta.get("name", "")), True),
            "map_name": self_text.add(str(meta.get("name", "")), False),
            "date": self_text.add(date),
            "date_label": self_text.add(date_label),
            "route_name": self_text.add(str(meta.get("routeName", "")), True),
        })

    # Speaker names are appended after the story text pool so inserting them
    # does not shift already emitted text offsets.
    speaker_offsets = [0] * len(speaker_pool)
    for name, index in speaker_pool.items():
        speaker_offsets[index] = self_text.add(name, True)

    # -------- images (unique content, stable order by first source id)
    referenced_ids = set()
    for sid in scene_ids:
        for op in scenes[sid]:
            if op["op"] == "image":
                try:
                    referenced_ids.add(int(str(op["image"])[1:]))
                except ValueError:
                    pass
    for item in gallery:
        try:
            referenced_ids.add(int(str(item["image"])[1:]))
        except (KeyError, ValueError):
            pass
    referenced_ids.add(1)  # title fallback
    referenced_ids = sorted(referenced_ids)
    file_by_id: dict[int, str] = {}
    missing_source_paths = []
    for iid in referenced_ids:
        key = f"i{iid}"
        if key not in paths:
            missing_source_paths.append(iid)
        rel = paths.get(key, "bg_black.jpg")
        fp = os.path.join(root, "src/common/images", rel)
        if not os.path.exists(fp):
            die(f"引用的图片不存在: {key} -> {rel}")
        file_by_id[iid] = fp
    # bg_black is the fallback for the source's own missing ids.
    black_path = os.path.join(root, "src/common/images", "bg_black.jpg")
    if not os.path.exists(black_path):
        die("缺少 bg_black.jpg")

    unique: dict[str, int] = {}
    entries: list[ImageEntry] = []
    id_to_entry: dict[int, int] = {}
    # entry 0 = solid black
    entries.append(ImageEntry(IMG_SOLID, 1, 1, b""))
    for iid in referenced_ids:
        fp = file_by_id[iid]
        digest = hashlib.sha1(open(fp, "rb").read()).hexdigest()
        if digest not in unique:
            rel = os.path.basename(fp)
            if rel.startswith("portrait_"):
                entry = encode_portrait(fp, args.portrait_w, args.portrait_h, args.portrait_quality)
            else:
                entry = encode_scene(fp, args.scene_w, args.scene_h, args.scene_quality)
            unique[digest] = len(entries)
            entries.append(entry)
        id_to_entry[iid] = unique[digest]
    # The source runtime maps unknown numeric ids to bg_black; do the same.
    imap = bytearray()
    max_image_id = max(referenced_ids) if referenced_ids else 0
    imap += struct.pack("<H", 0)  # id 0 = black
    for iid in range(1, max_image_id + 1):
        imap += struct.pack("<H", id_to_entry.get(iid, 0))
    # Special UI images: the RW5 logo (RGBA) is packed like a portrait mask.
    logo_index = 0xFFFF
    logo_path = os.path.join(root, "src/common/ui/logo.png")
    if os.path.exists(logo_path):
        logo = encode_portrait(logo_path, args.logo_w, args.logo_h, args.logo_quality)
        logo_index = len(entries)
        entries.append(logo)

    # image blob + records (24 bytes each)
    image_blob = bytearray()
    image_records = bytearray()
    for entry in entries:
        off = len(image_blob)
        image_blob += entry.data
        mask_off = len(image_blob)
        image_blob += entry.mask
        image_records += struct.pack("<IIIIHHBBH", off, len(entry.data), mask_off,
                                     len(entry.mask), entry.width, entry.height,
                                     entry.kind, 0, 0)

    # -------- catalogue / gallery / routes
    chapter_blob = bytearray()
    for chapter in cat["chapters"]:
        chapter_blob += struct.pack("<HBBI", int(chapter["id"]),
                                    int(chapter.get("route", -1)) & 0xFF, 0,
                                    self_text.add(str(chapter.get("name", "")), True))
    route_blob = bytearray()
    for index, route in enumerate(cat["routes"]):
        # chapter count for this route index
        count = sum(1 for c in cat["chapters"] if int(c.get("route", -1)) == index)
        route_blob += struct.pack("<IHH", self_text.add(str(route.get("name", "")), True),
                                  len(route.get("scenes", [])), count)

    gallery_groups: list[dict] = []
    by_group: dict[str, dict] = {}
    for item in gallery:
        source = str(item.get("source", ""))
        m = re.match(r"^(cg|ef)_([a-z]+[0-9]+)_", source, re.I)
        key = (m.group(1).lower() + "_" + m.group(2).lower()) if m else source.lower()
        group = by_group.get(key)
        if group is None:
            group = {"label": str(item.get("label", "")), "items": []}
            by_group[key] = group
            gallery_groups.append(group)
        try:
            iid = int(str(item["image"])[1:])
        except (KeyError, ValueError):
            die(f"CG 目录图片无效: {item}")
        group["items"].append(id_to_entry.get(iid, 0))
    # Groups are not contiguous in gallery.txt; concatenate each group's items so
    # the pack can store a first/count slice.  Group order stays first-appearance.
    gallery_items: list[int] = []
    for group in gallery_groups:
        group["first"] = len(gallery_items)
        group["count"] = len(group["items"])
        gallery_items.extend(group["items"])
    gallery_blob = bytearray()
    for group in gallery_groups:
        gallery_blob += struct.pack("<IHBB", self_text.add(group["label"]), group["first"],
                                    group["count"], 0)
    gallery_item_blob = b"".join(struct.pack("<H", i) for i in gallery_items)

    # -------- choices / maps
    choice_group_blob = bytearray()
    for group in choice_groups:
        choice_group_blob += struct.pack("<HBBHH", group["first"], group["count"], 0, group["var"], 0)
    choice_item_blob = bytearray()
    for item in choice_items:
        choice_item_blob += struct.pack("<IiI", item["text"], item["value"], item["cond"])
    map_group_blob = bytearray()
    for group in map_groups:
        map_group_blob += struct.pack("<HBBHH", group["first"], group["count"], 0, group["var"], 0)
    map_item_blob = b"".join(struct.pack("<i", v) for v in map_items)

    # map labels: key = (scene << 16) | cursor
    map_labels = []
    for key, value in cat.get("mapLabels", {}).items():
        scene_s, cursor_s = str(key).split(":", 1)
        map_labels.append(((int(scene_s) << 16) | int(cursor_s), self_text.add(str(value))))
    map_labels.sort()
    map_label_blob = b"".join(struct.pack("<II", key, off) for key, off in map_labels)

    # flow labels/dates: key = cursor in the flow scene, kind 0=label/1=date
    flow = []
    for key, value in cat.get("flowLabels", {}).items():
        flow.append((int(key), 0, self_text.add(str(value), True)))
    for key, value in cat.get("flowDates", {}).items():
        flow.append((int(key), 1, self_text.add(str(value))))
    flow.sort()
    flow_blob = b"".join(struct.pack("<IBBHI", key, kind, 0, 0, off) for key, kind, off in flow)

    # -------- progress metadata
    # Values copied verbatim from title-progress.js (source, not a build report).
    clear_flags = {4: 12003, 5: 12004, 6: 12005, 7: 12006, 8: 12007, 9: 12008,
                   10: 12573, 12: 12574, 13: 12575, 14: 12576}
    heroine_endings = [4, 5, 6, 7, 13]
    flow_endings = {222: 1, 261: 2, 289: 3, 321: 4, 350: 5, 374: 6, 405: 7,
                    441: 8, 456: 9, 489: 10, 510: 11, 523: 12, 548: 13, 566: 14}
    globals_keys = [12001, 12002, 12010, 12011, 12012, 12013]
    progress = bytearray()
    progress += struct.pack("<HBBBB", 14, len(heroine_endings), len(flow_endings),
                            len(globals_keys), 0)
    for ending in range(1, 15):
        progress += struct.pack("<IB", clear_flags.get(ending, 0), 1 if ending in heroine_endings else 0)
    for cursor, ending in sorted(flow_endings.items()):
        progress += struct.pack("<HB", cursor, ending)
    for key in globals_keys:
        progress += struct.pack("<I", key)
    progress += struct.pack("<III", 14003, 24006, 24006)

    # -------- meta
    meta = bytearray()
    meta += struct.pack("<IIII", 24006, len(var_ids), len(entries),
                        len(gallery_groups))
    meta += struct.pack("<HHHHHH", int(cat["flowScene"]), int(cat["initial"]),
                        int(cat.get("mapScene", 0)), len(scene_records),
                        len(chapter_blob) // 8, len(route_blob) // 8)
    meta += struct.pack("<IIII", len(choice_groups), len(choice_items), len(map_groups),
                        len(map_items))
    meta += struct.pack("<II", len(block_offsets), len(op_blob))
    meta += struct.pack("<IIII", self_text.add("Summer Pockets REFLECTION BLUE"),
                        self_text.add("夏日口袋 REFLECTION BLUE"),
                        self_text.add("v0.1.0-summer"),
                        self_text.add("基于 RW5 0.3.0 源快照的 AI Passport 阅读器"))
    meta += struct.pack("<HHHH", logo_index, 1, 0, 0)

    # scene index: id -> index (scene ids are 1..422 in this source)
    max_scene_id = max(scene_ids)
    scene_index = bytearray()
    index_by_id = {rec["id"]: i for i, rec in enumerate(scene_records)}
    for sid in range(max_scene_id + 1):
        scene_index += struct.pack("<H", index_by_id.get(sid, 0xFFFF))

    scene_blob = bytearray()
    for rec in scene_records:
        scene_blob += struct.pack("<HHIHHBBBBHHIIIII", rec["id"], 0, rec["first_op"],
                                  rec["op_count"], rec["next"], rec["route"] & 0xFF,
                                  rec["phase"], rec["flags"], 0, rec["first_block"],
                                  rec["block_count"], rec["name"], rec["map_name"],
                                  rec["date"], rec["date_label"], rec["route_name"])
    block_blob = b"".join(struct.pack("<I", off) for off in block_offsets)
    var_blob = b"".join(struct.pack("<I", vid) for vid in sorted(var_ids))

    expr_blob, expr_idx = compiler.finish()

    # -------- assemble
    b = Builder(args)
    b.text = self_text
    b.add(SEC_TEXT, self_text.blob(), len(self_text.codes))
    b.add(SEC_OPS, bytes(op_blob), len(op_blob))
    b.add(SEC_OPIDX, block_blob, len(block_offsets))
    b.add(SEC_SCENE, bytes(scene_blob), len(scene_records))
    b.add(SEC_CHAPTER, bytes(chapter_blob), len(chapter_blob) // 8)
    b.add(SEC_ROUTE, bytes(route_blob), len(route_blob) // 8)
    b.add(SEC_GALLERY, bytes(gallery_blob), len(gallery_groups))
    b.add(SEC_GALLERY_ITEM, gallery_item_blob, len(gallery_items))
    b.add(SEC_IMAGE, bytes(image_records), len(entries))
    b.add(SEC_IMAP, bytes(imap), max_image_id + 1)
    b.add(SEC_EXPR, expr_blob, len(expr_blob))
    b.add(SEC_EXPRIDX, expr_idx, len(compiler.programs) + 1)
    b.add(SEC_VARS, var_blob, len(var_ids))
    b.add(SEC_CHOICE, bytes(choice_group_blob), len(choice_groups))
    b.add(SEC_CHOICE_ITEM, bytes(choice_item_blob), len(choice_items))
    b.add(SEC_MAP, bytes(map_group_blob), len(map_groups))
    b.add(SEC_MAP_ITEM, bytes(map_item_blob), len(map_items))
    b.add(SEC_MAPLABEL, map_label_blob, len(map_labels))
    b.add(SEC_FLOWLABEL, flow_blob, len(flow))
    b.add(SEC_PROGRESS, bytes(progress), len(progress))
    b.add(SEC_META, bytes(meta), len(meta))
    b.add(SEC_SCENE_INDEX, bytes(scene_index), len(scene_index) // 2)
    b.add(SEC_SPEAKER, b"".join(struct.pack("<I", off) for off in speaker_offsets), len(speaker_offsets))
    b.add(SEC_IMGDATA, bytes(image_blob), len(image_blob))
    b.add(SEC_CHARTAB, self_text.chartab(), len(self_text.chars))
    pack = b.build()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    tmp = args.out + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(pack)
    os.replace(tmp, args.out)

    total_images = sum(e.size for e in entries)
    report = {
        "source": os.path.abspath(root),
        "pack": os.path.abspath(args.out),
        "pack_bytes": len(pack),
        "pack_sha256": hashlib.sha256(pack).hexdigest(),
        "scenes": len(scene_records),
        "ops": sum(rec["op_count"] for rec in scene_records),
        "text_ops": sum(1 for sid in scene_ids for op in scenes[sid] if op["op"] == "text"),
        "unique_texts": len(self_text),
        "vars": len(var_ids),
        "choices": len(choice_items),
        "map_items": len(map_items),
        "gallery_groups": len(gallery_groups),
        "gallery_items": len(gallery_items),
        "images": len(entries),
        "image_bytes": total_images,
        "image_scene_bytes": sum(e.size for e in entries if e.kind == IMG_SCENE),
        "image_portrait_bytes": sum(e.size for e in entries if e.kind == IMG_PORTRAIT),
        "image_params": {
            "scene_max": [args.scene_w, args.scene_h],
            "scene_quality": args.scene_quality,
            "portrait_box": [args.portrait_w, args.portrait_h],
            "portrait_quality": args.portrait_quality,
            "logo_box": [args.logo_w, args.logo_h],
        },
        "missing_source_ids": sorted(set(range(1, max_image_id + 1)) - set(id_to_entry)),
        "fallback_to_black": len(missing_source_paths),
        "fallback_ids": missing_source_paths,
        "sections": {str(sec): len(data) for sec, (_count, data) in sorted(b.sections.items())},
    }
    if args.report:
        os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
        with open(args.report, "w", encoding="utf-8") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=1)
    log(f"pack {len(pack)} bytes, images {total_images} bytes, {len(entries)} entries")
    log(f"art {args.scene_w}x{args.scene_h} q{args.scene_quality}; "
        f"portrait {args.portrait_w}x{args.portrait_h} q{args.portrait_quality}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", default=os.environ.get("SUMMER_SOURCE", ""),
                    help="RW5 source snapshot (contains src/ and converted-story/)")
    ap.add_argument("--out", default="main/summer_data/summer_pack.bin")
    ap.add_argument("--report", default="")
    ap.add_argument("--scene-w", type=int, default=288)
    ap.add_argument("--scene-h", type=int, default=162)
    ap.add_argument("--scene-quality", type=int, default=20)
    ap.add_argument("--portrait-w", type=int, default=140)
    ap.add_argument("--portrait-h", type=int, default=180)
    ap.add_argument("--portrait-quality", type=int, default=30)
    ap.add_argument("--logo-w", type=int, default=300)
    ap.add_argument("--logo-h", type=int, default=86)
    ap.add_argument("--logo-quality", type=int, default=80)
    args = ap.parse_args()
    if not args.source:
        ap.error("需要 --source(或 SUMMER_SOURCE)指向 RW5 源快照")
    if not os.path.isdir(args.source):
        ap.error(f"源目录不存在: {args.source}")
    return build(args)


if __name__ == "__main__":
    raise SystemExit(main())
