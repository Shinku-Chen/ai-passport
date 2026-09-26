#!/usr/bin/env python3
"""Every asset name the firmware looks up must be one the packer stores.

The galgame reader asks the pack for a handful of names by literal (the
`gal_assets_find` calls in `main/gal/gal_app.c`). Those names come from the
packer, so a mismatch is silent: the release that stored the title backdrop as
`index_bg.png` while the firmware asked for `bg/index_bg.png` booted into a black
title screen with the artwork sitting unused in the pack. The first release
shipped a placeholder pack, whose solid fills did carry the prefix, so nothing
noticed until real artwork was packed.

Neither the artwork nor Pillow is needed here: the tests compare the packer's
declarations with the firmware's literals.
"""

from pathlib import Path
import importlib.util
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
PACKER = ROOT / "tools" / "gal" / "pack_assets.py"
LOOKUP_RE = re.compile(r'gal_assets_find\s*\(\s*[^,]+,\s*"([^"]+)"')


def load_packer():
    """Import the packer without running it; it imports Pillow lazily."""
    spec = importlib.util.spec_from_file_location("gal_pack_assets", PACKER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def firmware_lookups() -> set[str]:
    names: set[str] = set()
    for path in sorted((ROOT / "main").rglob("*.c")):
        names.update(LOOKUP_RE.findall(path.read_text(encoding="utf-8")))
    return names


class GalAssetNames(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.packer = load_packer()
        cls.lookups = firmware_lookups()

    def test_the_firmware_still_asks_for_names(self) -> None:
        self.assertTrue(
            self.lookups,
            "no gal_assets_find literal found: the firmware's asset lookups moved, "
            "so this test no longer watches the contract it was written for",
        )

    def test_every_firmware_lookup_is_packed(self) -> None:
        missing = sorted(self.lookups - set(self.packer.UI_IMAGES))
        self.assertEqual(
            [], missing,
            "the firmware looks up names the packer does not pack, so it would fall "
            "back to a solid fill or a black screen",
        )

    def test_every_packed_ui_image_is_looked_up(self) -> None:
        unused = sorted(set(self.packer.UI_IMAGES) - self.lookups)
        self.assertEqual([], unused, "the packer carries UI images nothing looks up")

    def test_ui_images_are_names_the_placeholder_pack_also_uses(self) -> None:
        placeholder = {name for name, _ in self.packer.PLACEHOLDER_IMAGES}
        missing = sorted(set(self.packer.UI_IMAGES) - placeholder)
        self.assertEqual(
            [], missing,
            "the placeholder and the real pack must answer the same names; "
            "otherwise CI and a clone without artwork disagree about the title screen",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
