#!/usr/bin/env python3
"""Independent audit of the manosaba source snapshot and the packed image.

This tool deliberately does NOT import tools/manosaba_pack.py: it parses the
source JSON/catalog and the MNPK binary with its own readers, then compares
every scene/op, every text choice/link, every expression result, and every
book/gallery/image reference.

Usage:
  python tools/manosaba_audit.py --source <watch-src> --pack main/gal_data/manosaba.pack \
      --report build/manosaba-audit.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import struct
import sys

SEC = {
    "META": 0, "STR": 1, "SCENES": 2, "ROUTES": 3, "CHAPTERS": 4, "OPS": 5,
    "TEXTS": 6, "LINKS": 7, "ITEMS": 8, "POSES": 9, "CHARS": 10, "MOVES": 11,
    "MOVE_ENTRIES": 12, "VIS": 13, "AREAS": 14, "EVIDENCE": 15, "MAP_AREAS": 16,
    "EXPRS": 17, "EXPR_DATA": 18, "IMAGES": 19, "IMGDATA": 20, "BOOK": 21,
    "GALLERY": 22, "GALLERY_ITEMS": 23, "VARS": 24, "LINK_OPS": 25,
    "MOVE_OPS": 26, "BOOKKEYS": 27, "STROFF": 28,
}


class Audit:
    def __init__(self):
        self.errors = []
        self.warnings = []
        self.checks = 0

    def check(self, cond, msg):
        self.checks += 1
        if not cond:
            self.errors.append(msg)
        return cond

    def warn(self, msg):
        self.warnings.append(msg)


def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def read_catalog(root):
    text = open(os.path.join(root, "src/common/catalog.js"), encoding="utf-8").read()
    m = re.search(r"export const catalog=(\{.*\});\s*$", text, re.S)
    if not m:
        raise SystemExit("catalog.js parse failed")
    return json.loads(m.group(1))


def read_assets(root):
    text = open(os.path.join(root, "src/common/assets.js"), encoding="utf-8").read()
    bg = json.loads(re.search(r"const bg=(\{.*?\});", text, re.S).group(1))
    portraits = json.loads(re.search(r"const portraits=(\{.*?\});", text, re.S).group(1))
    book_images = json.loads(re.search(r"const bookImages=(\{.*?\});", text, re.S).group(1))
    return bg, portraits, book_images


class Pack:
    def __init__(self, data):
        self.data = data
        self.meta = []
        self.sec = {}
        self.count = {}
        self.len = {}
        self.open()

    def open(self):
        d = self.data
        if len(d) < 24 or d[:8] != b"MNPK0001":
            raise SystemExit("bad pack magic")
        ver, total, parts, _ = struct.unpack_from("<IIII", d, 8)
        if ver != 1 or total != len(d) or parts != 29:
            raise SystemExit("bad pack header")
        for i in range(parts):
            t, off, cnt, size = struct.unpack_from("<IIII", d, 24 + 16 * i)
            if t != i:
                raise SystemExit("section order")
            self.sec[t] = d[off:off + size] if size else b""
            self.count[t] = cnt
            self.len[t] = size
        self.meta = list(struct.unpack_from("<26I", self.sec[SEC["META"]], 0))

    def rec(self, sec, fmt, index):
        size = struct.calcsize(fmt)
        return struct.unpack_from(fmt, self.sec[sec], index * size)

    def strings(self):
        blob = self.sec[SEC["STR"]]
        return blob.split(b"\0")[:-1]

    def string(self, sid):
        blob = self.sec[SEC["STR"]]
        off = struct.unpack_from("<I", self.sec[SEC["STROFF"]], sid * 4)[0]
        end = blob.index(b"\0", off)
        return blob[off:end].decode("utf-8")

    def text(self, off, length):
        units = struct.unpack_from("<%dH" % length, self.sec[SEC["TEXTS"]], off * 2)
        return "".join(chr(u) for u in units)


# ---------------------------------------------------------------- source model
def load_source(root, catalog):
    scenes = {}
    chunks = 0
    files = []
    story = os.path.join(root, "src/common/story")
    for scene in catalog["scenes"]:
        directory = os.path.join(story, scene)
        if not os.path.isdir(directory):
            raise SystemExit("missing scene dir " + scene)
        nums = sorted(int(f[:-4]) for f in os.listdir(directory) if re.fullmatch(r"\d+\.txt", f))
        if nums != list(range(len(nums))) or not nums:
            raise SystemExit("bad chunks " + scene)
        ops = []
        for n in nums:
            data = load_json(os.path.join(directory, "%d.txt" % n))
            part = data["ops"] if isinstance(data, dict) else data
            if len(part) > 512:
                raise SystemExit("chunk too long " + scene)
            ops.extend(part)
            chunks += 1
            files.append(os.path.join(directory, "%d.txt" % n))
        if len(ops) != catalog["scenes"][scene]["length"]:
            raise SystemExit("scene length mismatch " + scene)
        scenes[scene] = ops
    return scenes, chunks, files


# ---------------------------------------------------------------- expressions
class ExprError(Exception):
    pass


TOK = re.compile(r'"((?:\\.|[^"\\])*)"|\'((?:\\.|[^\'\\])*)\'|(\d+(?:\.\d+)?)|([A-Za-z_][A-Za-z_0-9]*)|(&&|\|\||==|!=|>=|<=|[()!,+*/%<>=-])')


class SOURCE_EXPR:
    """Independent implementation of src/common/expressions.js semantics."""

    def __init__(self, text):
        self.text = str(text).replace("{", "").replace("}", "")
        self.tokens = []
        pos = 0
        while pos < len(self.text) and self.text[pos].isspace():
            pos += 1
        for m in TOK.finditer(self.text):
            if m.start() != pos:
                raise ExprError("unsupported expression: " + self.text)
            pos = m.end()
            while pos < len(self.text) and self.text[pos].isspace():
                pos += 1
            if m.group(1) is not None:
                self.tokens.append(("str", m.group(1).replace('\\"', '"')))
            elif m.group(2) is not None:
                self.tokens.append(("str", m.group(2).replace("\\'", "'")))
            elif m.group(3) is not None:
                self.tokens.append(("num", float(m.group(3))))
            elif m.group(4) is not None:
                self.tokens.append(("id", m.group(4)))
            else:
                self.tokens.append(("op", m.group(5)))
        self.at = 0
        self.prec = {"||": 1, "&&": 2, "=": 3, "==": 3, "!=": 3, ">": 4, "<": 4,
                     ">=": 4, "<=": 4, "+": 5, "-": 5, "*": 6, "/": 6, "%": 6}

    def eval(self, variables):
        self.at = 0
        if not self.tokens:
            return False
        value = self.parse(0, variables)
        if self.at != len(self.tokens):
            raise ExprError("trailing tokens: " + self.text)
        return value

    def primary(self, variables):
        if self.at >= len(self.tokens):
            raise ExprError("incomplete")
        kind, value = self.tokens[self.at]
        self.at += 1
        if kind == "num":
            return value
        if kind == "str":
            return value
        if kind == "id":
            if value == "true" or value == "True":
                return True
            if value == "false" or value == "False":
                return False
            if self.at < len(self.tokens) and self.tokens[self.at] == ("op", "("):
                self.at += 1
                args = []
                if self.at < len(self.tokens) and self.tokens[self.at] != ("op", ")"):
                    args.append(self.parse(0, variables))
                    while self.at < len(self.tokens) and self.tokens[self.at] == ("op", ","):
                        self.at += 1
                        args.append(self.parse(0, variables))
                if self.at >= len(self.tokens) or self.tokens[self.at] != ("op", ")"):
                    raise ExprError("bad call")
                self.at += 1
                if value == "max":
                    return max(args)
                if value == "min":
                    return min(args)
                if value == "Sin":
                    return _sin(args[0])
                if value == "Cos":
                    return _cos(args[0])
                if value == "abs":
                    return abs(args[0])
                raise ExprError("unknown function " + value)
            return variables.get(value, 0)
        if value == "!":
            return not self.primary(variables)
        if value == "-":
            return -self.primary(variables)
        if value == "+":
            return +self.primary(variables)
        if value == "(":
            v = self.parse(0, variables)
            if self.at >= len(self.tokens) or self.tokens[self.at] != ("op", ")"):
                raise ExprError("paren")
            self.at += 1
            return v
        raise ExprError("bad primary " + value)

    def parse(self, min_prec, variables):
        left = self.primary(variables)
        while self.at < len(self.tokens) and self.tokens[self.at][0] == "op":
            op = self.tokens[self.at][1]
            prec = self.prec.get(op)
            if not prec or prec < min_prec:
                break
            self.at += 1
            right = self.parse(prec + 1, variables)
            if op == "||":
                left = bool(left) or bool(right)
            elif op == "&&":
                left = bool(left) and bool(right)
            elif op in ("=", "=="):
                left = type(left) is type(right) and left == right
            elif op == "!=":
                left = not (type(left) is type(right) and left == right)
            elif op == ">":
                left = _num(left) > _num(right)
            elif op == "<":
                left = _num(left) < _num(right)
            elif op == ">=":
                left = _num(left) >= _num(right)
            elif op == "<=":
                left = _num(left) <= _num(right)
            elif op == "+":
                left = _num(left) + _num(right)
            elif op == "-":
                left = _num(left) - _num(right)
            elif op == "*":
                left = _num(left) * _num(right)
            elif op == "/":
                left = _div(left, right)
            elif op == "%":
                left = _mod(left, right)
        return left


def _num(v):
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    if isinstance(v, str):
        return 0.0
    return float(v)


def _div(a, b):
    a = _num(a); b = _num(b)
    if b == 0:
        if a == 0:
            return float("nan")
        return math.inf if a > 0 else -math.inf
    return a / b


def _mod(a, b):
    a = _num(a); b = _num(b)
    if b == 0:
        return float("nan")
    return math.fmod(a, b)


def _sin(v):
    v = _num(v)
    return math.sin(v) if math.isfinite(v) else float("nan")


def _cos(v):
    v = _num(v)
    return math.cos(v) if math.isfinite(v) else float("nan")


class PACK_EXPR:
    """Evaluate the pack's RPN bytecode independently."""

    def __init__(self, pack):
        self.pack = pack
        self.var_names = []
        self.var_types = []
        self.var_defaults = []
        for i in range(pack.count[SEC["VARS"]]):
            name, typ, pad, pad2, bits = struct.unpack_from("<IBBHI", pack.sec[SEC["VARS"]], i * 12)
            self.var_names.append(pack.string(name))
            self.var_types.append(typ)
            self.var_defaults.append(struct.unpack("<f", struct.pack("<I", bits))[0])

    def eval(self, expr_id, variables):
        off, ln = struct.unpack_from("<IH", self.pack.sec[SEC["EXPRS"]], expr_id * 6)
        code = self.pack.sec[SEC["EXPR_DATA"]][off:off + ln]
        st = []
        pc = 0
        while pc < len(code):
            op = code[pc]
            pc += 1
            if op == 1:
                st.append(struct.unpack_from("<f", code, pc)[0]); pc += 4
            elif op == 2:
                sid = struct.unpack_from("<H", code, pc)[0]; pc += 2
                st.append(self.pack.string(sid))
            elif op == 3:
                vid = struct.unpack_from("<H", code, pc)[0]; pc += 2
                st.append(variables.get(vid, 0))
            elif op == 4:
                st.append(True)
            elif op == 5:
                st.append(False)
            elif op == 6:
                st[-1] = -_num(st[-1])
            elif op == 7:
                st[-1] = not _truthy(st[-1])
            elif op in (8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20):
                b = st.pop(); a = st.pop()
                if op == 8: st.append(_num(a) + _num(b))
                elif op == 9: st.append(_num(a) - _num(b))
                elif op == 10: st.append(_num(a) * _num(b))
                elif op == 11: st.append(_div(a, b))
                elif op == 12: st.append(_mod(a, b))
                elif op == 13: st.append(type(a) is type(b) and a == b)
                elif op == 14: st.append(not (type(a) is type(b) and a == b))
                elif op == 15: st.append(_num(a) < _num(b))
                elif op == 16: st.append(_num(a) <= _num(b))
                elif op == 17: st.append(_num(a) > _num(b))
                elif op == 18: st.append(_num(a) >= _num(b))
                elif op == 19: st.append(bool(a) and bool(b))
                elif op == 20: st.append(bool(a) or bool(b))
            elif op in (21, 22):
                extra = code[pc]; pc += 1
                args = st[-(extra + 2):]
                del st[-(extra + 2):]
                vals = [_num(x) for x in args]
                st.append(max(vals) if op == 21 else min(vals))
            elif op == 23:
                st[-1] = _sin(st[-1])
            elif op == 24:
                st[-1] = _cos(st[-1])
            elif op == 25:
                st[-1] = abs(_num(st[-1]))
            else:
                raise ExprError("bad pack expr op %d" % op)
        if len(st) != 1:
            raise ExprError("pack expr stack")
        return st[0]


