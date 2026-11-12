#!/usr/bin/env python3
"""《anemoi》红米手表版源快照 -> AI Passport 320x240 资源包 (ANEMPK1)。

源快照只读：本脚本读取 xuanyide01/anemoi-rw5 的快照目录（catalog.js、assets.js、
portraits/*.txt、story/*.txt、helpers.js、story-labels.txt、resources/*.json、
gallery.txt），写出一个二进制包与一份 JSON 清单。

包结构与固件端 main/anemoi/anemoi_pack.h 逐字节对应；所有多字节字段小端。
段表见 SECTION_NAMES / anemoi_pack.h 的说明。立绘按源版面算出的屏幕像素尺寸
预先缩放（层内只缩，不把表情放大成全身），运行时 1:1 合成，不再缩放。

用法：
  python tools/anemoi_pack.py --source <anemoi-rw5> --scope common
  python tools/anemoi_pack.py --source <anemoi-rw5> --scope all --measure
"""

from __future__ import annotations

import argparse
import collections
import glob
import hashlib
import io
import json
import os
import re
import struct
import subprocess
import sys
import time
from typing import Optional
import zlib

# ---------------------------------------------------------------- 常量

MAGIC = b"ANEMPK1\x00"
# 文本 id 宽度：正式卷用 u16（每卷字符串数远小于 65,535）；全量单包超过这个上限，
# 所以另有“宽 id”测量变体（--wide-ids），只用于量全量字节，不用于运行期固件。
IDC = "H"          # struct 格式字符（H = u16，I = u32）
ID_BYTES = 2       # 宽度（字节）
VERSION = 1
HEADER_SIZE = 28

SEC_TEXT = 0
SEC_TXIDX = 1
SEC_OPS = 2
SEC_SCENE = 3
SEC_SCNID = 4
SEC_ENTRY = 5
SEC_CHAP = 6
SEC_VAR = 7
SEC_EXPR = 8
SEC_COND = 9
SEC_CALL = 10
SEC_SET = 11
SEC_CHOICE = 12
SEC_NAMELIST = 13
SEC_POSE = 14
SEC_ACTOR = 15
SEC_IMAGE = 16
SEC_BLOB = 17
SEC_REQ = 18
SEC_HELPER = 19
SEC_MICRO = 20
SEC_GAL = 21
SEC_NAMEMAP = 22
SEC_META = 23
SEC_COUNT = 24

SECTION_NAMES = {
    SEC_TEXT: "text", SEC_TXIDX: "txidx", SEC_OPS: "ops", SEC_SCENE: "scene",
    SEC_SCNID: "scnid", SEC_ENTRY: "entry", SEC_CHAP: "chap", SEC_VAR: "var",
    SEC_EXPR: "expr", SEC_COND: "cond", SEC_CALL: "call", SEC_SET: "set",
    SEC_CHOICE: "choice", SEC_NAMELIST: "namelist", SEC_POSE: "pose",
    SEC_ACTOR: "actor", SEC_IMAGE: "image", SEC_BLOB: "blob", SEC_REQ: "req",
    SEC_HELPER: "helper", SEC_MICRO: "micro", SEC_GAL: "gal", SEC_NAMEMAP: "namemap",
    SEC_META: "meta",
}

# 指令种类（与 anemoi_pack.h 一致）
OP_TEXT, OP_BG, OP_PORTRAIT, OP_COPY_BODIES, OP_COPY_CG, OP_COMMIT, OP_INIT_TIME = range(7)
OP_IF_FALSE, OP_IF_TRUE, OP_GOTO, OP_CALL, OP_RETURN, OP_JUMP, OP_CALL_SCENE = range(7, 14)
OP_CHOICE, OP_BOARD, OP_END, OP_OMITTED, OP_SKIP_GAME, OP_ROUTE_CHECK = range(14, 20)
OP_SET, OP_NATIVE, OP_ARRIVE_CHOICE, OP_BOUNDARY = range(20, 24)
OP_SET_DYN = 24  # 动态键赋值：a=base 文本 id，b=索引表达式，c=值表达式

# 表达式字节码
E_NUM, E_STR, E_VAR, E_ARRAY, E_UN, E_BIN, E_CALL, E_METHOD = range(1, 9)
E_VARDYN = 9  # 动态变量键：base 文本 id + 索引表达式

UNARY_OPS = {"-": 1, "+": 2, "!": 3, "~": 4}
BINARY_OPS = {"+": 1, "-": 2, "*": 3, "/": 4, "%": 5, "==": 6, "!=": 7, "<": 8,
              "<=": 9, ">": 10, ">=": 11, "&": 12, "|": 13, "^": 14, "<<": 15,
              ">>": 16, "&&": 17, "||": 18}

METHODS = {"left": 0, "right": 1, "mid": 2, "cnt": 3, "len": 3, "tonum": 4,
           "search": 5, "get_size": 6, "get": 7, "get_file_name": 8, "check": 9}

# 只被 C/native 按名读写的变量：故事 op 从不引用它们，必须显式注册进 VAR，
# 否则运行期 anem_var_id_by_name() 失败（$$set_date 写 a:1、时段后缀读
# $time_control、记录位写 z:201.. 都受影响）。
NATIVE_VAR_NAMES = [
    "$time_control", "a:0", "a:1", "a:2", "a:3", "a:4", "a:5", "a:10", "a:11",
    "a:12", "a:13", "a:14", "a:134", "$select_btn", "$jump_label_no", "fa",
]

# native 支持表：C 端 anemoi_script.c 的 native 分派必须覆盖这里的全部名字
# （host 测试 test_anemoi_script.c 会用包内 meta 的 natives= 清单核对）。
SUPPORTED_NATIVES = {
    "$$set_time_control", "$$init_time_control", "$$get_time_control_suffix",
    "$$get_bg_time_control_name", "$$chop_bg_blur_name", "$$set_date",
    "$$exists_g00", "$$exists_image", "$$bold",
    "$$set_bs_dress_type", "$$set_bs_overlay_face_type", "$$get_bs_obj_index",
    "$$init_bbs_data", "$$set_bbs_request_data", "$$check_character_bbs_disp",
    "$$get_record_flag", "$$record_flag_on", "$$record_flag_off",
    "$$get_record_total", "$$get_hhp_total_play_count", "$$get_hhp_play_level",
    "$$get_hhp_play_result", "$$get_hhp_play_count", "$$has_hhp_skill",
    "$$get_urace_grand_entry_flag", "$$init_urace_user_data", "$$init_urace_am_data",
    "$$init_urace_pm_data", "$$init_urace_day_data", "$$set_urace_medal_flag",
    "$$check_route", "$$route_clear",
    "global.get_scene_name", "global.get_line_no", "global.farcall",
    "global.math.rand", "global.math.abs", "global.math.tostr",
    "global.math.tostr_zero", "global.math.linear", "global.random",
    "global.system.check_debug_flag", "$$check_debug_mode_enable",
    "global.bgm.check", "global.syscom.check_joypad_mode",
    "$$get_wipe_type_ready", "$$get_wipe_wait_ready", "$$get_disp_cg_msg_close",
    "$$get_disp_cg_wipe_no", "global.get_mwnd", "$$get_toast_size",
    "$$input_update", "$$get_time_control", "$$get_hhp_play_count",
}

TIME_SUFFIX = {0: "_01", 1: "_02", 2: "_03", 3: "_04", 4: "_80", 5: "_04", 6: "_05"}

LAYOUT_SCALE = 320.0 / 432.0  # 由 --layout-scale 覆盖
LAYOUT_BASE_H = 286
LAYOUT_SOURCE_H = 1260
ROWS_PER_BLOCK = 16
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def log(msg: str) -> None:
    print(msg, flush=True)


# ---------------------------------------------------------------- 源快照

class Source:
    def __init__(self, root: str, overlay_path: Optional[str] = None):
        self.root = root
        self.source_commit = self._git_commit(root)
        self.overlay_info = None
        cat_src = self._read("src", "common", "catalog.js")
        start = cat_src.index("{", cat_src.index("export const catalog="))
        self.catalog = json.loads(cat_src[start:cat_src.rindex("}") + 1])
        assets_src = self._read("src", "common", "assets.js")
        m = re.search(r"export const assets=(\{.*?\});", assets_src, re.S)
        if not m:
            raise SystemExit("assets.js 解析失败")
        self.assets = json.loads(m.group(1))
        self.portraits = {}
        for path in sorted(glob.glob(os.path.join(root, "src", "common", "portraits", "*.txt"))):
            self.portraits.update(json.load(open(path, encoding="utf-8")))
        helpers_src = self._read("src", "common", "helpers.js")
        m = re.search(r"export const helpers=(\{.*\});", helpers_src, re.S)
        if not m:
            raise SystemExit("helpers.js 解析失败")
        self.helpers = json.loads(m.group(1))
        if overlay_path:
            self._apply_overlay(overlay_path)
        self.labels = json.loads(self._read("src", "common", "story-labels.txt"))
        self.requests = json.loads(self._read("resources", "request-texts.json"))
        self.gallery = json.loads(self._read("src", "common", "gallery.txt"))
        specs = self._read("src", "common", "portrait-specs.js")
        m = re.search(r"export const characterDefaults=(\{.*?\});", specs, re.S)
        self.char_defaults = json.loads(m.group(1)) if m else {}
        self.story_dir = os.path.join(root, "src", "common", "story")
        self._file_index = collections.defaultdict(list)
        for name in os.listdir(self.story_dir):
            mm = re.match(r"^(\d+)-(\d+)\.txt$", name)
            if mm:
                self._file_index[int(mm.group(1))].append(name)
        for key in self._file_index:
            self._file_index[key].sort()
        self._ops_cache = {}

    @staticmethod
    def _git_commit(root: str) -> str:
        try:
            return subprocess.check_output(
                ["git", "-C", root, "rev-parse", "HEAD"],
                stderr=subprocess.DEVNULL, text=True).strip()
        except (OSError, subprocess.CalledProcessError):
            return ""

    def _apply_overlay(self, path: str) -> None:
        """Apply a traceable source compatibility overlay without editing the
        pinned source tree. The overlay is recorded in the pack manifest."""
        raw = open(path, "rb").read()
        doc = json.loads(raw.decode("utf-8"))
        expected = str(doc.get("source_commit") or "")
        if expected and self.source_commit and expected != self.source_commit:
            raise SystemExit("source overlay commit mismatch: %s != %s" %
                             (expected, self.source_commit))
        applied = []
        for patch in doc.get("patches") or []:
            helper = str(patch.get("helper") or "")
            index = int(patch.get("op", -1))
            field = str(patch.get("field") or "condition")
            if helper not in self.helpers:
                raise SystemExit("source overlay helper missing: %s" % helper)
            ops = self.helpers[helper].get("ops") or []
            if index < 0 or index >= len(ops):
                raise SystemExit("source overlay op out of range: %s[%d]" % (helper, index))
            before = patch.get("before")
            after = patch.get("after")
            if ops[index].get(field) != before:
                raise SystemExit("source overlay before mismatch: %s[%d].%s" %
                                 (helper, index, field))
            ops[index][field] = after
            applied.append({"helper": helper, "op": index, "field": field,
                            "reason": patch.get("reason") or ""})
        self.overlay_info = {
            "path": os.path.relpath(os.path.abspath(path), REPO_ROOT),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "source_commit": expected,
            "patches": applied,
        }

    def _read(self, *parts) -> str:
        return open(os.path.join(self.root, *parts), encoding="utf-8").read()

    def scene_ids(self):
        return sorted(int(k) for k in self.catalog["scenes"])

    def scene(self, sid: int):
        return self.catalog["scenes"].get(str(sid))

    def source_name_map(self):
        return {scene["source"].lower(): int(sid)
                for sid, scene in self.catalog["scenes"].items()}

    def ops(self, sid: int):
        if sid in self._ops_cache:
            return self._ops_cache[sid]
        ops = []
        for name in self._file_index.get(sid, []):
            data = json.load(open(os.path.join(self.story_dir, name), encoding="utf-8"))
            if isinstance(data, list):
                ops.extend(data)
            else:
                block = data["ops"]
                block.schemas = data["schemas"]  # type: ignore[attr-defined]
                block.pool = data.get("pool")    # type: ignore[attr-defined]
                ops.extend(block)
        self._ops_cache[sid] = ops
        return ops

    def inst(self, ops, index: int):
        row = ops[index]
        if not isinstance(row, list):
            return row
        schema = ops.schemas[row[0]]
        op = {"op": schema[0]}
        for i in range(1, len(schema)):
            value = row[i]
            if getattr(ops, "pool", None) and isinstance(value, dict) and "$ref" in value:
                value = ops.pool[value["$ref"]]
            op[schema[i]] = value
        return op


