"""Preserve the difference between absent sent and the valid mail id zero."""
import importlib.util
from pathlib import Path
import struct
from types import SimpleNamespace
import unittest


spec = importlib.util.spec_from_file_location(
    "steins_mobile_pack", Path(__file__).resolve().parents[1] / "tools/steins_mobile_pack.py"
)
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)

audit_spec = importlib.util.spec_from_file_location(
    "steins_mobile_audit", Path(__file__).resolve().parents[1] / "tools/steins_mobile_audit.py"
)
auditor = importlib.util.module_from_spec(audit_spec)
audit_spec.loader.exec_module(auditor)


class KeyPhoneSentTest(unittest.TestCase):
    def test_missing_sent_and_explicit_mail_zero(self):
        for fields, expected in (({}, 0xFFFF), ({"sent": 0}, 0), ({"sent": 233}, 233)):
            with self.subTest(fields=fields):
                source = {"windows": {}, "keyphone": {"n0": fields}}
                # Empty source labels do not add strings to the real pool.
                sections = packer.build_windows(source, {"n0": 0}, packer.Strings(None))
                self.assertEqual(struct.unpack_from("<H", sections["KEYPHONE"], 10)[0], expected)

    def test_audit_accepts_absent_encodings_but_preserves_explicit_zero(self):
        for fields, stored, valid in (
            ({}, 0xFFFF, True), ({}, 0, True), ({}, 233, False),
            ({"sent": 0}, 0, True), ({"sent": 0}, 0xFFFF, False),
            ({"sent": 233}, 233, True), ({"sent": 233}, 0, False),
        ):
            with self.subTest(fields=fields, stored=stored):
                source = SimpleNamespace(keyphone={"n0": dict(
                    skip="n0", go="n0", limit=1, after=1, source="phone", **fields)})
                pack = SimpleNamespace(
                    recs=lambda *_: [(0, 0, 0, 1, 1, stored, 0, 0)] * 24,
                    str_=lambda _: "phone")
                audit = auditor.Audit(source, pack)
                audit.node_id = lambda _: "n0"
                audit.c_keyphone()
                self.assertEqual(all(check["ok"] for check in audit.checks), valid)


if __name__ == "__main__":
    unittest.main()