def _truthy(v):
    if isinstance(v, bool):
        return v
    if isinstance(v, str):
        return v != ""
    return bool(v)


def expr_text(value):
    return value.get("expr", "") if isinstance(value, dict) else value


def split_assign(text):
    m = re.match(r"\s*([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.+)$", str(text))
    if not m:
        raise ExprError("assignment")
    return m.group(1), m.group(2)


# ---------------------------------------------------------------- image checks
def jpeg_size(blob):
    for i in range(max(0, len(blob) - 9)):
        if blob[i] == 0xFF and blob[i + 1] in (0xC0, 0xC1, 0xC2):
            h = (blob[i + 5] << 8) | blob[i + 6]
            w = (blob[i + 7] << 8) | blob[i + 8]
            return w, h
    return None


def src_num(value, variables):
    if value is None:
        return None
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return float(value)
    text = value.get("expr", "") if isinstance(value, dict) else str(value)
    if text is None or text == "" or text == "undefined":
        return None
    try:
        return float(text)
    except ValueError:
        return float(SOURCE_EXPR(text).eval(variables))


def src_vector(value, count, variables):
    if value is None:
        return [None] * count
    if isinstance(value, list):
        parts = value
    else:
        raw = value.get("expr", "") if isinstance(value, dict) else str(value)
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
        out.append(src_num(parts[i], variables) if i < len(parts) else None)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--pack", required=True)
    ap.add_argument("--report", default="")
    args = ap.parse_args()
    root = os.path.abspath(args.source)
    catalog = read_catalog(root)
    bg_map, portraits, book_images = read_assets(root)
    scenes_src, chunks, files = load_source(root, catalog)
    src_images = sorted(os.listdir(os.path.join(root, "src/common/images")))
    pack = Pack(open(args.pack, "rb").read())
    audit = Audit()
    report = {
        "source": root.replace("\\", "/"),
        "pack": os.path.abspath(args.pack).replace("\\", "/"),
        "pack_sha256": hashlib.sha256(pack.data).hexdigest(),
        "pack_bytes": len(pack.data),
        "story_chunks": chunks,
        "source_scenes": len(scenes_src),
        "source_ops": 0,
        "source_text_utf8": 0,
        "pack_scenes": pack.count[SEC["SCENES"]],
        "pack_ops": pack.count[SEC["OPS"]],
        "pack_texts": pack.count[SEC["TEXTS"]],
        "images": pack.count[SEC["IMAGES"]],
        "book_entries": pack.count[SEC["BOOK"]],
        "gallery_groups": pack.count[SEC["GALLERY"]],
        "gallery_items": pack.count[SEC["GALLERY_ITEMS"]],
        "errors": [],
        "warnings": [],
        "checks": 0,
    }

    scene_names = list(catalog["scenes"].keys())
    audit.check(pack.count[SEC["SCENES"]] == len(scene_names), "scene count")
    audit.check(pack.meta[1] == scene_names.index(catalog["initial"]), "initial scene")
    for i, name in enumerate(scene_names):
        sid, nm, route1, flags, start, count = struct.unpack_from("<IIHHII", pack.sec[SEC["SCENES"]], i * 20)
        audit.check(pack.string(sid) == name, "scene %d id" % i)
        audit.check(pack.string(nm) == catalog["scenes"][name]["name"], "scene %d name" % i)
        audit.check((route1 - 1) == catalog["scenes"][name]["route"], "scene %d route" % i)

    pexpr = PACK_EXPR(pack)
    rng = random.Random(20261102)
    strings = [pack.string(i) for i in range(pack.count[SEC["STR"]])]
    var_base = {}
    for i, name in enumerate(pexpr.var_names):
        typ = pexpr.var_types[i]
        if typ == 1:
            var_base[i] = ""
        elif typ == 2:
            var_base[i] = bool(pexpr.var_defaults[i])
        else:
            var_base[i] = float(pexpr.var_defaults[i])
    # op-by-op comparison
    text_bytes = 0
    op_errors = 0
    for scene_i, scene in enumerate(scene_names):
        src_ops = scenes_src[scene]
        report["source_ops"] += len(src_ops)
        report["source_text_utf8"] += sum(len(o.get("text", "").encode("utf-8"))
                                          for o in src_ops if o["op"] in ("text", "pause"))
        sid, nm, route1, flags, start, count = struct.unpack_from("<IIHHII", pack.sec[SEC["SCENES"]], scene_i * 20)
        audit.check(count == len(src_ops), "scene %s op count" % scene)
        for k, src in enumerate(src_ops):
            oi = start + k
            if oi >= pack.count[SEC["OPS"]]:
                op_errors += 1
                break
            op, oflags, a, b, c, d = struct.unpack_from("<BBHHHI", pack.sec[SEC["OPS"]], oi * 12)
            kind = src["op"]
            expect = {"text": 0, "char": 1, "bg": 2, "focus": 3, "call": 4, "jump": 5,
                      "hideAll": 6, "offer": 7, "visibility": 8, "return": 9, "damage": 10,
                      "choice": 11, "cancelCall": 12, "if": 13, "set": 14, "autosave": 15,
                      "resetBook": 16, "clueHide": 17, "clueImage": 18, "evidence": 19,
                      "flag": 20, "end": 21, "recordChoice": 22, "map": 23, "stop": 24,
                      "bookClear": 25, "execution": 26, "pause": 27, "bookUpdate": 29}.get(kind, -1)
            if kind == "jump" and isinstance(src.get("target"), dict):
                expect = 28
            if expect < 0:
                audit.check(False, "unknown source op %s" % kind)
                op_errors += 1
                continue
            if not audit.check(op == expect, "%s:%d opcode %d != %d" % (scene, k, op, expect)):
                op_errors += 1
                continue
            try:
                if kind == "text":
                    audit.check(pack.string(a) == src.get("speaker", ""), "%s:%d speaker" % (scene, k))
                    audit.check(pack.string(c) == src.get("actor", ""), "%s:%d actor" % (scene, k))
                    audit.check(pack.text(d, b) == src.get("text", ""), "%s:%d text" % (scene, k))
                    audit.check(bool(oflags & 1) == bool(src.get("debate")), "%s:%d debate" % (scene, k))
                    links = src.get("links") or []
                    if links:
                        # link_ops side table lookup
                        found = None
                        for li in range(pack.count[SEC["LINK_OPS"]]):
                            opidx, first, lc, _ = struct.unpack_from("<IHBB", pack.sec[SEC["LINK_OPS"]], li * 8)
                            if opidx == oi:
                                found = (first, lc)
                                break
                        audit.check(found is not None, "%s:%d link table" % (scene, k))
                        if found:
                            audit.check(found[1] == len(links), "%s:%d link count" % (scene, k))
                            for n, link in enumerate(links):
                                ls, lc, loff, llen, _ = struct.unpack_from("<HHIHH", pack.sec[SEC["LINKS"]], (found[0] + n) * 12)
                                audit.check(ls < len(scene_names) and scene_names[ls] == link["target"][0],
                                            "%s:%d link scene" % (scene, k))
                                audit.check(lc == link["target"][1], "%s:%d link cursor" % (scene, k))
                                audit.check(pack.text(loff, llen) == link.get("text", ""), "%s:%d link text" % (scene, k))
                elif kind == "choice":
                    items = src.get("items") or []
                    audit.check(b == len(items), "%s:%d choice count" % (scene, k))
                    audit.check(c == int(src.get("menu", 0)), "%s:%d choice menu" % (scene, k))
                    for n, it in enumerate(items):
                        tok, tlen, to, end, when, iscene, branch, lock = struct.unpack_from("<IHHHHHBB", pack.sec[SEC["ITEMS"]], (a + n) * 16)
                        audit.check(pack.text(tok, tlen) == it.get("text", ""), "%s:%d item text" % (scene, k))
                        audit.check(to == it["to"] and end == it["end"], "%s:%d item target" % (scene, k))
                        audit.check(bool(branch) == (it.get("kind") == "branch"), "%s:%d item kind" % (scene, k))
                        audit.check(bool(lock) == bool(it.get("lock")), "%s:%d item lock" % (scene, k))
                        if it.get("when"):
                            audit.check(when != 0xFFFF, "%s:%d item when" % (scene, k))
                elif kind in ("jump", "call"):
                    target = src["target"]
                    if isinstance(target, dict):
                        audit.check(pack.string(a) == target.get("expr", ""), "%s:%d dynamic template" % (scene, k))
                    else:
                        # pack stores scene index in a; decode via scenes list
                        tscene = scene_names[a] if a < len(scene_names) else "?"
                        audit.check(tscene == target[0], "%s:%d jump scene %s" % (scene, k, tscene))
                        audit.check(b == target[1], "%s:%d jump cursor" % (scene, k))
                elif kind == "offer":
                    audit.check(a == src["to"] and b == src["after"], "%s:%d offer target" % (scene, k))
                    audit.check(c == int(src.get("menu", 0)), "%s:%d offer menu" % (scene, k))
                    audit.check(pack.string(d) == src.get("text", ""), "%s:%d offer text" % (scene, k))
                    audit.check(bool(oflags & 1) == bool(src.get("lock")), "%s:%d offer lock" % (scene, k))
                elif kind == "bg":
                    actor = src.get("actor", "")
                    sub = src.get("sub", "")
                    image = src.get("image", "")
                    if image == "SolidColor":
                        # scene.js:9: only exact #000000 is black; #000000FF is white.
                        expected = "solid_black.png" if str(src.get("color", "")).lower() == "#000000" else "solid_white.png"
                    elif image == "Transparent":
                        expected = "solid_black.png"
                    else:
                        prefix = "stills:" if actor == "Stills" else "tricks:" if actor == "Tricks" else "mainbackground:"
                        expected = bg_map.get(prefix + image)
                    imrec = struct.unpack_from("<IIIIIIIIHHHBB", pack.sec[SEC["IMAGES"]], b * 40)
                    audit.check(pack.string(imrec[0]) == expected, "%s:%d bg image" % (scene, k))
                    audit.check(pack.string(a) == actor, "%s:%d bg actor" % (scene, k))
                    audit.check(pack.string(c) == sub, "%s:%d bg sub" % (scene, k))
                    audit.check(bool(oflags & 1) == bool(src.get("visible")), "%s:%d bg visible" % (scene, k))
                elif kind == "char":
                    pose, sx, sy, py, sc, vis = struct.unpack_from("<HHHHHBx", pack.sec[SEC["CHARS"]], a * 12)
                    actor, appearance, sub, _z, face_img, def_img = struct.unpack_from("<IIIIHH", pack.sec[SEC["POSES"]], pose * 20)
                    audit.check(pack.string(actor) == src["actor"], "%s:%d char actor" % (scene, k))
                    audit.check(pack.string(sub) == (src.get("sub") or ""), "%s:%d char sub" % (scene, k))
                    base = re.split(r"[-,]", src["actor"])[0].lower()
                    face = _face(src.get("appearance", ""))
                    expected = None
                    if face:
                        expected = portraits.get(base + ":" + face) or portraits.get(base + ":Default")
                    if face_img != 0xFFFF:
                        rec = struct.unpack_from("<IIIIIIIIHHHBB", pack.sec[SEC["IMAGES"]], face_img * 40)
                        audit.check(pack.string(rec[0]) == expected, "%s:%d portrait image" % (scene, k))
                    else:
                        audit.check(expected is None, "%s:%d portrait missing face image" % (scene, k))
                    def_expected = portraits.get(base + ":Default")
                    if def_img != 0xFFFF:
                        rec = struct.unpack_from("<IIIIIIIIHHHBB", pack.sec[SEC["IMAGES"]], def_img * 40)
                        audit.check(pack.string(rec[0]) == def_expected, "%s:%d default portrait" % (scene, k))
                    else:
                        audit.check(def_expected is None, "%s:%d default portrait missing" % (scene, k))
                    # 立绘位置/缩放表达式:与源 scene/position/scale 对照
                    for trial in range(3):
                        vars_p = dict(var_base) if trial == 0 else {
                            i: (rng.choice(strings[1:]) if pexpr.var_types[i] == 1 else
                                (bool(rng.getrandbits(1)) if pexpr.var_types[i] == 2 else float(rng.randint(-3, 12))))
                            for i in range(len(pexpr.var_names))
                        }
                        vars_s = {pexpr.var_names[i]: vars_p[i] for i in vars_p}
                        scene_v = src_vector(src.get("scene"), 2, vars_s)
                        pos_v = src_vector(src.get("position"), 3, vars_s)
                        scale_v = src_vector(src.get("scale"), 3, vars_s)
                        for eid, expected in ((sx, scene_v[0]), (sy, scene_v[1]),
                                              (py, pos_v[1]), (sc, scale_v[0])):
                            if eid == 0xFFFF:
                                audit.check(expected is None, "%s:%d char expr missing" % (scene, k))
                            else:
                                got = pexpr.eval(eid, vars_p)
                                ok = (expected is None and got == 0) or (
                                    expected is not None and abs(float(got) - expected) <= 1e-4)
                                audit.check(ok, "%s:%d char expr %r != %r" % (scene, k, got, expected))
                elif kind == "focus":
                    audit.check(pack.string(a) == src.get("actor", ""), "%s:%d focus" % (scene, k))
                elif kind == "visibility":
                    actors = src.get("actors") or []
                    audit.check(b == len(actors), "%s:%d vis count" % (scene, k))
                    for n, actor in enumerate(actors):
                        sid = struct.unpack_from("<H", pack.sec[SEC["VIS"]], (a + n) * 2)[0]
                        audit.check(pack.string(sid) == actor, "%s:%d vis actor" % (scene, k))
                    audit.check(bool(oflags & 1) == bool(src.get("visible")), "%s:%d vis visible" % (scene, k))
                elif kind == "if":
                    _check_expr(audit, scene, k, "if", expr_text(src["expr"]), pack, a, src)
                elif kind == "set":
                    lhs, rhs = split_assign(expr_text(src["expr"]))
                    vid, eid = a, b
                    audit.check(pack.string(struct.unpack_from("<I", pack.sec[SEC["VARS"]], vid * 12)[0]) == lhs,
                                "%s:%d set var" % (scene, k))
                    _check_expr(audit, scene, k, "set", rhs, pack, eid, src)
                elif kind == "resetBook":
                    tscene = scene_names[a] if a < len(scene_names) else "?"
                    audit.check(tscene == src["target"][0] and b == src["target"][1], "%s:%d resetBook target" % (scene, k))
                    audit.check(bool(oflags & 1) == bool(src.get("onlyDirect")), "%s:%d resetBook onlyDirect" % (scene, k))
                elif kind == "clueImage":
                    src_img = book_images.get("Clue:" + str(src.get("id")))
                    rec = struct.unpack_from("<IIIIIIIIHHHBB", pack.sec[SEC["IMAGES"]], a * 40)
                    audit.check(pack.string(rec[0]) == src_img, "%s:%d clue image" % (scene, k))
                elif kind == "evidence":
                    cats = src.get("categories") or []
                    audit.check(b == len(cats), "%s:%d evidence count" % (scene, k))
                    for n, cat in enumerate(cats):
                        got = pack.sec[SEC["EVIDENCE"]][a + n]
                        audit.check(got == {"Clue": 0, "Profile": 1, "Rule": 2, "Note": 3, "Map": 4}[cat],
                                    "%s:%d evidence cat" % (scene, k))
                elif kind == "bookUpdate":
                    bk = struct.unpack_from("<IIIHHHH", pack.sec[SEC["BOOKKEYS"]], a * 20)
                    audit.check(bk[0] == {"Clue": 0, "Profile": 1, "Rule": 2, "Note": 3, "Map": 4}[src["category"]],
                                "%s:%d bookUpdate cat" % (scene, k))
                    audit.check(pack.string(bk[1]) == str(src["id"]), "%s:%d bookUpdate id" % (scene, k))
                    audit.check(b == int(src["version"]), "%s:%d bookUpdate version" % (scene, k))
                elif kind == "flag":
                    audit.check(pack.string(struct.unpack_from("<I", pack.sec[SEC["VARS"]], a * 12)[0]) == src["variable"],
                                "%s:%d flag var" % (scene, k))
                    audit.check(b == int(src["number"]), "%s:%d flag number" % (scene, k))
                elif kind in ("end", "stop"):
                    default = "end" if kind == "end" else "stop"
                    audit.check(pack.string(a) == (src.get("kind") or default), "%s:%d end kind" % (scene, k))
                elif kind == "recordChoice":
                    audit.check(a == int(src["number"]), "%s:%d recordChoice" % (scene, k))
                elif kind == "map":
                    areas = src.get("areas") or []
                    audit.check(b == len(areas), "%s:%d map count" % (scene, k))
                    for n, area in enumerate(areas):
                        aid = struct.unpack_from("<H", pack.sec[SEC["MAP_AREAS"]], (a + n) * 2)[0]
                        arec = struct.unpack_from("<II", pack.sec[SEC["AREAS"]], aid * 8)
                        audit.check(pack.string(arec[0]) == area, "%s:%d map area" % (scene, k))
                    audit.check(pack.string(struct.unpack_from("<I", pack.sec[SEC["VARS"]], c * 12)[0]) == src.get("flag", ""),
                                "%s:%d map flag" % (scene, k))
                elif kind == "pause":
                    audit.check(pack.text(d, b) == (src.get("text") or "点击继续"), "%s:%d pause text" % (scene, k))
            except (IndexError, struct.error, KeyError) as exc:
                audit.check(False, "%s:%d parse %s" % (scene, k, exc))
                op_errors += 1
        # max cursor
        audit.check(count > 0, "scene %s empty" % scene)

    # images: every source image and only those, with declared geometry
    names = {}
    for i in range(pack.count[SEC["IMAGES"]]):
        rec = struct.unpack_from("<IIIIIIIIHHHBB", pack.sec[SEC["IMAGES"]], i * 40)
        names[pack.string(rec[0])] = (i, rec)
    for name in src_images:
        audit.check(name in names, "missing source image " + name)
    titles = {"title-1.jpg", "title-2.jpg"}
    audit.check(set(names.keys()) == set(src_images) | titles, "unexpected image names")
    for name, (i, rec) in names.items():
        kind = rec[11]
        top = pack.sec[SEC["IMGDATA"]][rec[1]:rec[1] + rec[2]]
        dim = jpeg_size(top)
        audit.check(dim is not None, "jpeg header " + name)
        if kind == 0:
            audit.check(dim == (320, 150), "top dims " + name)
            audit.check(rec[4] > 0 and jpeg_size(pack.sec[SEC["IMGDATA"]][rec[3]:rec[3] + rec[4]]) == (160, 45),
                        "aux dims " + name)
        elif kind == 1:
            audit.check(dim == (320, 150), "book dims " + name)
        else:
            audit.check(dim == (rec[8], rec[9]), "portrait dims " + name)
            stride = (rec[8] + 7) // 8
            audit.check(rec[6] >= stride * rec[9], "portrait mask " + name)

    # book / gallery / chapters / routes / areas
    book_entries = []
    for i in range(pack.count[SEC["BOOK"]]):
        cat, sid, noff, nlen, pad, toff, tlen, ver, img, pad2 = struct.unpack_from("<IIIHHIHHHH", pack.sec[SEC["BOOK"]], i * 28)
        book_entries.append((cat, pack.string(sid), ver, pack.text(noff, nlen), pack.text(toff, tlen), img))
    src_book = load_json(os.path.join(root, "src/common/book.txt"))
    src_book_rows = sorted(({"Clue": 0, "Profile": 1, "Rule": 2, "Note": 3, "Map": 4}[v["category"]],
                            str(v["id"]), int(v.get("version", 1)), v.get("name", ""), v.get("text", ""))
                           for v in src_book.values())
    got_book = [(c, i, ver, name, text) for c, i, ver, name, text, _ in book_entries]
    audit.check(sorted(got_book) == sorted(src_book_rows), "book entries")
    for c, i, ver, name, text, img in book_entries:
        if img != 0xFFFF:
            src_img = book_images.get({0: "Clue", 1: "Profile", 2: "Rule", 3: "Note", 4: "Map"}[c] + ":" + i)
            rec = struct.unpack_from("<IIIIIIIIHHHBB", pack.sec[SEC["IMAGES"]], img * 40)
            audit.check(pack.string(rec[0]) == src_img, "book image " + i)

    src_gallery = load_json(os.path.join(root, "src/common/gallery.txt"))
    src_groups = {}
    for item in src_gallery:
        src_groups.setdefault(item.get("group") or item.get("source"), []).append(item["image"])
    audit.check(pack.count[SEC["GALLERY"]] == len(src_groups), "gallery groups")
    src_group_order = []
    src_group_index = {}
    for item in src_gallery:
        g = item.get("group") or item.get("source")
        if g not in src_group_index:
            src_group_index[g] = len(src_group_order)
            src_group_order.append({"label": item.get("label", g), "items": []})
        src_group_order[src_group_index[g]]["items"].append(item["image"])
    audit.check(len(src_group_order) == pack.count[SEC["GALLERY"]], "gallery group order")
    for gi, sg in enumerate(src_group_order):
        loff, llen, first, gcount = struct.unpack_from("<IHHH", pack.sec[SEC["GALLERY"]], gi * 10)
        label = pack.text(loff, llen)
        audit.check(label == sg["label"], "gallery label %d" % gi)
        audit.check(gcount == len(sg["items"]), "gallery count %d" % gi)
        for n, expected_image in enumerate(sg["items"]):
            img, grp = struct.unpack_from("<HH", pack.sec[SEC["GALLERY_ITEMS"]], (first + n) * 4)
            rec = struct.unpack_from("<IIIIIIIIHHHBB", pack.sec[SEC["IMAGES"]], img * 40)
            audit.check(grp == gi, "gallery group index")
            audit.check(pack.string(rec[0]) == expected_image, "gallery image %s" % label)

    for i in range(pack.count[SEC["CHAPTERS"]]):
        scene, name, route1, flags = struct.unpack_from("<IIIH", pack.sec[SEC["CHAPTERS"]], i * 14)
        src = catalog["chapters"][i]
        audit.check(scene_names[scene] == src["id"], "chapter scene %d" % i)
        audit.check(pack.string(name) == src["name"], "chapter name %d" % i)
        audit.check((route1 - 1) == src["route"], "chapter route %d" % i)
    for i in range(pack.count[SEC["ROUTES"]]):
        name, first, count = struct.unpack_from("<IHH", pack.sec[SEC["ROUTES"]], i * 8)
        audit.check(pack.string(name) == catalog["routes"][i]["name"], "route name %d" % i)
        audit.check(count == len(catalog["routes"][i]["scenes"]), "route count %d" % i)
    for i in range(pack.count[SEC["AREAS"]]):
        sid, name = struct.unpack_from("<II", pack.sec[SEC["AREAS"]], i * 8)
        key = pack.string(sid)
        audit.check(catalog["areas"].get(key) == pack.string(name), "area %s" % key)

    expr_checks = 0
    for scene_i, scene in enumerate(scene_names):
        src_ops = scenes_src[scene]
        sid, nm, route1, flags, start, count = struct.unpack_from("<IIHHII", pack.sec[SEC["SCENES"]], scene_i * 20)
        for k, src in enumerate(src_ops):
            if src["op"] not in ("if", "set"):
                continue
            op, oflags, a, b, c, d = struct.unpack_from("<BBHHHI", pack.sec[SEC["OPS"]], (start + k) * 12)
            if src["op"] == "if":
                src_expr = expr_text(src["expr"])
                eid = a
            else:
                lhs, src_expr = split_assign(expr_text(src["expr"]))
                eid = b
            try:
                ast = SOURCE_EXPR(src_expr)
            except ExprError as exc:
                audit.check(False, "%s:%d expression parse %s" % (scene, k, exc))
                continue
            for trial in range(6):
                variables = dict(var_base) if trial == 0 else {
                    i: (rng.choice(strings[1:]) if pexpr.var_types[i] == 1 else
                        (bool(rng.getrandbits(1)) if pexpr.var_types[i] == 2 else float(rng.randint(-3, 12))))
                    for i in range(len(pexpr.var_names))
                }
                src_vars = {pexpr.var_names[i]: variables[i] for i in variables}
                try:
                    sv = ast.eval(src_vars)
                    pv = pexpr.eval(eid, variables)
                except Exception as exc:
                    audit.check(False, "%s:%d expression eval %s" % (scene, k, exc))
                    break
                if isinstance(sv, float) and isinstance(pv, float):
                    if math.isnan(sv) and math.isnan(pv):
                        ok = True
                    elif math.isinf(sv) or math.isinf(pv):
                        ok = (sv == pv)
                    else:
                        ok = abs(sv - pv) <= 1e-4
                else:
                    ok = (sv == pv) and (type(sv) is type(pv) or (isinstance(sv, bool) and isinstance(pv, bool)))
                if not audit.check(ok, "%s:%d expression result %r != %r" % (scene, k, sv, pv)):
                    break
                expr_checks += 1

    report["checks"] = audit.checks
    report["errors"] = audit.errors[:200]
    report["warnings"] = audit.warnings
    report["expression_checks"] = expr_checks
    report["source_files"] = len(files)
    report["source_images"] = len(src_images)
    report["pack_text_units"] = pack.count[SEC["TEXTS"]]
    report["op_errors"] = op_errors
    if args.report:
        os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
        with open(args.report, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=1)
    print(json.dumps({k: v for k, v in report.items() if k not in ("errors", "warnings")},
                     ensure_ascii=False, indent=1))
    if audit.errors:
        print("AUDIT FAIL (%d errors)" % len(audit.errors), file=sys.stderr)
        for e in audit.errors[:20]:
            print("  " + e, file=sys.stderr)
        return 1
    print("AUDIT PASS: %d checks" % audit.checks)
    return 0


def _face(appearance):
    for part in str(appearance or "").split(","):
        if re.match(r"^(Normal|Smile|Angry|Pensive|Cry|Flushed|Surprised|Fearful|Determined|Default)\d*$", part) or re.fullmatch(r"\d+", part):
            return part
    return ""


def _check_expr(audit, scene, k, kind, text, pack, expr_id, src):
    # result comparison is done in the expression pass; this only checks ids exist
    audit.check(expr_id < pack.count[SEC["EXPRS"]], "%s:%d %s expr id" % (scene, k, kind))


if __name__ == "__main__":
    raise SystemExit(main())
