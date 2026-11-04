#!/usr/bin/env python3
"""Independent CLANNAD (RW5 watch port) source audit and reference interpreter.

This tool parses the *source checkout* on its own: it does not import the
packer, does not read a previous build report, and does not trust the source
project's own test reports for its numbers.  Later, `--pack` re-parses the
built pack byte-by-byte and compares every op with this interpreter's reading
of the source.

The interpreter mirrors `src/common/engine.js` of the source snapshot:

  * catalog.initial / flowScene / scenes[].length / scenes[].entries
  * 512-op block files `story/<scene>/<block>.txt`
  * text / choice / dynamicChoice / set / native / goto / ifFalse / ifTrue /
    call / jumpScene / callScene / return / end / background / bodies / bodyAdd
  * value(): variables, integer operators, native calls, string methods
  * JS truthiness / truncating division / strict equality on the values that
    actually occur in this script

Usage:
  python3 tools/clannad_audit.py --source <clannad-rw5> --sweep --json <out>
  python3 tools/clannad_audit.py --source <clannad-rw5> --pack <pack.bin> --json <out>
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import zlib
from collections import Counter, OrderedDict, defaultdict

BS = chr(92)
BLOCK = 512
VAR_KINDS = {
    "f": 0, "g": 1, "s": 2, "a": 3, "z": 4, "cur_call.l": 5,
    "cur_call.k": 6, "counter": 7, "front.mwnd": 8, "c": 9, "pcmch": 10,
    "cur_call.$psc_no": 11, "cur_call.$obj_no": 12,
    "cur_call.$obj_no1": 13, "cur_call.$obj_no2": 14,
}
KIND_NAMES = {v: k for k, v in VAR_KINDS.items()}
NO_INDEX = 0xFFFFF


def balanced_json(text: str, marker: str):
    start = text.index(marker) + len(marker)
    while text[start] not in "{[":
        start += 1
    depth = 0
    instr = False
    esc = False
    for j in range(start, len(text)):
        c = text[j]
        if instr:
            if esc:
                esc = False
            elif c == BS:
                esc = True
            elif c == '"':
                instr = False
            continue
        if c == '"':
            instr = True
        elif c in "{[":
            depth += 1
        elif c in "}]":
            depth -= 1
            if depth == 0:
                return json.loads(text[start:j + 1])
    raise ValueError("unbalanced JSON at " + marker)


# ---------------------------------------------------------------- source model
class Source:
    def __init__(self, root: str):
        self.root = root
        common = os.path.join(root, "src/common")
        self.catalog = balanced_json(
            open(os.path.join(common, "catalog.js"), encoding="utf-8").read(),
            "export const catalog=")
        assets = balanced_json(
            open(os.path.join(common, "assets.js"), encoding="utf-8").read(),
            "const assets=")
        self.assets = {str(k).lower(): v for k, v in assets.items()}
        self.gallery = json.load(open(os.path.join(common, "gallery.txt"), encoding="utf-8"))
        self.scenes = {int(k): v for k, v in self.catalog["scenes"].items()}
        self.blocks = {}
        self.chunk_files = sorted(
            os.path.join(dp, f)
            for dp, _, fs in os.walk(os.path.join(common, "story"))
            for f in fs if f.endswith(".txt"))
        self.op_counts = Counter()
        for path in self.chunk_files:
            rel = os.path.relpath(path, os.path.join(common, "story"))
            scene_s, block_s = rel.replace(BS, "/").split("/")
            scene = int(scene_s)
            block = int(block_s.split(".")[0])
            self.blocks[(scene, block)] = json.load(open(path, encoding="utf-8"))
        for ops in self.blocks.values():
            for op in ops:
                self.op_counts[op["op"]] += 1

    def block_for(self, scene: int, cursor: int):
        return self.blocks[(scene, cursor // BLOCK)]

    def count_vars(self, state) -> int:
        return len(state.variables)


class RefError(Exception):
    pass


class End(Exception):
    pass


# ---------------------------------------------------------------- value model
def truthy(v) -> bool:
    if isinstance(v, str):
        return len(v) > 0
    return bool(v)


def as_int(v) -> int:
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        return v
    if isinstance(v, float):
        return int(v)
    if isinstance(v, str):
        try:
            return int(v.strip() or 0)
        except ValueError:
            return 0
    return 0


def js_str(v) -> str:
    if isinstance(v, str):
        return v
    if v is None:
        return "undefined"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, float) and v.is_integer():
        return str(int(v))
    return str(v)


def js_utf16_len(text: str) -> int:
    return sum(2 if ord(ch) > 0xFFFF else 1 for ch in text)


def js_slice(text: str, start: int, end=None) -> str:
    # JS String.prototype.slice on UTF-16 code units, clamped; the source text
    # contains BMP characters only (checked by the audit), so code points ==
    # UTF-16 code units for these strings.
    n = len(text)
    if start < 0:
        start = max(0, n + start)
    start = min(n, start)
    if end is None:
        end = n
    elif end < 0:
        end = max(0, n + end)
    end = min(n, end)
    if end <= start:
        return ""
    return text[start:end]


class State:
    __slots__ = ("chapter", "root_chapter", "cursor", "background",
                 "regular_background", "body", "bodies", "speaker", "text",
                 "choices", "history", "variables", "stack", "ended",
                 "progression", "counters")

    def __init__(self, chapter=None, story=False):
        self.chapter = chapter
        self.root_chapter = chapter
        self.cursor = 0
        self.background = "kuro"
        self.regular_background = "kuro"
        self.body = ""
        self.bodies = []
        self.speaker = ""
        self.text = ""
        self.choices = []
        self.history = []
        self.variables = {}
        self.stack = []
        self.ended = False
        self.progression = "story" if story else "chapter"
        self.counters = {}

    def clone(self):
        st = State()
        for slot in self.__slots__:
            value = getattr(self, slot)
            if isinstance(value, list):
                value = list(value)
            elif isinstance(value, dict):
                value = dict(value)
            setattr(st, slot, value)
        return st


def var_key(kind: str, index) -> str:
    return kind if index is None else f"{kind}:{index}"


class Interpreter:
    def __init__(self, source: Source):
        self.src = source
        self.steps = 0
        self.jumps = 0
        self.string_bytes = 0
        self.string_keys = set()
        self.max_string_len = 0

    # -- value() -----------------------------------------------------------
    def value(self, e, s: State):
        if not isinstance(e, list):
            return e
        op = e[0]
        if op == "v":
            key = var_key(e[1], self.value(e[2], s) if len(e) > 2 else None)
            if key in s.variables:
                return s.variables[key]
            if re.match(r"^(k|cur_call\.k)", e[1]):
                return ""
            return 0
        if op == "call":
            args = [self.value(a, s) for a in e[2]]
            receiver = self.value(e[3], s) if len(e) > 3 else None
            receiver_key = var_key(e[3][1], self.value(e[3][2], s)) \
                if len(e) > 3 and isinstance(e[3], list) and e[3][0] == "v" \
                else ""
            return self.native(e[1], args, s, receiver, receiver_key)
        a = self.value(e[1], s)
        if len(e) == 2:
            if op == "-":
                return -as_int(a)
            if op == "+":
                return +as_int(a)
            if op == "!":
                return 0 if truthy(a) else 1
            if op == "~":
                return ~as_int(a)
            raise RefError("bad unary " + op)
        if op == "&&":
            return self.value(e[2], s) if truthy(a) else 0
        if op == "||":
            return 1 if truthy(a) else self.value(e[2], s)
        b = self.value(e[2], s)
        if op == "+":
            if isinstance(a, str) or isinstance(b, str):
                return js_str(a) + js_str(b)
            return as_int(a) + as_int(b)
        ai, bi = as_int(a), as_int(b)
        if op == "-":
            return ai - bi
        if op == "*":
            return ai * bi
        if op == "/":
            if bi == 0:
                return 0
            q = abs(ai) // abs(bi)
            return q if (ai >= 0) == (bi >= 0) else -q
        if op == "%":
            return 0 if bi == 0 else int(math_fmod(ai, bi))
        if op == "==":
            return 1 if (a == b and type(a) is type(b) or (isinstance(a, (int, float)) and isinstance(b, (int, float)) and a == b)) else 0
        if op == "!=":
            return 0 if (a == b and (type(a) is type(b) or (isinstance(a, (int, float)) and isinstance(b, (int, float))))) else 1
        if op == "<":
            return 1 if ai < bi else 0
        if op == "<=":
            return 1 if ai <= bi else 0
        if op == ">":
            return 1 if ai > bi else 0
        if op == ">=":
            return 1 if ai >= bi else 0
        if op == "&":
            return ai & bi
        if op == "|":
            return ai | bi
        if op == "^":
            return ai ^ bi
        if op == "<<":
            return (ai << bi) & 0xFFFFFFFF if bi < 32 else 0
        if op == ">>":
            return ai >> bi
        raise RefError("bad op " + str(op))

    # -- native calls ------------------------------------------------------
    def native(self, name, args, s: State, receiver, receiver_key):
        if re.search(r"\.counter\[\]\.start", name):
            s.counters[receiver_key] = {
                "value": as_int(args[1]) if len(args) > 1 else 0,
                "ticking": len(args) < 2,
            }
            return 0
        if name == "global.random":
            return 0
        if name == "global.get_scene_name":
            return self.src.scenes[s.chapter]["source"]
        if name == "global.get_line_no":
            return 0
        if name == "$filename_translate":
            return str(args[0] if args else "").lower()
        if name == "global.namae":
            return {"％Ａ": "渚", "％Ｂ": "秋生", "％Ｃ": "朋也", "％Ｄ": "朋也",
                    "％Ｅ": "朋也", "％Ｆ": "朋也"}.get(args[0] if args else "", "朋也")
        if re.search(r"\.check$|\.check\(\)$|\.on_down$|\.check_skip$", name):
            return 1
        if re.search(r"\.counter\[\]\.get$", name):
            counter = s.counters.get(receiver_key)
            if not counter:
                return 0
            if counter["ticking"]:
                counter["value"] += 1000
            return counter["value"]
        m = re.search(r"\.(?:s|cur_call\.k)\[\]\.(?:search|len|cnt|mid|left|tonum)$", name)
        if m:
            text = js_str(receiver if receiver is not None else "")
            method = name[name.rfind(".") + 1:]
            if method == "search":
                return text.find(js_str(args[0] if args else ""))
            if method in ("len", "cnt"):
                return js_utf16_len(text)
            if method == "mid":
                start = as_int(args[0]) if args else 0
                length = as_int(args[1]) if len(args) > 1 else None
                return js_slice(text, start, None if length is None else start + length)
            if method == "left":
                return js_slice(text, 0, as_int(args[0]) if args else 0)
            return as_int(text)
        if name == "global.timewait_key":
            return 1
        if name == "$disp_dialog":
            return 1
        if re.search(r"\.get_auto_mode_onoff_flag$", name):
            return 0
        if re.search(r"\.get_(?:open|close)_anime_(?:type|time)$", name):
            return 0
        if re.search(r"\.get_(?:no_wipe_anime_onoff|koe_dont_stop_onoff)$", name):
            return 0
        if re.search(r"\.get_filter_color_[rgba]$", name):
            return 0
        if name == "$search_chara_center":
            return 0
        raise RefError("unsupported native: " + name)

    # -- state machine -----------------------------------------------------
    def assign(self, target, value, s: State):
        if not isinstance(target, list) or target[0] != "v":
            raise RefError("bad assign target")
        key = var_key(target[1], self.value(target[2], s) if len(target) > 2 else None)
        if isinstance(value, str):
            self.string_bytes += len(value.encode("utf-8"))
            self.string_keys.add(key)
            self.max_string_len = max(self.max_string_len, len(value.encode("utf-8")))
        s.variables[key] = value

    def jump(self, s: State, scene_id: int, entry, call: bool):
        scene = self.src.scenes.get(int(scene_id))
        if scene is None:
            raise RefError("missing scene " + str(scene_id))
        if call:
            if len(s.stack) >= 24:
                raise RefError("call stack overflow")
            s.stack.append([s.chapter, s.cursor + 1])
        s.chapter = int(scene_id)
        entries = scene["entries"]
        e = int(entry or 0)
        s.cursor = entries[e] if 0 <= e < len(entries) else 0
        s.ended = False
        if not call and not s.stack:
            s.root_chapter = int(scene_id)

    def returned(self, s: State):
        if s.stack:
            chapter, cursor = s.stack.pop()
            s.chapter = chapter
            s.cursor = cursor
            return {"type": "jump"}
        s.ended = True
        return {"type": "end"}

    def read_frame(self, s: State):
        scene = self.src.scenes.get(s.chapter)
        if scene is None:
            raise RefError("missing current scene " + str(s.chapter))
        total = scene["length"]
        base = (s.cursor // BLOCK) * BLOCK
        for _ in range(400):
            if s.cursor >= total:
                return self.returned(s)
            local = s.cursor - base
            if local < 0 or local >= BLOCK:
                return {"type": "yield"}
            ops = self.src.blocks.get((s.chapter, base // BLOCK))
            if ops is None or local >= len(ops):
                return {"type": "yield"}
            o = ops[local]
            self.steps += 1
            op = o["op"]
            if op == "text":
                s.speaker = o.get("speaker") or ""
                s.text = o.get("text") or ""
                return {"type": "text", "speaker": s.speaker, "text": s.text}
            if op == "choice":
                items = [dict(x, target=o["target"]) for x in o["items"]]
                return {"type": "choice", "items": items, "id": o.get("id")}
            if op == "dynamicChoice":
                items = [{"text": js_str(self.value(x, s)), "value": i,
                          "target": o["target"]} for i, x in enumerate(o["labels"])]
                return {"type": "choice", "items": items, "id": o.get("id")}
            if op == "set":
                self.assign(o["target"], self.value(o["value"], s), s)
            elif op == "native":
                self.value(o["call"], s)
            elif op == "goto":
                s.cursor = o["to"] - 1
            elif op == "ifFalse":
                if not truthy(self.value(o["condition"], s)):
                    s.cursor = o["to"] - 1
            elif op == "ifTrue":
                if truthy(self.value(o["condition"], s)):
                    s.cursor = o["to"] - 1
            elif op == "call":
                if len(s.stack) >= 24:
                    raise RefError("call stack overflow")
                s.stack.append([s.chapter, s.cursor + 1])
                s.cursor = o["to"] - 1
            elif op in ("jumpScene", "callScene"):
                self.jump(s, o["scene"], o.get("entry", 0), op == "callScene")
                return {"type": "jump"}
            elif op == "return":
                return self.returned(s)
            elif op == "end":
                s.ended = True
                return {"type": "end"}
            elif op == "background":
                s.background = o["image"]
                if o.get("clearBody"):
                    s.bodies = []
                    s.body = ""
            elif op == "bodies":
                s.bodies = list(o.get("images", []))[:3]
                s.body = s.bodies[0] if s.bodies else ""
            elif op == "bodyAdd":
                image = o["image"]
                family = image[:4]
                at = next((i for i, b in enumerate(s.bodies) if b[:4] == family), -1)
                if at >= 0:
                    s.bodies[at] = image
                else:
                    if len(s.bodies) >= 3:
                        s.bodies.pop(0)
                    s.bodies.append(image)
                s.body = s.bodies[0] if s.bodies else ""
            else:
                raise RefError("unsupported op " + op)
            s.cursor += 1
        return {"type": "yield"}

    def next(self, s: State):
        s.history.append(checkpoint(s))
        if len(s.history) > 8:
            s.history.pop(0)
        s.cursor += 1

    def choose(self, s: State, item, index: int):
        s.history.append(checkpoint(s))
        if len(s.history) > 8:
            s.history.pop(0)
        self.assign(item["target"], item["value"], s)
        s.choices.append(index)
        if len(s.choices) > 300:
            s.choices.pop(0)
        s.cursor += 1

    def next_chapter(self, s: State):
        if s.ended:
            return None
        s.history.append(checkpoint(s))
        if len(s.history) > 8:
            s.history.pop(0)
        return {"chapter": s.root_chapter}

    def read_chapter_skip(self, s: State, context):
        for _ in range(64):
            frame = self.read_frame(s)
            if frame["type"] != "text" or s.root_chapter != context["chapter"]:
                return frame
            s.cursor += 1
        return {"type": "yield"}


def math_fmod(a: int, b: int) -> float:
    # JS % keeps the sign of the dividend; Python's math.fmod matches.
    import math
    return math.fmod(a, b)


def checkpoint(s: State):
    st = s.clone()
    st.history = []
    return st


def serialize_save_estimate(s: State):
    """Estimate the compact save blob the C firmware stores (before deflate)."""
    out = bytearray()
    out += struct.pack("<BBHHHHBBHHHH", 1, 1 if s.progression == "story" else 0,
                       s.chapter, s.root_chapter, s.cursor, 0, 0, 0,
                       hash_asset(s.background), *[hash_asset(b) for b in (s.bodies + ["", "", ""])[:3]])
    # variables
    out += struct.pack("<H", len(s.variables))
    for key, value in s.variables.items():
        kind, _, idx = key.partition(":")
        out += bytes([VAR_KINDS.get(kind, 15)])
        out += struct.pack("<I", int(idx) if idx else NO_INDEX)
        if isinstance(value, str):
            raw = value.encode("utf-8")
            out += struct.pack("<BH", 1, len(raw)) + raw
        else:
            out += struct.pack("<Bi", 0, int(value))
    out += struct.pack("<H", len(s.counters))
    for key, counter in s.counters.items():
        out += struct.pack("<IiB", 0, counter["value"], 1 if counter["ticking"] else 0)
    out += struct.pack("<H", len(s.stack))
    for chapter, cursor in s.stack:
        out += struct.pack("<HH", chapter, cursor)
    raw = bytes(out)
    return len(raw), len(zlib.compress(raw, 9))


def hash_asset(name: str) -> int:
    return zlib.crc32(name.encode("utf-8")) & 0xFFFF


# ---------------------------------------------------------------- traversal
class Walker:
    def __init__(self, src: Source):
        self.src = src
        self.interp = Interpreter(src)

    def run(self, state: State, policy, max_ops=2_000_000, transcript=True,
            cycle_detect=True):
        h = hashlib.sha256()
        fnv = 1469598103934665603

        def mix(data: bytes):
            nonlocal fnv
            for byte in data:
                fnv ^= byte
                fnv = (fnv * 1099511628211) & 0xFFFFFFFFFFFFFFFF

        counts = Counter()
        loops = 0
        last = None
        seen = set()
        self.interp.steps = 0
        while self.interp.steps < max_ops:
            frame = self.interp.read_frame(state)
            kind = frame["type"]
            if kind == "choice":
                if cycle_detect:
                    signature = (state.chapter, state.cursor,
                                 tuple(sorted(state.variables.items())),
                                 tuple(map(tuple, state.stack)))
                    if signature in seen:
                        counts["cycle"] += 1
                        return {"stop": "loop", "counts": counts,
                                "hash": h.hexdigest(), "fnv": fnv, "state": state,
                                "last": last, "end_scene": 0, "end_op": 0}
                    seen.add(signature)
            if kind == "text":
                counts["text"] += 1
                if transcript:
                    speaker = (frame["speaker"] or "").encode("utf-8")
                    text = (frame["text"] or "").encode("utf-8")
                    h.update(b"T")
                    h.update(speaker)
                    h.update(b"\x00")
                    h.update(text)
                    h.update(b"\x00")
                    mix(b"T")
                    mix(speaker)
                    mix(b"\x00")
                    mix(text)
                    mix(b"\x00")
                last = ("text", state.chapter, state.cursor)
                self.interp.next(state)
            elif kind == "choice":
                counts["choice"] += 1
                if transcript:
                    h.update(b"C")
                    mix(b"C")
                    for item in frame["items"]:
                        label = js_str(item["text"]).encode("utf-8")
                        h.update(label)
                        h.update(b"\x01")
                        mix(label)
                        mix(b"\x01")
                index = policy(frame, state)
                counts["choice_value_" + str(index)] += 1
                self.interp.choose(state, frame["items"][index], index)
            elif kind == "end":
                counts["end"] += 1
                return {"stop": "end", "counts": counts, "hash": h.hexdigest(),
                        "fnv": fnv, "state": state, "last": last,
                        "end_scene": state.chapter, "end_op": state.cursor}
            elif kind in ("jump", "yield"):
                loops += 1
                if loops > 2_000_000:
                    return {"stop": "loop", "counts": counts, "hash": h.hexdigest(),
                            "fnv": fnv, "state": state, "last": last}
                if kind == "yield" and state.ended:
                    return {"stop": "end", "counts": counts, "hash": h.hexdigest(),
                            "fnv": fnv, "state": state, "last": last}
            else:
                raise RefError("bad frame " + kind)
        return {"stop": "max_ops", "counts": counts, "hash": h.hexdigest(),
                "fnv": fnv, "state": state, "last": last}


def first_policy(frame, state):
    return 0


def last_policy(frame, state):
    return len(frame["items"]) - 1


# ---------------------------------------------------------------- audits
def load_source(root: str) -> Source:
    return Source(root)


def audit_choices(src: Source):
    """Enumerate every choice op in the source and its target/items."""
    places = []
    dynamic = 0
    for (scene, block), ops in sorted(src.blocks.items()):
        for i, op in enumerate(ops):
            if op["op"] == "choice":
                places.append({
                    "scene": scene, "block": block, "op": i + block * BLOCK,
                    "id": op.get("id"), "target": op["target"],
                    "items": [{"text": it.get("text"), "value": it.get("value"),
                               "serial": it.get("serial")} for it in op["items"]],
                })
            elif op["op"] == "dynamicChoice":
                dynamic += 1
                places.append({
                    "scene": scene, "block": block, "op": i + block * BLOCK,
                    "id": op.get("id"), "target": op["target"],
                    "labels": op["labels"], "dynamic": True,
                })
    targets = Counter()
    for p in places:
        targets[json.dumps(p["target"], ensure_ascii=False)] += 1
    return {"count": len(places), "dynamic": dynamic,
            "targets": dict(targets), "places": places}


def audit_entries(src: Source):
    """Verify that every jump entry used by the script exists in the source
    scene's entry table, and report the highest used entry index."""
    used = defaultdict(Counter)
    for ops in src.blocks.values():
        for op in ops:
            if op["op"] in ("jumpScene", "callScene"):
                used[int(op["scene"])][int(op.get("entry") or 0)] += 1
    missing = []
    for scene, entries in used.items():
        if scene not in src.scenes:
            missing.append((scene, "scene"))
            continue
        table = src.scenes[scene]["entries"]
        for entry in entries:
            if entry >= len(table):
                missing.append((scene, entry))
    return {"used_scene_count": len(used), "missing": missing,
            "max_entry": max((e for c in used.values() for e in c), default=0),
            "entries": {str(k): dict(v) for k, v in used.items()}}


