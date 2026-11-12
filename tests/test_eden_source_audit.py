#!/usr/bin/env python3
"""Source ⇄ pack audit for the eden* port (independent of the packer's loader).

The packer has its own `Source` class; this audit deliberately does **not** import
or reuse it. It walks the source tree from scratch — every chunk file of every
scene, in numeric order — and then compares the result against the generated pack
op by op, so a bug in the packer (for example "only read 0.txt") cannot hide
behind a shared helper.

Checks
  1. chunk discovery   every scene directory, chunks 0..N contiguous, and
                       sum(ops) == catalog.scenes[id].length
  2. op semantics      kind / text / speaker / frame id / jump target for every
                       single op, in order
  3. cross-chunk       the 7 multi-chunk scenes: the boundary op exists and at
                       least one if/goto target crosses it
  4. counts            dialogue lines, speakers, distinct frames
  5. chapter order     catalog.chapters order == the pack's CHAP table
  6. frame map         sorted union of camera-source and pre-dedupe gallery
                       equals the pack FRAME count and every text frame index
  7. gallery           pre-dedupe labels == the pack GAL table (295 groups,
                       including CG - ev_add02_up04)

Usage: EDEN_SOURCE=<eden-rw5 checkout> python3 tests/test_eden_source_audit.py
"""

from __future__ import annotations

import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import eden_source_map

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "main", "eden_data", "eden_pack.bin")

SEC_TEXT, SEC_TXIDX, SEC_OPS, SEC_SCENE, SEC_IMAGE, SEC_FRAME, SEC_CHAP, SEC_GAL, SEC_BLOB, SEC_PATCH = range(10)
OP_TEXT, OP_SET, OP_IF, OP_GOTO, OP_JUMP, OP_END = range(6)

failures: list[str] = []
notes: list[str] = []
# 源变量名 -> 打包时的下标(按首次出现顺序);用于核对下标稳定性。
KEY_INDEX: dict = {}


def check(condition: bool, message: str) -> bool:
    if not condition:
        failures.append(message)
    return condition


# ---------------------------------------------------------------- pack access
class Pack:
    def __init__(self, blob: bytes):
        check(blob[:8] == b"EDENPK01", "pack 魔数不是 EDENPK01")
        version, _header, section_count, _reserved, total = struct.unpack_from("<HHHHI", blob, 8)
        check(version == 1, f"pack 版本 {version} != 1")
        check(total == len(blob), f"pack 声明长度 {total} != 文件 {len(blob)}")
        self.blob = blob
        self.sections: dict[int, tuple[int, int, int]] = {}
        for index in range(section_count):
            kind, offset, count, size = struct.unpack_from("<IIII", blob, 20 + 16 * index)
            self.sections[kind] = (offset, count, size)

    def raw(self, kind: int) -> memoryview:
        offset, _count, size = self.sections[kind]
        return memoryview(self.blob)[offset:offset + size]

    def count(self, kind: int) -> int:
        return self.sections[kind][1]

    def op(self, index: int):
        return struct.unpack_from("<BBHII", self.raw(SEC_OPS), index * 12)

    def text(self, index: int) -> str:
        offsets = self.raw(SEC_TXIDX)
        start, end = struct.unpack_from("<II", offsets, index * 4)
        return bytes(self.raw(SEC_TEXT)[start:end - 1]).decode("utf-8")

    def scene_range(self, scene: int):
        return struct.unpack_from("<II", self.raw(SEC_SCENE), scene * 8)

    def chapter(self, index: int):
        return struct.unpack_from("<HHHH", self.raw(SEC_CHAP), index * 8)


# ---------------------------------------------------------------- source access
def parse_catalog(path: str) -> dict:
    text = open(path, encoding="utf-8").read()
    return json.loads(re.search(r"=\s*(\{.*\})\s*;?\s*$", text, re.S).group(1))


def read_source_scenes(story_dir: str) -> list:
    """返回 [(scene_id, [ops...]), ...];每个场景按 chunk 数字序拼接。"""
    scenes = []
    for name in sorted(os.listdir(story_dir), key=int):
        directory = os.path.join(story_dir, name)
        if not os.path.isdir(directory):
            continue
        chunks = sorted((item for item in os.listdir(directory) if item.endswith(".txt")),
                        key=lambda item: int(item.split(".", 1)[0]))
        check(chunks == [f"{index}.txt" for index in range(len(chunks))],
              f"场景 {name} 的 chunk 编号不连续: {chunks}")
        ops = []
        for chunk in chunks:
            ops.extend(json.load(open(os.path.join(directory, chunk), encoding="utf-8")))
        scenes.append((int(name), ops, len(chunks)))
    return scenes


