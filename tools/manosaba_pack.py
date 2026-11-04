#!/usr/bin/env python3
"""Pack the current manosaba watch source into the reader's flash image.

Usage:
  python tools/manosaba_pack.py --source <watch-src> --out main/gal_data/manosaba.pack
  python tools/manosaba_pack.py --source <watch-src> --dry-run

The source tree is read-only.  Every story chunk (all numeric files, not only
0.txt), every catalog scene/chapter/route, all expression literals and every
referenced image is consumed.  The output is a single little-endian binary that
the C reader maps from flash; nothing is compressed at runtime.

Image policy (no crop):
  * background / CG / trick / title art -> full 320x240 composition, fitted with
    aspect ratio preserved (black letterbox), split into a 320x150 top part and a
    160x45 bottom part (displayed 2x behind the dialogue panel).
  * book art -> fitted into the 320x150 art area (single part).
  * portraits -> fitted into 150x150, bottom aligned at runtime; JPEG colour plus
    a 1bpp alpha mask at the same resolution.
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
from collections import OrderedDict

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

MAGIC = b"MNPK0001"
VERSION = 1

SEC_META = 0
SEC_STR = 1
SEC_SCENES = 2
SEC_ROUTES = 3
SEC_CHAPTERS = 4
SEC_OPS = 5
SEC_TEXTS = 6
SEC_LINKS = 7
SEC_ITEMS = 8
SEC_POSES = 9
SEC_CHAR_DETAILS = 10
SEC_MOVES = 11
SEC_MOVE_ENTRIES = 12
SEC_VIS = 13
SEC_AREAS = 14
SEC_EVIDENCE_CATS = 15
SEC_MAP_AREAS = 16
SEC_EXPRS = 17
SEC_EXPR_DATA = 18
SEC_IMAGES = 19
SEC_IMGDATA = 20
SEC_BOOK = 21
SEC_GALLERY = 22
SEC_GALLERY_ITEMS = 23
SEC_VARS = 24
SEC_LINK_OPS = 25
SEC_MOVE_OPS = 26
SEC_BOOKKEYS = 27
SEC_STROFF = 28
SECTION_COUNT = 29

# op codes (must match main/gal_pack.h)
OP_TEXT = 0
OP_CHAR = 1
OP_BG = 2
OP_FOCUS = 3
OP_CALL = 4
OP_JUMP = 5
OP_HIDE_ALL = 6
OP_OFFER = 7
OP_VISIBILITY = 8
OP_RETURN = 9
OP_DAMAGE = 10
OP_CHOICE = 11
OP_CANCEL_CALL = 12
OP_IF = 13
OP_SET = 14
OP_AUTOSAVE = 15
OP_RESET_BOOK = 16
OP_CLUE_HIDE = 17
OP_CLUE_IMAGE = 18
OP_EVIDENCE = 19
OP_FLAG = 20
OP_END = 21
OP_RECORD_CHOICE = 22
OP_MAP = 23
OP_STOP = 24
OP_BOOK_CLEAR = 25
OP_EXECUTION = 26
OP_PAUSE = 27
OP_DYNAMIC_JUMP = 28
OP_BOOK_UPDATE = 29

# expression bytecode.  Operands are little endian:
#   X_PUSH_NUM <f32>    X_PUSH_STR <u16 id>    X_PUSH_VAR <u16 id>
#   X_MAX/X_MIN <u8 extra-args>; everything else has no operand.
X_PUSH_NUM = 1
X_PUSH_STR = 2
X_PUSH_VAR = 3
X_PUSH_TRUE = 4
X_PUSH_FALSE = 5
X_NEG = 6
X_NOT = 7
X_ADD = 8
X_SUB = 9
X_MUL = 10
X_DIV = 11
X_MOD = 12
X_EQ = 13
X_NE = 14
X_LT = 15
X_LE = 16
X_GT = 17
X_GE = 18
X_AND = 19
X_OR = 20
X_MAX = 21
X_MIN = 22
X_SIN = 23
X_COS = 24
X_ABS = 25

CATEGORY_IDS = {"Clue": 0, "Profile": 1, "Rule": 2, "Note": 3, "Map": 4}

ART_W, ART_H = 320, 150
STRIP_W, STRIP_H = 160, 45
SCREEN_W, SCREEN_H = 320, 240
VIEW_H = 176  # visible game/CG viewport: 0..175; panel starts at 176
PORTRAIT_W, PORTRAIT_H = 150, 150


def die(msg: str) -> None:
    print("ERROR:", msg, file=sys.stderr)
    raise SystemExit(1)


def read_json_file(path: str):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def load_catalog(root: str):
    text = open(os.path.join(root, "src/common/catalog.js"), encoding="utf-8").read()
    m = re.search(r"export const catalog=(\{.*\});\s*$", text, re.S)
    if not m:
        die("catalog.js: export const catalog=... not found")
    return json.loads(m.group(1))


class Pool:
    def __init__(self):
        self.order = []
        self.index = {}

    def add(self, s):
        if s is None:
            s = ""
        s = str(s)
        if s in self.index:
            return self.index[s]
        idx = len(self.order)
        self.index[s] = idx
        self.order.append(s)
        return idx

    def blob(self):
        out = bytearray()
        for s in self.order:
            out += s.encode("utf-8") + b"\0"
        return bytes(out)


class TextPool:
    def __init__(self):
        self.units = []
        self.by_text = {}

    def add(self, s):
        if s is None:
            s = ""
        if s in self.by_text:
            return self.by_text[s]
        off = len(self.units)
        for ch in s:
            cp = ord(ch)
            if cp > 0xFFFF:
                die("non-BMP character in story text (U+%04X)" % cp)
            if 0xD800 <= cp <= 0xDFFF:
                die("surrogate in story text")
            self.units.append(cp)
        rec = (off, len(s))
        self.by_text[s] = rec
        return rec

    def blob(self):
        return struct.pack("<%dH" % len(self.units), *self.units) if self.units else b""


TOKEN_RE = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    r"|\d+(?:\.\d+)?|[A-Za-z_][A-Za-z_0-9]*"
    r"|&&|\|\||==|!=|>=|<=|[()!,+*/%<>=-]"
)
PREC = {"||": 1, "&&": 2, "=": 3, "==": 3, "!=": 3, ">": 4, "<": 4, ">=": 4,
        "<=": 4, "+": 5, "-": 5, "*": 6, "/": 6, "%": 6}
FUNCS = {"max": (X_MAX, 1), "min": (X_MIN, 1), "Sin": (X_SIN, 0), "Cos": (X_COS, 0),
         "abs": (X_ABS, 0)}


class VarsTable:
    def __init__(self, strings: Pool):
        self.strings = strings
        self.names = []
        self.index = {}

    def id(self, name):
        if name in self.index:
            return self.index[name]
        if len(self.names) >= 200:
            die("too many variables")
        self.index[name] = len(self.names)
        self.names.append(name)
        return self.index[name]


class ExprCompiler:
    def __init__(self, strings: Pool, vars_table: VarsTable):
        self.strings = strings
        self.vars = vars_table
        self.by_source = {}
        self.by_hash = {}
        self.blobs = []

    def add_blob(self, code: bytes) -> int:
        h = hashlib.sha256(code).digest()
        if h in self.by_hash:
            return self.by_hash[h]
        idx = len(self.blobs)
        self.blobs.append(code)
        self.by_hash[h] = idx
        return idx

    def compile(self, source) -> int:
        key = "0" if source is None else str(source)
        if key in self.by_source:
            return self.by_source[key]
        idx = self._compile(key)
        self.by_source[key] = idx
        return idx

    def _compile(self, key: str) -> int:
        code = bytearray()
        s = key.replace("{", "").replace("}", "")
        tokens = TOKEN_RE.findall(s)
        if "".join(tokens).replace(" ", "") != s.replace(" ", ""):
            die("unsupported expression syntax: %r" % key)
        pos = 0

        def emit_f32(value: float):
            code.append(X_PUSH_NUM)
            code.extend(struct.pack("<f", float(value)))

        def emit(op, operand=None):
            code.append(op)
            if operand is None:
                return
            if op == X_PUSH_STR or op == X_PUSH_VAR:
                code.extend(struct.pack("<H", operand))
            elif op in (X_MAX, X_MIN):
                code.append(operand)

        def primary():
            nonlocal pos
            if pos >= len(tokens):
                die("incomplete expression: %r" % key)
            tok = tokens[pos]
            pos += 1
            if tok == "!":
                primary()
                emit(X_NOT)
                return
            if tok == "-":
                primary()
                emit(X_NEG)
                return
            if tok == "+":
                primary()
                return
            if tok == "(":
                parse(0)
                if pos >= len(tokens) or tokens[pos] != ")":
                    die("unbalanced parentheses: %r" % key)
                pos += 1
                return
            if tok[0] in "\"'":
                lit = tok[1:-1].replace('\\"', '"').replace("\\'", "'")
                emit(X_PUSH_STR, self.strings.add(lit))
                return
            if re.match(r"^\d", tok):
                emit_f32(float(tok))
                return
            if tok.lower() == "true":
                emit(X_PUSH_TRUE)
                return
            if tok.lower() == "false":
                emit(X_PUSH_FALSE)
                return
            if pos < len(tokens) and tokens[pos] == "(":
                pos += 1
                argc = 0
                if pos < len(tokens) and tokens[pos] != ")":
                    parse(0)
                    argc += 1
                    while pos < len(tokens) and tokens[pos] == ",":
                        pos += 1
                        parse(0)
                        argc += 1
                if pos >= len(tokens) or tokens[pos] != ")":
                    die("bad function call: %r" % key)
                pos += 1
                if tok not in FUNCS:
                    die("unsupported function %s in %r" % (tok, key))
                op, min_args = FUNCS[tok]
                if argc < min_args:
                    die("function %s needs arguments" % tok)
                if op in (X_MAX, X_MIN):
                    if argc > 16:
                        die("too many max/min arguments")
                    emit(op, argc - 2)
                else:
                    if argc != 1:
                        die("function %s takes one argument" % tok)
                    emit(op)
                return
            emit(X_PUSH_VAR, self.vars.id(tok))

        def parse(min_prec):
            nonlocal pos
            primary()
            while pos < len(tokens) and tokens[pos] in PREC:
                op = tokens[pos]
                prec = PREC[op]
                if prec < min_prec:
                    break
                pos += 1
                parse(prec + 1)
                emit({"||": X_OR, "&&": X_AND, "=": X_EQ, "==": X_EQ, "!=": X_NE,
                      ">": X_GT, "<": X_LT, ">=": X_GE, "<=": X_LE, "+": X_ADD,
                      "-": X_SUB, "*": X_MUL, "/": X_DIV, "%": X_MOD}[op])

        if not tokens:
            emit(X_PUSH_FALSE)
        else:
            parse(0)
            if pos != len(tokens):
                die("trailing tokens in expression %r" % key)
        return self.add_blob(bytes(code))

    def finish(self):
        data = bytearray()
        offs = []
        for blob in self.blobs:
            offs.append((len(data), len(blob)))
            data += blob
        return bytes(data), offs


class ImagePacker:
    def __init__(self, images_root: str, ui_root: str, art_quality: int, portrait_quality: int):
        if Image is None:
            die("Pillow is required for image packing (pip install pillow)")
        self.images_root = images_root
        self.ui_root = ui_root
        self.art_quality = art_quality
        self.portrait_quality = portrait_quality
        self.records = []
        self.map = {}
        self.data = bytearray()

    def index(self, name: str) -> int:
        if name in self.map:
            return self.map[name]
        idx = len(self.records)
        self.map[name] = idx
        self.records.append({"name": name, "off": 0, "len": 0, "aux_off": 0,
                             "aux_len": 0, "mask_off": 0, "mask_len": 0,
                             "w": 0, "h": 0, "kind": 0, "filled": False})
        return idx

    def _append(self, top: bytes, aux: bytes, mask: bytes):
        off = len(self.data)
        self.data.extend(top)
        aux_off = len(self.data)
        if aux:
            self.data.extend(aux)
        mask_off = len(self.data)
        if mask:
            self.data.extend(mask)
        return off, aux_off, mask_off

    def fill(self, name: str):
        rec = self.records[self.map[name]]
        if rec["filled"]:
            return
        if name.startswith("title-") and os.path.exists(os.path.join(self.ui_root, name)):
            path = os.path.join(self.ui_root, name)
            top, bottom = self._title(path, quality=78)
            off, aux_off, mask_off = self._append(top, bottom, b"")
            rec.update(off=off, len=len(top), aux_off=aux_off, aux_len=len(bottom),
                       mask_off=mask_off, mask_len=0, w=ART_W, h=ART_H, kind=0)
        else:
            path = os.path.join(self.images_root, name)
            if not os.path.exists(path):
                die("missing image %s" % path)
            im = Image.open(path)
            if name.startswith("p_"):
                top, size, mask = self._portrait(im)
                off, aux_off, mask_off = self._append(top, b"", mask)
                rec.update(off=off, len=len(top), aux_off=aux_off, aux_len=0,
                           mask_off=mask_off, mask_len=len(mask), w=size[0], h=size[1], kind=2)
            elif name.startswith("book_"):
                top = self._art(im)
                off, aux_off, mask_off = self._append(top, b"", b"")
                rec.update(off=off, len=len(top), aux_off=aux_off, aux_len=0,
                           mask_off=mask_off, mask_len=0, w=ART_W, h=ART_H, kind=1)
            else:
                top, bottom = self._full(path, quality=self.art_quality)
                off, aux_off, mask_off = self._append(top, bottom, b"")
                rec.update(off=off, len=len(top), aux_off=aux_off, aux_len=len(bottom),
                           mask_off=mask_off, mask_len=0, w=ART_W, h=ART_H, kind=0)
        rec["filled"] = True

    def fill_all(self):
        for rec in self.records:
            self.fill(rec["name"])

    def _full(self, path, quality):
        im = Image.open(path).convert("RGB")
        w, h = im.size
        # Top-aligned cover for the 320x176 viewport.  The old code fitted the
        # whole 320x240 canvas (contain) and centred it, leaving top letterbox;
        # that is why game backgrounds appeared low.  Cover scales to fill both
        # axes, crops horizontally around the centre, and crops the bottom.
        s = max(SCREEN_W / w, VIEW_H / h)
        nw, nh = max(1, round(w * s)), max(1, round(h * s))
        cover = im.resize((nw, nh), Image.LANCZOS)
        left = max(0, (nw - SCREEN_W) // 2)
        cover = cover.crop((left, 0, left + SCREEN_W, VIEW_H))
        canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
        canvas.paste(cover, (0, 0))
        top = canvas.crop((0, 0, ART_W, ART_H))
        bottom = canvas.crop((0, ART_H, SCREEN_W, SCREEN_H)).resize((STRIP_W, STRIP_H), Image.LANCZOS)
        return _jpeg(top, quality), _jpeg(bottom, quality)

    def _title(self, path, quality):
        im = Image.open(path).convert("RGB")
        # The watch title art has the original 432x514 portrait composition with
        # baked-in touch buttons in its lower part.  Crop that decorative footer
        # (not story art) and fit the character group into the landscape screen.
        if im.height > 340:
            im = im.crop((0, 0, im.width, 340))
        w, h = im.size
        sc = min(SCREEN_W / w, SCREEN_H / h)
        nw, nh = max(1, round(w * sc)), max(1, round(h * sc))
        canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
        canvas.paste(im.resize((nw, nh), Image.LANCZOS), ((SCREEN_W - nw) // 2, (SCREEN_H - nh) // 2))
        top = canvas.crop((0, 0, ART_W, ART_H))
        bottom = canvas.crop((0, ART_H, SCREEN_W, SCREEN_H)).resize((STRIP_W, STRIP_H), Image.LANCZOS)
        return _jpeg(top, quality), _jpeg(bottom, quality)

    def _art(self, im):
        im = im.convert("RGBA")
        w, h = im.size
        s = min((ART_W - 2) / w, (ART_H - 2) / h)
        fitted = im.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)
        canvas = Image.new("RGB", (ART_W, ART_H), (0, 0, 0))
        canvas.paste(fitted, ((ART_W - fitted.width) // 2, (ART_H - fitted.height) // 2), fitted)
        return _jpeg(canvas, self.art_quality)

    def _portrait(self, im):
        im = im.convert("RGBA")
        w, h = im.size
        s = min(PORTRAIT_W / w, PORTRAIT_H / h)
        fitted = im.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)
        rgb = Image.new("RGB", fitted.size, (0, 0, 0))
        rgb.paste(fitted, mask=fitted.split()[3])
        alpha = fitted.split()[3].point(lambda v: 255 if v >= 96 else 0)
        bw, bh = fitted.size
        stride = (bw + 7) // 8
        data = bytearray(stride * bh)
        px = alpha.load()
        for y in range(bh):
            base = y * stride
            for x in range(bw):
                if px[x, y]:
                    data[base + (x >> 3)] |= 0x80 >> (x & 7)
        return _jpeg(rgb, self.portrait_quality), (bw, bh), bytes(data)


def _jpeg(im, quality):
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality, optimize=True, subsampling=2)
    return buf.getvalue()


def build(args):
    root = os.path.abspath(args.source)
    catalog = load_catalog(root)
    story_root = os.path.join(root, "src/common/story")
    images_root = os.path.join(root, "src/common/images")
    ui_root = os.path.join(root, "src/common/ui")
    book_catalog = read_json_file(os.path.join(root, "src/common/book.txt"))
    gallery_catalog = read_json_file(os.path.join(root, "src/common/gallery.txt"))

    strings = Pool()
    texts = TextPool()
    vars_table = VarsTable(strings)
    expr = ExprCompiler(strings, vars_table)
    images = ImagePacker(images_root, ui_root, args.art_quality, args.portrait_quality)

    scene_names = list(catalog["scenes"].keys())
    scene_index = {name: i for i, name in enumerate(scene_names)}
    if catalog["initial"] not in scene_index:
        die("initial scene missing from catalog")
    routes = catalog["routes"]
    chapters = catalog["chapters"]
    areas = catalog["areas"]
    area_ids = list(areas.keys())
    area_index = {a: i for i, a in enumerate(area_ids)}

    # ---- load story chunks -------------------------------------------------
    chunk_files = []
    for scene in scene_names:
        d = os.path.join(story_root, scene)
        if not os.path.isdir(d):
            die("missing story directory %s" % scene)
        nums = sorted(int(f[:-4]) for f in os.listdir(d) if re.fullmatch(r"\d+\.txt", f))
        if nums != list(range(len(nums))) or not nums:
            die("scene %s has non-contiguous/empty chunks: %s" % (scene, nums))
        ops = []
        for n in nums:
            data = read_json_file(os.path.join(d, "%d.txt" % n))
            chunk_ops = data["ops"] if isinstance(data, dict) else data
            if not isinstance(chunk_ops, list) or len(chunk_ops) > 512:
                die("scene %s chunk %d invalid" % (scene, n))
            ops.extend(chunk_ops)
            chunk_files.append(os.path.join(d, "%d.txt" % n))
        want = catalog["scenes"][scene]["length"]
        if len(ops) != want:
            die("scene %s length mismatch: chunks=%d catalog=%d" % (scene, len(ops), want))
        catalog["scenes"][scene]["ops"] = ops

    assets = open(os.path.join(root, "src/common/assets.js"), encoding="utf-8").read()
    bg_map = json.loads(re.search(r"const bg=(\{.*?\});", assets, re.S).group(1))
    portrait_map = json.loads(re.search(r"const portraits=(\{.*?\});", assets, re.S).group(1))
    book_image_map = json.loads(re.search(r"const bookImages=(\{.*?\});", assets, re.S).group(1))
    FACE_RE = re.compile(r"^(Normal|Smile|Angry|Pensive|Cry|Flushed|Surprised|Fearful|Determined|Default)\d*$")

    def portrait_for(actor, appearance):
        base = str(actor).split("-")[0].split(",")[0].lower()
        face = None
        for part in str(appearance or "").split(","):
            if FACE_RE.match(part) or re.fullmatch(r"\d+", part):
                face = part
                break
        if face:
            src = portrait_map.get(base + ":" + face)
            if not src:
                src = portrait_map.get(base + ":Default")
            if src:
                return images.index(src), face
        return 0xFFFF, face

    def default_portrait(actor):
        src = portrait_map.get(str(actor).split("-")[0].split(",")[0].lower() + ":Default")
        return images.index(src) if src else 0xFFFF

    book_key_order = sorted({(e["category"], str(e["id"])) for e in book_catalog.values()},
                            key=lambda x: (CATEGORY_IDS[x[0]], x[1]))
    book_key_index = {k: i for i, k in enumerate(book_key_order)}
    book_key_names = {}
    for key, entry in book_catalog.items():
        k = (entry["category"], str(entry["id"]))
        book_key_names.setdefault(k, entry.get("name", ""))
    links = []
    items = []
    poses = []
    pose_map = {}
    char_details = []
    char_detail_map = {}
    vis = []
    vis_lists = {}
    move_entries = []
    moves = []
    move_map = {}
    move_ops = []
    link_ops = []
    ops_out = []
    scene_records = []
    evidence_cats = []
    map_areas = []

    def lookup_scene(name):
        if name not in scene_index:
            die("jump target scene %r is not in catalog" % name)
        return scene_index[name]

    def add_pose(actor, appearance, sub):
        key = (actor, appearance, sub)
        if key not in pose_map:
            face_img, face = portrait_for(actor, appearance)
            # Only the face token is stored: the raw source appearance string is a
            # long Unity layer list (up to 240 bytes) and the reader only needs the
            # resolved portrait image.  Keep the token for audit/readability.
            key_record = (actor, face or "", sub, face_img, default_portrait(actor))
            pose_map[key] = len(poses)
            poses.append(key_record)
        return pose_map[key]

    def compile_vector(value, count):
        if value is None:
            return [0xFFFF] * count
        if isinstance(value, dict):
            value = value.get("expr", "")
        if isinstance(value, list):
            parts = [None if v is None else str(v.get("expr", "") if isinstance(v, dict) else v)
                     for v in value]
        else:
            raw = str(value)
            parts = []
            depth = 0
            start = 0
            for i, ch in enumerate(raw):
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                elif ch == "," and depth == 0:
                    parts.append(raw[start:i])
                    start = i + 1
            parts.append(raw[start:])
        out = []
        for i in range(count):
            part = parts[i] if i < len(parts) else None
            if part is None or part.strip() in ("", "undefined"):
                out.append(0xFFFF)
            else:
                out.append(expr.compile(part.strip()))
        return out

    def compile_move(mv):
        clear = 1 if mv.get("clearChoices") not in (None, False, 0) else 0
        entries = []
        for actor in mv.get("actors") or []:
            params = mv.get("params") or {}
            for key, kind in (("ScenePositionX", 0), ("ScenePositionY", 1),
                              ("PositionY", 2), ("Scale", 3)):
                if key in params:
                    raw = params[key]
                    if isinstance(raw, dict):
                        raw = raw.get("expr", "")
                    entries.append((strings.add(actor), kind,
                                    expr.compile(str(raw).split("|")[-1])))
        return clear, entries

    def rec(opc, a=0, b=0, c=0, d=0, flags=0):
        ops_out.append((opc, flags & 0xFF, a & 0xFFFF, b & 0xFFFF, c & 0xFFFF, d & 0xFFFFFFFF))
        return len(ops_out) - 1

    for scene_pos, scene in enumerate(scene_names):
        start_op = len(ops_out)
        scene_len = len(catalog["scenes"][scene]["ops"])
        for op in catalog["scenes"][scene]["ops"]:
            kind = op["op"]
            if op.get("moves"):
                clear = 0
                entries = []
                for mv in op["moves"]:
                    c, e = compile_move(mv)
                    clear |= c
                    entries.extend(e)
                key = (clear, tuple(entries))
                if key not in move_map:
                    move_map[key] = len(moves)
                    moves.append({"first": len(move_entries), "count": len(entries), "clear": clear})
                    move_entries.extend(entries)
                move_ops.append((len(ops_out), move_map[key]))

            if kind == "text":
                tok, tlen = texts.add(op.get("text", ""))
                speaker = strings.add(op.get("speaker", ""))
                actor = strings.add(op.get("actor", ""))
                lk = op.get("links") or []
                if lk:
                    first = len(links)
                    for link in lk:
                        target = link.get("target")
                        if not isinstance(target, list) or len(target) != 2:
                            die("text link target is not [scene,cursor]")
                        loff, llen = texts.add(link.get("text", ""))
                        links.append((lookup_scene(target[0]), int(target[1]), loff, llen))
                    link_ops.append((len(ops_out), first, len(lk)))
                rec(OP_TEXT, a=speaker, b=tlen, c=actor, d=tok,
                    flags=1 if op.get("debate") else 0)
            elif kind == "char":
                pose = add_pose(op["actor"], op.get("appearance") or "", op.get("sub") or "")
                vector = compile_vector(op.get("scene"), 2)
                vector.append(compile_vector(op.get("position"), 3)[1])
                vector.append(compile_vector(op.get("scale"), 3)[0])
                key = (pose, tuple(vector), 1 if op.get("visible") else 0)
                if key not in char_detail_map:
                    char_detail_map[key] = len(char_details)
                    char_details.append(key)
                rec(OP_CHAR, a=char_detail_map[key], flags=1 if op.get("visible") else 0)
            elif kind == "bg":
                actor = op.get("actor", "")
                image = op.get("image", "")
                if image == "SolidColor":
                    # scene.js:9 recognizes only exact #000000 as black;
                    # #000000FF is white in this source.
                    src = "solid_black.png" if str(op.get("color", "")).lower() == "#000000" else "solid_white.png"
                elif image == "Transparent":
                    src = "solid_black.png"
                else:
                    prefix = "stills:" if actor == "Stills" else "tricks:" if actor == "Tricks" else "mainbackground:"
                    src = bg_map.get(prefix + image)
                    if not src:
                        die("bg %s/%s has no asset mapping" % (actor, image))
                rec(OP_BG, a=strings.add(actor), b=images.index(src), c=strings.add(op.get("sub") or ""),
                    flags=1 if op.get("visible") else 0)
            elif kind == "focus":
                rec(OP_FOCUS, a=strings.add(op.get("actor", "")))
            elif kind in ("call", "jump"):
                target = op.get("target")
                if isinstance(target, dict) and "expr" in target:
                    if kind == "call":
                        die("dynamic call target is unsupported")
                    rec(OP_DYNAMIC_JUMP, a=strings.add(str(target["expr"])))
                else:
                    if not isinstance(target, list) or len(target) != 2:
                        die("bad %s target: %r" % (kind, target))
                    scen = lookup_scene(target[0])
                    cur = int(target[1])
                    if cur < 0 or cur >= catalog["scenes"][target[0]]["length"]:
                        die("%s target cursor out of range: %r" % (kind, target))
                    rec(OP_CALL if kind == "call" else OP_JUMP, a=scen, b=cur)
            elif kind == "hideAll":
                rec(OP_HIDE_ALL, flags=1 if op.get("all") else 0)
            elif kind == "offer":
                if op.get("when"):
                    die("offer with conditions is not supported")
                label = strings.add(op.get("text", ""))
                to, after = int(op["to"]), int(op["after"])
                if not (0 <= to < scene_len and 0 <= after < scene_len):
                    die("offer target out of range in %s" % scene)
                rec(OP_OFFER, a=to, b=after, c=int(op["menu"]), d=label,
                    flags=1 if op.get("lock") else 0)
            elif kind == "visibility":
                actors = tuple(op.get("actors") or [])
                if actors not in vis_lists:
                    vis_lists[actors] = len(vis)
                    for actor in actors:
                        vis.append(strings.add(actor))
                rec(OP_VISIBILITY, a=vis_lists[actors], b=len(actors),
                    flags=1 if op.get("visible") else 0)
            elif kind == "return":
                rec(OP_RETURN)
            elif kind == "damage":
                rec(OP_DAMAGE)
            elif kind == "choice":
                first = len(items)
                for it in op.get("items") or []:
                    tok, tlen = texts.add(it.get("text", ""))
                    to, end = int(it["to"]), int(it["end"])
                    if not (0 <= to < scene_len and 0 <= end <= scene_len):
                        die("choice target out of range in %s" % scene)
                    whens = it.get("when") or []
                    if len(whens) > 1:
                        die("choice item with multiple conditions")
                    wid = expr.compile(whens[0]) if whens else 0xFFFF
                    items.append((lookup_scene(scene), to, end, tok, tlen, wid,
                                  1 if it.get("kind") == "branch" else 0,
                                  1 if it.get("lock") else 0))
                rec(OP_CHOICE, a=first, b=len(op.get("items") or []), c=int(op.get("menu", 0)),
                    flags=1 if op.get("clear") else 0)
            elif kind == "cancelCall":
                rec(OP_CANCEL_CALL)
            elif kind == "if":
                esrc = op["expr"].get("expr", "") if isinstance(op["expr"], dict) else op["expr"]
                rec(OP_IF, a=expr.compile(esrc), b=int(op["to"]))
            elif kind == "set":
                esrc = op["expr"].get("expr", "") if isinstance(op["expr"], dict) else op["expr"]
                m = re.match(r"\s*([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.+)$", str(esrc))
                if not m:
                    die("bad assignment: %r" % esrc)
                rec(OP_SET, a=vars_table.id(m.group(1)), b=expr.compile(m.group(2)))
            elif kind == "autosave":
                rec(OP_AUTOSAVE)
            elif kind == "resetBook":
                target = op.get("target")
                if not isinstance(target, list) or len(target) != 2:
                    die("bad resetBook target")
                rec(OP_RESET_BOOK, a=lookup_scene(target[0]), b=int(target[1]),
                    flags=1 if op.get("onlyDirect") else 0)
            elif kind == "clueHide":
                rec(OP_CLUE_HIDE)
            elif kind == "clueImage":
                src = book_image_map.get("Clue:" + str(op.get("id")))
                if not src:
                    die("clueImage %r has no book image" % op.get("id"))
                rec(OP_CLUE_IMAGE, a=images.index(src), b=strings.add(str(op.get("id"))))
            elif kind == "evidence":
                first = len(evidence_cats)
                for cat in op.get("categories") or []:
                    if cat not in CATEGORY_IDS:
                        die("unknown evidence category %r" % cat)
                    evidence_cats.append(CATEGORY_IDS[cat])
                rec(OP_EVIDENCE, a=first, b=len(op.get("categories") or []))
            elif kind == "bookUpdate":
                cat = op["category"]
                if cat not in CATEGORY_IDS:
                    die("unknown book category %r" % cat)
                rec(OP_BOOK_UPDATE, a=book_key_index[(cat, str(op["id"]))],
                    b=int(op.get("version", 0)) & 0xFFFF)
            elif kind == "flag":
                rec(OP_FLAG, a=vars_table.id(op["variable"]), b=int(op["number"]))
            elif kind == "end":
                rec(OP_END, a=strings.add(op.get("kind", "end")))
            elif kind == "recordChoice":
                rec(OP_RECORD_CHOICE, a=int(op["number"]))
            elif kind == "map":
                first = len(map_areas)
                for area in op.get("areas") or []:
                    if area not in area_index:
                        die("map area %r missing from catalog" % area)
                    map_areas.append(area_index[area])
                rec(OP_MAP, a=first, b=len(op.get("areas") or []), c=vars_table.id(op.get("flag", "")))
            elif kind == "stop":
                rec(OP_STOP, a=strings.add(op.get("kind", "stop")))
            elif kind == "bookClear":
                rec(OP_BOOK_CLEAR)
            elif kind == "execution":
                rec(OP_EXECUTION)
            elif kind == "pause":
                tok, tlen = texts.add(op.get("text", "点击继续"))
                rec(OP_PAUSE, b=tlen, d=tok)
            else:
                die("unknown op %r in %s" % (kind, scene))
        scene_records.append((start_op, len(ops_out) - start_op))

    # ---- book metadata ----------------------------------------------------
    book_records = []
    for key, entry in book_catalog.items():
        cat = entry["category"]
        name_off, name_len = texts.add(entry.get("name", ""))
        text_off, text_len = texts.add(entry.get("text", ""))
        src = book_image_map.get(cat + ":" + str(entry.get("id")))
        img = images.index(src) if src else 0xFFFF
        book_records.append((CATEGORY_IDS[cat], strings.add(str(entry.get("id"))),
                             name_off, name_len, text_off, text_len,
                             int(entry.get("version", 1)), img))
    book_records.sort(key=lambda r: (r[0], strings.order[r[1]], r[6]))

    # ---- gallery ----------------------------------------------------------
    gallery_groups = []
    gallery_group_map = {}
    gallery_items = []
    for item in gallery_catalog:
        group = item.get("group") or item.get("source")
        if group not in gallery_group_map:
            gallery_group_map[group] = len(gallery_groups)
            label_off, label_len = texts.add(item.get("label", group))
            gallery_groups.append({"label_off": label_off, "label_len": label_len,
                                   "first": len(gallery_items), "count": 0})
        g = gallery_group_map[group]
        gallery_items.append((images.index(item["image"]), g))
        gallery_groups[g]["count"] += 1
    if len(gallery_groups) != 61 or len(gallery_items) != 172:
        print("WARN gallery groups/items: %d/%d" % (len(gallery_groups), len(gallery_items)))

    # ---- title art --------------------------------------------------------
    title_a = images.index("title-1.jpg")
    title_b = images.index("title-2.jpg") if os.path.exists(os.path.join(ui_root, "title-2.jpg")) else title_a

    # Include every source image, not only the ones referenced by the current
    # op stream: expressions that are currently unreachable still belong to the
    # source package and must not silently disappear.
    for name in sorted(os.listdir(images_root)):
        if os.path.isfile(os.path.join(images_root, name)):
            images.index(name)

    # ---- fill all referenced images ---------------------------------------
    images.fill_all()

    # ---- serialize --------------------------------------------------------
    scenes_bin = bytearray()
    for i, scene in enumerate(scene_names):
        info = catalog["scenes"][scene]
        start, count = scene_records[i]
        sflags = 0
        if scene.startswith("System_"):
            sflags |= 1
        if re.search(r"_Trial\d", scene):
            sflags |= 2
        if scene.endswith("_TrialInit"):
            sflags |= 4
        scenes_bin += struct.pack("<IIHHII", strings.add(scene), strings.add(info["name"]),
                                  int(info.get("route", -1)) + 1, sflags, start, count)

    routes_bin = bytearray()
    for route in routes:
        ids = route.get("scenes") or []
        first_entry = None
        if ids:
            for i, ch in enumerate(chapters):
                if ch["id"] == ids[0]:
                    first_entry = i
                    break
        if first_entry is None:
            die("route %r first scene is not a chapter entry" % route.get("name"))
        # verify the route's scenes are exactly the contiguous chapter slice
        slice_ids = [ch["id"] for ch in chapters[first_entry:first_entry + len(ids)]]
        if slice_ids != ids:
            die("route %r chapter slice is not contiguous" % route.get("name"))
        routes_bin += struct.pack("<IHH", strings.add(route.get("name", "")), first_entry, len(ids))

    chapters_bin = bytearray()
    for ch in chapters:
        if ch["id"] not in scene_index:
            die("chapter entry %r has no scene" % ch["id"])
        chapters_bin += struct.pack("<IIIH", scene_index[ch["id"]],
                                    strings.add(ch.get("name", "")),
                                    int(ch.get("route", -1)) + 1, 0)

    ops_bin = bytearray()
    for opc, flags, a, b, c, d in ops_out:
        ops_bin += struct.pack("<BBHHHI", opc, flags, a, b, c, d)

    links_bin = bytearray()
    for scene, cursor, off, length in links:
        links_bin += struct.pack("<HHIHH", scene, cursor, off, length, 0)
    items_bin = bytearray()
    for sid, to, end, tok, tlen, wid, branch, lock in items:
        items_bin += struct.pack("<IHHHHHBB", tok, tlen, to, end, wid, sid, branch, lock)
    poses_bin = bytearray()
    for actor, appearance, sub, face_img, default_img in poses:
        poses_bin += struct.pack("<IIIIHH", strings.add(actor), strings.add(appearance),
                                 strings.add(sub), 0, face_img, default_img)
    char_bin = bytearray()
    for pose, vector, visible in char_details:
        sx, sy, py, scale = vector
        char_bin += struct.pack("<HHHHHBx", pose, sx, sy, py, scale, visible)
    moves_bin = bytearray()
    for mv in moves:
        moves_bin += struct.pack("<HHBx", mv["first"], mv["count"], mv["clear"])
    move_entries_bin = bytearray()
    for actor, kind, eid in move_entries:
        move_entries_bin += struct.pack("<IHBx", actor, eid, kind)
    vis_bin = bytearray()
    for s in vis:
        vis_bin += struct.pack("<H", s)
    areas_bin = bytearray()
    for aid in area_ids:
        areas_bin += struct.pack("<II", strings.add(aid), strings.add(areas[aid]))
    evcats_bin = bytes(evidence_cats)
    map_areas_bin = bytearray()
    for a in map_areas:
        map_areas_bin += struct.pack("<H", a)
    image_gallery = {}
    for gi, (img, _group) in enumerate(gallery_items):
        image_gallery.setdefault(img, gi)
    images_bin = bytearray()
    for idx, rec in enumerate(images.records):
        images_bin += struct.pack("<IIIIIIIIHHHBB", strings.add(rec["name"]),
                                  rec["off"], rec["len"], rec["aux_off"], rec["aux_len"],
                                  rec["mask_off"], rec["mask_len"],
                                  image_gallery.get(idx, 0xFFFF),
                                  rec["w"], rec["h"], 0, rec["kind"], 0)
    book_bin = bytearray()
    for cat, id_str, noff, nlen, toff, tlen, ver, img in book_records:
        book_bin += struct.pack("<IIIHHIHHHH", cat, id_str, noff, nlen, 0, toff, tlen, ver, img, 0)
    bookkeys_bin = bytearray()
    rec_cursor = 0
    for cat, bid in book_key_order:
        want_cat = CATEGORY_IDS[cat]
        want_id = strings.add(bid)
        first = rec_cursor
        while rec_cursor < len(book_records) and book_records[rec_cursor][0] < want_cat:
            rec_cursor += 1
        first = rec_cursor
        count = 0
        while rec_cursor + count < len(book_records):
            r = book_records[rec_cursor + count]
            if r[0] != want_cat or r[1] != want_id:
                break
            count += 1
        if count == 0:
            die("book key %s/%s has no entries" % (cat, bid))
        noff, nlen = texts.add(book_key_names[(cat, bid)])
        bookkeys_bin += struct.pack("<IIIHHHH", want_cat, want_id, noff, nlen, first, count, 0)
        rec_cursor += count
    if rec_cursor != len(book_records):
        die("book records were not fully grouped")
    gallery_bin = bytearray()
    for g in gallery_groups:
        gallery_bin += struct.pack("<IHHH", g["label_off"], g["label_len"], g["first"], g["count"])
    gallery_items_bin = bytearray()
    for img, g in gallery_items:
        gallery_items_bin += struct.pack("<HH", img, g)
    DEFAULT_VALUES = {"maxHp": 10.0, "currentHp": 10.0, "g_pi": 3.141592653589793,
                      "hasWitchBook": 1.0}
    vars_bin = bytearray()
    for name in vars_table.names:
        if name in ("hasWitchBook",):
            vtype = 2
        elif name in ("isDebugDirectJump",):
            vtype = 2
        elif name in ("stageName", "chosenAreaId", "chosenEvidenceId"):
            vtype = 1
        else:
            vtype = 0
        default = DEFAULT_VALUES.get(name, 0.0)
        vars_bin += struct.pack("<IBBxxf", strings.add(name), vtype, 0, default)
    link_ops_bin = bytearray()
    for op_idx, first, count in link_ops:
        link_ops_bin += struct.pack("<IHBx", op_idx, first, count)
    move_ops_bin = bytearray()
    for op_idx, midx in move_ops:
        move_ops_bin += struct.pack("<IH", op_idx, midx)

    # engine.js can end with kind='trialFail' even though no source END/STOP op
    # carries that literal; reserve it in the string table so C can expose the
    # exact end kind without inventing a numeric sentinel.
    strings.add("trialFail")
    str_blob = strings.blob()
    text_blob = texts.blob()
    expr_data, expr_offsets = expr.finish()
    stroff_bin = bytearray()
    running = 0
    for s_ in strings.order:
        stroff_bin += struct.pack("<I", running)
        running += len(s_.encode("utf-8")) + 1
    # exprs_bin must be rebuilt after expr.finish()
    exprs_bin = bytearray()
    for off, ln in expr_offsets:
        exprs_bin += struct.pack("<IH", off, ln)

    sections = {
        SEC_STR: str_blob,
        SEC_SCENES: bytes(scenes_bin),
        SEC_ROUTES: bytes(routes_bin),
        SEC_CHAPTERS: bytes(chapters_bin),
        SEC_OPS: bytes(ops_bin),
        SEC_TEXTS: text_blob,
        SEC_LINKS: bytes(links_bin),
        SEC_ITEMS: bytes(items_bin),
        SEC_POSES: bytes(poses_bin),
        SEC_CHAR_DETAILS: bytes(char_bin),
        SEC_MOVES: bytes(moves_bin),
        SEC_MOVE_ENTRIES: bytes(move_entries_bin),
        SEC_VIS: bytes(vis_bin),
        SEC_AREAS: bytes(areas_bin),
        SEC_EVIDENCE_CATS: bytes(evcats_bin),
        SEC_MAP_AREAS: bytes(map_areas_bin),
        SEC_EXPRS: bytes(exprs_bin),
        SEC_EXPR_DATA: expr_data,
        SEC_IMAGES: bytes(images_bin),
        SEC_IMGDATA: bytes(images.data),
        SEC_BOOK: bytes(book_bin),
        SEC_GALLERY: bytes(gallery_bin),
        SEC_GALLERY_ITEMS: gallery_items_bin,
        SEC_BOOKKEYS: bytes(bookkeys_bin),
        SEC_STROFF: bytes(stroff_bin),
        SEC_VARS: bytes(vars_bin),
        SEC_LINK_OPS: bytes(link_ops_bin),
        SEC_MOVE_OPS: bytes(move_ops_bin),
    }

    header_size = 8 + 16 + SECTION_COUNT * 16
    meta = struct.pack(
        "<26I", VERSION, scene_index[catalog["initial"]], len(scene_names), len(routes),
        len(chapters), len(ops_out), len(book_records), len(gallery_groups),
        len(gallery_items), len(images.records), title_a, title_b, len(vars_table.names),
        len(texts.units), len(expr.blobs), len(poses), len(links), len(items),
        len(moves), len(vis), len(areas), len(evidence_cats), len(map_areas),
        len(char_details), len(link_ops), len(move_ops))
    sections[SEC_META] = meta

    body = bytearray()
    table = bytearray()
    for sid in range(SECTION_COUNT):
        blob = sections.get(sid, b"")
        while len(body) % 4:          # 段首 4 字节对齐:u32 字段可直接读取
            body += bytes(1)
        offset = header_size + len(body) if blob else 0
        body += blob
        count = _section_count(sid, blob)
        table += struct.pack("<IIII", sid, offset, count, len(blob))

    total = header_size + len(body)
    out = bytearray(MAGIC + struct.pack("<IIII", VERSION, total, SECTION_COUNT, 0))
    out += table
    out += body
    if len(out) != total:
        die("pack size mismatch %d != %d" % (len(out), total))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(out)

    chars = set()
    for s in strings.order:
        chars.update(ch for ch in s if ord(ch) > 127 or ch == " ")
    for t in texts.by_text:
        chars.update(ch for ch in t if ord(ch) > 127 or ch == " ")
    corpus_path = os.path.join(os.path.dirname(os.path.abspath(args.out)), "chars.txt")
    with open(corpus_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("".join(sorted(chars)) + "\n")

    report = {
        "source": root.replace("\\", "/"),
        "pack": os.path.abspath(args.out).replace("\\", "/"),
        "pack_bytes": len(out),
        "pack_sha256": hashlib.sha256(out).hexdigest(),
        "scenes": len(scene_names),
        "story_chunks": len(chunk_files),
        "ops": len(ops_out),
        "texts": len(texts.by_text),
        "text_utf16_units": len(texts.units),
        "text_utf8_bytes": sum(len(t.encode("utf-8")) for t in texts.by_text),
        "strings": len(strings.order),
        "images": len(images.records),
        "image_bytes": len(images.data),
        "book_entries": len(book_records),
        "gallery_groups": len(gallery_groups),
        "gallery_items": len(gallery_items),
        "expressions": len(expr.blobs),
        "variables": len(vars_table.names),
        "portraits": sum(1 for r in images.records if r["kind"] == 2),
        "sections": {str(k): len(v) for k, v in sections.items()},
    }
    report_path = os.path.splitext(args.out)[0] + ".json"
    with open(report_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=1)
    print(json.dumps(report, ensure_ascii=False, indent=1))
    return 0


def _section_count(sid, blob):
    sizes = {
        SEC_META: 26 * 4,
        SEC_SCENES: 20, SEC_ROUTES: 8, SEC_CHAPTERS: 14, SEC_OPS: 12,
        SEC_LINKS: 12, SEC_ITEMS: 16, SEC_POSES: 20, SEC_CHAR_DETAILS: 12,
        SEC_MOVES: 6, SEC_MOVE_ENTRIES: 8, SEC_VIS: 2, SEC_AREAS: 8,
        SEC_EVIDENCE_CATS: 1, SEC_MAP_AREAS: 2, SEC_EXPRS: 6,
        SEC_EXPR_DATA: 1, SEC_IMAGES: 40, SEC_IMGDATA: 1, SEC_BOOK: 28,
        SEC_GALLERY: 10, SEC_GALLERY_ITEMS: 4, SEC_VARS: 12,
        SEC_LINK_OPS: 8, SEC_MOVE_OPS: 6, SEC_BOOKKEYS: 20, SEC_STROFF: 4,
    }
    if sid == SEC_STR:
        return blob.count(b"\0")
    if sid == SEC_TEXTS:
        return len(blob) // 2
    unit = sizes.get(sid)
    if unit is None:
        die("unknown section %d" % sid)
    if sid == SEC_META:
        return 26
    return len(blob) // unit


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack manosaba watch source into a flash image")
    ap.add_argument("--source", required=True, help="watch source repo (read-only)")
    ap.add_argument("--out", default="main/gal_data/manosaba.pack")
    ap.add_argument("--art-quality", type=int, default=70)
    ap.add_argument("--portrait-quality", type=int, default=74)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    if args.dry_run:
        args.out = os.path.join(os.environ.get("TEMP", "/tmp"), "manosaba-dryrun.pack")
    return build(args)


if __name__ == "__main__":
    raise SystemExit(main())