def audit_gallery(src: Source):
    groups = OrderedDict()
    for item in src.gallery:
        key = item.get("group") or item["source"]
        groups.setdefault(key, []).append(item)
    labels = Counter(item["label"] for item in src.gallery)
    return {"entries": len(src.gallery), "groups": len(groups),
            "variants": {k: len(v) for k, v in groups.items()},
            "labels": dict(labels)}


def audit_images(src: Source):
    """Resolve every image name used by the story/gallery to a source file."""
    bg, body = set(), set()
    unresolved = []
    for ops in src.blocks.values():
        for op in ops:
            if op["op"] == "background":
                name = op["image"].lower()
                if name not in src.assets:
                    unresolved.append(op["image"])
                else:
                    bg.add(src.assets[name])
            elif op["op"] in ("bodies", "bodyAdd"):
                names = op.get("images") or ([op["image"]] if op.get("image") else [])
                for name in names:
                    low = name.lower()
                    if low not in src.assets:
                        unresolved.append(name)
                    else:
                        body.add(src.assets[low])
    gallery = set()
    for item in src.gallery:
        low = item["image"].lower()
        if low not in src.assets:
            unresolved.append(item["image"])
        else:
            gallery.add(src.assets[low])
    all_files = set(os.listdir(os.path.join(src.root, "src/common/images")))
    used = bg | body | gallery
    return {"bg_files": len(bg), "body_files": len(body), "gallery_files": len(gallery),
            "unique_used": len(used), "total_files": len(all_files),
            "unreferenced": sorted(all_files - used), "unresolved": sorted(set(unresolved))}


