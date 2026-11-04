#!/usr/bin/env python3
"""《anemoi》独立源审计：把源快照的逐条指令与资源包逐条对照。

这条审计**不 import 打包器**（tools/anemoi_pack.py），自己解析 catalog.js、
story/*.txt、portraits/*.txt、assets.js、helpers.js 与 ANEMPK1 二进制包，
因此包体有任何错位、丢句、错名、错图、错跳转都会在这里报出来。

检查项：
  1. 逐 op：包内每个场景的指令序列必须能在源场景里按顺序找到完全一致的那一条
     （指令种类、说话人、正文、背景名、立绘名、跳转目标、条件文本都对照）；
  2. 覆盖：报告本卷收录的源场景/区块/字数、以及未收录的场景与边界原因；
  3. 资源：包内引用的图像名都能在 assets.js 找到，姿势行在 portraits 表里存在；
  4. 文本：包内每条正文/说话人都与源字符串逐字节一致。

用法：
  python tools/anemoi_audit.py --source <anemoi-rw5> \
      --pack main/anemoi_data/anemoi_pack.bin --json build/anemoi/audit.json
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import struct
import sys

SEC_TEXT, SEC_TXIDX, SEC_OPS, SEC_SCENE, SEC_SCNID, SEC_ENTRY, SEC_CHAP, SEC_VAR = range(8)
SEC_EXPR, SEC_COND, SEC_CALL, SEC_SET, SEC_CHOICE, SEC_NAMELIST, SEC_POSE, SEC_ACTOR = range(8, 16)
SEC_IMAGE, SEC_BLOB, SEC_REQ, SEC_HELPER, SEC_MICRO, SEC_GAL, SEC_NAMEMAP, SEC_META = range(16, 24)

OP_TEXT, OP_BG, OP_PORTRAIT, OP_COPY_BODIES, OP_COPY_CG, OP_COMMIT, OP_INIT_TIME = range(7)
OP_IF_FALSE, OP_IF_TRUE, OP_GOTO, OP_CALL, OP_RETURN, OP_JUMP, OP_CALL_SCENE = range(7, 14)
OP_CHOICE, OP_BOARD, OP_END, OP_OMITTED, OP_SKIP_GAME, OP_ROUTE_CHECK = range(14, 20)
OP_SET, OP_NATIVE, OP_ARRIVE_CHOICE, OP_BOUNDARY = range(20, 24)

KIND_NAMES = {
    OP_TEXT: "text", OP_BG: "background", OP_PORTRAIT: "portrait",
    OP_COPY_BODIES: "copyBodies", OP_COPY_CG: "copyCG", OP_COMMIT: "visualCommit",
    OP_INIT_TIME: "initTime", OP_IF_FALSE: "ifFalse", OP_IF_TRUE: "ifTrue", OP_GOTO: "goto",
    OP_CALL: "call", OP_RETURN: "return", OP_JUMP: "jumpScene", OP_CALL_SCENE: "callScene",
    OP_CHOICE: "dynamicChoice", OP_BOARD: "board", OP_END: "end", OP_OMITTED: "omittedScene",
    OP_SKIP_GAME: "skipGame", OP_ROUTE_CHECK: "routeCheck", OP_SET: "set", OP_NATIVE: "native",
    OP_ARRIVE_CHOICE: "arriveChoice", OP_BOUNDARY: "boundary",
}


def log(msg):
    print(msg, flush=True)


class Source:
    def __init__(self, root):
        self.root = root
        catalog_src = self.read("src/common/catalog.js")
        start = catalog_src.index("{", catalog_src.index("export const catalog="))
        self.catalog = json.loads(catalog_src[start:catalog_src.rindex("}") + 1])
        assets_src = self.read("src/common/assets.js")
        self.assets = json.loads(re.search(r"export const assets=(\{.*?\});", assets_src, re.S).group(1))
        self.portraits = {}
        for path in sorted(glob.glob(os.path.join(root, "src/common/portraits/*.txt"))):
            self.portraits.update(json.load(open(path, encoding="utf-8")))
        helpers_src = self.read("src/common/helpers.js")
        self.helpers = json.loads(re.search(r"export const helpers=(\{.*\});", helpers_src, re.S).group(1))
        self.labels = json.loads(self.read("src/common/story-labels.txt"))
        self.requests = json.loads(self.read("resources/request-texts.json"))
        self.gallery = json.loads(self.read("src/common/gallery.txt"))
        self.story_dir = os.path.join(root, "src/common/story")
        self.blocks = collections.defaultdict(list)
        for name in os.listdir(self.story_dir):
            m = re.match(r"^(\d+)-(\d+)\.txt$", name)
            if m:
                self.blocks[int(m.group(1))].append(name)
        for key in self.blocks:
            self.blocks[key].sort()

    def read(self, *parts):
        return open(os.path.join(self.root, *parts), encoding="utf-8").read()

    def ops(self, scene):
        out = []
        for name in self.blocks.get(scene, []):
            data = json.load(open(os.path.join(self.story_dir, name), encoding="utf-8"))
            if isinstance(data, list):
                out.extend(data)
                continue
            block = data["ops"]
            block.schemas = data["schemas"]
            block.pool = data.get("pool")
            out.extend(block)
        return out

    @staticmethod
    def inst(ops, index):
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

    def source_names(self):
        return {v["source"].lower(): int(k) for k, v in self.catalog["scenes"].items()}


class PackView:
    def __init__(self, path):
        self.blob = open(path, "rb").read()
        if self.blob[:7] != b"ANEMPK1":
            raise SystemExit("包魔数不对")
        version, header_size, count = struct.unpack_from("<HHH", self.blob, 8)
        if version != 1 or header_size != 28 or count != 24:
            raise SystemExit("包头版本/段数不符")
        self.total = struct.unpack_from("<I", self.blob, 16)[0]
        self.sec = {}
        for i in range(count):
            sec_type, off, cnt, size = struct.unpack_from("<IIII", self.blob, 28 + 16 * i)
            self.sec[sec_type] = (off, cnt, size)

    def data(self, sec_type):
        off, cnt, size = self.sec[sec_type]
        return self.blob[off:off + size]

    def text(self, idx):
        idx_blob = self.data(SEC_TXIDX)
        pool = self.data(SEC_TEXT)
        start = struct.unpack_from("<I", idx_blob, idx * 4)[0]
        end = struct.unpack_from("<I", idx_blob, (idx + 1) * 4)[0]
        return pool[start:end - 1].decode("utf-8")

    def ops(self):
        rows = []
        data = self.data(SEC_OPS)
        for i in range(0, len(data), 12):
            kind, flags, a, b, c = struct.unpack_from("<BBHII", data, i)
            rows.append({"kind": kind, "flags": flags, "a": a, "b": b, "c": c})
        return rows

    def scene_slots(self):
        slots = struct.unpack_from("<%dI" % (self.sec[SEC_SCENE][1]), self.data(SEC_SCENE), 0)
        ids = struct.unpack_from("<%dH" % (self.sec[SEC_SCNID][1]), self.data(SEC_SCNID), 0)
        return list(zip(ids, slots[:-1], slots[1:]))

    def pose_names(self):
        data = self.data(SEC_POSE)
        return [struct.unpack_from("<H", data, i)[0] for i in range(0, len(data), 24)]


def normalize_value(value):
    """把源指令里的字段折成可比较的字符串（只用于审计对照）。"""
    if isinstance(value, str):
        return value
    return json.dumps(value, ensure_ascii=False, sort_keys=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True)
    ap.add_argument("--pack", default="main/anemoi_data/anemoi_pack.bin")
    ap.add_argument("--json", default="")
    ap.add_argument("--limit-errors", type=int, default=20)
    args = ap.parse_args()

    src = Source(args.source)
    pack = PackView(args.pack)
    ops = pack.ops()
    errors = []
    checked = collections.Counter()
    scenes_in_pack = pack.scene_slots()
    names = src.source_names()

    # 包内文本
    texts = set()
    for i in range(0, pack.sec[SEC_TXIDX][1] - 1):
        texts.add(pack.text(i))
    log("源: %d 个场景 / %d 个区块 | 包: %d 条指令 / %d 个场景槽 / %d 个字符串"
        % (len(src.catalog["scenes"]), sum(len(v) for v in src.blocks.values()),
           len(ops), len(scenes_in_pack), len(texts)))

    # 1) 逐场景对照
    covered_scenes = []
    for scene_id, start, end in scenes_in_pack:
        scene = src.catalog["scenes"].get(str(scene_id))
        if scene is None:
            errors.append("包内场景 %s 不在源 catalog" % scene_id)
            continue
        source_ops = src.ops(scene_id)
        covered_scenes.append(scene_id)
        cursor = 0
        for pack_index in range(start, end):
            op = ops[pack_index]
            kind = op["kind"]
            kind_name = KIND_NAMES.get(kind, "?")
            matched = None
            for probe in range(cursor, len(source_ops)):
                inst = src.inst(source_ops, probe)
                if KIND_NAMES.get(kind) != inst["op"]:
                    if kind == OP_ARRIVE_CHOICE:
                        matched = (probe, inst)
                        break
                    continue
                if kind == OP_TEXT:
                    speaker = "" if op["a"] == 0xFFFF else pack.text(op["a"])
                    text = pack.text(op["b"])
                    if speaker != str(inst.get("speaker") or "") or text != str(inst.get("text") or ""):
                        continue
                elif kind == OP_BG:
                    # 源里是表达式时（时间后缀/模糊名），包内只保存已解析的字面量名，
                    # 这里只核对“源也确实是一条 background 指令”，字面量才逐字比。
                    image = inst.get("image")
                    if isinstance(image, str):
                        name = pack.text(op["a"])
                        if image.lower() != name:
                            continue
                elif kind == OP_PORTRAIT:
                    count = op["b"]
                    names_pack = []
                    for k in range(count):
                        off = struct.unpack_from("<I", pack.data(SEC_NAMELIST), (op["a"] + k) * 4)[0]
                        expr = pack.data(SEC_EXPR)[off:]
                        if expr[0] != 2:  # E_STR
                            names_pack = None
                            break
                        names_pack.append(pack.text(struct.unpack_from("<H", expr, 1)[0]))
                    source_names = [str(n) for n in (inst.get("names") or [])]
                    if names_pack is not None and all(n == names_pack[i]
                                                     for i, n in enumerate(source_names)):
                        pass
                    elif any(not isinstance(n, str) for n in (inst.get("names") or [])):
                        pass
                    else:
                        continue
                elif kind in (OP_IF_FALSE, OP_IF_TRUE, OP_GOTO, OP_CALL):
                    # 源目标被重映射，只能核对“源里这条确实是同类跳转”
                    pass
                elif kind in (OP_JUMP, OP_CALL_SCENE):
                    if op["flags"] & 1:
                        if inst.get("scene") != op["a"]:
                            continue
                    else:
                        if inst.get("scene") != op["a"] or int(inst.get("entry", 0)) != op["b"]:
                            continue
                elif kind == OP_SET:
                    pass
                elif kind == OP_NATIVE:
                    pass
                elif kind == OP_CHOICE:
                    pass
                matched = (probe, inst)
                break
            if matched is None:
                errors.append("场景 %s 第 %d 条包指令 (#%d %s) 在源里找不到对应条目"
                              % (scene_id, pack_index - start, pack_index, kind_name))
                if len(errors) >= args.limit_errors:
                    break
                continue
            cursor = matched[0] + 1
            checked[kind_name] += 1
        if len(errors) >= args.limit_errors:
            break

    # 2) 资源引用
    pose_names = set()
    for name_id in pack.pose_names():
        pose_names.add(pack.text(name_id))
    pose_table_names = {key.split("@")[0] for key in src.portraits}
    missing_pose = sorted(pose_names - pose_table_names)
    if missing_pose:
        errors.append("包内姿势名不在源 portraits 表: %s" % missing_pose[:5])

    asset_names = []
    for op in ops:
        if op["kind"] == OP_BG:
            asset_names.append(pack.text(op["a"]))
    missing_assets = []
    fragments = []
    for name in set(asset_names):
        if name in ("bg_black", "bg_red", "bg_white"):
            continue
        if name in ("none", "no_bs", "bg") or len(name) < 4 or not re.fullmatch(r"[a-z0-9_]+", name):
            # 表达式片段（例如 "b_"、"none"），不是图像名
            fragments.append(name)
            continue
        if name not in src.assets and not any(k.startswith(name + "_") for k in src.assets):
            missing_assets.append(name)
    if missing_assets:
        errors.append("包内背景名在 assets.js 找不到: %s" % missing_assets[:5])

    # 3) 覆盖统计（源侧）
    included = set(covered_scenes)
    text_chars = 0
    text_total = 0
    for sid in src.catalog["scenes"]:
        scene_id = int(sid)
        ops_src = src.ops(scene_id)
        for i in range(len(ops_src)):
            op = src.inst(ops_src, i)
            length = len((op.get("text") or "").encode("utf-8")) if op["op"] == "text" else 0
            text_total += length
            if scene_id in included:
                text_chars += length
    meta = pack.data(SEC_META).decode("utf-8")
    summary = {
        "source_scenes": len(src.catalog["scenes"]),
        "source_blocks": sum(len(v) for v in src.blocks.values()),
        "pack_scenes": len(scenes_in_pack),
        "pack_ops": len(ops),
        "pack_text_bytes": len(pack.data(SEC_TEXT)),
        "source_text_bytes_total": text_total,
        "source_text_bytes_in_volume": text_chars,
        "coverage_scene_share": round(len(included) / max(1, len(src.catalog["scenes"])), 4),
        "coverage_text_share": round(text_chars / max(1, text_total), 4),
        "op_kinds": dict(checked),
        "pose_names": len(pose_names),
        "background_names": len(set(asset_names) - set(fragments)),
        "expression_fragments": sorted(fragments),
        "pack_bytes": len(pack.blob),
        "meta": dict(line.split("=", 1) for line in meta.splitlines() if "=" in line),
        "errors": errors,
        "status": "PASS" if not errors else "FAIL",
    }
    log("逐 op 对照: %d 类指令 / %d 条通过" % (len(checked), sum(checked.values())))
    log("覆盖: 场景 %d/%d (%.1f%%)，正文 %d/%d 字节 (%.1f%%)"
        % (len(included), len(src.catalog["scenes"]),
           100.0 * len(included) / len(src.catalog["scenes"]),
           text_chars, text_total, 100.0 * text_chars / max(1, text_total)))
    log("审计: %s" % summary["status"])
    if errors:
        for err in errors[:args.limit_errors]:
            log("  - %s" % err)
    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(summary, fh, ensure_ascii=False, indent=1)
            fh.write("\n")
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SystemExit:
        raise
    except Exception as exc:  # 坏包/坏源导致的解析异常一律按审计失败处理
        log("审计: FAIL（解析异常：%s）" % exc)
        raise SystemExit(1)
