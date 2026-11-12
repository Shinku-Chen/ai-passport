#!/usr/bin/env python3
"""Exact ANEMOI binary/source audit.

This audit deliberately does not import the packer or its scope loader. It
reads the pinned source story JSON, the reviewed scope manifest, and the binary
pack directly. It enforces scene-set equality, exact per-scene range lengths,
source op order, total checked == scope total == pack op count, static
if/goto/call relocation, and static cross-scene entry references.
"""
from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
from collections import Counter
from pathlib import Path

MAGIC = b"ANEMPK1\0"
SEC_OPS, SEC_SCENE, SEC_SCNID, SEC_ENTRY, SEC_IMAGE = 2, 3, 4, 5, 16

OP_TEXT, OP_BG, OP_PORTRAIT, OP_COPY_BODIES, OP_COPY_CG = 0, 1, 2, 3, 4
OP_COMMIT, OP_INIT_TIME, OP_IF_FALSE, OP_IF_TRUE, OP_GOTO = 5, 6, 7, 8, 9
OP_CALL, OP_RETURN, OP_JUMP, OP_CALL_SCENE = 10, 11, 12, 13
OP_CHOICE, OP_BOARD, OP_END, OP_OMITTED, OP_SKIP_GAME, OP_ROUTE_CHECK = 14, 15, 16, 17, 18, 19
OP_SET, OP_NATIVE, OP_ARRIVE_CHOICE, OP_BOUNDARY = 20, 21, 22, 23
OP_SET_DYN = 24

KIND = {
    "text": OP_TEXT,
    "background": OP_BG,
    "portrait": OP_PORTRAIT,
    "copyBodies": OP_COPY_BODIES,
    "copyCG": OP_COPY_CG,
    "visualCommit": OP_COMMIT,
    "initTime": OP_INIT_TIME,
    "ifFalse": OP_IF_FALSE,
    "ifTrue": OP_IF_TRUE,
    "goto": OP_GOTO,
    "call": OP_CALL,
    "return": OP_RETURN,
    "jumpScene": OP_JUMP,
    "callScene": OP_CALL_SCENE,
    "dynamicChoice": OP_CHOICE,
    "board": OP_BOARD,
    "end": OP_END,
    "omittedScene": OP_OMITTED,
    "skipGame": OP_SKIP_GAME,
    "routeCheck": OP_ROUTE_CHECK,
    "set": OP_SET,
    "native": OP_NATIVE,
    "arriveChoice": OP_ARRIVE_CHOICE,
    "boundary": OP_BOUNDARY,
    "setDyn": OP_SET_DYN,
}
KIND_NAME = {value: key for key, value in KIND.items()}
CHAPTER_HEADS = {125, 131}


class AuditError(Exception):
    pass


def source_common(source_root: Path) -> Path:
    candidates = (source_root / "src" / "common", source_root / "common", source_root)
    for path in candidates:
        if (path / "story").is_dir():
            return path
    raise AuditError(f"source story directory not found under {source_root}")