def book_of(catalog, sid: int) -> str:
    scene = catalog["scenes"].get(str(sid))
    if not scene:
        return "?"
    m = re.match(r"^(\d{3})_", scene["source"])
    return m.group(1) if m else scene["source"]


COMMON_BOOKS = {"001", "010", "110", "090", "arrive"}


def is_route_chapter(scene_source: str) -> bool:
    """路线“正篇”场景：020_スピカ0801 这类编号 >= 800，或 0000_ 序章。

    020_スピカ0001..0009 / 030_愛乃0001..0008 是共通篇的委托信与任务段落，
    属于本卷；0801 起才是女主角路线正篇（分卷边界）。
    """
    digits = re.findall(r"(\d{4})", scene_source or "")
    if not digits:
        return False
    number = int(digits[-1])
    if number >= 800:
        return True
    return "0000_" in (scene_source or "")


def compute_scope(src: Source, mode: str):
    """取景（common / common-full / all / book:NNN）。

    common       : 共通篇主线 + 委托任务（entry 10 起），委托**信**（entry 0 前缀）不收录。
    common-full  : 在 common 基础上把委托信（请求场景的 [0, entry10) 前缀）也收录。
    all          : 全部场景整段（全量编码测量，不出可刷包）。
    book:NNN     : 单个书目整段（分卷候选实测）。
    """
    if mode.startswith("book:"):
        prefix = mode.split(":", 1)[1]
        included = {}
        for sid in src.scene_ids():
            scene = src.scene(sid)
            m = re.match(r"^([0-9]{3})_", scene["source"])
            book = m.group(1) if m else scene["source"]
            if book != prefix:
                continue
            ops = src.ops(sid)
            if ops:
                included[sid] = [(0, len(ops))]
        if not included:
            raise SystemExit("book:%s 没有可收录场景" % prefix)
        return included, collections.Counter(), {"boundary": []}
    if mode == "all":
        included = {}
        for sid in src.scene_ids():
            ops = src.ops(sid)
            if ops:
                included[sid] = [(0, len(ops))]
        return included, collections.Counter(), {"boundary": []}

    with_letters = mode == "common-full"
    ranges = collections.defaultdict(list)
    queues = collections.deque()
    scanned = set()
    boundary = collections.Counter()
    detail = []

    def add(sid, a, b):
        if b > a:
            ranges[sid].append((a, b))
            queues.append((sid, a, b))

    def add_scene(sid):
        add(sid, 0, len(src.ops(sid)))

    def add_missing_part(sid):
        """把场景里尚未覆盖的段落补进来（递归闭包：真正的补集）。"""
        total = len(src.ops(sid))
        merged = []
        for a, b in sorted(ranges.get(sid, [])):
            if merged and a <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], b))
            else:
                merged.append((a, b))
        cursor = 0
        for a, b in merged:
            add(sid, cursor, a)
            cursor = max(cursor, b)
        add(sid, cursor, total)

    def include_request(target, entry):
        if is_route_chapter(src.scene(target)["source"]):
            return False
        if with_letters:
            add_missing_part(target)
        else:
            st = int(src.scene(target)["entries"].get(str(entry), 0))
            add(target, st, len(src.ops(target)))
        return True

    def scan():
        while queues:
            sid, a, b = queues.popleft()
            ops = src.ops(sid)
            for i in range(a, b):
                if (sid, i) in scanned:
                    continue
                scanned.add((sid, i))
                op = src.inst(ops, i)
                kind = op["op"]
                if kind in ("callScene", "jumpScene"):
                    target = op.get("scene")
                    entry = op.get("entry")
                    if not isinstance(target, int) or not isinstance(entry, int):
                        boundary[(sid, kind, "dynamic")] += 1
                        detail.append({"scene": sid, "op": i, "kind": kind,
                                       "reason": "dynamic target"})
                        continue
                    if target not in src._file_index:
                        boundary[(sid, kind, "missing")] += 1
                        detail.append({"scene": sid, "op": i, "kind": kind,
                                       "reason": "missing scene %s" % target})
                        continue
                    if book_of(src.catalog, target) in COMMON_BOOKS:
                        add_scene(target)
                        continue
                    if not include_request(target, entry):
                        boundary[(sid, kind, "route-chapter")] += 1
                        detail.append({"scene": sid, "op": i, "kind": kind,
                                       "target": target,
                                       "chapter": src.scene(target)["source"]})
                elif kind == "native":
                    call = op.get("call") or []
                    if len(call) >= 3 and call[0] == "call" and call[1] == "$$set_bbs_request_data":
                        names = src.source_name_map()
                        for scene_idx, entry_idx in ((5, 6), (7, 8)):
                            if len(call[2]) <= entry_idx:
                                continue
                            scene_name, entry = call[2][scene_idx], call[2][entry_idx]
                            if not isinstance(scene_name, str) or not isinstance(entry, int):
                                continue
                            target = names.get(scene_name.lower())
                            if target is None or target not in src._file_index:
                                boundary[(sid, "board", "missing")] += 1
                                detail.append({"scene": sid, "op": i, "kind": "board",
                                               "reason": "missing scene %s" % scene_name})
                                continue
                            if not include_request(target, entry):
                                boundary[(sid, "board", "route-chapter")] += 1
                                detail.append({"scene": sid, "op": i, "kind": "board",
                                               "target": target,
                                               "chapter": src.scene(target)["source"]})
                elif kind in ("goto", "ifFalse", "ifTrue", "call"):
                    to = op.get("to")
                    if isinstance(to, list) or not isinstance(to, int):
                        boundary[(sid, kind, "dynamic")] += 1
                        detail.append({"scene": sid, "op": i, "kind": kind,
                                       "reason": "dynamic jump label"})

    flow = int(src.catalog["flowScene"])
    add_scene(flow)
    scan()
    # 所有“委托任务”段落（entry 10 起）都属于共通篇；add() 入队后必须再扫描，
    # 否则这些任务里的 callScene / native / preview 依赖会漏（旧实现的缺口）。
    for sid in src.scene_ids():
        scene = src.scene(sid)
        if book_of(src.catalog, sid) in COMMON_BOOKS:
            continue
        if is_route_chapter(scene["source"]):
            continue
        entries = scene.get("entries") or {}
        if "10" not in entries:
            continue
        add(sid, int(entries["10"]), len(src.ops(sid)))
    scan()
    # 新收录的段可能又带出新的任务引用，循环到固定点。
    for _ in range(8):
        before = sum(len(v) for v in ranges.values())
        scan()
        after = sum(len(v) for v in ranges.values())
        if after == before:
            break
    merged = {}
    for sid, rs in ranges.items():
        rs.sort()
        out = []
        for a, b in rs:
            if out and a <= out[-1][1]:
                out[-1][1] = max(out[-1][1], b)
            else:
                out.append([a, b])
        merged[sid] = [tuple(x) for x in out]
    return merged, boundary, {"boundary": detail}


def load_scope_manifest(src: "Source", path: str):
    """Load an explicit, reviewed source-range manifest.

    This is intentionally data-only: it cannot add arbitrary source text or
    flags, and every range is checked against the pinned source tree.
    """
    raw = open(path, "rb").read()
    doc = json.loads(raw.decode("utf-8"))
    expected = str(doc.get("source_commit") or "")
    if expected and src.source_commit and expected != src.source_commit:
        raise SystemExit("scope manifest source commit mismatch: %s != %s" %
                         (expected, src.source_commit))
    included = {}
    for sid_text, ranges in (doc.get("ranges") or {}).items():
        sid = int(sid_text)
        if not src.scene(sid):
            raise SystemExit("scope manifest scene missing: %d" % sid)
        ops = src.ops(sid)
        checked = []
        for pair in ranges:
            if not isinstance(pair, list) or len(pair) != 2:
                raise SystemExit("scope manifest bad range: %s" % (pair,))
            a, b = int(pair[0]), int(pair[1])
            if a < 0 or b > len(ops) or b <= a:
                raise SystemExit("scope manifest range out of bounds: %d [%d,%d), len=%d" %
                                 (sid, a, b, len(ops)))
            checked.append((a, b))
        checked.sort()
        for previous, current in zip(checked, checked[1:]):
            if current[0] < previous[1]:
                raise SystemExit("scope manifest ranges overlap: scene %d %s and %s" %
                                 (sid, previous, current))
        included[sid] = checked
    if not included:
        raise SystemExit("scope manifest has no ranges")
    boundary = collections.Counter()
    detail = list(doc.get("boundary_detail") or [])
    for item in detail:
        boundary[(item.get("scene"), item.get("kind"), item.get("reason"))] += 1
    info = {
        "path": os.path.relpath(os.path.abspath(path), REPO_ROOT),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "source_commit": expected,
        "schema": doc.get("schema"),
        "name": doc.get("name"),
        "boundary_detail": detail,
    }
    return included, boundary, {"boundary": detail}, info