def audit_glyphs(src: Source):
    chars = Counter()
    def add(text):
        for ch in text or "":
            chars[ch] += 1
    for ops in src.blocks.values():
        for op in ops:
            if op["op"] == "text":
                add(op.get("speaker")); add(op.get("text"))
            elif op["op"] == "choice":
                for item in op["items"]:
                    add(item.get("text"))
            elif op["op"] == "set" and isinstance(op.get("value"), str):
                add(op["value"])
    for route in src.catalog["routes"]:
        add(route["name"])
    for chapter in src.catalog["chapters"]:
        add(chapter["name"])
    for item in src.gallery:
        add(item["label"])
    pua = sorted(ch for ch in chars if 0xE000 <= ord(ch) <= 0xF8FF)
    astral = sorted(ch for ch in chars if ord(ch) > 0xFFFF)
    return {"count": len(chars), "pua": len(pua), "astral": len(astral),
            "occurences": sum(chars.values()), "chars": "".join(sorted(chars))}


def sweep(src: Source, walker: Walker, max_ops: int):
    chapters = [c["id"] for c in src.catalog["chapters"]]
    routes = OrderedDict()
    for chapter in src.catalog["chapters"]:
        routes.setdefault(chapter["route"], []).append(chapter["id"])
    results = []
    uniq = Counter()
    for chapter in chapters:
        for name, policy in (("first", first_policy), ("last", last_policy)):
            state = State(chapter, story=False)
            outcome = walker.run(state, policy, max_ops=max_ops)
            uniq.update(state.variables.keys())
            results.append({
                "chapter": chapter, "policy": name, "stop": outcome["stop"],
                "text": outcome["counts"]["text"], "choice": outcome["counts"]["choice"],
                "end": outcome["counts"]["end"], "hash": outcome["hash"],
                "final_scene": state.chapter, "vars": len(state.variables),
                "save": serialize_save_estimate(state),
                "end_scene": outcome.get("end_scene", 0),
                "end_op": outcome.get("end_op", 0),
                "fnv": outcome.get("fnv", 0),
            })
    flow = []
    for name, policy in (("first", first_policy), ("last", last_policy)):
        state = State(src.catalog["flowScene"], story=True)
        outcome = walker.run(state, policy, max_ops=max_ops)
        flow.append({"policy": name, "stop": outcome["stop"],
                     "text": outcome["counts"]["text"],
                     "choice": outcome["counts"]["choice"],
                     "end": outcome["counts"]["end"], "hash": outcome["hash"],
                     "final_scene": state.chapter, "vars": len(state.variables),
                     "save": serialize_save_estimate(state),
                     "end_scene": outcome.get("end_scene", 0),
                     "end_op": outcome.get("end_op", 0),
                     "fnv": outcome.get("fnv", 0)})
    ends = Counter()
    for ops in src.blocks.values():
        for op in ops:
            if op["op"] == "end":
                ends[0] += 1
    return {"chapter_runs": results, "flow": flow,
            "max_vars": max(r["vars"] for r in results),
            "max_save": max(r["save"] for r in results),
            "end_ops": ends[0]}


