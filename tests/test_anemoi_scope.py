#!/usr/bin/env python3
"""Source checks for the reviewed ANEMOI common-required scope.

The scope manifest is data-only: it may select source ranges, but it must not
invent text or flags.  The last section records why the Yang task-3 gate is a
source deadlock instead of claiming a natural route.  Run with ANEMOI_SOURCE
pointing at the pinned anemoi-rw5 snapshot.
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import anemoi_pack as pack  # noqa: E402

SCOPE = os.path.join(ROOT, "main", "anemoi_data", "anemoi_scope.json")
MANIFEST = os.path.join(ROOT, "main", "anemoi_data", "anemoi_pack.json")
PACK = os.path.join(ROOT, "main", "anemoi_data", "anemoi_pack.bin")
SYMBOLS = os.path.join(ROOT, "assets", "fonts", "gal_cjk_symbols.txt")


def sha256(path: str) -> str:
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def call_targets(src, target: int):
    found = []
    for sid in src.scene_ids():
        for index, op in enumerate(src.ops(sid)):
            if op.get("op") in ("callScene", "jumpScene") and op.get("scene") == target:
                found.append((sid, index, int(op.get("entry", 0))))
    return found


def writes(src, var: int):
    found = []
    for sid in src.scene_ids():
        for index, op in enumerate(src.ops(sid)):
            target = op.get("target") or []
            if (op.get("op") in ("set", "add", "sub") and len(target) >= 3
                    and target[0] == "v" and target[1] == "a" and target[2] == var):
                found.append((sid, index, op.get("value")))
    return found


def test_scope(src):
    included, _boundary, detail, info = pack.load_scope_manifest(src, SCOPE)

    assert info["source_commit"] == src.source_commit
    assert info["schema"] == 1
    assert info["sha256"] == sha256(SCOPE)
    assert len(included) == 80
    assert sum(b - a for ranges in included.values() for a, b in ranges) == 61257

    # Full entry-0 scenes are the ones the shipped 71-scene pack truncated or
    # omitted.  The call sites below are the reason each range is required.
    for sid in (26, 27, 28, 90):
        assert included[sid] == [(0, len(src.ops(sid)))], sid
    for sid in range(167, 175):
        assert included[sid] == [(0, len(src.ops(sid)))], sid

    assert (2, 145, 0) in call_targets(src, 26)
    assert (2, 279, 0) in call_targets(src, 27)
    assert (19, 338, 0) in call_targets(src, 28)
    assert (20, 340, 0) in call_targets(src, 28)
    assert (21, 336, 0) in call_targets(src, 28)
    assert (2, 1030, 0) in call_targets(src, 90)
    for scene, first in ((134, 380), (135, 195), (136, 129)):
        for offset in range(8):
            assert (scene, first + offset * 4, 0) in call_targets(src, 167 + offset)
    # After any of the eight 小詠 play calls, each scene increments a13 before
    # returning to the board; the range check proves the increment is packed.
    for scene, op_index in ((134, 622), (135, 442), (136, 483)):
        assert src.ops(scene)[op_index]["target"] == ["v", "a", 13]
        assert any(start <= op_index < end for start, end in included[scene])

    # Chapter-head boundaries stay partial; 205/206/262 belong to books that
    # the current release never shipped.
    assert included[125] == [(0, 160)]
    assert included[131] == [(0, 149)]
    assert 205 not in included and 206 not in included and 262 not in included
    heads = {(item["scene"], item["kind"]) for item in detail["boundary"]}
    assert (125, "chapter-head") in heads and (131, "chapter-head") in heads


def test_yang_source_deadlock(src):
    helper = src.helpers["393:$$check_character_bbs_disp"]["ops"]
    condition = helper[128]["condition"]
    assert condition[0] == "&&"
    assert condition[2] == [">=", ["v", "a", 17], 2], condition
    assert writes(src, 17) == [(93, 904, ["+", ["v", "a", 17], 1])]
    assert writes(src, 300) == [(93, 87, ["+", ["v", "a", 300], 1])]
    assert {sid for sid, _i, _v in writes(src, 12)} == set(range(91, 99))

    # scene393 gates the ch3 board block on helper($ch_no=3); a12==2 selects
    # request 040_淡雪0003.  The same task completion is what writes a12/a17 in
    # scene93, so the >=2 condition cannot be reached before the task exists.
    board = src.ops(393)
    assert board[26]["op"] == "ifFalse"
    assert board[26]["condition"] == ["call", "$$check_character_bbs_disp", [3]]
    assert board[29]["op"] == "ifTrue" and board[29]["condition"] == [
        "==", ["v", "a", 12], 2]
    assert "040_淡雪0003" in json.dumps(board[40], ensure_ascii=False)
    task = src.ops(93)
    assert task[87]["target"] == ["v", "a", 300]
    assert task[903]["target"] == ["v", "a", 12]
    assert task[904]["target"] == ["v", "a", 17]


def test_manifest_and_font(src):
    manifest = json.load(open(MANIFEST, encoding="utf-8"))
    scope = json.load(open(SCOPE, encoding="utf-8"))
    assert manifest["source_overlay"] is None
    assert manifest["source_commit"] == src.source_commit
    assert manifest["scope_manifest"]["sha256"] == sha256(SCOPE)
    assert manifest["scope_manifest"]["source_commit"] == src.source_commit
    assert manifest["packer_args"]["scope_manifest"] == "main/anemoi_data/anemoi_scope.json"
    assert manifest["packer_args"]["layout_scale"] == 0.52
    assert manifest["packer_args"]["palette_colors"] == 32
    assert manifest["packer_args"]["jpeg_quality"] == 24
    assert manifest["ops"] == 61257
    assert manifest["scenes"] == 80
    assert manifest["included_scenes"] == {
        str(k): v for k, v in sorted(scope["ranges"].items(), key=lambda item: int(item[0]))}
    assert manifest["font"]["gal_cjk_16.c"]["sha256"] == \
        sha256(os.path.join(ROOT, "assets", "fonts", "gal_cjk_16.c"))
    assert manifest["font"]["gal_cjk_symbols.txt"]["sha256"] == sha256(SYMBOLS)
    if os.path.exists(PACK):
        assert manifest["bytes"] == os.path.getsize(PACK)
        assert manifest["sha256"] == sha256(PACK)
        blob = open(PACK, "rb").read()
        total = struct.unpack_from("<I", blob, 16)[0]
        scene_count = struct.unpack_from("<I", blob, 28 + 16 * 3 + 8)[0]
        assert total == len(blob)
        assert scene_count == 81  # slot sentinel for 80 scenes
        # Every character in the shipped pack must be present in the 16px font
        # inventory.  The gate's gal_font.py --check also verifies glyph lookup.
        _kind, text_off, _text_count, text_size = struct.unpack_from("<IIII", blob, 28)
        chars = {c for c in blob[text_off:text_off + text_size].decode("utf-8")
                 if ord(c) >= 0x20}
        inventory = {c for line in open(SYMBOLS, encoding="utf-8")
                     if not line.startswith("#") for c in line.rstrip("\n")
                     if ord(c) >= 0x20}
        assert not (chars - inventory), sorted(chars - inventory)[:20]


def main():
    source = os.environ.get("ANEMOI_SOURCE", "")
    if not source or not os.path.isdir(source):
        raise SystemExit("需要 ANEMOI_SOURCE 指向 anemoi-rw5 快照")
    src = pack.Source(source)
    test_scope(src)
    test_yang_source_deadlock(src)
    test_manifest_and_font(src)
    print("PASS test_anemoi_scope (80 scenes/61257 ops; Yang gate is SOURCE_DEADLOCK, no overlay)")


if __name__ == "__main__":
    main()