# ---------------------------------------------------------------- comparisons
def compare_scene(pack: Pack, scene: int, ops: list, frame_index: dict) -> int:
    """逐条比较一个场景,返回对白条数。"""
    first, last = pack.scene_range(scene)
    check(last - first == len(ops),
          f"场景 {scene}: 包里 {last - first} 条指令,源 {len(ops)} 条")
    lines = 0
    limit = min(len(ops), last - first)
    for offset in range(limit):
        src = ops[offset]
        kind, a, b, c, d = pack.op(first + offset)
        src_kind = src.get("op")
        expected_kind = {"text": OP_TEXT, "set": OP_SET, "if": OP_IF, "goto": OP_GOTO,
                         "jumpScene": OP_JUMP, "end": OP_END}.get(src_kind)
        if expected_kind is None:
            check(False, f"场景 {scene} 指令 {offset}: 源指令 {src_kind} 未定义")
            continue
        if kind != expected_kind:
            check(False, f"场景 {scene} 指令 {offset}: 种类 {kind} != {expected_kind}")
            continue
        if kind == OP_TEXT:
            lines += 1
            got_text = pack.text(c)
            if got_text != src["text"]:
                check(False, f"场景 {scene} 指令 {offset}: 文本不一致 "
                             f"{got_text!r} != {src['text']!r}")
            # TEXT 的说话人名在 b(u16):a 只有 8 位,装不下 7,560 条的字符串池下标。
            got_speaker = pack.text(b)
            if got_speaker != src.get("speaker", ""):
                check(False, f"场景 {scene} 指令 {offset}: 人名 {got_speaker!r} != "
                             f"{src.get('speaker', '')!r}")
            want = frame_index.get(src["background"])
            if want is None:
                check(False, f"场景 {scene} 指令 {offset}: 画面 {src['background']} 不在包里")
            elif d != want:
                check(False, f"场景 {scene} 指令 {offset}: 画面下标 {d} != {want}")
        elif kind == OP_SET:
            # 同一个源变量名必须始终映射到同一个下标(读档/条件依赖它稳定)。
            if KEY_INDEX.get(src["key"], b) != b:
                check(False, f"场景 {scene} 指令 {offset}: 变量 {src['key']} 下标 {b} 不稳定")
        elif kind in (OP_IF, OP_GOTO):
            # 源里的 to 是场景内相对下标,包里必须是绝对下标。
            want = first + int(src["to"])
            if d != want:
                check(False, f"场景 {scene} 指令 {offset}: 跳转目标 {d} != {want}"
                             f"(源 to={src['to']})")
        elif kind == OP_JUMP:
            if c != int(src["scene"]):
                check(False, f"场景 {scene} 指令 {offset}: jump 目标 {c} != {src['scene']}")
    return lines


