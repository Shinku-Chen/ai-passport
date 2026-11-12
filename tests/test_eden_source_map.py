#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for tools/eden_source_map.py (no pinned media needed)."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("eden_source_map", ROOT / "tools" / "eden_source_map.py")
eden_source_map = importlib.util.module_from_spec(spec)
sys.modules["eden_source_map"] = eden_source_map
spec.loader.exec_module(eden_source_map)


class SourceMapTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "reports").mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, rows):
        (self.root / "reports" / name).write_text(json.dumps(rows), encoding="utf-8")

    def test_sorted_union_and_duplicate_label(self):
        self.write("camera-source.json", [
            {"frame": "f_b"}, {"frame": "f_b"}, {"frame": "f_a"},
        ])
        self.write("gallery-source-before-dedupe.json", [
            {"image": "f_b", "group": "up02", "label": "CG - up02"},
            {"image": "f_b", "group": "up04", "label": "CG - up04"},
            {"image": "f_c", "group": "cg", "label": "CG - c"},
        ])
        index, manifest = eden_source_map.build_frame_map(self.root)
        self.assertEqual(list(index), ["f_a", "f_b", "f_c"])
        self.assertEqual(manifest["camera_unique_frames"], 2)
        self.assertEqual(manifest["gallery_unique_images"], 2)
        self.assertEqual(manifest["frame_count"], 3)
        self.assertIn("f_b", manifest["duplicate_gallery_images"])
        groups = eden_source_map.build_gallery_groups(self.root, index)
        self.assertEqual(groups, [
            (1, 0, "CG - up02"),
            (1, 1, "CG - up04"),
            (2, 2, "CG - c"),
        ])

    def test_missing_reports_fail_loudly(self):
        with self.assertRaises(SystemExit):
            eden_source_map.build_frame_map(self.root)
        self.write("camera-source.json", [{"frame": "f_a"}])
        with self.assertRaises(SystemExit):
            eden_source_map.build_frame_map(self.root)

    def test_bad_row_fails(self):
        self.write("camera-source.json", [{"frame": 1}])
        self.write("gallery-source-before-dedupe.json", [{"image": "f_b"}])
        with self.assertRaises(SystemExit):
            eden_source_map.build_frame_map(self.root)


if __name__ == "__main__":
    unittest.main()