def write_fixtures(src: Source, walker: Walker, path: str):
    """Emit a tracked C header with every chapter-entry policy run + flow runs."""
    rows = []
    for chapter in [c["id"] for c in src.catalog["chapters"]]:
        for policy, name in ((first_policy, "first"), (last_policy, "last")):
            state = State(chapter, story=False)
            outcome = walker.run(state, policy, max_ops=400000)
            rows.append((chapter, 0 if name == "first" else 1, outcome))
    flow = []
    for policy in (first_policy, last_policy):
        state = State(src.catalog["flowScene"], story=True)
        outcome = walker.run(state, policy, max_ops=400000)
        flow.append((src.catalog["flowScene"], 0 if policy is first_policy else 1,
                     outcome))
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("// Generated by tools/clannad_audit.py --fixtures; do not edit.\n")
        fh.write("// Each row: chapter entry, policy (0=first,1=last), text/choice\n")
        fh.write("// counts, end scene/op, FNV-1a of the transcript.\n")
        fh.write("#pragma once\n#include <stdint.h>\n\n")
        fh.write("typedef struct {\n    uint16_t chapter;\n    uint8_t policy;\n"
                 "    uint16_t end_scene;\n    uint16_t end_op;\n"
                 "    uint32_t texts;\n    uint32_t choices;\n"
                 "    uint64_t fnv;\n} clannad_route_fixture_t;\n\n")
        fh.write("static const clannad_route_fixture_t CLANNAD_ROUTES[] = {\n")
        for chapter, policy, out in rows:
            fh.write("    {%u, %u, %u, %u, %u, %u, 0x%016xULL},\n" % (
                chapter, policy, out.get("end_scene", 0), out.get("end_op", 0),
                out["counts"]["text"], out["counts"]["choice"], out.get("fnv", 0)))
        fh.write("};\n\n")
        fh.write("static const clannad_route_fixture_t CLANNAD_FLOW[] = {\n")
        for chapter, policy, out in flow:
            fh.write("    {%u, %u, %u, %u, %u, %u, 0x%016xULL},\n" % (
                chapter, policy, out.get("end_scene", 0), out.get("end_op", 0),
                out["counts"]["text"], out["counts"]["choice"], out.get("fnv", 0)))
        fh.write("};\n\n")
        fh.write("#define CLANNAD_ROUTE_COUNT %u\n" % len(rows))
        fh.write("#define CLANNAD_FLOW_COUNT %u\n" % len(flow))
    print("wrote", path, len(rows), "route fixtures")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="clannad-rw5 source checkout")
    ap.add_argument("--pack", default="", help="built pack for cross-check")
    ap.add_argument("--sweep", action="store_true", help="run all chapter/flow traversals")
    ap.add_argument("--explore", action="store_true", help="enumerate choice branches to endings")
    ap.add_argument("--explore-limit", type=int, default=200_000)
    ap.add_argument("--max-ops", type=int, default=2_000_000)
    ap.add_argument("--json", default="")
    ap.add_argument("--fixtures", default="", help="write a C fixture header for the VM test")
    args = ap.parse_args()

    src = load_source(args.source)
    report = {
        "source": os.path.abspath(args.source),
        "catalog": {k: src.catalog[k] for k in ("count", "initial", "flowScene",
                                                "blockSize", "dialogue", "choices")},
        "chunks": len(src.chunk_files),
        "ops": dict(src.op_counts),
        "scenes": len(src.scenes),
        "routes": len(src.catalog["routes"]),
        "chapters": len(src.catalog["chapters"]),
        "choice_audit": audit_choices(src),
        "entry_audit": audit_entries(src),
        "gallery_audit": audit_gallery(src),
        "image_audit": audit_images(src),
        "glyph_audit": audit_glyphs(src),
    }
    walker = Walker(src)
    if args.sweep:
        report["sweep"] = sweep(src, walker, args.max_ops)
    if args.explore:
        report["explore"] = explore_all(src, args.explore_limit)
    if args.fixtures:
        write_fixtures(src, walker, args.fixtures)
    if args.pack:
        report["pack"] = cross_check_pack(src, args.pack)
    out = json.dumps(report, ensure_ascii=False, indent=1, default=str)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            fh.write(out)
        print(f"wrote {args.json}")
    else:
        print(out)
    if args.pack and report.get("pack", {}).get("result") != "passed":
        print("pack audit FAILED:", json.dumps(report["pack"].get("errors", []),
                                               ensure_ascii=False), file=sys.stderr)
        return 1
    return 0


