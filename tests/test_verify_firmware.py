#!/usr/bin/env python3
"""Host tests for the sound-keychain firmware-layout verifier.

`tools/verify_firmware.py` on this branch enforces the product layout:
a 2 MB `factory` app partition immediately followed by the `voicefs` SPIFFS data
partition. These tests pin the parser and that layout contract.
"""

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "verify_firmware", ROOT / "tools" / "verify_firmware.py"
)
assert SPEC and SPEC.loader
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)


# (kind, subtype, offset, size, label) — must match the product partitions.csv.
PRODUCT_LAYOUT = (
    (0, 0, 0x10000, VERIFY.APP_MAX_SIZE, "factory"),
    (1, 0x82, 0x210000, 0x5F0000, "voicefs"),
)


def sample_table(entries) -> bytes:
    raw = bytearray(b"\xff" * VERIFY.PARTITION_TABLE_SIZE)
    for index, (kind, subtype, offset, size, label) in enumerate(entries):
        VERIFY.ENTRY.pack_into(
            raw,
            index * VERIFY.ENTRY.size,
            0x50AA,
            kind,
            subtype,
            offset,
            size,
            label.encode().ljust(16, b"\0"),
            0,
        )
    marker = len(entries) * VERIFY.ENTRY.size
    struct.pack_into("<H", raw, marker, 0xEBEB)
    raw[marker + 16 : marker + 32] = hashlib.md5(raw[:marker]).digest()
    return bytes(raw)


def merged_image(table: bytes, app_size: int) -> bytes:
    blob = bytearray(b"\xff" * 0x300000)
    blob[
        VERIFY.PARTITION_TABLE_OFFSET : VERIFY.PARTITION_TABLE_OFFSET + len(table)
    ] = table
    blob[0x10000] = 0xE9   # ESP application image magic
    return bytes(blob)


class PartitionParserTest(unittest.TestCase):
    def test_parses_product_layout_and_md5(self) -> None:
        partitions, found_md5 = VERIFY.parse_partition_table(sample_table(PRODUCT_LAYOUT))
        self.assertTrue(found_md5)
        self.assertEqual([item.label for item in partitions], ["factory", "voicefs"])
        self.assertEqual(partitions[0].size, VERIFY.APP_MAX_SIZE)

    def test_rejects_bad_md5(self) -> None:
        raw = bytearray(sample_table(PRODUCT_LAYOUT))
        raw[28] ^= 1
        with self.assertRaisesRegex(ValueError, "MD5"):
            VERIFY.parse_partition_table(bytes(raw))


class PartitionLayoutTest(unittest.TestCase):
    def _run(self, merged: bytes, app_size: int) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "FoloToy-AI-Passport.bin").write_bytes(b"\xe9" + b"\0" * (app_size - 1))
            VERIFY.verify_partition_layout(merged, build)

    def test_accepts_product_layout(self) -> None:
        self._run(merged_image(sample_table(PRODUCT_LAYOUT), 1024), 1024)

    def test_rejects_oversized_application(self) -> None:
        table = sample_table(PRODUCT_LAYOUT)
        with self.assertRaisesRegex(ValueError, "exceeds factory partition"):
            self._run(merged_image(table, VERIFY.APP_MAX_SIZE + 1), VERIFY.APP_MAX_SIZE + 1)

    def test_rejects_missing_md5(self) -> None:
        raw = bytearray(sample_table(PRODUCT_LAYOUT))
        marker = len(PRODUCT_LAYOUT) * VERIFY.ENTRY.size
        raw[marker : marker + 2] = b"\xff\xff"   # drop the MD5 marker
        with self.assertRaisesRegex(ValueError, "MD5"):
            self._run(merged_image(bytes(raw), 1024), 1024)


if __name__ == "__main__":
    unittest.main()
