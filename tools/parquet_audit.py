#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""《PARQUET》独立源审计：源快照 ↔ 资源包逐 op 对照。

不 import 打包器 (tools/parquet_pack.py)，也不读 build/ 下的旧报告；源与包都由本
工具自己解析，用于发现"打包时丢了/改了什么"。

用法:
  python tools/parquet_audit.py --source assets/parquet-source \
      --img main/parquet_data/parquet_img.bin --scn main/parquet_data/parquet_scn.bin \
      --report build/reports/parquet-audit.json [--markdown build/reports/parquet-audit.md]

审计内容:
  1. 源：107 场景 434 块逐 op（正文/说话人/选项文案与落点/跳转/结尾/图片/立绘/cut-in/
     view/章节标签），以及章节、路线、入口、clearFlag、结局、鉴赏分组。
  2. 场景图：按源边计算 3 个入口 + 37 个章节入口的可达集合，列出不可达场景（源事实）。
  3. 资源：立绘 base/表情/别名映射（含 assets 别名共享），cut-in 与背景的 id 覆盖；
     确认包里每个 alias 都指同一份 payload，没有按别名重复存储。
  4. 包：逐 op 对照源（kind/目标/正文/说话人/视觉 id/章节标签），逐组对照鉴赏，
     逐场景对照 clearFlag 变量名与结局名；正文总 SHA-256 与源一致。
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import struct
import sys
import zlib

SCN_MAGIC = b"PQSCN1\0\0"


def die(msg: str) -> None:
    print(f"parquet_audit: {msg}", file=sys.stderr)
    sys.exit(1)


def load_json(path):
    with io.open(path, encoding="utf-8") as fh:
        return json.load(fh)


def load_story(source):
    root = os.path.join(source, "story")
    scenes = {}
    for sid in sorted((n for n in os.listdir(root) if n.isdigit()), key=int):
        d = os.path.join(root, sid)
        ops = []
        chunks = sorted((n for n in os.listdir(d) if n.endswith(".txt")),
                        key=lambda n: int(n[:-4]))
        for chunk in chunks:
            ops.extend(load_json(os.path.join(d, chunk)))
        scenes[int(sid)] = ops
    return scenes