def normalize(expr):
    if not isinstance(expr, list):
        if isinstance(expr, bool):
            return ("num", 1 if expr else 0)
        if isinstance(expr, (int, float)):
            return ("num", int(expr))
        if isinstance(expr, str):
            return ("str", expr)
        return ("num", 0)
    if not expr:
        return ("array", [])
    head = expr[0]
    if not isinstance(head, str):
        return ("array", [normalize(x) for x in expr])
    if head == "v":
        if len(expr) == 2:
            return ("var", str(expr[1]))
        key_part = expr[2]
        if isinstance(key_part, (int, str)):
            return ("var", str(expr[1]) + ":" + str(key_part))
        # 动态键：["v","z",["+",201,["v",...]]] —— 不能把表达式 repr 当变量名，
        # 否则运行期读写的是一条不存在的变量（记录位 z:201+ 全失效）。
        return ("varidx", str(expr[1]), normalize(key_part))
    if head == "call":
        name = str(expr[1])
        args = [normalize(a) for a in (expr[2] or [])]
        receiver = normalize(expr[3]) if len(expr) > 3 else None
        method = None
        path = None
        if "." in name:
            prefix, last = name.rsplit(".", 1)
            if last in METHODS:
                method, path = last, prefix
        if method is not None:
            if receiver is None:
                if path and path.startswith("global."):
                    receiver = ("var", path[len("global."):])
                else:
                    receiver = ("str", path or "")
            return ("method", method, receiver, args)
        return ("call", name, args, receiver)
    if head in UNARY_OPS and len(expr) == 2:
        return ("un", head, normalize(expr[1]))
    if head in BINARY_OPS and len(expr) == 3:
        return ("bin", head, normalize(expr[1]), normalize(expr[2]))
    if head == "index":
        return ("method", "get", normalize(expr[1]), [normalize(expr[2])])
    # 其余情况是字面量数组（选项文案、立绘名列表等）
    return ("array", [normalize(x) for x in expr])


def ast_to_json(ast):
    tag = ast[0]
    if tag in ("num", "str", "var"):
        return [tag, ast[1]]
    if tag == "varidx":
        return ["varidx", ast[1], ast_to_json(ast[2])]
    if tag == "array":
        return ["array", [ast_to_json(x) for x in ast[1]]]
    if tag == "un":
        return ["un", ast[1], ast_to_json(ast[2])]
    if tag == "bin":
        return ["bin", ast[1], ast_to_json(ast[2]), ast_to_json(ast[3])]
    if tag == "method":
        return ["method", ast[1], ast_to_json(ast[2]), [ast_to_json(x) for x in ast[3]]]
    if tag == "call":
        return ["call", ast[1], [ast_to_json(x) for x in ast[2]],
                ast_to_json(ast[3]) if ast[3] is not None else None]
    raise SystemExit("AST 序列化失败: %r" % (ast,))


def ast_from_json(obj):
    tag = obj[0]
    if tag in ("num", "str", "var"):
        return (tag, obj[1])
    if tag == "varidx":
        return ("varidx", obj[1], ast_from_json(obj[2]))
    if tag == "array":
        return ("array", [ast_from_json(x) for x in obj[1]])
    if tag == "un":
        return ("un", obj[1], ast_from_json(obj[2]))
    if tag == "bin":
        return ("bin", obj[1], ast_from_json(obj[2]), ast_from_json(obj[3]))
    if tag == "method":
        return ("method", obj[1], ast_from_json(obj[2]), [ast_from_json(x) for x in obj[3]])
    if tag == "call":
        return ("call", obj[1], [ast_from_json(x) for x in obj[2]],
                ast_from_json(obj[3]) if obj[3] is not None else None)
    raise SystemExit("AST 还原失败: %r" % (obj,))


def expr_strings(ast, out: set) -> None:
    tag = ast[0]
    if tag == "str":
        out.add(ast[1])
    elif tag == "array":
        for item in ast[1]:
            expr_strings(item, out)
    elif tag == "un":
        expr_strings(ast[2], out)
    elif tag == "bin":
        expr_strings(ast[2], out)
        expr_strings(ast[3], out)
    elif tag == "method":
        expr_strings(ast[2], out)
        for item in ast[3]:
            expr_strings(item, out)
    elif tag == "varidx":
        expr_strings(ast[2], out)
    elif tag == "call":
        for item in ast[2]:
            expr_strings(item, out)
        if ast[3] is not None:
            expr_strings(ast[3], out)


def native_calls(ast, out: set) -> None:
    tag = ast[0]
    if tag == "array":
        for item in ast[1]:
            native_calls(item, out)
    elif tag == "un":
        native_calls(ast[2], out)
    elif tag == "bin":
        native_calls(ast[2], out)
        native_calls(ast[3], out)
    elif tag == "method":
        native_calls(ast[2], out)
        for item in ast[3]:
            native_calls(item, out)
    elif tag == "varidx":
        native_calls(ast[2], out)
    elif tag == "call":
        out.add(ast[1])
        for item in ast[2]:
            native_calls(item, out)
        if ast[3] is not None:
            native_calls(ast[3], out)


# ---------------------------------------------------------------- 立绘几何

def body_display_size(base_size, layout, scale=None):
    bw, bh = int(base_size[0]), int(base_size[1])
    if layout:
        lay_h = min(LAYOUT_BASE_H, int(round(layout[0] * LAYOUT_BASE_H / LAYOUT_SOURCE_H)))
    else:
        lay_h = LAYOUT_BASE_H
    if bh <= 0:
        bh = 1
    width = int(round(bw * lay_h / bh))
    scale = LAYOUT_SCALE if scale is None else scale
    return max(1, int(round(width * scale))), max(1, int(round(lay_h * scale)))


def face_display_size(pose_row, body_screen):
    bw = int(pose_row["baseSize"][0]) or 1
    scale = body_screen[0] / float(bw)
    return (max(1, int(round(pose_row["w"] * scale))),
            max(1, int(round(pose_row["h"] * scale))))


# ---------------------------------------------------------------- 图像编码

def _quantize_rgba(im, dw, dh, colors):
    """RGB quantise with bounded alpha buckets for partial edges.

    A palette entry has one alpha value. A plain max-alpha per RGB index makes
    every semi-transparent edge pixel opaque when the same RGB also occurs in
    the opaque interior. Keep the clear index separate and split each RGB
    index into opaque / low-partial / high-partial buckets. This is bounded by
    1 + 3 * colors entries and requires no frame-sized RGBA buffer at runtime.
    """
    rgba = im.convert("RGBA").resize((dw, dh), 1)
    raw = rgba.tobytes()
    alphas = raw[3::4]
    rgb = rgba.convert("RGB")
    rgb_colors = max(1, colors)
    q = rgb.quantize(colors=rgb_colors, method=Image.FASTOCTREE,
                     dither=Image.Dither.NONE)
    pal = q.getpalette()[:rgb_colors * 3]
    pal += [0] * (rgb_colors * 3 - len(pal))
    groups = {}
    pixel_keys = []
    for idx, a in zip(list(q.getdata()), alphas):
        if a < 8:
            pixel_keys.append(None)
            continue
        bucket = 0 if a >= 248 else (1 if a < 128 else 2)
        key = (idx, bucket)
        g = groups.get(key)
        if g is None:
            groups[key] = [a, 1]
        else:
            g[0] += a
            g[1] += 1
        pixel_keys.append(key)
    keys = sorted(groups)
    index_of = {key: i + 1 for i, key in enumerate(keys)}
    pal_out = [0, 0, 0]
    alpha_out = [0]
    for key in keys:
        idx, bucket = key
        r, g, b = pal[idx * 3:idx * 3 + 3]
        pal_out += [r, g, b]
        if bucket == 0:
            aout = 255
        else:
            aout = max(8, min(247, int(round(groups[key][0] / groups[key][1]))))
        alpha_out.append(aout)
    out_data = [0 if key is None else index_of[key] for key in pixel_keys]
    pal_out += [0] * (768 - len(pal_out))
    out = Image.new("P", (dw, dh))
    out.putpalette(pal_out[:768])
    out.putdata(out_data)
    out.info["transparency"] = bytes(alpha_out)
    return out


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _filter_rows(indices, w, h):
    out = bytearray()
    prev = bytearray(w)
    for y in range(h):
        row = indices[y * w:(y + 1) * w]
        sub = bytes((row[x] - (row[x - 1] if x else 0)) & 0xFF for x in range(w))
        up = bytes((row[x] - prev[x]) & 0xFF for x in range(w))
        paeth = bytes((row[x] - _paeth(row[x - 1] if x else 0, prev[x],
                                       prev[x - 1] if x else 0)) & 0xFF for x in range(w))
        cands = [(0, bytes(row)), (1, sub), (2, up), (4, paeth)]
        best = min(cands, key=lambda c: sum(min(v, 256 - v) for v in c[1]))
        out.append(best[0])
        out += best[1]
        prev = bytearray(row)
    return bytes(out)


def palette_of(im):
    pal = im.getpalette() or []
    transparency = im.info.get("transparency")
    count = len(pal) // 3
    if isinstance(transparency, (bytes, bytearray)):
        alpha = list(transparency) + [255] * (count - len(transparency))
    elif isinstance(transparency, int):
        alpha = [255] * count
        if 0 <= transparency < count:
            alpha[transparency] = 0
    else:
        alpha = [255] * count
    return [(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2], alpha[i]) for i in range(count)]


def resize_to_palette(im, dw, dh):
    if (im.width, im.height) == (dw, dh):
        return list(im.getdata()), palette_of(im)  # noqa: deprecation
    pal = palette_of(im)
    rgba = im.convert("RGBA").resize((dw, dh), 1)  # PIL.Image.BOX
    raw = rgba.tobytes()
    cache = {}
    indices = []
    opaque = [i for i, e in enumerate(pal) if e[3] >= 8]
    clear = [i for i, e in enumerate(pal) if e[3] < 8]
    for i in range(0, len(raw), 4):
        key = raw[i:i + 4]
        got = cache.get(key)
        if got is None:
            cr, cg, cb, ca = key
            if ca < 8:
                got = clear[0] if clear else 0
            else:
                best, bd = 0, 1 << 60
                for j in opaque:
                    e = pal[j]
                    d = ((e[0] - cr) ** 2 + (e[1] - cg) ** 2 + (e[2] - cb) ** 2
                         + 3 * (e[3] - ca) ** 2)
                    if d < bd:
                        bd, best = d, j
                got = best
            cache[key] = got
        indices.append(got)
    return indices, pal[:max(indices) + 1]


