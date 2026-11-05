#!/usr/bin/env python3
"""Keep the opaque supplier profile byte-exact and tied to the compiled driver."""

import hashlib
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "assets/battery/wanli-500mah-profile2-xil40x-20261020.txt"
DRIVER = ROOT / "components/bsp/src/bsp_battery.c"
SOURCE_SHA256 = "7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919"
PAYLOAD_SHA256 = "fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5"


def parse_profile(text: str) -> bytes:
    lines = text.splitlines()
    if len(lines) != 80 or any(re.fullmatch(r"[0-9A-F]{2}", line) is None for line in lines):
        raise ValueError("profile must contain exactly 80 uppercase two-digit hex lines")
    return bytes(int(line, 16) for line in lines)


class BatteryProfileTest(unittest.TestCase):
    def test_supplier_source_and_decoded_hashes(self) -> None:
        source = PROFILE.read_bytes()
        self.assertEqual(hashlib.sha256(source).hexdigest(), SOURCE_SHA256)
        payload = parse_profile(source.decode("ascii"))
        self.assertEqual(len(payload), 80)
        self.assertEqual(hashlib.sha256(payload).hexdigest(), PAYLOAD_SHA256)

    def test_driver_matches_all_vendor_bytes(self) -> None:
        source = DRIVER.read_text(encoding="utf-8")
        match = re.search(
            r"static\s+const\s+uint8_t\s+s_battery_profile\[\]\s*=\s*\{([^}]*)\};",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(match, "use an inferred array size so the static assertion catches truncation")
        assert match is not None
        tokens = [token.strip() for token in match.group(1).strip().rstrip(",").split(",")]
        self.assertTrue(all(re.fullmatch(r"0x[0-9A-Fa-f]{2}", token) for token in tokens))
        actual = bytes(int(token, 16) for token in tokens)
        self.assertEqual(actual, parse_profile(PROFILE.read_text(encoding="ascii")))
        self.assertRegex(source, r"#define\s+CW_PROFILE_SIZE\s+80\b")
        self.assertRegex(source, r"_Static_assert\(sizeof\(s_battery_profile\)\s*==\s*CW_PROFILE_SIZE")

    def test_parser_rejects_truncation_extra_bytes_and_bad_tokens(self) -> None:
        valid = ["00"] * 80
        cases = [valid[:-1], valid + ["00"], valid + [""]]
        for invalid in ("", "0", "000", "0x00", "GG", "ff", " 00", "00 ", "00 # comment"):
            cases.append(valid[:40] + [invalid] + valid[41:])
        for lines in cases:
            with self.subTest(lines=len(lines), token=lines[40]):
                with self.assertRaises(ValueError):
                    parse_profile("\n".join(lines) + "\n")


if __name__ == "__main__":
    unittest.main()