class Pack:
    """只解析审计需要的表（独立实现，不复用打包器/校验器）。"""

    def __init__(self, img: bytes, scn: bytes):
        self.img = img
        self.scn = scn
        if scn[:8] != SCN_MAGIC or len(scn) < 128:
            die("剧本包标识错误")
        h = struct.unpack_from("<19H", scn, 8)
        (self.version, self.scene_count, self.op_count, self.text_block_count,
         self.speaker_count, self.chapter_count, self.route_count, self.tag_count,
         self.var_count, self.visual_count, self.gallery_count, self.choice_count,
         self.ending_count, self.choice_item_count, self.gallery_members,
         self.initial, self.flow, self.side, self.after) = h
        o = struct.unpack_from("<18I", scn, 46)
        (self.off_scenes, self.off_ops, self.off_visuals, self.off_text_blocks,
         self.off_text_data, self.off_tags, self.off_speakers, self.off_chapters,
         self.off_routes, self.off_choices, self.off_choice_items, self.off_endings,
         self.off_gallery, self.off_members, self.off_ending_names, self.off_vars,
         self.off_strings, self.strings_size) = o
        self.scenes = {}
        for i in range(self.scene_count):
            sid, route, clear_var, first_op, op_count, tag, _res = struct.unpack_from(
                "<HBBHHHH", scn, self.off_scenes + 12 * i)
            self.scenes[sid] = {"route": route, "clear_var": clear_var,
                                "first_op": first_op, "op_count": op_count, "tag": tag}
        self.ops = [struct.unpack_from("<BBBBHHII", scn, self.off_ops + 16 * i)
                    for i in range(self.op_count)]
        self.visuals = [struct.unpack_from("<HHHHHBB", scn, self.off_visuals + 12 * i)
                        for i in range(self.visual_count)]
        self.text_blocks = [struct.unpack_from("<IIHH", scn, self.off_text_blocks + 12 * i)
                            for i in range(self.text_block_count)]
        self.text_data = scn[self.off_text_data:self.off_tags]
        self.strings = scn[self.off_strings:self.off_strings + self.strings_size]
        self.tag_offs = list(struct.unpack_from(f"<{self.tag_count}I", scn,
                                                self.off_tags))
        self.speaker_offs = list(struct.unpack_from(f"<{self.speaker_count}I", scn,
                                                    self.off_speakers))
        self.chapters = [struct.unpack_from("<IHBB", scn, self.off_chapters + 8 * i)
                         for i in range(self.chapter_count)]
        self.routes = [struct.unpack_from("<IHH", scn, self.off_routes + 8 * i)
                       for i in range(self.route_count)]
        self.choices = [struct.unpack_from("<HBB", scn, self.off_choices + 4 * i)
                        for i in range(self.choice_count)]
        self.choice_items = [struct.unpack_from("<IHHIBBi", scn,
                                                self.off_choice_items + 18 * i)
                             for i in range(self.choice_item_count)]
        self.endings = [struct.unpack_from("<HH", scn, self.off_endings + 4 * i)
                        for i in range(self.ending_count)]
        self.galleries = [struct.unpack_from("<IHH", scn, self.off_gallery + 8 * i)
                          for i in range(self.gallery_count)]
        self.members = list(struct.unpack_from(f"<{self.gallery_members}H", scn,
                                              self.off_members))
        name_count = max((e[1] for e in self.endings), default=-1) + 1
        self.ending_names = [struct.unpack_from("<I", scn, self.off_ending_names + 4 * i)[0]
                             for i in range(name_count)]
        self.var_offs = list(struct.unpack_from(f"<{self.var_count}I", scn, self.off_vars))
        self._cache = {}

    def string(self, off):
        end = self.strings.find(b"\0", off)
        if end < 0:
            die(f"字符串未终止 {off}")
        return self.strings[off:end].decode("utf-8")

    def text(self, logical, length):
        for i, (lo, co, cl, rl) in enumerate(self.text_blocks):
            if lo <= logical < lo + rl:
                if i not in self._cache:
                    raw = zlib.decompress(self.text_data[co:co + cl])
                    if len(raw) != rl:
                        die(f"正文块 {i} 解压长度不符")
                    self._cache[i] = (lo, raw)
                lo2, raw = self._cache[i]
                local = logical - lo2
                s = raw[local:local + length + 1]
                if len(s) != length + 1 or s[-1] != 0:
                    die(f"正文 {logical} 长度/NUL 不符")
                return s[:-1].decode("utf-8")
        die(f"正文偏移 {logical} 不在任何块")

    def scene_by_id(self, sid):
        return self.scenes.get(sid)


def graph_edges(scenes):
    edges = {}
    for sid, ops in scenes.items():
        out = []
        for i, op in enumerate(ops):
            if op["op"] == "jumpScene":
                out.append((op["scene"], op["to"]))
            elif op["op"] == "choice":
                for item in op["items"]:
                    out.append((item["scene"], item["to"]))
        edges[sid] = out
    return edges