AFTER_FLAGS = [1, 2, 3, 4, 5, 6, 9, 12, 13, 15, 30, 31]


def state_signature(state: State):
    return (state.chapter, state.cursor, tuple(map(tuple, state.stack)),
            tuple(sorted(state.variables.items())),
            tuple(sorted((k, v["value"], v["ticking"]) for k, v in state.counters.items())))


def explore_from(src: Source, start: int, story: bool, limit: int = 200_000,
                 max_depth: int = 400):
    """Enumerate choice branches from one entry; return reachable end sites and
    one choice path per site. States are memoised by their full VM signature."""
    interp = Interpreter(src)
    visited = set()
    ends = {}
    stack = [(State(start, story), [])]
    states = 0
    while stack and states < limit:
        state, path = stack.pop()
        if len(path) > max_depth:
            continue
        while True:
            states += 1
            if states >= limit:
                break
            frame = interp.read_frame(state)
            kind = frame["type"]
            if kind == "text":
                interp.next(state)
                continue
            if kind == "choice":
                sig = state_signature(state)
                if sig in visited:
                    break
                visited.add(sig)
                for index in range(len(frame["items"])):
                    child = state.clone()
                    interp.choose(child, frame["items"][index], index)
                    stack.append((child, path + [(state.chapter, state.cursor, index)]))
                break
            if kind in ("jump", "yield"):
                if kind == "yield" and state.ended:
                    ends.setdefault((state.chapter, state.cursor), path)
                    break
                continue
            if kind == "end":
                ends.setdefault((state.chapter, state.cursor), path)
                break
            raise RefError("bad frame " + kind)
    return ends, states, len(visited)