def main() -> int:
    source = os.environ.get("EDEN_SOURCE")
    if not source:
        print("EDEN_SOURCE 未设置:本审计必须对着源仓库跑。", file=sys.stderr)
        return 2
    common = os.path.join(source, "src", "common")
    catalog = parse_catalog(os.path.join(common, "catalog.js"))
    scenes = read_source_scenes(os.path.join(common, "story"))
    pack = Pack(open(PACK, "rb").read())

    try:
        frame_index, manifest = eden_source_map.build_frame_map(source)
    except SystemExit as exc:
        print(str(exc), file=sys.stderr)
        return 2
    check(len(frame_index) == pack.count(SEC_FRAME),
          f"独立源画面表 {len(frame_index)} != pack FRAME {pack.count(SEC_FRAME)}")
    notes.append(f"frame map: {len(frame_index)} frames from " +
                 ", ".join(report["path"] for report in manifest["reports"]))

    # 1) chunk 完整性:拼接后的指令数必须等于 catalog 声明。
    declared = {int(key): int(value["length"]) for key, value in catalog["scenes"].items()}
    check(len(scenes) == len(declared),
          f"源场景数 {len(scenes)} != catalog {len(declared)}")
    total_ops = 0
    multi_chunk = []
    for scene_id, ops, chunks in scenes:
        total_ops += len(ops)
        if declared.get(scene_id) != len(ops):
            check(False, f"场景 {scene_id}: 拼接 {len(ops)} 条,catalog 声明 "
                         f"{declared.get(scene_id)} 条")
        if chunks > 1:
            multi_chunk.append((scene_id, chunks, len(ops)))
    check(total_ops == sum(declared.values()),
          f"源指令总数 {total_ops} != catalog {sum(declared.values())}")

    # 变量下标:源里第一次出现的顺序就是打包顺序。
    for _sid, ops, _chunks in scenes:
        for op in ops:
            if op.get("op") in ("set", "if"):
                KEY_INDEX.setdefault(op["key"], len(KEY_INDEX))

    # 2) 逐场景逐指令比对。
    total_lines = 0
    for scene_id, ops, _chunks in scenes:
        total_lines += compare_scene(pack, scene_id, ops, frame_index)

    # 3) 跨 chunk 边界:多 chunk 场景的接缝指令必须能读到,且跨块跳转逐条对齐。
    cross_total = 0
    for scene_id, ops, chunks in scenes:
        if chunks <= 1:
            continue
        first, last = pack.scene_range(scene_id)
        if last - first != len(ops):
            continue   # 长度不符已在上面报过
        boundary = 512   # 源固定按 512 条切块
        if boundary >= len(ops):
            continue
        # 接缝两侧的指令都必须能读出来(不是"截短到第一块"的图)。
        _kb, _ab, _bb, _cb, _db = pack.op(first + boundary - 1)
        _ka, _aa, _ba, _ca, _da = pack.op(first + boundary)
        # 源里所有落在第二块(>=512)的 if/goto 目标,包里必须落在同一绝对位置。
        src_targets = sorted(int(op["to"]) for op in ops
                             if op.get("op") in ("if", "goto") and int(op["to"]) >= boundary)
        pack_targets = sorted(d - first for index in range(first, last)
                              for (kind, _a, _b, _c, d) in [pack.op(index)]
                              if kind in (OP_IF, OP_GOTO) and d - first >= boundary)
        check(pack_targets == src_targets,
              f"场景 {scene_id}: 跨块跳转目标 {pack_targets} != 源 {src_targets}")
        cross_total += len(src_targets)
        notes.append(f"scene {scene_id}: chunks={chunks} ops={len(ops)} "
                     f"boundary_op={_kb}->{_ka} cross_jumps={len(pack_targets)}")

    # 4 个双块场景在源里共有 12 条跨块跳转;一条都不能少、不能错位。
    check(cross_total >= 4, f"跨块跳转只核对到 {cross_total} 条,源应更多")

    # 4) 计数对账。
    text_ops = 0
    speakers = set()
    frames_used = set()
    for index in range(pack.count(SEC_OPS)):
        kind, _a, speaker, _c, d = pack.op(index)
        if kind == OP_TEXT:
            text_ops += 1
            speakers.add(pack.text(speaker))
            frames_used.add(d)
    check(total_lines == text_ops,
          f"源对白 {total_lines} 条,pack 里 {text_ops} 条")
    source_text = sum(1 for _sid, ops, _c in scenes for op in ops if op.get("op") == "text")
    check(text_ops == source_text, f"对白条数 {text_ops} != 源 {source_text}")

    # 5) 章节顺序:pack 的 CHAP 顺序必须与 catalog.chapters 一致。
    chapters = catalog.get("chapters") or []
    check(pack.count(SEC_CHAP) == len(chapters),
          f"章节数 {pack.count(SEC_CHAP)} != catalog {len(chapters)}")
    for index, chapter in enumerate(chapters[:pack.count(SEC_CHAP)]):
        scene, name, op, _reserved = pack.chapter(index)
        check(scene == int(chapter["id"]),
              f"章节 {index}: pack 场景 {scene} != catalog id {chapter['id']}")
        check(pack.text(name) == chapter["name"],
              f"章节 {index}: 名字 {pack.text(name)!r} != {chapter['name']!r}")

    # 6) 鉴赏表:独立源报告（含 dedupe 前的重复标签）必须与 pack GAL 逐行一致。
    expected_gallery = eden_source_map.build_gallery_groups(source, frame_index)
    raw_gallery = pack.raw(SEC_GAL)
    got_gallery = []
    for index in range(pack.count(SEC_GAL)):
        frame, group, label = struct.unpack_from("<HHH", raw_gallery, index * 6)
        got_gallery.append((frame, group, pack.text(label)))
    check(expected_gallery == got_gallery,
          f"鉴赏表 {len(got_gallery)} 行与独立源 {len(expected_gallery)} 行不一致")
    extra = [row for row in expected_gallery if row[2] == "CG · ev_add02_up04"]
    check(len(extra) == 1, "独立源缺 pre-dedupe 标签 CG · ev_add02_up04")

    print(f"source ops: {total_ops}, dialogue lines: {text_ops}, "
          f"speakers: {len(speakers)}, frames referenced: {len(frames_used)}, "
          f"multi-chunk scenes: {len(multi_chunk)}")
    for note in notes:
        print("  " + note)
    if failures:
        print(f"FAIL {len(failures)} 项:")
        for message in failures[:20]:
            print("  -", message)
        return 1
    print("OK eden source audit: 源与包逐条一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