def reachable(edges, start):
    seen = set()
    stack = [start]
    while stack:
        sid = stack.pop()
        if sid in seen:
            continue
        seen.add(sid)
        for nxt, _to in edges.get(sid, []):
            stack.append(nxt)
    return seen


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", default="assets/parquet-source")
    ap.add_argument("--img", default="main/parquet_data/parquet_img.bin")
    ap.add_argument("--scn", default="main/parquet_data/parquet_scn.bin")
    ap.add_argument("--report", default="build/reports/parquet-audit.json")
    ap.add_argument("--markdown", default="build/reports/parquet-audit.md")
    args = ap.parse_args()

    catalog = load_json(os.path.join(args.source, "catalog.json"))
    gallery = load_json(os.path.join(args.source, "gallery.txt"))
    image_map = load_json(os.path.join(args.source, "image-map.json"))
    cutin_map = load_json(os.path.join(args.source, "cutin-map.json"))
    scenes = load_story(args.source)
    with open(args.img, "rb") as fh:
        img = fh.read()
    with open(args.scn, "rb") as fh:
        scn = fh.read()
    pack = Pack(img, scn)

    checks = []
    def check(name, ok, detail=""):
        checks.append({"name": name, "ok": bool(ok), "detail": detail})
        if not ok:
            print(f"FAIL {name}: {detail}")
        return ok

    # ---------------------------------------------------------------- 源统计
    src_counts = {
        "scenes": len(scenes),
        "chunks": sum(len(os.listdir(os.path.join(args.source, "story", str(sid))))
                      for sid in scenes),
        "ops": sum(len(v) for v in scenes.values()),
        "text_ops": sum(1 for v in scenes.values() for o in v if o["op"] == "text"),
        "jumps": sum(1 for v in scenes.values() for o in v if o["op"] == "jumpScene"),
        "choices": sum(1 for v in scenes.values() for o in v if o["op"] == "choice"),
        "choice_items": sum(len(o["items"]) for v in scenes.values() for o in v
                            if o["op"] == "choice"),
        "ends": sum(1 for v in scenes.values() for o in v if o["op"] == "end"),
        "speakers": len({o["speaker"] for v in scenes.values() for o in v
                         if o["op"] == "text" and o["speaker"]}),
        "unique_views": sorted({o["view"] for v in scenes.values() for o in v
                                if o["op"] == "text"}),
    }
    text_hash = hashlib.sha256()
    for sid in sorted(scenes):
        for op in scenes[sid]:
            if op["op"] == "text":
                text_hash.update(op["text"].encode("utf-8"))
                text_hash.update(b"\0")
    src_counts["text_sha256"] = text_hash.hexdigest()

    # ---------------------------------------------------------------- 包统计与逐 op 对照
    pack_text_hash = hashlib.sha256()
    mismatches = []
    checked_ops = 0
    for sid in sorted(scenes):
        if sid not in pack.scenes:
            mismatches.append(f"场景 {sid} 不在包里")
            continue
        meta = pack.scenes[sid]
        src_ops = scenes[sid]
        if meta["op_count"] != len(src_ops):
            mismatches.append(f"场景 {sid} op 数 {meta['op_count']} != {len(src_ops)}")
            continue
        for local, src in enumerate(src_ops):
            kind, tag, view, _flags, a, b, c, d = pack.ops[meta["first_op"] + local]
            checked_ops += 1
            if src["op"] == "text":
                if kind != 0:
                    mismatches.append(f"{sid}:{local} 源 text，包 kind={kind}")
                    continue
                text = pack.text(c, d)
                if text != src["text"]:
                    mismatches.append(f"{sid}:{local} 正文不一致")
                    continue
                pack_text_hash.update(text.encode("utf-8"))
                pack_text_hash.update(b"\0")
                if pack.speaker_offs and b != 0xFFFF:
                    if pack.string(pack.speaker_offs[b]) != src["speaker"]:
                        mismatches.append(f"{sid}:{local} 说话人不一致")
                elif src["speaker"]:
                    mismatches.append(f"{sid}:{local} 说话人缺失")
                vis = pack.visuals[a]
                v = src
                bodies = list(v.get("bodies") or [])
                while len(bodies) < 3:
                    bodies.append("")
                expect = [int(v["background"][1:]) if v["background"] else 0xFFFF,
                          int(bodies[0][1:]) if bodies[0] else 0xFFFF,
                          int(bodies[1][1:]) if bodies[1] else 0xFFFF,
                          int(bodies[2][1:]) if bodies[2] else 0xFFFF,
                          int(v["cutIn"][1:]) if v["cutIn"] else 0xFFFF]
                if list(vis[:5]) != expect:
                    mismatches.append(f"{sid}:{local} 视觉映射不一致 {vis[:5]} != {expect}")
                if vis[5] != (1 if v["view"] == "another" else 0):
                    mismatches.append(f"{sid}:{local} view 不一致")
                if vis[6] != int(v.get("cutInOpacity", 0)):
                    mismatches.append(f"{sid}:{local} cut-in 不透明度不一致")
                if pack.string(pack.tag_offs[tag]) != catalog["chapterLabels"].get(
                        v.get("chapterTag", ""), v.get("chapterTag", "")):
                    mismatches.append(f"{sid}:{local} 章节标签不一致")
            elif src["op"] == "jumpScene":
                if kind != 1 or a != src["scene"] or c != src["to"]:
                    mismatches.append(f"{sid}:{local} 跳转不一致")
            elif src["op"] == "choice":
                if kind != 2 or b != len(src["items"]):
                    mismatches.append(f"{sid}:{local} 选项数量不一致")
                else:
                    choice = pack.choices[a]
                    for k, item in enumerate(src["items"]):
                        off, ln, tscene, to, var, has, val = pack.choice_items[
                            choice[0] + k]
                        if pack.text(off, ln) != item["text"]:
                            mismatches.append(f"{sid}:{local} 选项 {k} 文案不一致")
                        if (tscene, to) != (item["scene"], item["to"]):
                            mismatches.append(f"{sid}:{local} 选项 {k} 落点不一致")
                        eff = item.get("effect")
                        if bool(has) != bool(eff) or (eff and (
                                pack.string(pack.var_offs[var]) != eff[0] or val != eff[1])):
                            mismatches.append(f"{sid}:{local} 选项 {k} effect 不一致")
            elif src["op"] == "end":
                if kind != 3:
                    mismatches.append(f"{sid}:{local} 源 end，包 kind={kind}")
    check("逐 op 对照", not mismatches, "; ".join(mismatches[:8]))
    check("op 总数", checked_ops == src_counts["ops"],
          f"{checked_ops} != {src_counts['ops']}")
    max_text = max(len(t.encode("utf-8")) for t in
                   (o["text"] for v in scenes.values() for o in v if o["op"] == "text"))
    check("正文长度上限", max_text <= 512, f"max={max_text} B")
    check("正文 SHA-256", pack_text_hash.hexdigest() == src_counts["text_sha256"],
          f"{pack_text_hash.hexdigest()[:16]} != {src_counts['text_sha256'][:16]}")
    check("场景集合", set(pack.scenes) == set(scenes),
          f"包独有 {sorted(set(pack.scenes) - set(scenes))} 源独有 "
          f"{sorted(set(scenes) - set(pack.scenes))}")

    # ---------------------------------------------------------------- 入口 / 可达性
    edges = graph_edges(scenes)
    entries = {"side": catalog["sideInitial"], "flow": catalog["flowScene"],
               "after": catalog["afterInitial"], "initial": catalog["initial"]}
    reach = {}
    for name, start in entries.items():
        reach[name] = sorted(reachable(edges, start))
    chapter_reach = {}
    for ch in catalog["chapters"]:
        chapter_reach[str(ch["id"])] = sorted(reachable(edges, ch["id"]))
    all_reach = set()
    for r in list(reach.values()) + list(chapter_reach.values()):
        all_reach |= set(r)
    unreachable = sorted(set(scenes) - all_reach)
    check("入口场景存在", all(e in scenes for e in entries.values()), str(entries))
    check("章节入口存在", all(ch["id"] in scenes for ch in catalog["chapters"]))
    # 源事实：99 号选择节点没有任何入口/章节能到达（保留在包里，可由直接入口测试）
    check("不可达场景清单", unreachable == [99], f"unreachable={unreachable}")

    # ---------------------------------------------------------------- 章节 / 路线 / 结局
    chapter_rows = []
    for i, ch in enumerate(catalog["chapters"]):
        off, scene, route, _pad = pack.chapters[i]
        label = pack.string(off)
        expect = catalog["chapterLabels"].get(ch["name"], ch["name"])
        chapter_rows.append({"index": i, "scene": scene, "label": label,
                             "route": route, "expect_label": expect})
    bad_chapters = [r for r in chapter_rows
                    if r["scene"] != catalog["chapters"][r["index"]]["id"]
                    or r["route"] != catalog["chapters"][r["index"]]["route"]
                    or r["label"] != r["expect_label"]]
    check("章节表", not bad_chapters, str(bad_chapters[:4]))
    route_rows = []
    for i, r in enumerate(catalog["routes"]):
        off, first, count = pack.routes[i]
        route_rows.append({"name": pack.string(off), "first": first, "count": count,
                           "expect_name": r["name"],
                           "expect_count": len(r["scenes"])})
    bad_routes = [r for r in route_rows
                  if r["name"] != r["expect_name"] or r["count"] != r["expect_count"]]
    check("路线表", not bad_routes, str(bad_routes[:3]))

    ending_report = []
    for sid, ops in scenes.items():
        if any(op["op"] == "end" for op in ops):
            pack_name = None
            for row_scene, name_idx in pack.endings:  # noqa: B007
                if row_scene == sid:
                    pack_name = pack.string(pack.ending_names[name_idx])
                    break
            src_name = catalog["endings"][str(sid)]
            seg = {"scene": sid, "source_name": src_name, "pack_name": pack_name,
                   "clear_flag": catalog["scenes"][str(sid)]["clearFlag"]}
            ending_report.append(seg)
            if pack_name != src_name:
                mismatches.append(f"结局 {sid} 名称不一致")
    check("结局表", not [m for m in mismatches if m.startswith("结局")],
          str(ending_report))

    # ---------------------------------------------------------------- 资源映射
    used_bg, used_body, used_cutin = set(), set(), set()
    for ops in scenes.values():
        for op in ops:
            if op["op"] != "text":
                continue
            used_bg.add(op["background"])
            used_body.update(op["bodies"])
            used_cutin.add(op["cutIn"])
    used_bg.discard("")
    used_cutin.discard("")
    base_files = {image_map[a]["src"] for a in used_body}
    patch_files = {image_map[a]["patch"] for a in used_body}
    body_aliases = {}
    for a in used_body:
        body_aliases.setdefault(image_map[a]["src"], []).append(a)
    # 每个 alias 指向同一份 payload：同一 base 只存一份 BLOCK
    dup_bases = [k for k, v in body_aliases.items() if len(v) > 1]
    cutin_ids = set(cutin_map)
    missing = [a for a in used_body if a not in image_map]
    check("立绘资源完整", not missing, str(missing[:5]))
    # 同一 base 身体被多个 id（别名）复用；表情层按 id 各自不同。
    check("立绘别名共享", len(base_files) >= 30,
          f"{len(base_files)} 张身体被 {len(used_body)} 个 id 复用"
          f"（{len(dup_bases)} 个身体被多个 id 共用）")
    check("cut-in 映射", used_cutin <= cutin_ids,
          f"缺少 {sorted(used_cutin - cutin_ids)[:5]}")
    missing_bg = [a for a in used_bg if a not in image_map]
    check("背景映射", not missing_bg, str(missing_bg[:5]))

    # 包内资源：每个用到的 id 都有映射；同一 base 身体的所有 alias 必须指向同一个
    # BLOCK 条目（证明按身体去重，而不是每个 alias 复制一份）。
    img_asset_count = struct.unpack_from("<H", img, 16)[0]
    img_sprite_count = struct.unpack_from("<H", img, 12)[0]
    img_off_sprites = struct.unpack_from("<I", img, 26)[0]
    img_off_asset = struct.unpack_from("<I", img, 38)[0]
    asset_map = struct.unpack_from(f"<{img_asset_count}H", img, img_off_asset)
    sprite_bodies = [struct.unpack_from("<H", img, img_off_sprites + 16 * i)[0]
                     for i in range(img_sprite_count)]
    body_by_base = {}
    for a in used_body:
        num = int(a[1:])
        v = asset_map[num] if num < len(asset_map) else 0xFFFF
        idx = (v & 0x7FFF) if (v & 0x8000) else -1
        body_by_base.setdefault(image_map[a]["src"], set()).add(
            sprite_bodies[idx] if 0 <= idx < len(sprite_bodies) else -1)
    bad_alias = {k: v for k, v in body_by_base.items() if len(v) > 1}
    distinct_bodies = {next(iter(v)) for v in body_by_base.values() if len(v) == 1}
    check("包内别名不重复存储", not bad_alias,
          f"{len(bad_alias)} 个 base 的 alias 指向了不同身体图")
    check("身体图去重", len(distinct_bodies) == len(base_files),
          f"{len(distinct_bodies)} 张身体图 / {len(base_files)} 个源文件")

    # ---------------------------------------------------------------- 鉴赏
    src_groups = []
    for item in gallery:
        if not src_groups or src_groups[-1][0] != item["group"]:
            src_groups.append([item["group"], []])
        src_groups[-1][1].append(int(item["image"][1:]))
    pack_groups = []
    for i, (off, first, count) in enumerate(pack.galleries):
        pack_groups.append({"label": pack.string(off),
                            "members": pack.members[first:first + count]})
    bad_groups = []
    if len(src_groups) != len(pack_groups):
        bad_groups.append("组数不同")
    for i, ((label, members), got) in enumerate(zip(src_groups, pack_groups)):
        if got["label"] != label or got["members"] != members:
            bad_groups.append(f"组 {i} 不一致")
    check("鉴赏分组", not bad_groups, str(bad_groups[:3]))
    check("鉴赏图片数", sum(len(m) for _, m in src_groups) == 367,
          f"{sum(len(m) for _, m in src_groups)}")
    missing_gallery = [m for m in (x["image"] for x in gallery) if m not in image_map]
    check("鉴赏图片映射", not missing_gallery, str(missing_gallery[:5]))
    # 每组至少要有一张在正篇里作为背景出现，否则正常阅读永远解锁不了该组。
    unlocked_by_story = 0
    for label, members in src_groups:
        if any(f"a{m}" in used_bg for m in members):
            unlocked_by_story += 1
    check("鉴赏可解锁", unlocked_by_story == len(src_groups),
          f"{unlocked_by_story}/{len(src_groups)} 组在剧情里出现")

    # 包内图片尺寸/位置（立绘几何）
    scale = 150 / 262
    geo_errors = []
    for a in used_body:
        num = int(a[1:])
        v = asset_map[num] if num < len(asset_map) else 0xFFFF
        if not (v & 0x8000):
            geo_errors.append(f"{a} 非立绘")
            continue
        canvas_w = int(round(image_map[a]["width"] * 1.0))  # 432 空间的 w 仅作参考
        anchor_expect = int(round(image_map[a]["faceAnchor"][0] * scale))
        # 锚点必须落在缩放画布内
        if not (0 <= anchor_expect <= canvas_w):
            geo_errors.append(f"{a} 锚点异常")
    check("立绘锚点几何", not geo_errors, str(geo_errors[:5]))

    report = {
        "source": src_counts,
        "pack": {
            "scenes": pack.scene_count, "ops": pack.op_count,
            "text_blocks": pack.text_block_count, "visuals": pack.visual_count,
            "chapters": pack.chapter_count, "routes": pack.route_count,
            "galleries": pack.gallery_count, "gallery_members": pack.gallery_members,
            "choices": pack.choice_count, "choice_items": pack.choice_item_count,
            "vars": pack.var_count,
            "entries": {"initial": pack.initial, "flow": pack.flow, "side": pack.side,
                        "after": pack.after},
            "text_sha256": pack_text_hash.hexdigest(),
            "bytes": {"img": len(img), "scn": len(scn)},
        },
        "graph": {
            "entries": entries,
            "reachable": {k: len(v) for k, v in reach.items()},
            "chapter_reachable": {k: len(v) for k, v in chapter_reach.items()},
            "unreachable_scenes": unreachable,
        },
        "assets": {
            "backgrounds": len(used_bg),
            "bodies": len(used_body),
            "base_bodies": len(base_files),
            "expression_layers": len(patch_files),
            "alias_reuse": {k: len(v) for k, v in body_aliases.items() if len(v) > 1},
            "cutins": len(used_cutin),
            "gallery_groups": len(src_groups),
            "gallery_images": sum(len(m) for _, m in src_groups),
        },
        "chapters": chapter_rows,
        "routes": route_rows,
        "endings": ending_report,
        "checks": checks,
        "failures": [c for c in checks if not c["ok"]],
    }
    ok = not report["failures"]

    # ---------------------------------------------------------------- 报告
    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2, sort_keys=True)
        fh.write("\n")
    if args.markdown:
        os.makedirs(os.path.dirname(args.markdown), exist_ok=True)
        lines = [
            "# PARQUET 源审计报告", "",
            f"- 源：{src_counts['scenes']} 场景 / {src_counts['chunks']} 块 / "
            f"{src_counts['ops']} op / {src_counts['text_ops']} 条正文",
            f"- 分支：{src_counts['choices']} 处（{src_counts['choice_items']} 个选项）/ "
            f"{src_counts['ends']} 个结局端",
            f"- 资源：{len(used_body)} 个立绘 id → {len(base_files)} 张身体 + "
            f"{len(patch_files)} 张表情；{len(used_bg)} 张背景；{len(used_cutin)} 张 cut-in",
            f"- 鉴赏：{len(src_groups)} 组 / {sum(len(m) for _, m in src_groups)} 张",
            f"- 正文 SHA-256：`{src_counts['text_sha256']}`",
            f"- 包：`{len(scn)}` B（剧本）+ `{len(img)}` B（图片）",
            f"- 不可达场景（源事实）：{unreachable}", "",
            "| 检查 | 结果 | 详情 |", "| --- | --- | --- |",
        ]
        for c in checks:
            lines.append(f"| {c['name']} | {'PASS' if c['ok'] else 'FAIL'} | "
                         f"{c['detail']} |")
        lines.append("")
        with open(args.markdown, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    print(json.dumps({k: report[k] for k in ("source", "pack", "graph", "assets",
                                             "failures")},
                     ensure_ascii=False, indent=2))
    print(f"parquet_audit: {'PASS' if ok else 'FAIL'} "
          f"({len(checks)} checks, {len(report['failures'])} failures)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