def progress_state(chapter: int, story: bool):
    state = State(chapter, story)
    for bit in AFTER_FLAGS:
        state.variables[f"g:{bit}"] = 1
    state.variables["g:0"] = 13
    state.variables["g:45"] = 1
    return state


def explore_state(src: Source, state, limit: int):
    interp = Interpreter(src)
    visited = set()
    ends = {}
    stack = [(state, [])]
    states = 0
    while stack and states < limit:
        state, path = stack.pop()
        while True:
            states += 1
            if states >= limit:
                break
            frame = interp.read_frame(state)
            kind = frame["type"]
            if kind == "text":
                interp.next(state)
                continue
            if kind == "choice":
                sig = state_signature(state)
                if sig in visited:
                    break
                visited.add(sig)
                for index in range(len(frame["items"])):
                    child = state.clone()
                    interp.choose(child, frame["items"][index], index)
                    stack.append((child, path + [(state.chapter, state.cursor, index)]))
                break
            if kind in ("jump", "yield"):
                if kind == "yield" and state.ended:
                    ends.setdefault((state.chapter, state.cursor), path)
                    break
                continue
            if kind == "end":
                ends.setdefault((state.chapter, state.cursor), path)
                break
            raise RefError("bad frame " + kind)
    return ends, states, len(visited)


def explore_all(src: Source, limit: int):
    flow_ends, flow_states, flow_visited = explore_state(
        src, State(src.catalog["flowScene"], True), limit)
    prog_ends, prog_states, prog_visited = explore_state(
        src, progress_state(src.catalog["flowScene"], True), limit)
    after_ends, after_states, after_visited = explore_state(
        src, progress_state(6800, True), limit)
    chapter_ends = {}
    chapter_sites = {}
    states = 0
    for chapter in [c["id"] for c in src.catalog["chapters"]]:
        ends, st, vis = explore_from(src, chapter, False, limit)
        states += st
        for site, path in ends.items():
            chapter_sites.setdefault(site, []).append(chapter)
            chapter_ends.setdefault(site, {"start": chapter, "path": path})
    all_sites = set(chapter_ends) | set(flow_ends) | set(prog_ends) | set(after_ends)
    return {
        "flow": {"ends": {f"{k[0]}:{k[1]}": v for k, v in flow_ends.items()},
                 "states": flow_states, "visited": flow_visited},
        "flow_progress": {"ends": {f"{k[0]}:{k[1]}": v for k, v in prog_ends.items()},
                          "states": prog_states, "visited": prog_visited},
        "after_progress": {"ends": {f"{k[0]}:{k[1]}": v for k, v in after_ends.items()},
                           "states": after_states, "visited": after_visited},
        "chapter": {"ends": {f"{k[0]}:{k[1]}": v for k, v in chapter_ends.items()},
                    "states": states, "sites": len(chapter_sites)},
        "all_sites": sorted(f"{a}:{b}" for a, b in all_sites),
        "sites_only_flow": sorted(f"{a}:{b}" for a, b in set(flow_ends) - set(chapter_ends)),
        "sites_only_progress_flow": sorted(
            f"{a}:{b}" for a, b in (set(prog_ends) | set(after_ends)) - set(flow_ends)),
    }


