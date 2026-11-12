#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build the expected EDEN frame index from pinned tracked source reports.

The packer's frame table is the sorted union of:

  * every frame id referenced by a source text op
    ('reports/camera-source.json', one row per op), and
  * every image id in the pre-dedupe CG gallery
    ('reports/gallery-source-before-dedupe.json').

Both reports are tracked in the pinned eden-rw5 source.  The post-dedupe
'reports/gallery-source.json' is not the pack input: it removes one
duplicate gallery label ('CG - ev_add02_up04', same frame as
'CG - ev_add02_up02'), which produces 2177 frames instead of the 2178 in
the committed pack.  Missing reports must fail explicitly; silently using an
empty gallery would make a strict source audit look green for the wrong
reason.

This module never reads the pack.  It is safe to use as the independent
expected-map source for tests/test_eden_source_audit.py.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

CAMERA_REPORT = "reports/camera-source.json"
GALLERY_REPORT = "reports/gallery-source-before-dedupe.json"
DEDUPE_REPORT = "reports/gallery-deduplication.json"


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _load_rows(source_root: Path, relative: str, key: str):
    path = source_root / relative
    if not path.is_file():
        raise SystemExit(f"missing tracked source report: {path}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise SystemExit(f"cannot parse {path}: {exc}")
    if not isinstance(data, list) or not data:
        raise SystemExit(f"{path}: expected a non-empty JSON array")
    for index, row in enumerate(data):
        if not isinstance(row, dict) or not isinstance(row.get(key), str):
            raise SystemExit(f"{path}: row {index} has no string {key!r}")
    return data, {"path": relative, "sha256": _sha256(path), "rows": len(data)}


def build_frame_map(source_root):
    """Return (frame_id -> index, manifest) for the pinned source."""
    root = Path(source_root)
    camera, camera_meta = _load_rows(root, CAMERA_REPORT, "frame")
    gallery, gallery_meta = _load_rows(root, GALLERY_REPORT, "image")

    frames = {row["frame"] for row in camera} | {row["image"] for row in gallery}
    ordered = sorted(frames)
    frame_index = {frame: index for index, frame in enumerate(ordered)}

    duplicate_labels = {}
    image_groups = {}
    for row in gallery:
        image_groups.setdefault(row["image"], set()).add(
            (row.get("group", ""), row.get("label", "")))
    for image, groups in image_groups.items():
        if len(groups) > 1:
            duplicate_labels[image] = sorted(groups)

    manifest = {
        "source": str(root),
        "reports": [camera_meta, gallery_meta],
        "camera_unique_frames": len({row["frame"] for row in camera}),
        "gallery_unique_images": len({row["image"] for row in gallery}),
        "frame_count": len(ordered),
        "ordering": "sorted frame id (same order as tools/eden_pack.py used_frames.sort())",
        "duplicate_gallery_images": duplicate_labels,
    }
    dedupe_path = root / DEDUPE_REPORT
    if dedupe_path.is_file():
        try:
            dedupe = json.loads(dedupe_path.read_text(encoding="utf-8"))
            manifest["dedupe_report"] = {
                "path": DEDUPE_REPORT,
                "sha256": _sha256(dedupe_path),
                "removedGroups": dedupe.get("removedGroups", []),
            }
        except (OSError, ValueError):
            pass
    return frame_index, manifest


def build_gallery_groups(source_root, frame_index):
    """Return the pack's expected (frame, group, label) rows.

    Mirrors the packer's stable grouping: first appearance of (group,label)
    wins, and the group index is the number of groups seen so far.  The
    pre-dedupe report keeps the duplicate 'CG - ev_add02_up04' label.
    """
    root = Path(source_root)
    gallery, _meta = _load_rows(root, GALLERY_REPORT, "image")
    rows = []
    seen = set()
    for item in gallery:
        frame = item["image"]
        if frame not in frame_index:
            continue
        key = (item.get("group", ""), item.get("label", ""))
        if key in seen:
            continue
        seen.add(key)
        rows.append((frame_index[frame], len(seen) - 1, item.get("label", "")))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, help="eden-rw5 checkout")
    parser.add_argument("--out", required=True, help="frame map JSON output")
    parser.add_argument("--manifest", help="optional manifest JSON output")
    args = parser.parse_args()

    frame_index, manifest = build_frame_map(args.source)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"frames": frame_index}, ensure_ascii=False,
                              separators=(",", ":")) + "\n", encoding="utf-8")
    if args.manifest:
        Path(args.manifest).write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"frames {len(frame_index)}; reports {[r['path'] for r in manifest['reports']]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