def git_commit(path: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            stderr=subprocess.DEVNULL, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def load_catalog(common: Path) -> dict:
    text = (common / "catalog.js").read_text(encoding="utf-8")
    start = text.index("{", text.index("export const catalog="))
    return json.loads(text[start:text.rindex("}") + 1])


def load_source_ops(common: Path, sid: int) -> list:
    ops = []
    for path in sorted(common.glob(f"story/{sid}-*.txt")):
        data = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(data, list):
            ops.extend(data)
        else:
            ops.extend(data.get("ops") or [])
    return ops


def parse_pack(blob: bytes) -> dict:
    if len(blob) < 28 + 24 * 16 or blob[:8] != MAGIC:
        raise AuditError("pack magic/header truncated")
    version, header_size, section_count = struct.unpack_from("<HHH", blob, 8)
    if (version, header_size, section_count) != (1, 28, 24):
        raise AuditError(f"pack header {version}/{header_size}/{section_count}")
    total = struct.unpack_from("<I", blob, 16)[0]
    if total != len(blob):
        raise AuditError(f"pack total {total} != file {len(blob)}")
    sections = {}
    for index in range(section_count):
        kind, offset, count, size = struct.unpack_from("<IIII", blob, 28 + 16 * index)
        if kind in sections:
            raise AuditError(f"duplicate pack section {kind}")
        sections[kind] = (offset, count, size)
    for required in (SEC_OPS, SEC_SCENE, SEC_SCNID, SEC_ENTRY, SEC_IMAGE):
        if required not in sections:
            raise AuditError(f"missing pack section {required}")
    op_offset, op_count, _ = sections[SEC_OPS]
    scene_offset, scene_count, _ = sections[SEC_SCENE]
    scnid_offset, scnid_count, _ = sections[SEC_SCNID]
    entry_offset, entry_count, _ = sections[SEC_ENTRY]
    image_offset, image_count, _ = sections[SEC_IMAGE]
    if scene_count != scnid_count + 1:
        raise AuditError(f"scene slots {scene_count} != scnid {scnid_count} + 1")
    slots = list(struct.unpack_from(f"<{scene_count}I", blob, scene_offset))
    scene_ids = list(struct.unpack_from(f"<{scnid_count}H", blob, scnid_offset))
    entry_rows = []
    entry_index = {}
    for index in range(entry_count):
        sid, entry, op = struct.unpack_from("<HHI", blob, entry_offset + 8 * index)
        entry_rows.append((sid, entry, op))
        entry_index[(sid, entry)] = index
    image_kinds = Counter()
    for index in range(image_count):
        kind = struct.unpack_from("<B", blob, image_offset + 16 * index + 12)[0]
        image_kinds[kind] += 1
    return {
        "blob": blob,
        "sections": sections,
        "op_offset": op_offset,
        "op_count": op_count,
        "scene_offset": scene_offset,
        "scene_count": scene_count,
        "scnid_offset": scnid_offset,
        "scnid_count": scnid_count,
        "entry_offset": entry_offset,
        "entry_count": entry_count,
        "entry_rows": entry_rows,
        "entry_index": entry_index,
        "slots": slots,
        "scene_ids": scene_ids,
        "slot_index": {sid: index for index, sid in enumerate(scene_ids)},
        "image_count": image_count,
        "image_kinds": dict(image_kinds),
    }


def pack_op(pack: dict, index: int):
    if index < 0 or index >= pack["op_count"]:
        raise AuditError(f"pack op index out of range: {index}")
    return struct.unpack_from("<BBHII", pack["blob"], pack["op_offset"] + 12 * index)


def scope_ranges(scope: dict, common: Path):
    errors = []
    expected_commit = str(scope.get("source_commit") or "")
    actual_commit = git_commit(common)
    if expected_commit and not actual_commit:
        errors.append("source commit unavailable")
    elif expected_commit and expected_commit != actual_commit:
        errors.append(f"source commit mismatch: scope {expected_commit} != source {actual_commit}")
    if not expected_commit:
        errors.append("scope source_commit missing")
    raw_ranges = scope.get("ranges")
    if not isinstance(raw_ranges, dict) or not raw_ranges:
        return {}, {}, errors + ["scope ranges missing"]
    ranges = {}
    source_ops = {}
    for sid_text, raw_pairs in sorted(raw_ranges.items(), key=lambda item: int(item[0])):
        sid = int(sid_text)
        ops = load_source_ops(common, sid)
        source_ops[sid] = ops
        if not ops:
            errors.append(f"scene {sid}: source ops missing")
        checked = []
        for pair in raw_pairs or []:
            if not isinstance(pair, list) or len(pair) != 2:
                errors.append(f"scene {sid}: bad scope range {pair!r}")
                continue
            a, b = int(pair[0]), int(pair[1])
            if a < 0 or b <= a or b > len(ops):
                errors.append(f"scene {sid}: scope range out of bounds [{a},{b}) len={len(ops)}")
                continue
            checked.append((a, b))
        checked.sort()
        for previous, current in zip(checked, checked[1:]):
            if current[0] < previous[1]:
                errors.append(f"scene {sid}: scope ranges overlap {previous} and {current}")
        ranges[sid] = checked
    return ranges, source_ops, errors


def expected_kind(sid: int, op: dict, ranges):
    source_kind = op.get("op")
    if source_kind in ("goto", "ifFalse", "ifTrue"):
        target = op.get("to")
        if isinstance(target, int) and not any(a <= target < b for a, b in ranges):
            if sid in CHAPTER_HEADS:
                return OP_RETURN, "chapter_head_return"
            return OP_BOUNDARY, "boundary_return"
    if source_kind == "set":
        target = op.get("target") or []
        if len(target) > 2 and not isinstance(target[2], (int, str)):
            return OP_SET_DYN, "set_dyn"
        value = op.get("value")
        if (isinstance(value, list) and len(value) >= 2 and value[0] == "call"
                and value[1] == "$$input_update"):
            return OP_ARRIVE_CHOICE, "arrive_poll"
    return KIND.get(source_kind), None


def audit(source_root: Path, scope: dict, blob: bytes, baseline_blob: bytes | None = None):
    common = source_common(Path(source_root))
    catalog = load_catalog(common)
    pack = parse_pack(blob)
    ranges, source_ops, errors = scope_ranges(scope, common)
    scope_ids = set(ranges)
    pack_ids = pack["scene_ids"]
    if len(set(pack_ids)) != len(pack_ids):
        errors.append("pack scene set contains duplicate ids")
    missing = sorted(scope_ids - set(pack_ids))
    extra = sorted(set(pack_ids) - scope_ids)
    if missing or extra:
        errors.append(f"pack scene set != scope: missing={missing} extra={extra}")
    if scope_ids != set(pack_ids):
        # The per-scene checks below still cover the intersection.
        pass

    starts = {}
    for index, sid in enumerate(pack_ids):
        starts[sid] = (pack["slots"][index], pack["slots"][index + 1])

    def map_op(sid: int, source_index: int):
        if sid not in starts:
            return None
        cursor = starts[sid][0]
        for a, b in ranges.get(sid, []):
            if a <= source_index < b:
                return cursor + (source_index - a)
            cursor += b - a
        return None

    counts = Counter()
    samples = {"branch_samples": [], "entry_samples": []}
    checked = 0
    for sid in sorted(scope_ids):
        if sid not in starts:
            continue
        source = source_ops.get(sid) or []
        declared = ranges.get(sid, [])
        want_len = sum(b - a for a, b in declared)
        start, end = starts[sid]
        got_len = end - start
        if got_len != want_len:
            errors.append(f"scene {sid}: pack length {got_len} != scope {want_len}")
            continue
        cursor = start
        for a, b in declared:
            for local in range(a, b):
                op = source[local]
                if not isinstance(op, dict):
                    errors.append(f"scene {sid} source {local}: non-dict op")
                    cursor += 1
                    checked += 1
                    continue
                expected, special = expected_kind(sid, op, declared)
                if special:
                    counts[special] += 1
                if expected is None:
                    errors.append(f"scene {sid} source {local}: unknown op {op.get('op')!r}")
                    cursor += 1
                    checked += 1
                    continue
                kind, flags, a_field, b_field, c_field = pack_op(pack, cursor)
                if kind != expected:
                    errors.append(
                        f"scene {sid} source {local} {op.get('op')}: "
                        f"pack kind {kind} expected {expected}")
                    cursor += 1
                    checked += 1
                    continue

                source_kind = op.get("op")
                if source_kind in ("ifFalse", "ifTrue", "goto", "call"):
                    target = op.get("to")
                    if isinstance(target, int):
                        counts["static_branch_targets"] += 1
                        expected_target = map_op(sid, target)
                        if expected_target is not None:
                            want_kind = {
                                "ifFalse": OP_IF_FALSE,
                                "ifTrue": OP_IF_TRUE,
                                "goto": OP_GOTO,
                                "call": OP_CALL,
                            }[source_kind]
                            if b_field != expected_target:
                                errors.append(
                                    f"scene {sid} source {local} {source_kind} target "
                                    f"{target}: pack b={b_field} expected {expected_target}")
                            if len(samples["branch_samples"]) < 5:
                                samples["branch_samples"].append({
                                    "scene": sid, "local": local, "pack_index": cursor,
                                    "expected_b": expected_target,
                                })
                        elif source_kind == "call":
                            counts["out_of_range_call"] += 1
                            errors.append(
                                f"scene {sid} source {local} call target {target} "
                                f"outside scope; pack b={b_field}")
                elif source_kind in ("jumpScene", "callScene"):
                    target_scene = int(op.get("scene"))
                    entry = op.get("entry")
                    want_kind = OP_JUMP if source_kind == "jumpScene" else OP_CALL_SCENE
                    if a_field != target_scene:
                        errors.append(
                            f"scene {sid} source {local} {source_kind}: "
                            f"pack scene {a_field} expected {target_scene}")
                    if isinstance(entry, int):
                        counts["static_cross_scene_entry_refs"] += 1
                        if target_scene in scope_ids:
                            meta = catalog["scenes"].get(str(target_scene), {})
                            entry_map = meta.get("entries") or {}
                            # The reviewed packer's documented fallback for an
                            # entry absent from catalog.entries is source op 0.
                            target_local = int(entry_map.get(str(entry), 0))
                            expected_entry = map_op(target_scene, target_local)
                        else:
                            expected_entry = None
                        want_entry = 0xFFFFFFFF if expected_entry is None else expected_entry
                        got_entry = pack["entry_index"].get((target_scene, entry))
                        if got_entry is None:
                            got_value = 0xFFFFFFFF
                            table_index = None
                        else:
                            got_value = pack["entry_rows"][got_entry][2]
                            table_index = got_entry
                        if got_value != want_entry:
                            errors.append(
                                f"scene {sid} source {local} {source_kind} -> "
                                f"{target_scene}:{entry}: entry table {got_value} "
                                f"expected {want_entry}")
                        if flags & 1:
                            errors.append(
                                f"scene {sid} source {local} {source_kind}: "
                                f"static entry flagged dynamic")
                        if (expected_entry is not None and table_index is not None
                                and len(samples["entry_samples"]) < 5):
                            samples["entry_samples"].append({
                                "scene": sid, "local": local,
                                "target_scene": target_scene, "entry": entry,
                                "table_index": table_index,
                                "expected_pack": expected_entry,
                            })
                    else:
                        counts["dynamic_entries"] += 1
                        if not (flags & 1):
                            errors.append(
                                f"scene {sid} source {local} {source_kind}: "
                                f"dynamic entry not flagged")
                        if c_field == 0:
                            errors.append(
                                f"scene {sid} source {local} {source_kind}: "
                                f"dynamic entry expression missing")
                cursor += 1
                checked += 1

    scope_total = sum(b - a for pairs in ranges.values() for a, b in pairs)
    if checked != scope_total:
        errors.append(f"coverage checked {checked} != scope total {scope_total}")
    if scope_total != pack["op_count"]:
        errors.append(f"coverage scope total {scope_total} != pack op count {pack['op_count']}")
    if checked != pack["op_count"]:
        errors.append(f"coverage checked {checked} != pack op count {pack['op_count']}")

    baseline_images = None
    baseline_kinds = None
    image_delta = None
    kind_delta = None
    if baseline_blob is not None:
        baseline = parse_pack(baseline_blob)
        baseline_images = baseline["image_count"]
        baseline_kinds = baseline["image_kinds"]
        image_delta = pack["image_count"] - baseline_images
        kind_delta = {
            str(kind): pack["image_kinds"].get(kind, 0) - baseline_kinds.get(kind, 0)
            for kind in sorted(set(pack["image_kinds"]) | set(baseline_kinds))
        }

    report = {
        "scenes": len(scope_ids),
        "scope_ops": scope_total,
        "pack_ops": pack["op_count"],
        "checked": checked,
        "static_branch_targets": counts["static_branch_targets"],
        "static_cross_scene_entry_refs": counts["static_cross_scene_entry_refs"],
        "dynamic_entries": counts["dynamic_entries"],
        "chapter_head_returns": counts["chapter_head_return"],
        "boundary_returns": counts["boundary_return"],
        "arrive_polls": counts["arrive_poll"],
        "set_dyn": counts["set_dyn"],
        "out_of_range_call": counts["out_of_range_call"],
        "entry_table_rows": pack["entry_count"],
        "image_rows": pack["image_count"],
        "image_row_kinds": pack["image_kinds"],
        "baseline_image_rows": baseline_images,
        "baseline_image_row_kinds": baseline_kinds,
        "image_row_delta": image_delta,
        "image_row_kind_delta": kind_delta,
        "details": samples,
    }
    return report, errors


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, help="pinned source root or src/common")
    parser.add_argument("--scope", default="main/anemoi_data/anemoi_scope.json")
    parser.add_argument("--pack", default="main/anemoi_data/anemoi_pack.bin")
    parser.add_argument("--baseline-pack", default="")
    parser.add_argument("--json", default="")
    args = parser.parse_args(argv)
    try:
        scope = json.loads(Path(args.scope).read_text(encoding="utf-8"))
        pack = Path(args.pack).read_bytes()
        baseline = Path(args.baseline_pack).read_bytes() if args.baseline_pack else None
        report, errors = audit(Path(args.source), scope, pack, baseline)
        print(
            "scenes=%d scope_ops=%d pack_ops=%d checked=%d "
            "static_branch_targets=%d static_cross_scene_entry_refs=%d "
            "dynamic_entries=%d chapter_head_returns=%d arrive_polls=%d "
            "boundary_returns=%d out_of_range_call=%d image_rows=%d image_row_kinds=%s "
            "entry_table_rows=%d" % (
                report["scenes"], report["scope_ops"], report["pack_ops"],
                report["checked"], report["static_branch_targets"],
                report["static_cross_scene_entry_refs"], report["dynamic_entries"],
                report["chapter_head_returns"], report["arrive_polls"],
                report["boundary_returns"], report["out_of_range_call"],
                report["image_rows"], report["image_row_kinds"],
                report["entry_table_rows"]))
        if args.baseline_pack:
            print(
                "baseline_image_rows=%d image_row_delta=%d image_row_kind_delta=%s" % (
                    report["baseline_image_rows"], report["image_row_delta"],
                    report["image_row_kind_delta"]))
        if args.json:
            Path(args.json).write_text(
                json.dumps({"report": report, "errors": errors}, ensure_ascii=False, indent=1)
                + "\n", encoding="utf-8")
        for error in errors[:20]:
            print("ERROR", error)
        if errors:
            print(f"FAIL errors={len(errors)}")
            return 1
        print("PASS")
        return 0
    except (AuditError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