def cross_check_pack(src: Source, pack_path: str):
    """Re-parse the built pack byte-by-byte and compare it with the source.

    This function intentionally has its own struct handling; it does not import
    tools/clannad_pack.py. Expressions are checked structurally (variable refs,
    non-empty operand streams); the production C VM is checked semantically by
    tests/test_clannad_vm.c against this tool's independent interpreter.
    """
    import struct
    import zlib

    blob = open(pack_path, "rb").read()
    errors = []
    counts = Counter()

    def fail(msg):
        if len(errors) < 40:
            errors.append(msg)

    if len(blob) < 24 or blob[:8] != b"CLANPK01":
        return {"result": "failed", "errors": ["bad magic"]}
    version, total, nsec = struct.unpack_from("<IIH", blob, 8)
    if version != 1 or total != len(blob):
        return {"result": "failed", "errors": ["bad header"]}
    secs = {}
    for i in range(nsec):
        typ, off, cnt, size = struct.unpack_from("<IIII", blob, 24 + i * 16)
        if off > len(blob) or size > len(blob) - off:
            return {"result": "failed", "errors": ["section out of range"]}
        secs[typ] = (off, cnt, size)

    def text(off):
        if off >= secs[0][2]:
            raise ValueError("text offset")
        endp = blob.find(b"\x00", secs[0][0] + off, secs[0][0] + secs[0][2])
        if endp < 0:
            raise ValueError("text not terminated")
        return blob[secs[0][0] + off:endp].decode("utf-8")

    cat_off = secs[1][0]
    scene_count, block_count, route_count, chapter_count = struct.unpack_from(
        "<HHHH", blob, cat_off)
    counts.update(scenes=scene_count, chunks=block_count, routes=route_count,
                  chapters=chapter_count)
    if scene_count != len(src.scenes):
        fail("scene count")
    if block_count != len(src.chunk_files):
        fail("chunk count")
    if route_count != len(src.catalog["routes"]):
        fail("route count")
    if chapter_count != len(src.catalog["chapters"]):
        fail("chapter count")

    scene_recs = []
    for i in range(scene_count):
        rec = struct.unpack_from("<HHHHHBBHH", blob, cat_off + 8 + i * 16)
        scene_recs.append({
            "id": rec[0], "name": text(rec[1]), "source": text(rec[2]),
            "length": rec[3], "entries_index": rec[4], "entry_count": rec[5],
            "first_block": rec[7], "block_count": rec[8]})
    block_recs = []
    boff = cat_off + 8 + scene_count * 16
    for i in range(block_count):
        off, comp, raw, ops, _ = struct.unpack_from("<IIIHH", blob, boff + i * 16)
        block_recs.append((off, comp, raw, ops))
    route_off = boff + block_count * 16
    routes = []
    for i in range(route_count):
        name_off, first, count, _ = struct.unpack_from("<HHHH", blob, route_off + i * 8)
        routes.append((text(name_off), first, count))
    chapter_off = route_off + route_count * 8
    chapters = []
    for i in range(chapter_count):
        sid, name_off, rid, _flags, _ = struct.unpack_from(
            "<HHBBH", blob, chapter_off + i * 8)
        chapters.append((sid, text(name_off), rid))
    for i, chapter in enumerate(src.catalog["chapters"]):
        if chapters[i][0] != chapter["id"] or chapters[i][1] != chapter["name"]:
            fail("chapter %d" % i)
            break
    for i, route in enumerate(src.catalog["routes"]):
        if routes[i][0] != route["name"]:
            fail("route %d" % i)
            break

    entry_off, _, entry_size = secs[2]
    entries = list(struct.unpack_from("<%dH" % (entry_size // 2), blob, entry_off))
    scene_order = sorted(src.scenes)
    for i, sid in enumerate(scene_order):
        rec = scene_recs[i]
        table = src.scenes[sid]["entries"]
        for k in range(33):
            want = table[k] if k < len(table) else 0
            if entries[rec["entries_index"] + k] != want:
                fail("entry %s[%d]" % (sid, k))
                break
        if rec["id"] != sid or rec["length"] != src.scenes[sid]["length"]:
            fail("scene record %s" % sid)
        if rec["name"] != (src.scenes[sid].get("name") or ""):
            fail("scene name %s" % sid)
        if rec["source"] != (src.scenes[sid].get("source") or ""):
            fail("scene source %s" % sid)

    spk_off = secs[10][0]
    speakers = [text(struct.unpack_from("<H", blob, spk_off + i * 2)[0])
                for i in range(secs[10][1])]
    asset_off, asset_count, _ = secs[4]
    assets = []
    for i in range(asset_count):
        rec = struct.unpack_from("<IIHHIIHHhhBBHHH", blob, asset_off + i * 36)
        assets.append({"w": rec[2], "h": rec[3], "mask_len": rec[5],
                       "kind": rec[10]})
    aname_off = secs[3][0]
    anames = [text(struct.unpack_from("<H", blob, aname_off + i * 2)[0])
              for i in range(secs[3][1])]
    counts["assets"] = asset_count
    gallery_off, gallery_count_field, gallery_size = secs[5]
    gallery_groups = gallery_count_field // 8
    gallery_entries = (gallery_size - gallery_groups * 8) // 2
    counts["gallery_groups"] = gallery_groups
    counts["gallery_entries"] = gallery_entries
    if gallery_entries != len(src.gallery):
        fail("gallery entry count")
    for g in range(gallery_groups):
        _label, first, count, _cover = struct.unpack_from(
            "<HHHH", blob, gallery_off + g * 8)
        for k in range(count):
            if first + k >= gallery_entries:
                fail("gallery range")
                break
            asset = struct.unpack_from("<H", blob, gallery_off + gallery_groups * 8 +
                                       (first + k) * 2)[0]
            if asset >= asset_count:
                fail("gallery asset")
                break

    alias_off, alias_count, alias_size = secs[9]
    alias = {}
    for i in range(alias_size // 6):
        name_off, asset, kind, _pad = struct.unpack_from("<HHBB", blob,
                                                         alias_off + i * 6)
        alias[(kind, text(name_off).lower())] = asset
    expected_bg = set()
    expected_sprite = set()
    for ops in src.blocks.values():
        for op in ops:
            if op["op"] == "background":
                expected_bg.add(op["image"].lower())
            elif op["op"] in ("bodies", "bodyAdd"):
                for name in (op.get("images") or
                             ([op["image"]] if op.get("image") else [])):
                    if name:
                        expected_sprite.add(name.lower())
    expected_gallery = {item["image"].lower() for item in src.gallery}
    for name in expected_bg:
        if (0, name) not in alias:
            fail("alias bg " + name)
            continue
        asset = assets[alias[(0, name)]]
        if asset["kind"] not in (0, 2):
            fail("bg kind " + name)
    for name in expected_sprite:
        if (1, name) not in alias:
            fail("alias sprite " + name)
            continue
        if assets[alias[(1, name)]]["kind"] != 1:
            fail("sprite kind " + name)
    for name in expected_gallery:
        if (0, name) not in alias:
            fail("alias gallery " + name)
            continue
        if assets[alias[(0, name)]]["kind"] != 2:
            fail("gallery kind " + name)

    op_codes = {"text": 0, "choice": 1, "dynamicChoice": 2, "set": 3, "native": 4,
                "goto": 5, "ifFalse": 6, "ifTrue": 7, "call": 8, "jumpScene": 9,
                "callScene": 10, "return": 11, "end": 12, "background": 13,
                "bodies": 14, "bodyAdd": 15}
    ops_off = secs[7][0]
    total_ops = 0
    for i, sid in enumerate(scene_order):
        rec = scene_recs[i]
        span = (src.scenes[sid]["length"] + 511) // 512
        for b in range(span):
            src_ops = src.blocks[(sid, b)]
            off, comp, raw_size, op_count = block_recs[rec["first_block"] + b]
            try:
                raw = zlib.decompress(blob[ops_off + off:ops_off + off + comp], -15)
            except zlib.error:
                fail("block inflate %s/%s" % (sid, b))
                continue
            if len(raw) != raw_size:
                fail("block raw %s/%s" % (sid, b))
                continue
            nops, tsize, esize, _ = struct.unpack_from("<HHHH", raw, 0)
            if nops != len(src_ops):
                fail("op count %s/%s" % (sid, b))
                continue
            extra = raw[8 + nops * 14: 8 + nops * 14 + esize]
            tbase = 8 + nops * 14 + esize

            def btext(o, ln):
                return raw[tbase + o:tbase + o + ln].decode("utf-8")

            for k, sop in enumerate(src_ops):
                total_ops += 1
                typ, n, x, y, z, toff, tlen, ex = struct.unpack_from(
                    "<BBHHHHHH", raw, 8 + k * 14)
                if typ != op_codes[sop["op"]]:
                    fail("op type %s/%s/%d" % (sid, b, k))
                    break
                if sop["op"] == "text":
                    if speakers[x] != (sop.get("speaker") or ""):
                        fail("speaker %s/%s/%d" % (sid, b, k))
                        break
                    if btext(toff, tlen) != (sop.get("text") or ""):
                        fail("text %s/%s/%d" % (sid, b, k))
                        break
                elif sop["op"] == "choice":
                    if n != len(sop["items"]) or extra[x] != n:
                        fail("choice count %s/%s/%d" % (sid, b, k))
                        break
                    for j, item in enumerate(sop["items"]):
                        v, serial, voff, vlen = struct.unpack_from(
                            "<iiHH", extra, x + 1 + j * 12)
                        if v != item["value"] or serial != item.get("serial", 0) or \
                                btext(voff, vlen) != (item.get("text") or ""):
                            fail("choice item %s/%s/%d" % (sid, b, k))
                            break
                elif sop["op"] in ("goto", "call"):
                    if x != sop["to"]:
                        fail("target %s/%s/%d" % (sid, b, k))
                        break
                elif sop["op"] in ("ifFalse", "ifTrue"):
                    if x != sop["to"] or y >= esize:
                        fail("branch %s/%s/%d" % (sid, b, k))
                        break
                elif sop["op"] in ("jumpScene", "callScene"):
                    if x != sop["scene"] or n != sop.get("entry", 0):
                        fail("jump %s/%s/%d" % (sid, b, k))
                        break
                elif sop["op"] == "background":
                    if alias.get((0, sop["image"].lower())) != x:
                        fail("bg %s/%s/%d" % (sid, b, k))
                        break
                    if n != (1 if sop.get("clearBody") else 0):
                        fail("clearBody %s/%s/%d" % (sid, b, k))
                        break
                elif sop["op"] == "bodies":
                    images = sop.get("images") or []
                    if n != min(3, len(images)):
                        fail("bodies %s/%s/%d" % (sid, b, k))
                        break
                    if any(alias.get((1, name.lower())) != (x, y, z)[j]
                           for j, name in enumerate(images[:3])):
                        fail("body asset %s/%s/%d" % (sid, b, k))
                        break
                elif sop["op"] == "bodyAdd":
                    if alias.get((1, sop["image"].lower())) != x:
                        fail("bodyAdd %s/%s/%d" % (sid, b, k))
                        break
                elif sop["op"] == "set":
                    if x >= esize:
                        fail("set target %s/%s/%d" % (sid, b, k))
                        break
                    code = extra[x]
                    target = sop["target"]
                    if code == 0x10:
                        kind = extra[x + 1]
                        index = struct.unpack_from("<H", extra, x + 2)[0]
                    elif code == 0x11:
                        kind = extra[x + 1]
                        index = None
                    elif code == 0x12:
                        kind = extra[x + 1]
                        index = None
                    else:
                        fail("set expr %s/%s/%d" % (sid, b, k))
                        break
                    if kind != VAR_KINDS[target[1]]:
                        fail("set kind %s/%s/%d" % (sid, b, k))
                        break
                    if len(target) > 2 and index is not None and index != target[2]:
                        fail("set index %s/%s/%d" % (sid, b, k))
                        break
                    if y >= esize:
                        fail("set value %s/%s/%d" % (sid, b, k))
                        break
                elif sop["op"] in ("native", "dynamicChoice"):
                    if x >= esize:
                        fail("expr %s/%s/%d" % (sid, b, k))
                        break
            counts["ops"] += len(src_ops)

    return {
        "result": "passed" if not errors else "failed",
        "errors": errors,
        "counts": dict(counts),
        "total_ops": total_ops,
        "pack_bytes": len(blob),
    }


if __name__ == "__main__":
    raise SystemExit(main())
