#!/usr/bin/env python3
"""Independent source-vs-pack audit for the Summer Pockets reader.

This tool deliberately does NOT import tools/summer_pack.py and does NOT read
any previous build report.  It parses the RW5 source snapshot (catalog.js,
assets.js, gallery.txt, converted-story/*.json) and the produced pack with its
own readers, then compares:

  * scene table, op stream, text/speaker strings, jump/call targets;
  * every condition expression (evaluated under deterministic assignments);
  * choice groups/items, map groups/items, chapter/route/flow labels;
  * gallery groups/variants, image id mapping, JPEG headers and RLE masks;
  * progress metadata (clear flags, heroine/flow endings, globals).

Exit code 0 = PASS, 1 = FAIL, 2 = bad input.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
from collections import OrderedDict

MAGIC = b"SUMMPK01"
SEC_TEXT, SEC_OPS, SEC_OPIDX, SEC_SCENE, SEC_CHAPTER, SEC_ROUTE = range(6)
SEC_GALLERY, SEC_GALLERY_ITEM, SEC_IMAGE, SEC_IMAP = range(6, 10)
SEC_EXPR, SEC_EXPRIDX, SEC_VARS, SEC_CHOICE, SEC_CHOICE_ITEM = range(10, 15)
SEC_MAP, SEC_MAP_ITEM, SEC_MAPLABEL, SEC_FLOWLABEL, SEC_PROGRESS = range(15, 20)
SEC_META, SEC_SCENE_INDEX, SEC_SPEAKER, SEC_IMGDATA, SEC_CHARTAB = range(20, 25)

OP_TEXT, OP_IMAGE, OP_SET, OP_ADD, OP_SUB, OP_IF, OP_GOTO, OP_CALL = range(8)
OP_CALLSCENE, OP_JUMPSCENE, OP_RETURN, OP_END, OP_NOP, OP_CHOICE, OP_MAP, OP_CLEAR = range(8, 16)

EX_CONST, EX_VAR, EX_DATE, EX_STRLIT, EX_NATIVE, EX_UNSUPPORTED = range(6)
EX_NOT, EX_NEG, EX_POS, EX_BNOT = range(6, 10)
EX_ADD, EX_SUB, EX_MUL, EX_DIV, EX_MOD, EX_AND, EX_OR, EX_XOR = range(10, 18)
EX_SHL, EX_SHR, EX_EQ, EX_NE, EX_LT, EX_LE, EX_GT, EX_GE = range(18, 26)
EX_LAND, EX_LOR, EX_STREQ, EX_STRNE, EX_VARKEY = range(26, 31)

VISIBLE_TEXT_BASE = str.maketrans({"䌷": "紬", "؟": "？"})


def visible_text(value, full_name=False):
    text = str(value or "").translate(VISIBLE_TEXT_BASE)
    text = re.sub(r"紬[·・\s]?文德斯", "紬文德斯", text)
    text = text.replace("Tsumugi", "紬文德斯")
    if full_name:
        text = re.sub(r"紬(?!文德斯)", "紬文德斯", text)
    return text


class AuditError(SystemExit):
    pass


def die(msg):
    print(f"summer_audit: ERROR: {msg}", file=sys.stderr)
    raise SystemExit(2)


# ---------------------------------------------------------------- pack reader
class Pack:
    def __init__(self, path):
        self.blob = open(path, "rb").read()
        b = self.blob
        if len(b) < 20 or b[:8] != MAGIC:
            die("pack magic")
        version, header_size, count, flags, total = struct.unpack_from("<HHHHI", b, 8)
        if version != 1 or total != len(b) or count != 25:
            die("pack header")
        if header_size != 20 + 16 * count:
            die("pack header size")
        self.sections = {}
        for i in range(count):
            typ, _flags, off, cnt, size = struct.unpack_from("<HHIII", b, 20 + 16 * i)
            if off + size > len(b) or off % 4:
                die(f"section {typ} range")
            self.sections[typ] = (off, cnt, size)

    def sec(self, typ):
        return self.sections[typ]

    def data(self, typ):
        off, _cnt, size = self.sections[typ]
        return self.blob[off : off + size]

    def u16s(self, typ):
        off, cnt, size = self.sections[typ]
        return list(struct.unpack_from(f"<{cnt}H", self.blob, off))

    def u32s(self, typ):
        off, cnt, size = self.sections[typ]
        return list(struct.unpack_from(f"<{cnt}I", self.blob, off))

    def text_at(self, off):
        if off >= self.text_codes:
            raise IndexError("text offset")
        out = []
        index = off
        while index < self.text_codes:
            byte = index // 2 * 3
            value = self.text[index // 2 * 3] | (self.text[index // 2 * 3 + 1] << 8) | (
                self.text[index // 2 * 3 + 2] << 16)
            code = (value >> 12) & 0xFFF if index & 1 else value & 0xFFF
            if code == 0:
                return "".join(out)
            if code > self.chartab_count:
                raise IndexError("chartab code")
            cp = self.chartab[code - 1]
            out.append(chr(cp))
            index += 1
        raise IndexError("unterminated text")

    def open(self):
        self.text = self.data(SEC_TEXT)
        self.text_codes = self.sections[SEC_TEXT][1]
        chartab_bytes = self.data(SEC_CHARTAB)
        self.chartab = list(struct.unpack_from(f"<{len(chartab_bytes)//2}H", chartab_bytes, 0))
        self.chartab_count = len(self.chartab)
        if len(self.text) != ((self.text_codes + 1) // 2) * 3:
            die("text size")
        self.ops = self.data(SEC_OPS)
        self.opidx = self.u32s(SEC_OPIDX)
        self.scene_records = self._records(SEC_SCENE, "<HHIHHBBBBHHIIIII", 40,
                                           ("id", "_pad", "first_op", "op_count", "next", "route",
                                            "phase", "flags", "_pad2", "first_block", "block_count",
                                            "name", "map_name", "date", "date_label", "route_name"))
        self.scenes = {r["id"]: r for r in self.scene_records}
        self.scene_index = self.u16s(SEC_SCENE_INDEX)
        self.chapters = self._records(SEC_CHAPTER, "<HBBI", 8, ("scene", "route", "_pad", "name"))
        self.routes = self._records(SEC_ROUTE, "<IHH", 8, ("name", "scene_count", "chapter_count"))
        self.gallery = self._records(SEC_GALLERY, "<IHBB", 8, ("label", "first", "count", "_pad"))
        self.gallery_items = self.u16s(SEC_GALLERY_ITEM)
        self.images = self._records(SEC_IMAGE, "<IIIIHHBBH", 24,
                                    ("off", "len", "mask_off", "mask_len", "w", "h", "kind",
                                     "flags", "_pad"))
        self.imap = self.u16s(SEC_IMAP)
        self.expr = self.data(SEC_EXPR)
        self.expridx = self.u32s(SEC_EXPRIDX)
        self.vars = self.u32s(SEC_VARS)
        self.choice_groups = self._records(SEC_CHOICE, "<HBBHH", 8,
                                           ("first", "count", "_pad", "var", "_pad2"))
        self.choice_items = self._records(SEC_CHOICE_ITEM, "<IiI", 12,
                                          ("text", "value", "cond"))
        self.map_groups = self._records(SEC_MAP, "<HBBHH", 8,
                                        ("first", "count", "_pad", "var", "_pad2"))
        self.map_items = [r["value"] for r in self._records(SEC_MAP_ITEM, "<i", 4, ("value",))]
        self.map_labels = self._records(SEC_MAPLABEL, "<II", 8, ("key", "text"))
        self.flow_labels = self._records(SEC_FLOWLABEL, "<IBBHI", 12,
                                         ("key", "kind", "_p", "_p2", "text"))
        self.progress = self._progress()
        self.meta = self._meta()
        self.speakers = self.u32s(SEC_SPEAKER)
        self.imgdata = self.data(SEC_IMGDATA)

    def _records(self, typ, fmt, size, names=None):
        off, cnt, length = self.sections[typ]
        if length != cnt * size:
            die(f"section {typ} record size")
        fields = names or [f"f{i}" for i in range(len(struct.Struct(fmt).unpack_from(self.blob, off)))]
        out = []
        for i in range(cnt):
            values = struct.unpack_from(fmt, self.blob, off + i * size)
            out.append(dict(zip(fields, values)))
        return out

    def _meta(self):
        off, cnt, size = self.sections[SEC_META]
        if size != 76:
            die("meta size")
        m = self.blob[off : off + 76]
        return {
            "special_var": struct.unpack_from("<I", m, 0)[0],
            "var_count": struct.unpack_from("<I", m, 4)[0],
            "image_count": struct.unpack_from("<I", m, 8)[0],
            "gallery_count": struct.unpack_from("<I", m, 12)[0],
            "flow_scene": struct.unpack_from("<H", m, 16)[0],
            "initial_scene": struct.unpack_from("<H", m, 18)[0],
            "map_scene": struct.unpack_from("<H", m, 20)[0],
            "scene_count": struct.unpack_from("<H", m, 22)[0],
            "chapter_count": struct.unpack_from("<H", m, 24)[0],
            "route_count": struct.unpack_from("<H", m, 26)[0],
            "choice_count": struct.unpack_from("<I", m, 28)[0],
            "choice_item_count": struct.unpack_from("<I", m, 32)[0],
            "map_count": struct.unpack_from("<I", m, 36)[0],
            "map_item_count": struct.unpack_from("<I", m, 40)[0],
            "block_count": struct.unpack_from("<I", m, 44)[0],
            "op_bytes": struct.unpack_from("<I", m, 48)[0],
            "title": struct.unpack_from("<I", m, 52)[0],
            "subtitle": struct.unpack_from("<I", m, 56)[0],
            "version": struct.unpack_from("<I", m, 60)[0],
            "tagline": struct.unpack_from("<I", m, 64)[0],
            "logo_image": struct.unpack_from("<H", m, 68)[0],
            "title_bg_image": struct.unpack_from("<H", m, 70)[0],
        }

    def _progress(self):
        off, cnt, size = self.sections[SEC_PROGRESS]
        if size != 154:
            die("progress size")
        p = self.blob[off : off + 154]
        max_ending, heroine_count, flow_count, globals_count, _pad = struct.unpack_from("<HBBBB", p, 0)
        cursor = 6
        clear = {}
        heroine = {}
        for ending in range(1, 15):
            clear[ending] = struct.unpack_from("<I", p, cursor)[0]
            heroine[ending] = p[cursor + 4]
            cursor += 5
        flow = []
        for _ in range(flow_count):
            flow.append(struct.unpack_from("<HB", p, cursor))
            cursor += 3
        globals_ = list(struct.unpack_from(f"<{globals_count}I", p, cursor))
        cursor += globals_count * 4
        restart, special, clear_count = struct.unpack_from("<III", p, cursor)
        return {
            "max_ending": max_ending, "heroine_count": heroine_count,
            "flow_count": flow_count, "globals_count": globals_count,
            "clear": clear, "heroine": heroine, "flow": flow, "globals": globals_,
            "restart_var": restart, "special_var": special,
            "clear_count_var": clear_count,
        }

    # ---- ops
    def _uvar(self, p, end):
        value = 0
        shift = 0
        for _ in range(5):
            if p >= end:
                raise IndexError("varint")
            b = self.ops[p]
            p += 1
            value |= (b & 0x7F) << shift
            if not (b & 0x80):
                return value
            shift += 7
        raise IndexError("varint long")

    def _svar(self, p, end):
        u = self._uvar(p, end)
        return (u >> 1) ^ -(u & 1), u

    def op_at(self, offset):
        end = len(self.ops)
        if offset >= end:
            raise IndexError("op offset")
        kind = self.ops[offset]
        p = offset + 1
        op = {"kind": kind, "offset": offset}
        if kind == OP_TEXT:
            speaker = self._uvar(p, end)
            p = self._after_uvar(p, end)
            text = self._uvar(p, end)
            p = self._after_uvar(p, end)
            op.update(speaker=speaker, text=text)
        elif kind == OP_IMAGE:
            image = self._uvar(p, end)
            p = self._after_uvar(p, end)
            layer = self._uvar(p, end)
            p = self._after_uvar(p, end)
            op.update(image=image, layer=layer)
        elif kind in (OP_SET, OP_ADD, OP_SUB):
            var = self._uvar(p, end)
            p = self._after_uvar(p, end)
            tag = self.ops[p]
            p += 1
            if tag == 0:
                value, _ = self._svar(p, end)
                p = self._after_svar(p, end)
                op.update(var=var, tag=tag, value=value)
            else:
                expr = self._uvar(p, end)
                p = self._after_uvar(p, end)
                op.update(var=var, tag=tag, expr=expr)
        elif kind == OP_IF:
            expr = self._uvar(p, end)
            p = self._after_uvar(p, end)
            target = self._uvar(p, end)
            p = self._after_uvar(p, end)
            truth = self.ops[p]
            p += 1
            op.update(expr=expr, target=target, truth=truth)
        elif kind in (OP_GOTO, OP_CALL):
            target = self._uvar(p, end)
            p = self._after_uvar(p, end)
            op.update(target=target)
        elif kind in (OP_CALLSCENE, OP_JUMPSCENE):
            scene = self._uvar(p, end)
            p = self._after_uvar(p, end)
            target = self._uvar(p, end)
            p = self._after_uvar(p, end)
            op.update(scene=scene, target=target)
        elif kind in (OP_RETURN, OP_END, OP_NOP):
            pass
        elif kind in (OP_CHOICE, OP_MAP):
            group = self._uvar(p, end)
            p = self._after_uvar(p, end)
            op.update(group=group)
        elif kind == OP_CLEAR:
            layer = self._uvar(p, end)
            p = self._after_uvar(p, end)
            op.update(layer=layer)
        else:
            raise IndexError(f"unknown op {kind}")
        op["next"] = p
        op["size"] = p - offset
        return op

    def _after_uvar(self, p, end):
        while True:
            b = self.ops[p]
            p += 1
            if not (b & 0x80):
                return p

    def _after_svar(self, p, end):
        return self._after_uvar(p, end)

    def scene_ops(self, rec):
        count = rec["op_count"]
        offset = self.opidx[rec["first_block"]] if rec["block_count"] else 0
        out = []
        block_positions = []
        for i in range(count):
            if i % 32 == 0:
                block_positions.append(offset)
            if i % 32 == 0:
                # Verify the block table against the real sequential offset.
                expected_block = rec["first_block"] + i // 32
                if self.opidx[expected_block] != offset:
                    raise AuditError(
                        f"scene {rec['id']} block {i//32} offset {self.opidx[expected_block]} != {offset}")
            op = self.op_at(offset)
            out.append(op)
            offset = op["next"]
        return out

    # ---- expressions
    def expr_eval(self, expr_id, vars_, date):
        if expr_id == 0:
            return 1
        start = self.expridx[expr_id - 1]
        end = self.expridx[expr_id]
        p = start
        stack = []
        while p < end:
            op = self.expr[p]
            p += 1
            if op == EX_CONST:
                value, p = self._read_svar_at(p, end)
                stack.append(("i", value))
            elif op == EX_VAR:
                var, p = self._read_uvar_at(p, end)
                stack.append(("i", self.var_get(vars_, var)))
            elif op == EX_VARKEY:
                key = stack.pop()
                stack.append(("i", self.var_get(vars_, key[1])))
            elif op == EX_DATE:
                stack.append(("s", date))
            elif op == EX_STRLIT:
                off, p = self._read_uvar_at(p, end)
                stack.append(("s", self.text_at(off)))
            elif op == EX_NATIVE:
                stack.append(("i", 0))
            elif op == EX_UNSUPPORTED:
                return None
            elif op in (EX_NOT, EX_NEG, EX_POS, EX_BNOT):
                value = stack.pop()
                if value[0] != "i":
                    return None
                if op == EX_NOT:
                    stack.append(("i", 0 if value[1] else 1))
                elif op == EX_NEG:
                    stack.append(("i", -value[1]))
                elif op == EX_POS:
                    stack.append(("i", value[1]))
                else:
                    stack.append(("i", ~value[1]))
            elif op in (EX_STREQ, EX_STRNE):
                right = stack.pop()
                left = stack.pop()
                equal = left[1] == right[1] if left[0] == right[0] == "s" else False
                stack.append(("i", (1 if equal else 0) if op == EX_STREQ else (0 if equal else 1)))
            else:
                right = stack.pop()
                left = stack.pop()
                if left[0] != "i" or right[0] != "i":
                    if op in (EX_EQ, EX_NE) and left[0] == right[0] == "s":
                        equal = left[1] == right[1]
                        stack.append(("i", (1 if equal else 0) if op == EX_EQ else (0 if equal else 1)))
                        continue
                    return None
                a, b = left[1], right[1]
                if op == EX_ADD: r = a + b
                elif op == EX_SUB: r = a - b
                elif op == EX_MUL: r = a * b
                elif op == EX_DIV: r = int(a / b) if b else 0
                elif op == EX_MOD: r = a % b if b else 0
                elif op == EX_AND: r = a & b
                elif op == EX_OR: r = a | b
                elif op == EX_XOR: r = a ^ b
                elif op == EX_SHL: r = a << (b & 31)
                elif op == EX_SHR: r = a >> (b & 31)
                elif op == EX_EQ: r = 1 if a == b else 0
                elif op == EX_NE: r = 1 if a != b else 0
                elif op == EX_LT: r = 1 if a < b else 0
                elif op == EX_LE: r = 1 if a <= b else 0
                elif op == EX_GT: r = 1 if a > b else 0
                elif op == EX_GE: r = 1 if a >= b else 0
                elif op == EX_LAND: r = 1 if a and b else 0
                elif op == EX_LOR: r = 1 if a or b else 0
                else: raise AuditError(f"expr op {op}")
                stack.append(("i", r))
        if len(stack) != 1:
            raise AuditError("expr stack")
        return stack[0][1]

    def _read_uvar_at(self, p, end):
        value = 0
        shift = 0
        while p < end:
            b = self.expr[p]
            p += 1
            value |= (b & 0x7F) << shift
            if not (b & 0x80):
                return value, p
            shift += 7
        raise IndexError("expr varint")

    def _read_svar_at(self, p, end):
        u, p = self._read_uvar_at(p, end)
        return (u >> 1) ^ -(u & 1), p

    def var_get(self, vars_, var):
        if var == 24006:
            return sum(1 for v in (12003, 12004, 12005, 12006, 12575, 12007, 12008)
                       if vars_.get(v, 0) == 1)
        return vars_.get(var, 0)


# ---------------------------------------------------------------- source
def load_object(path, prefix):
    text = open(path, encoding="utf-8").read()
    match = re.search(prefix + r"\s*=\s*(\{.*\})\s*;", text, re.S)
    if not match:
        die(f"{path}: object")
    return json.loads(match.group(1))


def load_source(root):
    catalog = load_object(os.path.join(root, "src/common/catalog.js"), "export const catalog")
    assets = open(os.path.join(root, "src/common/assets.js"), encoding="utf-8").read()
    match = re.search(r"const paths\s*=\s*(\{.*?\});", assets, re.S)
    if not match:
        die("assets paths")
    paths = json.loads(match.group(1))
    gallery = json.load(open(os.path.join(root, "src/common/gallery.txt"), encoding="utf-8"))
    scenes = {}
    for name in os.listdir(os.path.join(root, "converted-story")):
        if name.endswith(".json"):
            scenes[int(name[:-5])] = json.load(open(os.path.join(root, "converted-story", name),
                                                    encoding="utf-8"))
    return catalog, paths, gallery, scenes


def source_eval(expr, vars_, date):
    if isinstance(expr, (int, bool)):
        return int(expr)
    if not isinstance(expr, list) or not expr:
        raise AuditError(f"bad source expr {expr!r}")
    op = expr[0]
    if op == "v":
        key = source_eval(expr[1], vars_, date)
        if key == 24006:
            return sum(1 for v in (12003, 12004, 12005, 12006, 12575, 12007, 12008)
                       if vars_.get(v, 0) == 1)
        return vars_.get(int(key), 0)
    if op == "s":
        return date
    if op == "string":
        return str(expr[1])
    if op == "native":
        return 0
    if op == "unsupported":
        return None
    if op == "!":
        return 0 if source_eval(expr[1], vars_, date) else 1
    if op == "neg":
        return -source_eval(expr[1], vars_, date)
    if op == "pos":
        return +source_eval(expr[1], vars_, date)
    if op == "~":
        return ~source_eval(expr[1], vars_, date)
    if op == "&&":
        for sub in expr[1:]:
            if not source_eval(sub, vars_, date):
                return 0
        return 1
    if op == "||":
        for sub in expr[1:]:
            if source_eval(sub, vars_, date):
                return 1
        return 0
    a = source_eval(expr[1], vars_, date)
    b = source_eval(expr[2], vars_, date)
    if isinstance(a, str) or isinstance(b, str):
        if op == "==": return 1 if a == b else 0
        if op == "!=": return 1 if a != b else 0
        return None
    ops = {
        "+": lambda: a + b, "-": lambda: a - b, "*": lambda: a * b,
        "/": lambda: int(a / b) if b else 0, "%": lambda: a % b if b else 0,
        "&": lambda: a & b, "|": lambda: a | b, "^": lambda: a ^ b,
        "<<": lambda: a << (b & 31), ">>": lambda: a >> (b & 31),
        "==": lambda: 1 if a == b else 0, "!=": lambda: 1 if a != b else 0,
        "<": lambda: 1 if a < b else 0, "<=": lambda: 1 if a <= b else 0,
        ">": lambda: 1 if a > b else 0, ">=": lambda: 1 if a >= b else 0,
    }
    if op not in ops:
        raise AuditError(f"source op {op}")
    return ops[op]()


def collect_vars(expr, out):
    if isinstance(expr, list) and expr:
        if expr[0] == "v" and isinstance(expr[1], int):
            out.add(int(expr[1]))
        for sub in expr[1:]:
            collect_vars(sub, out)


def collect_source_vars(scenes):
    out = set()
    for ops in scenes.values():
        for op in ops:
            if "variable" in op and isinstance(op["variable"], int):
                out.add(op["variable"])
            if op["op"] == "if":
                collect_vars(op.get("value"), out)
            if op["op"] == "choice":
                for item in op.get("items", []):
                    collect_vars(item.get("condition", 1), out)
    out.add(24006)
    return out


def assignments(var_ids):
    ids = sorted(var_ids)
    patterns = []
    for mode in range(6):
        values = {}
        for var in ids:
            if mode == 0:
                v = 0
            elif mode == 1:
                v = 1 if var % 3 == 0 else 0
            elif mode == 2:
                v = var % 7
            elif mode == 3:
                v = 1 if var in (12003, 12004, 12005, 12006, 12007, 12008, 12575) else 0
            elif mode == 4:
                v = (var * 17) % 11 - 3
            else:
                v = 2 if var % 2 else -1
            values[var] = v
        patterns.append(values)
    return patterns


# ---------------------------------------------------------------- image helpers
def jpeg_size(data):
    if len(data) < 4 or data[0] != 0xFF or data[1] != 0xD8:
        return None
    p = 2
    while p + 4 <= len(data):
        if data[p] != 0xFF:
            p += 1
            continue
        marker = data[p + 1]
        p += 2
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        if p + 2 > len(data):
            return None
        seglen = (data[p] << 8) | data[p + 1]
        if seglen < 2 or p + seglen > len(data):
            return None
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB):
            if seglen < 7:
                return None
            h = (data[p + 3] << 8) | data[p + 4]
            w = (data[p + 5] << 8) | data[p + 6]
            return (w, h)
        p += seglen
    return None


def mask_ok(pack, entry):
    w, h = entry["w"], entry["h"]
    if entry["kind"] != 1:
        return True
    if entry["mask_len"] == 0:
        return False
    p = pack.imgdata[entry["mask_off"] : entry["mask_off"] + entry["mask_len"]]
    pos = 0

    def uvar():
        nonlocal pos
        value = 0
        shift = 0
        for _ in range(5):
            if pos >= len(p):
                raise IndexError
            b = p[pos]
            pos += 1
            value |= (b & 0x7F) << shift
            if not (b & 0x80):
                return value
            shift += 7
        raise IndexError

    try:
        for _y in range(h):
            runs = uvar()
            x = 0
            for _r in range(runs):
                x += uvar()
            if x != w:
                return False
    except IndexError:
        return False
    return pos == len(p)


# ---------------------------------------------------------------- audit
def audit(source_root, pack_path):
    catalog, paths, gallery, scenes = load_source(source_root)
    pack = Pack(pack_path)
    pack.open()
    failures = []
    checks = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)

    # ---- meta / entry points
    check(pack.meta["flow_scene"] == int(catalog["flowScene"]), "flowScene")
    check(pack.meta["initial_scene"] == int(catalog["initial"]), "initial")
    check(pack.meta["map_scene"] == int(catalog.get("mapScene", 0)), "mapScene")
    check(pack.meta["scene_count"] == len(scenes), "scene count")
    check(pack.meta["chapter_count"] == len(catalog["chapters"]), "chapter count")
    check(pack.meta["route_count"] == len(catalog["routes"]), "route count")
    check(pack.meta["gallery_count"] == len(gallery_groups(gallery)), "gallery groups")
    check(pack.meta["image_count"] == len(pack.images), "image count")
    check(pack.meta["var_count"] == len(pack.vars), "var count")
    check(sorted(pack.vars) == sorted(collect_source_vars(scenes)), "var ids")
    check(len(pack.scene_index) >= max(scenes) + 1, "scene index length")
    for sid in scenes:
        if sid < len(pack.scene_index):
            index = pack.scene_index[sid]
            check(index < len(pack.scene_records) and pack.scene_records[index]["id"] == sid,
                  f"scene index {sid}")
    check(pack.meta["logo_image"] < len(pack.images), "logo image")
    check(pack.meta["title_bg_image"] < len(pack.imap), "title bg id")
    checks.append("meta/entry points")

    # ---- scenes and ops
    var_ids = collect_source_vars(scenes)
    patterns = assignments(var_ids)
    dates = ["7月29日", "8月1日", "2月29日"]
    choice_seen = 0
    map_seen = 0
    total_ops = 0
    for sid in sorted(scenes):
        rec = pack.scenes.get(sid)
        check(rec is not None, f"missing scene {sid}")
        if not rec:
            continue
        source_ops = scenes[sid]
        meta = catalog["scenes"][str(sid)]
        check(rec["op_count"] == len(source_ops), f"scene {sid} op count")
        check(rec["next"] == int(meta.get("next", 0) or 0), f"scene {sid} next")
        route = rec["route"] - 256 if rec["route"] >= 128 else rec["route"]
        check(route == int(meta.get("route", -1)), f"scene {sid} route")
        phase = {"personal": 1, "final": 2}.get(str(meta.get("phase", "")), 0)
        check(rec["phase"] == phase, f"scene {sid} phase")
        check(pack.text_at(rec["name"]) == visible_text(meta.get("name", ""), True),
              f"scene {sid} name")
        source = str(meta.get("source", ""))
        m = re.search(r"(\d{2})(\d{2})(AM|PM|[a-z]?)$", source)
        check(pack.text_at(rec["route_name"]) == visible_text(meta.get("routeName", ""), True),
              f"scene {sid} routeName")
        raw_date = ""
        if m and 1 <= int(m.group(1)) <= 12 and 1 <= int(m.group(2)) <= 31:
            raw_date = "".join(chr(ord(c) + 0xFEE0) if "0" <= c <= "9" else c
                               for c in f"{int(m.group(1))}月{int(m.group(2))}日")
        check(pack.text_at(rec["date"]) == raw_date, f"scene {sid} raw date")
        check(pack.text_at(rec["date_label"]) == str(meta.get("date", "")),
              f"scene {sid} date label")
        check(pack.text_at(rec["map_name"]) == visible_text(meta.get("name", ""), False),
              f"scene {sid} map name")
        if source.startswith("_"):
            check(rec["flags"] & 1, f"scene {sid} skip flag")
        if re.match(r"^(?:RB)?90_", source):
            check(rec["flags"] & 2, f"scene {sid} map-hide flag")
        try:
            pack_ops = pack.scene_ops(rec)
        except AuditError as exc:
            failures.append(str(exc))
            continue
        if len(pack_ops) != len(source_ops):
            failures.append(f"scene {sid} pack op count")
            continue
        for index, (src, dst) in enumerate(zip(source_ops, pack_ops)):
            total_ops += 1
            where = f"scene {sid} op {index} {src['op']}"
            kind = src["op"]
            if kind == "text":
                check(dst["kind"] == OP_TEXT, where + " kind")
                check(pack.text_at(dst["text"]) == visible_text(src.get("text", "")), where + " text")
                speaker = pack.text_at(pack.speakers[dst["speaker"]]) if dst["speaker"] < len(pack.speakers) else "?"
                check(speaker == visible_text(src.get("speaker", ""), True), where + " speaker")
            elif kind == "image":
                want = int(str(src.get("image", ""))[1:]) if str(src.get("image", "")).startswith("i") else 0
                check(dst["kind"] == OP_IMAGE and dst["image"] == want, where + " image")
                check(dst["layer"] == int(src.get("layer", 0)), where + " layer")
            elif kind in ("set", "add", "sub"):
                want_kind = {"set": OP_SET, "add": OP_ADD, "sub": OP_SUB}[kind]
                check(dst["kind"] == want_kind, where + " kind")
                check(dst["var"] == int(src["variable"]), where + " var")
                value = src.get("value", 0)
                if isinstance(value, list):
                    check(dst.get("tag") == 1, where + " expr tag")
                    for pattern, date in zip(patterns, dates * 2):
                        want = source_eval(value, pattern, date)
                        got = pack.expr_eval(dst["expr"], pattern, date)
                        check(want == got, where + f" expr {want} != {got}")
                else:
                    check(dst.get("tag") == 0 and dst.get("value") == int(value),
                          where + " literal")
            elif kind == "if":
                check(dst["kind"] == OP_IF, where + " kind")
                check(dst["target"] == int(src["target"]), where + " target")
                check(dst["truth"] == (1 if src.get("truth") else 0), where + " truth")
                for pattern, date in zip(patterns, dates * 2):
                    want = source_eval(src.get("value"), pattern, date)
                    got = pack.expr_eval(dst["expr"], pattern, date)
                    check(want == got, where + f" cond {want} != {got}")
            elif kind in ("goto", "call"):
                want_kind = OP_GOTO if kind == "goto" else OP_CALL
                check(dst["kind"] == want_kind and dst["target"] == int(src["target"]),
                      where + " target")
            elif kind in ("callScene", "jumpScene"):
                want_kind = OP_CALLSCENE if kind == "callScene" else OP_JUMPSCENE
                check(dst["kind"] == want_kind and dst["scene"] == int(src["scene"]) and
                      dst["target"] == int(src["target"]), where + " scene/target")
            elif kind == "return":
                check(dst["kind"] == OP_RETURN, where + " kind")
            elif kind == "end":
                check(dst["kind"] == OP_END, where + " kind")
            elif kind == "nop":
                check(dst["kind"] == OP_NOP, where + " kind")
            elif kind == "clear":
                check(dst["kind"] == OP_CLEAR and dst["layer"] == int(src.get("layer", 0)),
                      where + " layer")
            elif kind == "choice":
                check(dst["kind"] == OP_CHOICE, where + " kind")
                group = dst["group"]
                check(0 <= group < len(pack.choice_groups), where + " group range")
                if 0 <= group < len(pack.choice_groups):
                    g = pack.choice_groups[group]
                    check(g["var"] == int(src["variable"]), where + " choice var")
                    check(g["count"] == len(src["items"]), where + " choice count")
                    for i, item in enumerate(src["items"]):
                        if g["first"] + i >= len(pack.choice_items):
                            failures.append(where + " choice item range")
                            continue
                        pi = pack.choice_items[g["first"] + i]
                        check(pack.text_at(pi["text"]) == visible_text(item.get("text", "")),
                              where + f" choice text {i}")
                        check(pi["value"] == int(item.get("value", 0)), where + f" choice value {i}")
                        cond = item.get("condition", 1)
                        for pattern, date in zip(patterns, dates * 2):
                            want = 1 if cond == 1 else source_eval(cond, pattern, date)
                            got = 1 if pi["cond"] == 0 else pack.expr_eval(pi["cond"], pattern, date)
                            check(want == got, where + f" choice cond {i}")
                choice_seen += 1
            elif kind == "map":
                check(dst["kind"] == OP_MAP, where + " kind")
                group = dst["group"]
                check(0 <= group < len(pack.map_groups), where + " map group")
                if 0 <= group < len(pack.map_groups):
                    g = pack.map_groups[group]
                    check(g["var"] == int(src["variable"]), where + " map var")
                    check(g["count"] == len(src["items"]), where + " map count")
                    for i, item in enumerate(src["items"]):
                        if g["first"] + i < len(pack.map_items):
                            check(pack.map_items[g["first"] + i] == int(item.get("value", 0)),
                                  where + f" map item {i}")
                map_seen += 1
            else:
                failures.append(f"{where} unsupported source op")
    checks.append(f"scene/op/text/targets ({total_ops} ops, {choice_seen} choices, {map_seen} maps)")

    # ---- chapter / route / flow / map labels
    group_by_key = OrderedDict()
    for item in gallery:
        source = str(item.get("source", ""))
        m = re.match(r"^(cg|ef)_([a-z]+[0-9]+)_", source, re.I)
        key = (m.group(1).lower() + "_" + m.group(2).lower()) if m else source.lower()
        group_by_key.setdefault(key, []).append(item)
    check(len(pack.chapters) == len(catalog["chapters"]), "chapters count")
    for i, chapter in enumerate(catalog["chapters"]):
        if i >= len(pack.chapters):
            break
        pc = pack.chapters[i]
        check(pc["scene"] == int(chapter["id"]), f"chapter {i} scene")
        check((pc["route"] if pc["route"] < 128 else pc["route"] - 256) ==
              int(chapter.get("route", -1)), f"chapter {i} route")
        check(pack.text_at(pc["name"]) == visible_text(chapter.get("name", ""), True),
              f"chapter {i} name")
    check(len(pack.routes) == len(catalog["routes"]), "routes count")
    for i, route in enumerate(catalog["routes"]):
        if i >= len(pack.routes):
            break
        pr = pack.routes[i]
        check(pack.text_at(pr["name"]) == visible_text(route.get("name", ""), True), f"route {i} name")
        check(pr["scene_count"] == len(route.get("scenes", [])), f"route {i} scenes")
        count = sum(1 for c in catalog["chapters"] if int(c.get("route", -1)) == i)
        check(pr["chapter_count"] == count, f"route {i} chapters")
    checks.append("chapters/routes")

    # map labels
    source_map = {}
    for key, value in catalog.get("mapLabels", {}).items():
        scene_s, cursor_s = str(key).split(":", 1)
        source_map[(int(scene_s) << 16) | int(cursor_s)] = visible_text(value)
    check(len(pack.map_labels) == len(source_map), "map label count")
    for entry in pack.map_labels:
        want = source_map.get(entry["key"])
        check(want is not None and pack.text_at(entry["text"]) == want,
              f"map label {entry['key']}")
    source_flow = {}
    for key, value in catalog.get("flowLabels", {}).items():
        source_flow[(int(key), 0)] = visible_text(value, True)
    for key, value in catalog.get("flowDates", {}).items():
        source_flow[(int(key), 1)] = visible_text(value)
    check(len(pack.flow_labels) == len(source_flow), "flow label count")
    for entry in pack.flow_labels:
        want = source_flow.get((entry["key"], entry["kind"]))
        check(want is not None and pack.text_at(entry["text"]) == want,
              f"flow label {entry['key']}/{entry['kind']}")
    checks.append(f"map labels ({len(source_map)}) / flow labels ({len(source_flow)})")

    # ---- gallery
    check(len(pack.gallery) == len(group_by_key), "gallery groups")
    index = 0
    for key, items in group_by_key.items():
        if index >= len(pack.gallery):
            break
        pg = pack.gallery[index]
        check(pack.text_at(pg["label"]) == visible_text(items[0].get("label", ""), False),
              f"gallery {key} label")
        check(pg["count"] == len(items), f"gallery {key} count")
        for j, item in enumerate(items):
            iid = int(str(item["image"])[1:])
            want = pack.imap[iid] if iid < len(pack.imap) else 0xFFFF
            got = pack.gallery_items[pg["first"] + j]
            check(want == got, f"gallery {key} variant {j}")
        index += 1
    checks.append(f"gallery ({len(group_by_key)} groups / {len(gallery)} items)")

    # ---- images / JPEG / masks
    referenced = set()
    for ops in scenes.values():
        for op in ops:
            if op["op"] == "image" and str(op.get("image", "")).startswith("i"):
                try:
                    referenced.add(int(str(op["image"])[1:]))
                except ValueError:
                    pass
    for item in gallery:
        try:
            referenced.add(int(str(item["image"])[1:]))
        except (KeyError, ValueError):
            pass
    referenced.add(int(pack.meta["title_bg_image"]))
    fallback = sorted(i for i in referenced if f"i{i}" not in paths)
    check(len(fallback) == 57, f"source fallback ids {len(fallback)}")
    for iid in sorted(referenced):
        entry_index = pack.imap[iid] if iid < len(pack.imap) else 0xFFFF
        check(entry_index < len(pack.images), f"image {iid} mapping")
        if entry_index >= len(pack.images):
            continue
        entry = pack.images[entry_index]
        if f"i{iid}" not in paths:
            fallback_entry = pack.imap[fallback[0]] if fallback else 0
            check(entry_index == fallback_entry, f"image {iid} fallback entry")
            check(entry["kind"] == 0, f"image {iid} fallback kind")
            continue
        rel = paths[f"i{iid}"]
        want_kind = 1 if rel.startswith("portrait_") else 0
        check(entry["kind"] == want_kind, f"image {iid} kind")
        jpeg = pack.imgdata[entry["off"] : entry["off"] + entry["len"]]
        size = jpeg_size(jpeg)
        check(size == (entry["w"], entry["h"]), f"image {iid} jpeg size {size}")
        if want_kind == 1:
            check(entry["w"] <= 120 and entry["h"] <= 150, f"portrait {iid} box")
            check(mask_ok(pack, entry), f"portrait {iid} mask")
        else:
            check(entry["w"] <= 256 and entry["h"] <= 144, f"scene {iid} box")
    # unique content entries: black + all unique source files + logo
    unique_files = {paths[f"i{i}"] for i in referenced if f"i{i}" in paths}
    check(len(pack.images) == 1 + len(unique_files | {"bg_black.jpg"}) + 1, "image entry count")
    portraits = sum(1 for e in pack.images if e["kind"] == 1)
    scenes_count = sum(1 for e in pack.images if e["kind"] == 0)
    check(portraits == 292 + 1, f"portrait entries {portraits}")
    check(scenes_count == 1032, f"scene entries {scenes_count}")
    if pack.meta["logo_image"] < len(pack.images):
        check(pack.images[pack.meta["logo_image"]]["kind"] == 1, "logo kind")
    checks.append(f"images ({len(referenced)} ids / {len(unique_files)} unique files / {len(fallback)} source fallbacks)")

    # ---- progress metadata
    clear_flags = {4: 12003, 5: 12004, 6: 12005, 7: 12006, 8: 12007, 9: 12008,
                   10: 12573, 12: 12574, 13: 12575, 14: 12576}
    heroines = [4, 5, 6, 7, 13]
    flow_endings = {222: 1, 261: 2, 289: 3, 321: 4, 350: 5, 374: 6, 405: 7,
                    441: 8, 456: 9, 489: 10, 510: 11, 523: 12, 548: 13, 566: 14}
    globals_keys = [12001, 12002, 12010, 12011, 12012, 12013]
    p = pack.progress
    check(p["max_ending"] == 14 and p["heroine_count"] == 5, "progress ending counts")
    for ending in range(1, 15):
        check(p["clear"][ending] == clear_flags.get(ending, 0), f"clear flag {ending}")
        check(p["heroine"][ending] == (1 if ending in heroines else 0), f"heroine {ending}")
    check(p["flow"] == sorted(flow_endings.items()), "flow endings")
    check(p["globals"] == globals_keys, "globals keys")
    check(p["restart_var"] == 14003 and p["special_var"] == 24006 and
          p["clear_count_var"] == 24006, "progress vars")
    checks.append("progress metadata")

    return failures, checks, {
        "scenes": len(scenes), "ops": total_ops, "text_codes": pack.text_codes,
        "images": len(pack.images), "image_ids": len(referenced),
        "gallery_groups": len(pack.gallery), "choices": choice_seen, "maps": map_seen,
        "pack_bytes": len(pack.blob),
    }


def gallery_groups(gallery):
    groups = OrderedDict()
    for item in gallery:
        source = str(item.get("source", ""))
        m = re.match(r"^(cg|ef)_([a-z]+[0-9]+)_", source, re.I)
        key = (m.group(1).lower() + "_" + m.group(2).lower()) if m else source.lower()
        groups.setdefault(key, []).append(item)
    return groups


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", default=os.environ.get("SUMMER_SOURCE", ""))
    ap.add_argument("--pack", default="main/summer_data/summer_pack.bin")
    ap.add_argument("--report", default="")
    args = ap.parse_args()
    if not args.source or not os.path.isdir(args.source):
        ap.error("需要 --source（RW5 源快照）")
    if not os.path.exists(args.pack):
        ap.error(f"找不到资源包 {args.pack}")
    failures, checks, stats = audit(args.source, args.pack)
    report = {"ok": not failures, "checks": checks, "stats": stats,
              "failures": failures[:200]}
    if args.report:
        os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
        json.dump(report, open(args.report, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    for check in checks:
        print(f"  PASS {check}")
    if failures:
        print(f"FAIL {len(failures)} issue(s):", file=sys.stderr)
        for item in failures[:30]:
            print(f"  - {item}", file=sys.stderr)
        return 1
    print("summer_audit: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