def encode_block_sprite(im, dw, dh, colors=None):
    if colors:
        im = _quantize_rgba(im, dw, dh, colors)
        indices, pal = list(im.getdata()), palette_of(im)
    else:
        indices, pal = resize_to_palette(im, dw, dh)
    # PIL returns a fixed 256-entry palette even after quantize(colors=32);
    # only write the entries actually referenced by the pixel indices. This
    # keeps the advertised palette-32 size and the real binary rows in sync.
    used = max(indices) + 1 if indices else 1
    pal_count = max(1, min(len(pal), used))
    pal = pal[:pal_count]
    out = bytearray()
    out += struct.pack("<HHHHHH", dw, dh, pal_count, ROWS_PER_BLOCK,
                       (dh + ROWS_PER_BLOCK - 1) // ROWS_PER_BLOCK, 0)
    out += struct.pack("<I", (dw + 1) * ROWS_PER_BLOCK)
    for r, g, b, _a in pal:
        out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    out += bytes(e[3] for e in pal)
    blocks = []
    for y0 in range(0, dh, ROWS_PER_BLOCK):
        rows = min(ROWS_PER_BLOCK, dh - y0)
        packed = _filter_rows(indices[y0 * dw:(y0 + rows) * dw], dw, rows)
        blocks.append(zlib.compress(packed, 9))
    for block in blocks:
        out += struct.pack("<I", len(block))
    for block in blocks:
        out += block
    return bytes(out)


from PIL import Image  # noqa: E402  (仅编码阶段需要 Pillow)


def encode_jpeg(im, quality):
    # TJpgDec decodes arbitrary dimensions and reports partial MCU rectangles.
    # Do not pad with black: the image row's w/h must match the encoded JPEG so
    # the runtime centers the actual pixels instead of shifting a padded canvas.
    rgb = im.convert("RGB")
    buf = io.BytesIO()
    rgb.save(buf, "JPEG", quality=quality, optimize=True, subsampling=2)
    return buf.getvalue()


# ---------------------------------------------------------------- 打包

class Interner:
    def __init__(self):
        self.items = []
        self.index = {}

    def add(self, value):
        if value in self.index:
            return self.index[value]
        idx = len(self.items)
        self.index[value] = idx
        self.items.append(value)
        return idx


class ExprCompiler:
    def __init__(self, packer):
        self.pack = packer
        self.blob = bytearray()

    def emit(self, data: bytes) -> int:
        off = len(self.blob)
        self.blob += data
        return off

    def compile(self, ast) -> int:
        tag = ast[0]
        if tag == "num":
            return self.emit(struct.pack("<Bi", E_NUM, int(ast[1])))
        if tag == "str":
            return self.emit(struct.pack("<B" + IDC, E_STR, self.pack.text_id(ast[1])))
        if tag == "var":
            return self.emit(struct.pack("<B" + IDC, E_VAR, self.pack.var_id(ast[1])))
        if tag == "varidx":
            start = self.emit(struct.pack("<B" + IDC, E_VARDYN,
                                          self.pack.text_id(ast[1])))
            self.compile(ast[2])
            return start
        if tag == "array":
            start = self.emit(struct.pack("<BH", E_ARRAY, len(ast[1])))
            for item in ast[1]:
                self.compile(item)
            return start
        if tag == "un":
            start = self.emit(struct.pack("<BB", E_UN, UNARY_OPS[ast[1]]))
            self.compile(ast[2])
            return start
        if tag == "bin":
            start = self.emit(struct.pack("<BB", E_BIN, BINARY_OPS[ast[1]]))
            self.compile(ast[2])
            self.compile(ast[3])
            return start
        if tag == "method":
            start = self.emit(struct.pack("<BBB", E_METHOD, METHODS[ast[1]], len(ast[3])))
            self.compile(ast[2])
            for item in ast[3]:
                self.compile(item)
            return start
        if tag == "call":
            name, args, receiver = ast[1], ast[2], ast[3]
            if (name not in SUPPORTED_NATIVES
                    and self.pack.helper_for(name, self.pack.current_scene) is None):
                self.pack.unknown_natives.add(name)
            start = self.emit(struct.pack("<B" + IDC + "B", E_CALL,
                                          self.pack.text_id(name),
                                          1 if receiver is not None else 0))
            self.blob += struct.pack("<B", len(args))
            if receiver is not None:
                self.compile(receiver)
            for item in args:
                self.compile(item)
            return start
        raise SystemExit("表达式编译失败: %r" % (ast,))


class Packer:
    def __init__(self, src: Source, args):
        self.src = src
        self.args = args
        self.text = Interner()
        self.text_blob = bytearray()
        self.var = Interner()
        self.expr = ExprCompiler(self)
        self.cond = Interner()
        self.call_table = Interner()
        self.set_table = Interner()
        self.choice = Interner()
        self.namelist_rows = []
        self.namelist_index = {}
        self.pose_rows = []
        self.actor_rows = []
        self.name_rows = []
        self.name_index = {}
        self.image_rows = []
        self.image_index = {}
        self.block_palette_max = 0
        self.blob = bytearray()
        self.ops = []
        self.op_scene = {}
        self.scene_slots = []
        self.scnid = []
        self.entry_rows = []
        self.chap_rows = []
        self.natives_used = set()
        self.helper_calls = set()
        self.unknown_natives = set()
        self.unmatched_images = set()
        self.boundary_jumps = 0
        self.included = {}
        self.boundary = collections.Counter()
        self.boundary_detail = {"boundary": []}
        self.detail = collections.Counter()
        self.section_stats = collections.Counter()
        self._expr_cache = {}
        self.text_frozen = False
        self.current_scene = 0
        self.helper_entries = []
        self.reachable_dresses = {}     # actor -> set(dress)
        self.reachable_times = {0}      # $time_control 取值
        self.all_dresses = False

    def helper_for(self, name, scene):
        """按源 engine-vm 的规则找脚本函数：先 <chapter>:<name>，再 <name>。"""
        src = self.src.helpers
        for key, scope in (("%d:%s" % (scene, name), scene), (name, 0xFFFF)):
            if key in src:
                return scope, key, src[key]
        return None

    # ------------------------------------------------------------ 字符串 / 变量
    def text_id(self, value: str) -> int:
        if self.text_frozen and value not in self.text.index:
            raise RuntimeError("文本表已冻结，禁止追加新字符串: %r" % (value,))
        return self.text.add(value)

    def var_id(self, key: str) -> int:
        return self.var.add(key)

    def build_text(self):
        self.text_frozen = True
        offsets = [0]
        for value in self.text.items:
            self.text_blob += value.encode("utf-8") + b"\x00"
            offsets.append(len(self.text_blob))
        return offsets

    # ------------------------------------------------------------ 表
    def cond_id(self, ast) -> int:
        key = "K" + json.dumps(ast_to_json(ast), ensure_ascii=False)
        if key in self._expr_cache:
            return self._expr_cache[key]
        idx = self.cond.add(self.expr.compile(ast))
        self._expr_cache[key] = idx
        return idx

    def call_id(self, ast) -> int:
        key = "C" + json.dumps(ast_to_json(ast), ensure_ascii=False)
        if key in self._expr_cache:
            return self._expr_cache[key]
        idx = self.call_table.add(self.expr.compile(ast))
        self._expr_cache[key] = idx
        return idx

    def set_id(self, var_key: str, ast) -> int:
        entry = (var_key, json.dumps(ast_to_json(ast), ensure_ascii=False))
        return self.set_table.add(entry)

    def choice_id(self, labels, enabled, target) -> int:
        entry = (json.dumps(ast_to_json(labels), ensure_ascii=False),
                 json.dumps(ast_to_json(enabled), ensure_ascii=False),
                 json.dumps(ast_to_json(target), ensure_ascii=False))
        return self.choice.add(entry)

    def namelist_id(self, exprs) -> int:
        key = tuple(json.dumps(ast_to_json(a), ensure_ascii=False) for a in exprs)
        got = self.namelist_index.get(key)
        if got is not None:
            return got
        start = len(self.namelist_rows)
        for ast in exprs:
            self.namelist_rows.append(self.expr.compile(ast))
        self.namelist_index[key] = start
        return start

    # ------------------------------------------------------------ 立绘
    def pose_rows_for(self, wanted_names):
        rows = collections.defaultdict(list)
        for key, row in self.src.portraits.items():
            name, _, dress = key.partition("@")
            rows[name].append((int(dress or 0), row))
        for name in rows:
            rows[name].sort(key=lambda r: r[0])
        wanted = set()
        for name in wanted_names:
            if name in rows:
                wanted.add(name)
            # 源 poseKey 的回退：n -> <prefix>_01 -> n@defaultDress
            m = re.match(r"^(.+?)(\d{2})_(\d{2})$", name)
            if m:
                alt = m.group(1) + m.group(2) + "_01"
                if alt in rows:
                    wanted.add(alt)
            m2 = re.match(r"^(?:bs[123]_)?([a-z]{2})$", name)
            if m2:
                alt = m2.group(1) + "11_01"
                if alt in rows:
                    wanted.add(alt)
        for name in sorted(wanted):
            actor_match = re.match(r"^(?:bs[123]_)?([a-z]{2})", name)
            actor = actor_match.group(1) if actor_match else ""
            allowed = self.reachable_dresses.get(actor)
            default = int((self.src.char_defaults.get(actor) or {}).get("dress", 1))
            for dress, row in rows[name]:
                if not self.all_dresses and allowed is not None:
                    if dress not in allowed and dress != default:
                        continue
                body, body_screen = self.body_image(row["base"], row["baseSize"],
                                                    row.get("layout"))
                face = self.face_image(row["face"], row, body_screen)
                layout = row.get("layout") or [LAYOUT_SOURCE_H, 500]
                self.pose_rows.append(struct.pack(
                    "<" + IDC + "11H", self.name_id(name), dress, body, face,
                    int(row.get("x", 0)), int(row.get("y", 0)),
                    int(row["w"]), int(row["h"]),
                    int(row["baseSize"][0]), int(row["baseSize"][1]),
                    int(layout[0]), int(layout[1])))
        actors = set()
        for name in sorted(wanted):
            m = re.match(r"^(?:bs[123]_)?([a-z]{2})", name)
            if not m:
                continue
            code = m.group(1)
            if code in actors:
                continue
            actors.add(code)
            default = int((self.src.char_defaults.get(code) or {}).get("dress", 1))
            self.actor_rows.append(struct.pack("<" + IDC + "3H", self.name_id(code), default, 0, 0))
        return sorted(wanted)

    def name_id(self, name: str) -> int:
        """立绘名 / 演员名 -> 名字表下标（POSE/ACTOR 用）。"""
        got = self.name_index.get(name)
        if got is not None:
            return got
        idx = len(self.name_rows)
        self.name_rows.append(struct.pack("<" + IDC, self.text_id(name)))
        self.name_index[name] = idx
        return idx

    # ------------------------------------------------------------ 图像
    def add_image(self, payload, w, h, kind, blob_first=False):
        if blob_first:
            off = len(self.blob)
            self.blob += payload
            length = len(payload)
        else:
            off = len(self.blob) - len(payload)
            length = len(payload)
        if kind == 1 and len(payload) >= 6:
            pal_count = struct.unpack_from("<H", payload, 4)[0]
            if pal_count > self.block_palette_max:
                self.block_palette_max = pal_count
        idx = len(self.image_rows)
        self.image_rows.append(struct.pack("<IIHHBBH", off, length, w, h, kind, 0, 0))
        return idx

    def body_image(self, relpath, base_size, layout):
        key = ("body", relpath)
        got = self.image_index.get(key)
        if got is not None:
            return got, body_display_size(base_size, layout)
        size = body_display_size(base_size, layout)
        path = os.path.join(self.src.root, "src", "common", "images", relpath)
        with Image.open(path) as im:
            payload = encode_block_sprite(im, size[0], size[1], self.args.palette_colors)
        idx = self.add_image(payload, size[0], size[1], 1, blob_first=True)
        self.image_index[key] = idx
        self.detail["body"] += 1
        return idx, size

    def face_image(self, relpath, row, body_screen):
        size = face_display_size(row, body_screen)
        key = ("face", relpath, size)
        got = self.image_index.get(key)
        if got is not None:
            return got
        path = os.path.join(self.src.root, "src", "common", "images", relpath)
        with Image.open(path) as im:
            payload = encode_block_sprite(im, size[0], size[1], self.args.palette_colors)
        idx = self.add_image(payload, size[0], size[1], 1, blob_first=True)
        self.image_index[key] = idx
        self.detail["face"] += 1
        return idx

    def scene_image(self, name: str):
        """背景 / CG 名字 -> IMAGE 下标；纯色背景是 SOLID 条目。"""
        key = ("scene", name)
        got = self.image_index.get(key)
        if got is not None:
            return got
        if name in ("bg_black", "bg_red", "bg_white"):
            color = {"bg_black": 0x0000, "bg_red": 0xF800, "bg_white": 0xFFFF}[name]
            off = len(self.blob)
            self.blob += struct.pack("<H", color)
            idx = len(self.image_rows)
            self.image_rows.append(struct.pack("<IIHHBBH", off, 2, 0, 0, 2, 0, 0))
            self.image_index[key] = idx
            self.detail["solid"] += 1
            return idx
        rel = self.resolve_asset(name)
        if rel is None:
            self.unmatched_images.add(name)
            return 0xFFFF
        path = os.path.join(self.src.root, "src", "common", "images", rel)
        with Image.open(path) as im:
            # Source UI is object-fit:contain on a #fcfaf0 art background.
            # Keep the contained image size; the runtime centres it on paper.
            aw, ah = self.args.art_width, self.args.art_height
            scale = min(aw / im.width, ah / im.height, 1.0)
            size = (max(1, int(round(im.width * scale))), max(1, int(round(im.height * scale))))
            small = im.resize(size, 1) if size != im.size else im.copy()
            assert small.width <= aw and small.height <= ah, small.size
            payload = encode_jpeg(small, self.args.jpeg_quality)
            img_w, img_h = small.size
        off = len(self.blob)
        self.blob += payload
        idx = len(self.image_rows)
        self.image_rows.append(struct.pack("<IIHHBBH", off, len(payload), img_w, img_h, 0, 0, 0))
        self.image_index[key] = idx
        self.detail["scene"] += 1
        return idx

    def resolve_asset(self, name: str):
        name = name.lower()
        best = None
        for key, rel in self.src.assets.items():
            if key == name or key.startswith(name + "_"):
                if best is None or key < best[0]:
                    best = (key, rel)
        return best[1] if best else None

    def asset_names_for(self, name: str):
        """源名字 -> 可达的实际资源键（名字本身 + 可达时间后缀）。"""
        name = name.lower()
        keys = set()
        if name in self.src.assets:
            keys.add(name)
        for suffix in self.reachable_time_suffixes():
            if name + suffix in self.src.assets:
                keys.add(name + suffix)
        return sorted(keys)

    # ------------------------------------------------------------ 场景 / 指令映射
    def prepare_scenes(self):
        start = 0
        for sid, ranges in sorted(self.included.items()):
            count = sum(b - a for a, b in ranges)
            self.scene_slots.append(start)
            self.scnid.append(sid)
            self.op_scene[sid] = (start, count, ranges)
            start += count
        self.scene_slots.append(start)

    def slot_of(self, sid):
        try:
            return self.scnid.index(sid)
        except ValueError:
            return None

    def map_op(self, sid, source_index):
        """源场景内指令下标 -> 包内全局指令下标；不在包内返回 None。"""
        ranges = self.included.get(sid)
        if not ranges:
            return None
        slot = self.slot_of(sid)
        if slot is None:
            return None
        base = self.scene_slots[slot]
        consumed = 0
        for a, b in ranges:
            if a <= source_index < b:
                return base + consumed + (source_index - a)
            consumed += b - a
        return None

    # ------------------------------------------------------------ 指令编译
    def emit(self, kind, flags=0, a=0, b=0, c=0):
        self.ops.append(struct.pack("<BB" + IDC + "II", kind, flags, a, b, c))

    def scan_state_space(self):
        """从本卷的 $$set_bs_dress_type / $$set_time_control 字面量推可达状态。"""
        for sid, ranges in self.included.items():
            ops = self.src.ops(sid)
            for a, b in ranges:
                for i in range(a, b):
                    op = self.src.inst(ops, i)
                    if op["op"] != "native":
                        continue
                    call = op.get("call") or []
                    if len(call) < 3 or call[0] != "call":
                        continue
                    name, args = call[1], call[2]
                    if name == "$$set_bs_dress_type" and len(args) >= 2:
                        actor, dress = args[0], args[1]
                        if isinstance(actor, str) and isinstance(dress, int):
                            self.reachable_dresses.setdefault(actor.lower(), set()).add(dress)
                        else:
                            self.all_dresses = True
                    elif name == "$$set_time_control" and args:
                        value = args[0]
                        if isinstance(value, int):
                            self.reachable_times.add(value)
        for name, helper in self.src.helpers.items():
            for op in helper.get("ops") or []:
                if op.get("op") != "native":
                    continue
                call = op.get("call") or []
                if len(call) < 3:
                    continue
                if call[1] == "$$set_time_control" and call[2] and isinstance(call[2][0], int):
                    self.reachable_times.add(call[2][0])
                if call[1] == "$$set_bs_dress_type" and len(call[2]) >= 2:
                    actor, dress = call[2][0], call[2][1]
                    if isinstance(actor, str) and isinstance(dress, int):
                        self.reachable_dresses.setdefault(actor.lower(), set()).add(dress)

    def reachable_time_suffixes(self):
        return sorted({TIME_SUFFIX.get(t, "_%02d" % (t + 1)) for t in self.reachable_times})

    def compile_all(self):
        self.scan_state_space()
        self.prepare_scenes()
        referenced_names = set()
        for sid, ranges in sorted(self.included.items()):
            ops = self.src.ops(sid)
            for a, b in ranges:
                for i in range(a, b):
                    op = self.src.inst(ops, i)
                    if op["op"] == "portrait":
                        for value in op.get("names") or []:
                            referenced_names.update(self._strings(value))
                    if op["op"] == "background":
                        for value in [op.get("image")]:
                            for name in self._strings(value):
                                referenced_names.add(name)
        pose_names = {n.lower().split("_act_")[0] for n in referenced_names
                      if n and not n.startswith(("b_", "bg", "cg_", "ef_", "_menu"))}
        self.wanted_pose_names = self.pose_rows_for(pose_names)
        for sid, ranges in sorted(self.included.items()):
            self.current_scene = sid
            ops = self.src.ops(sid)
            for a, b in ranges:
                for i in range(a, b):
                    self.compile_op(self.src.inst(ops, i), sid, i)

    def _strings(self, value):
        if isinstance(value, str):
            return [value]
        out = set()
        expr_strings(normalize(value), out)
        return sorted(out)

    def compile_op(self, op, sid, index):
        kind = op["op"]
        if kind == "text":
            speaker = str(op.get("speaker") or "")
            body = str(op.get("text") or "")
            self.emit(OP_TEXT, 0, self.text_id(speaker) if speaker else 0xFFFF,
                      self.text_id(body))
        elif kind == "background":
            names = self._strings(op.get("image"))
            layer = 1 if op.get("layer") == "cg" else 0
            name = names[0] if names else "bg_black"
            image = self.scene_image(name)
            self.emit(OP_BG, layer, self.text_id(name), image if image != 0xFFFF else 0xFFFF)
        elif kind == "portrait":
            mode = {"replace": 0, "change": 1, "delete": 2}.get(op.get("mode"), 0)
            exprs = [normalize(n) for n in (op.get("names") or [])]
            start = self.namelist_id(exprs)
            self.emit(OP_PORTRAIT, mode, start, len(exprs))
        elif kind == "copyBodies":
            self.emit(OP_COPY_BODIES, 1 if op.get("value") else 0)
        elif kind == "copyCG":
            self.emit(OP_COPY_CG, 1 if op.get("value") else 0)
        elif kind == "visualCommit":
            self.emit(OP_COMMIT)
        elif kind == "initTime":
            self.emit(OP_INIT_TIME, 1 if op.get("resetPortraits") else 0)
        elif kind in ("ifFalse", "ifTrue"):
            cid = self.cond_id(normalize(op.get("condition")))
            target = self.map_op(sid, int(op["to"]))
            if target is None:
                self.boundary_jumps += 1
                # Preserve the shipped chapter-head boundary encoding: a
                # chapter-head range ends with a return, not a new opcode that
                # older audit tooling did not see.
                chapter_head = any(
                    item.get("scene") == sid and item.get("kind") == "chapter-head"
                    for item in (self.boundary_detail.get("boundary") or []))
                self.emit(OP_RETURN if chapter_head else OP_BOUNDARY,
                          0, self.text_id("branch") if not chapter_head else 0, 0)
            else:
                self.emit(OP_IF_FALSE if kind == "ifFalse" else OP_IF_TRUE, 0, cid, target)
        elif kind == "goto":
            target = self.map_op(sid, int(op["to"]))
            if target is None:
                self.boundary_jumps += 1
                chapter_head = any(
                    item.get("scene") == sid and item.get("kind") == "chapter-head"
                    for item in (self.boundary_detail.get("boundary") or []))
                self.emit(OP_RETURN if chapter_head else OP_BOUNDARY,
                          0, self.text_id("goto") if not chapter_head else 0, 0)
            else:
                self.emit(OP_GOTO, 0, 0, target)
        elif kind == "call":
            target = self.map_op(sid, int(op["to"]))
            self.emit(OP_CALL, 0, 0, target if target is not None else 0)
        elif kind == "return":
            self.emit(OP_RETURN)
        elif kind in ("jumpScene", "callScene"):
            target = int(op["scene"])
            entry = op.get("entry")
            if isinstance(entry, int):
                self.emit(OP_JUMP if kind == "jumpScene" else OP_CALL_SCENE, 0,
                          target & 0xFFFF, entry)
            else:
                eid = self.expr.compile(normalize(entry))
                self.emit(OP_JUMP if kind == "jumpScene" else OP_CALL_SCENE, 1,
                          target & 0xFFFF, 0, eid)
        elif kind == "dynamicChoice":
            labels = normalize(op.get("labels"))
            enabled = normalize(op.get("enabled")) if op.get("enabled") is not None else ("num", 1)
            target = normalize(op.get("target"))
            self.emit(OP_CHOICE, 0, self.choice_id(labels, enabled, target))
        elif kind == "board":
            self.emit(OP_BOARD)
        elif kind == "end":
            self.emit(OP_END)
        elif kind == "omittedScene":
            self.emit(OP_OMITTED)
        elif kind == "skipGame":
            game = {"rps": 1, "quiz": 2, "snake": 3, "race": 4}.get(op.get("game"), 0)
            self.emit(OP_SKIP_GAME, 0, game)
        elif kind == "routeCheck":
            self.emit(OP_ROUTE_CHECK)
        elif kind == "set":
            target = op.get("target") or ["v", ""]
            if target[0] != "v":
                raise SystemExit("set 目标不是变量: %r" % (target,))
            if len(target) > 2 and not isinstance(target[2], (int, str)):
                # 动态键（记录位 z:201+n）：base 文本 id + 索引表达式 + 值表达式
                self.emit(OP_SET_DYN, 0, self.text_id(str(target[1])),
                          self.expr.compile(normalize(target[2])),
                          self.expr.compile(normalize(op.get("value"))))
                return
            key = str(target[1]) + (":" + str(target[2]) if len(target) > 2 else "")
            if self._is_arrive_poll(op):
                self.emit(OP_ARRIVE_CHOICE, 0, 11, int(op.get("to") or 0))
                return
            self.emit(OP_SET, 0, self.set_id(key, normalize(op.get("value"))))
        elif kind == "native":
            self.emit(OP_NATIVE, 0, self.call_id(normalize(op.get("call"))))
        else:
            raise SystemExit("未知剧情指令 %s（场景 %s 第 %d 条）" % (kind, sid, index))

    def _is_arrive_poll(self, op):
        value = op.get("value")
        return (isinstance(value, list) and len(value) >= 2 and value[0] == "call"
                and value[1] == "$$input_update")

    # ------------------------------------------------------------ 场景表 / entry / chap
    def build_tables(self):
        entries = set()
        for sid, ranges in self.included.items():
            ops = self.src.ops(sid)
            for a, b in ranges:
                for i in range(a, b):
                    op = self.src.inst(ops, i)
                    if op["op"] in ("jumpScene", "callScene"):
                        if isinstance(op.get("entry"), int):
                            entries.add((int(op["scene"]), int(op["entry"])))
        # 场景内的全部 catalog entry 也要能跳（routeCheck 用变量选 entry）。
        for sid in self.included:
            scene = self.src.scene(sid)
            if not scene:
                continue
            for entry_no in scene.get("entries", {}):
                entries.add((sid, int(entry_no)))
        for target, entry in sorted(entries):
            scene = self.src.scene(target)
            if not scene:
                continue
            entry_op = int(scene["entries"].get(str(entry), 0))
            mapped = self.map_op(target, entry_op) if target in self.included else None
            self.entry_rows.append(struct.pack("<HHI", target, entry,
                                               0xFFFFFFFF if mapped is None else mapped))
        name_map = {}
        for sid in self.src.scene_ids():
            scene = self.src.scene(sid)
            label = (self.src.labels.get("scenes", {}) or {}).get(str(sid), {})
            slot = self.slot_of(sid)
            first = self.scene_slots[slot] if slot is not None else 0xFFFFFFFF
            self.chap_rows.append(struct.pack("<H" + IDC + IDC + IDC + "I", sid & 0xFFFF,
                                              self.text_id(label.get("name") or scene["name"]),
                                              self.text_id(label.get("dayLabel") or ""),
                                              self.text_id(scene["source"]), first))
            name_map[scene["source"].lower()] = sid
        self.scene_source_map = name_map

    # ------------------------------------------------------------ 名字表（背景解析）
    def build_namemap(self):
        """所有需要的图像名字 -> IMAGE 下标；运行时按名字 + 时间后缀查表。"""
        names = set()
        for sid, ranges in self.included.items():
            ops = self.src.ops(sid)
            for a, b in ranges:
                for i in range(a, b):
                    op = self.src.inst(ops, i)
                    if op["op"] == "background":
                        for name in self._strings(op.get("image")):
                            names.add(name.lower())
        self.bg_name_list = sorted(n for n in names if n)
        rows = {}
        for name in self.bg_name_list:
            for key in self.asset_names_for(name):
                image = self.scene_image(key)
                if image != 0xFFFF:
                    rows[self.text_id(key)] = image
            if name in ("bg_black", "bg_red", "bg_white"):
                rows[self.text_id(name)] = self.scene_image(name)
            if name in self.src.assets:
                image = self.scene_image(name)
                if image != 0xFFFF:
                    rows[self.text_id(name)] = image
        # 名字表：base 名如果只是“时段变体的别名”（assets.js 里既有 bg002 又有
        # bg002_01/02/03），源 exists_image() 会答“不存在”（它的名字集是原始
        # G00 文件名，没有裸 base）。用 image 字段最高位标记别名，C 端据此回答
        # exists_image，并仍能按名字取图。
        suffixes = self.reachable_time_suffixes()
        alias = set()
        for tid in rows:
            name = self.text.items[tid]
            if any((name + sfx) in self.src.assets for sfx in suffixes):
                alias.add(tid)
        self.name_rows_map = [
            struct.pack("<" + IDC + "H", tid,
                        (img | 0x8000) if tid in alias else img)
            for tid, img in sorted(rows.items())]

    # ------------------------------------------------------------ helpers
    def build_helpers(self):
        if os.environ.get("ANEMOI_DEBUG_HELPERS"):
            log("helpers_needed=%r" % (sorted(set(self.helpers_needed)),))
        rows = []
        micro = bytearray()  # 12 B / 条；HELPER.micro_off 记的是记录下标
        index = {}
        queue = list(self.helpers_needed)
        while queue:
            name, scene = queue.pop(0)
            if (name, scene) in index:
                continue
            found = self.helper_for(name, scene)
            if found is None:
                self.unknown_natives.add(name)
                continue
            scope, key, helper = found
            index[(name, scene)] = len(rows)
            ops = helper.get("ops") or []
            off = len(micro) // 12
            for op in ops:
                kind = op["op"]
                if kind == "set":
                    target = op.get("target") or ["v", ""]
                    if len(target) > 2 and not isinstance(target[2], (int, str)):
                        micro += struct.pack("<BBHII", 7, 0, self.text_id(str(target[1])),
                                             self.expr.compile(normalize(target[2])),
                                             self.expr.compile(normalize(op.get("value"))))
                    else:
                        vkey = str(target[1]) + (":" + str(target[2]) if len(target) > 2 else "")
                        var = self.var_id(vkey)
                        sid = self.set_id(vkey, normalize(op.get("value")))
                        micro += struct.pack("<BBHII", 1, 0, var, sid, 0)
                elif kind == "native":
                    ast = normalize(op.get("call"))
                    self._note_call(ast, scope if scope != 0xFFFF else 0)
                    micro += struct.pack("<BBHII", 2, 0, 0, self.call_id(ast), 0)
                elif kind == "goto":
                    micro += struct.pack("<BBHII", 3, 0, 0, int(op.get("to", 0)), 0)
                elif kind in ("ifFalse", "ifTrue"):
                    cid = self.cond_id(normalize(op.get("condition")))
                    micro += struct.pack("<BBHII", 4 if kind == "ifFalse" else 5, 0, cid,
                                         int(op.get("to", 0)), 0)
                elif kind == "return":
                    if "value" in op:
                        eid = self.expr.compile(normalize(op["value"]))
                        # 契约：return 带值 => flags bit0 = 1，载荷偏移放 b，
                        # c 也置 1 以兼容旧读取端（c&1）。
                        micro += struct.pack("<BBHII", 6, 1, 0, eid, 1)
                    else:
                        micro += struct.pack("<BBHII", 6, 0, 0, 0, 0)
                else:
                    raise SystemExit("helper %s 出现未知指令 %s" % (key, kind))
            for op in ops:
                for value in ([op.get("call")] if op["op"] == "native"
                              else [op.get("value")] if op["op"] == "set"
                              else [op.get("condition")] if op["op"] in ("ifFalse", "ifTrue")
                              else []):
                    if value is None:
                        continue
                    self._note_call(normalize(value), scope if scope != 0xFFFF else 0)
            params = helper.get("params") or []
            rows.append(struct.pack("<" + IDC + IDC + "IHH", self.text_id(key.split(":", 1)[-1]),
                                    self.text_id("".join(params)), off, len(ops),
                                    scope & 0xFFFF))
        self.micro_blob = bytes(micro)
        return rows

    def _note_call(self, ast, scene):
        names = set()
        native_calls(ast, names)
        for name in names:
            if self.helper_for(name, scene) is not None:
                self.helpers_needed.append((name, scene))
            elif name not in SUPPORTED_NATIVES:
                self.unknown_natives.add(name)

    # ------------------------------------------------------------ 写出
    def title_image(self, name):
        """Pack one source title image for the requested unlock level."""
        rel_map = {
            "normal": "src/common/ui/title-bg.jpg",
            "anemoi": "src/common/ui/title-anemoi.jpg",
            "eternicle": "src/common/ui/title-eternicle.jpg",
        }
        if name not in rel_map:
            raise SystemExit("unknown title image: %s" % name)
        rel = rel_map[name]
        full = os.path.join(self.src.root, *rel.split("/"))
        raw = None
        if os.path.exists(full):
            with open(full, "rb") as fh:
                raw = fh.read()
        else:
            ref = self.src.source_commit
            if not ref:
                raise SystemExit("title image unavailable: %s (no source commit)" % rel)
            try:
                raw = subprocess.check_output(
                    ["git", "-C", self.src.root, "show", ref + ":" + rel],
                    stderr=subprocess.DEVNULL)
            except (OSError, subprocess.CalledProcessError) as exc:
                raise SystemExit("title image unavailable at %s: %s" % (ref, rel)) from exc
        with Image.open(io.BytesIO(raw)) as im:
            src_w, src_h = im.size
            scale = min(self.args.art_width / src_w, self.args.art_height / src_h)
            size = (max(1, int(round(src_w * scale))), max(1, int(round(src_h * scale))))
            small = im.convert("RGB").resize(size, 1)
            payload = encode_jpeg(small, max(self.args.jpeg_quality, 60))
            img_w, img_h = small.size
        self.title_sources[name] = {
            "path": rel,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "source_width": src_w,
            "source_height": src_h,
            "packed_width": img_w,
            "packed_height": img_h,
        }
        off = len(self.blob)
        self.blob += payload
        idx = len(self.image_rows)
        self.image_rows.append(struct.pack("<IIHHBBH", off, len(payload), img_w, img_h, 0, 1, 0))
        return idx

    def write_charset(self):
        chars = set()
        for value in self.text.items:
            chars.update(value)
        for code in (13, 10, 9):
            chars.discard(chr(code))
        path = os.path.join(self.args.out_dir, "anemoi_charset.txt")
        self.charset_text = "".join(sorted(chars))
        with open(path, "w", encoding="utf-8", newline=chr(10)) as fh:
            fh.write(self.charset_text)
        return len(chars)

    def write(self):
        helper_rows = self.build_helpers()
        self.title_sources = {}
        title_images = {name: self.title_image(name)
                        for name in ("normal", "anemoi", "eternicle")}
        title_image = title_images["normal"]
        volume_names = set()
        for sid, ranges in self.included.items():
            ops = self.src.ops(sid)
            for a, b in ranges:
                for i in range(a, b):
                    op = self.src.inst(ops, i)
                    if op["op"] == "background":
                        for name in self._strings(op.get("image")):
                            volume_names.add(name.lower())
        self.gallery_included = []
        for item in self.src.gallery:
            name = str(item.get("image") or "").lower()
            if name in volume_names or name in ("bg_black", "bg_red", "bg_white"):
                self.gallery_included.append(item)
        self.build_namemap()
        set_rows = []
        for var_key, dump in self.set_table.items:
            set_rows.append(struct.pack("<HHI", self.var_id(var_key), 0,
                                        self.expr.compile(ast_from_json(json.loads(dump)))))
        choice_rows = []
        for labels, enabled, target in self.choice.items:
            choice_rows.append(struct.pack(
                "<IIII", self.expr.compile(ast_from_json(json.loads(labels))),
                self.expr.compile(ast_from_json(json.loads(enabled))),
                self.expr.compile(ast_from_json(json.loads(target))), 0))
        cond_rows = [struct.pack("<I", off) for off in self.cond.items]
        call_rows = [struct.pack("<I", off) for off in self.call_table.items]
        namelist_rows = [struct.pack("<I", off) for off in self.namelist_rows]
        req_rows = []
        for key, req in sorted(self.src.requests.items()):
            req_rows.append(struct.pack("<" + IDC + IDC + IDC + "H", self.text_id(key),
                                        self.text_id(req.get("applicant") or ""),
                                        self.text_id(req.get("text") or ""), 0))
        gal_rows = []
        for item in self.gallery_included:
            image = self.scene_image(str(item.get("image") or "").lower())
            if image == 0xFFFF:
                continue
            gal_rows.append(struct.pack("<H" + IDC + IDC + "H", image & 0xFFFF,
                                        self.text_id(item.get("group") or ""),
                                        self.text_id(item.get("label") or ""), 0))
        # native 按名读写的变量（故事 op 不引用）＋按公式拼出来的记录位/进度位。
        for name in NATIVE_VAR_NAMES:
            self.var_id(name)
        for index in range(201, 451):
            self.var_id("z:%d" % index)
        # 只有 z: 记录位是运行期拼出来的（helpers.js: "z" + 201 + n，检查 <250）；
        # a:/g: 都是剧本里的静态键，会由 AST 正常入池。
        # 文本表一旦序列化就不能再追加：先把“稍后才按名引用的字符串”全部入池。
        # 变量名是这里的关键（VAR 段在 payloads 里才 text_id），否则 id 会落在
        # 文本表之外，运行期 anem_pack_text() 返回 NULL、所有按名取变量失败。
        var_text_ids = [self.text_id(k) for k in self.var.items]
        offsets = self.build_text()
        txidx = b"".join(struct.pack("<I", x) for x in offsets)
        payloads = {
            SEC_TEXT: bytes(self.text_blob),
            SEC_TXIDX: txidx,
            SEC_OPS: b"".join(self.ops),
            SEC_SCENE: b"".join(struct.pack("<I", x) for x in self.scene_slots),
            SEC_SCNID: b"".join(struct.pack("<H", x) for x in self.scnid),
            SEC_ENTRY: b"".join(self.entry_rows),
            SEC_CHAP: b"".join(self.chap_rows),
            SEC_VAR: b"".join(struct.pack("<" + IDC, tid) for tid in var_text_ids),
            SEC_EXPR: bytes(self.expr.blob),
            SEC_COND: b"".join(cond_rows),
            SEC_CALL: b"".join(call_rows),
            SEC_SET: b"".join(set_rows),
            SEC_CHOICE: b"".join(choice_rows),
            SEC_NAMELIST: b"".join(namelist_rows),
            SEC_POSE: b"".join(self.pose_rows),
            SEC_ACTOR: b"".join(self.actor_rows),
            SEC_IMAGE: b"".join(self.image_rows),
            SEC_BLOB: bytes(self.blob),
            SEC_REQ: b"".join(req_rows),
            SEC_HELPER: b"".join(helper_rows),
            SEC_MICRO: self.micro_blob,
            SEC_GAL: b"".join(gal_rows),
            SEC_NAMEMAP: b"".join(self.name_rows_map),
        }
        counts = {
            SEC_TEXT: len(self.text_blob), SEC_TXIDX: len(offsets),
            SEC_OPS: len(self.ops), SEC_SCENE: len(self.scene_slots),
            SEC_SCNID: len(self.scnid), SEC_ENTRY: len(self.entry_rows),
            SEC_CHAP: len(self.chap_rows), SEC_VAR: len(self.var.items),
            SEC_EXPR: len(self.expr.blob), SEC_COND: len(self.cond.items),
            SEC_CALL: len(self.call_table.items), SEC_SET: len(self.set_table.items),
            SEC_CHOICE: len(self.choice.items), SEC_NAMELIST: len(namelist_rows),
            SEC_POSE: len(self.pose_rows), SEC_ACTOR: len(self.actor_rows),
            SEC_IMAGE: len(self.image_rows), SEC_BLOB: len(self.blob),
            SEC_REQ: len(req_rows), SEC_HELPER: len(helper_rows),
            SEC_MICRO: len(self.micro_blob) // 12, SEC_GAL: len(gal_rows),
            SEC_NAMEMAP: len(self.name_rows_map),
        }
        meta = "\n".join([
            "title=anemoi",
            "source=xuanyide01/anemoi-rw5",
            "scope=%s" % self.args.scope,
            "screen=%dx%d" % (self.args.art_width, self.args.height),
            "art_height=%d" % self.args.art_height,
            "jpeg_quality=%d" % self.args.jpeg_quality,
            "layout_scale=%.3f" % self.args.layout_scale,
            "palette_colors=%d" % self.args.palette_colors,
            "ops=%d" % len(self.ops),
            "text_bytes=%d" % len(self.text_blob),
            "bodies=%d" % self.detail["body"],
            "faces=%d" % self.detail["face"],
            "scenes=%d" % len(self.scnid),
            "poses=%d" % len(self.pose_rows),
            "title_image=%d" % title_image,
            "title_image_normal=%d" % title_images["normal"],
            "title_image_anemoi=%d" % title_images["anemoi"],
            "title_image_eternicle=%d" % title_images["eternicle"],
            "flow_scene=%d" % int(self.src.catalog["flowScene"]),
            "natives=%s" % ",".join(sorted(self.natives_used)),
            "unknown_natives=%s" % ",".join(sorted(self.unknown_natives)),
            "boundary=%d" % sum(self.boundary.values()),
            "",
        ]).encode("utf-8")
        payloads[SEC_META] = meta
        counts[SEC_META] = len(meta)
        sections = []
        offset = HEADER_SIZE + 16 * SEC_COUNT
        body = bytearray()
        for sec in range(SEC_COUNT):
            data = payloads.get(sec, b"")
            sections.append((sec, offset + len(body), counts.get(sec, 0), len(data)))
            body += data
        total = offset + len(body)
        head = bytearray(MAGIC)
        head += struct.pack("<HHHHIII", VERSION, HEADER_SIZE, SEC_COUNT, 0, total, 0, 0)
        for sec, off, count, size in sections:
            head += struct.pack("<IIII", sec, off, count, size)
        blob = bytearray(head) + body
        crc = zlib.crc32(bytes(blob[HEADER_SIZE:])) & 0xFFFFFFFF
        struct.pack_into("<I", blob, 20, crc)
        os.makedirs(self.args.out_dir, exist_ok=True)
        path = os.path.join(self.args.out_dir, "anemoi_pack.bin")
        with open(path, "wb") as fh:
            fh.write(blob)
        font_paths = {
            "gal_cjk_16.c": "assets/fonts/gal_cjk_16.c",
            "gal_cjk_symbols.txt": "assets/fonts/gal_cjk_symbols.txt",
        }
        font_info = {}
        for key, rel in font_paths.items():
            full = os.path.join(REPO_ROOT, rel)
            if os.path.exists(full):
                data = open(full, "rb").read()
                font_info[key] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            else:
                font_info[key] = None
        image_row_kinds = collections.Counter()
        for row in self.image_rows:
            image_row_kinds[struct.unpack_from("<B", row, 12)[0]] += 1
        manifest = {
            "path": os.path.relpath(os.path.abspath(path), REPO_ROOT),
            "bytes": len(blob),
            "sha256": hashlib.sha256(blob).hexdigest(),
            "crc32": "0x%08x" % crc,
            "scope": ("manifest:" + self.args.scope_manifest
                      if self.args.scope_manifest else self.args.scope),
            "source_commit": getattr(self.src, "source_commit", ""),
            "source_overlay": getattr(self.src, "overlay_info", None),
            "scope_manifest": getattr(self, "scope_info", None),
            "packer_args": vars(self.args),
            "font": font_info,
            "capacity": {
                "factory_partition_bytes": 0x7F0000,
                "pack_bytes": len(blob),
            },
            "sections": {SECTION_NAMES[sec]: {"offset": off, "count": count, "size": size}
                         for sec, off, count, size in sections},
            "ops": len(self.ops),
            "scenes": len(self.scnid),
            "pose_rows": len(self.pose_rows),
            "images": dict(self.detail),
            "image_row_kinds": {
                "scene": image_row_kinds.get(0, 0),
                "body_face": image_row_kinds.get(1, 0),
                "solid": image_row_kinds.get(2, 0),
            },
            "block_palette": {
                "rgb_colors": self.args.palette_colors,
                "alpha_buckets": 3,
                "max_entries": self.block_palette_max,
            },
            "scope_notes": (
                "included_scenes and scope_manifest are authoritative; "
                "sections.image.count is the SEC_IMAGE row count; source image "
                "keys are not 1:1 with binary rows."
            ),
            "text_bytes": len(self.text_blob),
            "natives": sorted(self.natives_used),
            "unknown_natives": sorted(self.unknown_natives),
            "boundary": {"%s|%s|%s" % k: v for k, v in self.boundary.items()},
            "boundary_detail": self.boundary_detail["boundary"],
            "included_scenes": {str(k): v for k, v in sorted(self.included.items())},
            "unmatched_image_names": sorted(self.unmatched_images),
            "boundary_jumps": self.boundary_jumps,
            "jpeg_quality": self.args.jpeg_quality,
            "pose_names": getattr(self, "wanted_pose_names", []),
            "references": {
                "background_names": getattr(self, "bg_name_list", []),
                "pose_names": getattr(self, "wanted_pose_names", []),
                "gallery_images": [str(item.get("image") or "").lower()
                                   for item in self.gallery_included],
                "scenes": {str(k): [list(r) for r in v] for k, v in sorted(self.included.items())},
            },
            "gallery_total": len(self.src.gallery),
            "gallery_included": len(gal_rows),
            "title_image": title_image,
            "title_images": title_images,
            "title_sources": self.title_sources,
            "title_source": self.title_sources["normal"],
            "chars": self.write_charset(),
        }
        manifest["artifacts"] = [
            {"path": manifest["path"], "bytes": len(blob),
             "sha256": manifest["sha256"]},
            {"path": os.path.relpath(os.path.join(os.path.abspath(self.args.out_dir),
                                                  "anemoi_charset.txt"), REPO_ROOT),
             "bytes": len(self.charset_text.encode("utf-8")) if hasattr(self, "charset_text") else None},
        ]
        with open(os.path.join(self.args.out_dir, "anemoi_pack.json"), "w",
                  encoding="utf-8", newline="\n") as fh:
            json.dump(manifest, fh, ensure_ascii=False, indent=1)
            fh.write("\n")
        return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", default=os.environ.get("ANEMOI_SOURCE", ""), help="anemoi-rw5 快照")
    ap.add_argument("--out-dir", default=None,
                    help="输出目录。默认：正式打包为 main/anemoi_data；测量为 "
                         "build/measure-<UTC>-<config>/（隔离保留，不覆盖正式包）")
    ap.add_argument("--scope", default="common",
                    help="common | common-full | all | book:NNN")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    ap.add_argument("--art-width", type=int, default=320)
    ap.add_argument("--art-height", type=int, default=180)
    ap.add_argument("--jpeg-quality", type=int, default=32)
    ap.add_argument("--layout-scale", type=float, default=0.62,
                    help="立绘版面缩放（源 432x286 版面 -> 屏幕；只缩不放）")
    ap.add_argument("--palette-colors", type=int, default=32,
                    help="立绘调色板颜色数（0 = 沿用源调色板）")
    ap.add_argument("--scope-manifest", default="",
                    help="显式 source-range manifest（替代 --scope 的闭包取景）")
    ap.add_argument("--source-overlay", default="",
                    help="可追溯的 source compatibility overlay JSON")
    ap.add_argument("--measure", action="store_true")
    ap.add_argument("--wide-ids", action="store_true",
                    help="测量专用的 32 位文本 id 变体（全量单包字符串数超过 u16 上限时用）")
    args = ap.parse_args()
    if not args.source or not os.path.isdir(args.source):
        raise SystemExit("需要 --source 指向 anemoi-rw5 快照目录（或用 ANEMOI_SOURCE）")
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    config_tag = "s%03d-pal%d-q%d-%s" % (
        int(round(args.layout_scale * 100)), args.palette_colors, args.jpeg_quality,
        args.scope.replace(":", "-"))
    if args.measure:
        default_out = "build/measure-%s-%s" % (stamp, config_tag)
    else:
        default_out = "main/anemoi_data"
    args.out_dir = args.out_dir or default_out
    common_data = os.path.abspath("main/anemoi_data")
    out_abs = os.path.abspath(args.out_dir)
    if args.measure and (out_abs == common_data or out_abs.startswith(common_data + os.sep)):
        raise SystemExit(
            "拒绝在测量模式下写入正式目录 %s：请用 --out-dir 指定独立目录" % common_data)
    log("输出目录: %s%s" % (out_abs, "（测量模式，不覆盖正式包）" if args.measure else ""))
    global LAYOUT_SCALE, IDC, ID_BYTES
    LAYOUT_SCALE = args.layout_scale
    if args.wide_ids:
        if not args.measure:
            raise SystemExit("--wide-ids 只用于测量（--measure）")
        IDC, ID_BYTES = "I", 4
    src = Source(args.source, args.source_overlay or None)
    scope_info = None
    if args.scope_manifest:
        included, boundary, detail, scope_info = load_scope_manifest(src, args.scope_manifest)
        log("取景: 使用 scope manifest %s" % args.scope_manifest)
    else:
        included, boundary, detail = compute_scope(src, args.scope)
    log("取景: %d 场景 / %d 段" % (len(included), sum(len(v) for v in included.values())))
    packer = Packer(src, args)
    packer.included = included
    packer.boundary = boundary
    packer.boundary_detail = detail
    packer.scope_info = scope_info
    packer.helpers_needed = []
    # 预扫 native / helper 使用面
    for sid, ranges in included.items():
        ops = src.ops(sid)
        for a, b in ranges:
            for i in range(a, b):
                for value in _op_expressions(src.inst(ops, i)):
                    ast = normalize(value)
                    names = set()
                    native_calls(ast, names)
                    for name in names:
                        packer.natives_used.add(name)
                        # 源里同名“native”如果其实是 helpers.js 的脚本函数，
                        # 必须编译脚本函数本体（C 里的同名分支只是兜底，语义不等价）。
                        if packer.helper_for(name, sid) is not None:
                            packer.helpers_needed.append((name, sid))
                        elif name in SUPPORTED_NATIVES:
                            continue
                        else:
                            packer.unknown_natives.add(name)
    packer.compile_all()
    packer.build_tables()
    manifest = packer.write()
    log("包体: %d B (%.2f MiB), ops=%d, scenes=%d" % (
        manifest["bytes"], manifest["bytes"] / 1048576, manifest["ops"], manifest["scenes"]))
    for name, info in manifest["sections"].items():
        log("  %-9s %9d B  count=%d" % (name, info["size"], info["count"]))
    if manifest["unknown_natives"]:
        log("警告: 未实现的 native: %s" % ", ".join(manifest["unknown_natives"]))
    if args.measure:
        measure = {
            "kind": "full-content-encoding-measurement",
            "scope": args.scope,
            "config": {
                "layout_scale": args.layout_scale,
                "palette_colors": args.palette_colors,
                "jpeg_quality": args.jpeg_quality,
                "art_width": args.art_width,
                "art_height": args.art_height,
                "wide_ids": bool(args.wide_ids),
                "text_id_bytes": ID_BYTES,
            },
            "pack_bytes": manifest["bytes"],
            "pack_sha256": manifest["sha256"],
            "sections": manifest["sections"],
            "images": manifest["images"],
            "ops": manifest["ops"],
            "scenes": manifest["scenes"],
            "pose_rows": manifest["pose_rows"],
            "text_bytes": manifest["text_bytes"],
            "title_image": manifest.get("title_image"),
            "gallery_included": manifest.get("gallery_included"),
            "gallery_total": manifest.get("gallery_total"),
            "included_scenes": manifest["included_scenes"],
            "references": manifest["references"],
            "natives": manifest["natives"],
            "unknown_natives": manifest["unknown_natives"],
            "unmatched_image_names": manifest["unmatched_image_names"],
            "source": "xuanyide01/anemoi-rw5",
        }
        with open(os.path.join(args.out_dir, "measure.json"), "w", encoding="utf-8",
                  newline=chr(10)) as fh:
            json.dump(measure, fh, ensure_ascii=False, indent=1)
            fh.write(chr(10))
        log("测量包: %s（%d B, sha256=%s）" % (manifest["path"], manifest["bytes"],
                                            manifest["sha256"]))
        log("测量摘要: %s" % os.path.join(out_abs, "measure.json"))
        return 0
    log("已写出 %s" % manifest["path"])
    return 0


def _op_expressions(op):
    kind = op["op"]
    if kind in ("ifFalse", "ifTrue"):
        return [op.get("condition")]
    if kind == "set":
        return [op.get("value")]
    if kind == "native":
        return [op.get("call")]
    if kind == "portrait":
        return list(op.get("names") or [])
    if kind == "background":
        return [op.get("image")]
    if kind == "dynamicChoice":
        return [op.get("labels"), op.get("enabled"), op.get("target")]
    return []


if __name__ == "__main__":
    raise SystemExit(main())
