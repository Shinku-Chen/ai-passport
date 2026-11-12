#!/usr/bin/env python3
"""Formal negative/positive tests for the ANEMOI binary audit."""
from __future__ import annotations

import copy
import json
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import anemoi_binary_audit as audit  # noqa: E402

SOURCE = os.environ.get("ANEMOI_SOURCE", "")
SCOPE = ROOT / "main" / "anemoi_data" / "anemoi_scope.json"
PACK = ROOT / "main" / "anemoi_data" / "anemoi_pack.bin"
BASELINE = os.environ.get("ANEMOI_RELEASE_PACK", "")


def require(name: str, value: str) -> None:
    if not value or not Path(value).exists():
        raise AssertionError(f"{name} is required and must exist: {value!r}")


def expect_error(label: str, scope: dict, blob: bytes, needle: str) -> None:
    report, errors = audit.audit(Path(SOURCE), scope, blob)
    assert errors, f"{label}: audit unexpectedly passed ({report})"
    assert any(needle in error for error in errors), (label, errors[:5])
    assert not any("CRC" in error for error in errors), (label, errors[:5])
    print(f"PASS negative {label}: {errors[0]}")


def main() -> int:
    require("ANEMOI_SOURCE", SOURCE)
    require("ANEMOI scope", str(SCOPE))
    require("ANEMOI pack", str(PACK))
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    pack = PACK.read_bytes()
    baseline = Path(BASELINE).read_bytes() if BASELINE else None
    report, errors = audit.audit(Path(SOURCE), scope, pack, baseline)
    assert not errors, errors[:10]
    assert report["scenes"] == 80
    assert report["scope_ops"] == report["pack_ops"] == report["checked"] == 61257
    assert report["static_branch_targets"] == 15620
    assert report["static_cross_scene_entry_refs"] == 333
    assert report["dynamic_entries"] == 3
    assert report["chapter_head_returns"] == 2
    assert report["arrive_polls"] == 1
    assert report["set_dyn"] == 0
    assert report["boundary_returns"] == 0
    assert report["out_of_range_call"] == 0
    assert report["image_rows"] == 2068
    assert report["image_row_kinds"] == {0: 338, 1: 1728, 2: 2}
    if baseline is not None:
        assert report["baseline_image_rows"] == 2000
        assert report["image_row_delta"] == 68
        assert report["image_row_kind_delta"] == {"0": 16, "1": 52, "2": 0}
    print("PASS positive: 80 scenes / 61257 ops / 15620 branch targets / "
          "333 static cross-scene entry refs / 3 dynamic entries / "
          "2 chapter-head returns / 1 arrive poll")

    # Source range one op short: the exact pack length must fail.
    short = copy.deepcopy(scope)
    short_sid = None
    for sid, pairs in short["ranges"].items():
        for pair in pairs:
            if pair[1] - pair[0] > 1:
                pair[1] -= 1
                short_sid = sid
                break
        if short_sid is not None:
            break
    assert short_sid is not None
    expect_error("scope-range-one-op-short", short, pack, "pack length")

    # Pack scene missing/extra: mutate one binary scene id to an unused id.
    info = audit.parse_pack(pack)
    missing = bytearray(pack)
    struct.pack_into("<H", missing, info["scnid_offset"], 300)
    expect_error("pack-scene-set-mismatch", scope, bytes(missing), "pack scene set")

    # Same-kind jump target changed to another legal pack op.
    branch = report["details"]["branch_samples"][0]
    bad_branch = bytearray(pack)
    new_b = 0 if branch["expected_b"] != 0 else 1
    struct.pack_into("<I", bad_branch, info["op_offset"] + branch["pack_index"] * 12 + 4, new_b)
    expect_error("jump-target-relocation", scope, bytes(bad_branch), "target")

    # Static cross-scene entry value changed to another legal pack op.
    entry = report["details"]["entry_samples"][0]
    bad_entry = bytearray(pack)
    new_entry = 0 if entry["expected_pack"] != 0 else 1
    struct.pack_into("<I", bad_entry, info["entry_offset"] + entry["table_index"] * 8 + 4,
                     new_entry)
    expect_error("cross-scene-entry-value", scope, bytes(bad_entry), "entry table")

    # Scope source commit mismatch.
    bad_commit = copy.deepcopy(scope)
    bad_commit["source_commit"] = "0" * 40
    expect_error("scope-source-commit", bad_commit, pack, "source commit")

    # Scope bounds.
    bad_bounds = copy.deepcopy(scope)
    bad_bounds["ranges"]["2"] = [[0, 1000000]]
    expect_error("scope-range-bounds", bad_bounds, pack, "range out of bounds")

    print("PASS test_anemoi_binary_audit: positive + 6 meaningful negatives")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
