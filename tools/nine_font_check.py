#!/usr/bin/env python3
"""Offline font coverage gate for the 9-nine- reader.

Checks that the generated 16/20px LVGL fonts actually contain a glyph for every
character in (a) the package's story text and (b) the font symbol inventory
(which the font tool builds from main/ string literals). Does not need the
source OTF, so it can run in a clean checkout.

Usage: python3 tools/nine_font_check.py [--pack assets/nine/haruiro] [--fonts assets/fonts]
"""
from __future__ import annotations
import argparse, struct, sys, zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gal_font  # noqa: E402


def story_text(pack: Path) -> set[str]:
    s = (pack / 'nine_script.bin').read_bytes()
    rd = lambda o: struct.unpack_from('<I', s, o)[0]
    frames, blocks = rd(44), rd(48)
    block_off = rd(56)
    cache = {}
    chars = set()
    for i in range(frames):
        p = rd(48) + i * 24
        kind = rd(p)
        if kind != 0:
            continue
        block = rd(p + 8)
        if block not in cache:
            bp = block_off + block * 16
            off, comp = rd(bp), rd(bp + 4)
            cache[block] = zlib.decompress(s[off:off + comp])
        data = cache[block]
        off = rd(p + 12)
        ln = struct.unpack_from('<H', s, p + 16)[0]
        chars.update(data[off:off + ln].decode('utf-8'))
    return chars - set('\r\n\t')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pack', type=Path, default=Path('assets/nine/haruiro'))
    ap.add_argument('--fonts', type=Path, default=Path('assets/fonts'))
    args = ap.parse_args()
    symbols = set((args.fonts / 'nine_symbols.txt').read_text('utf-8'))
    pack_symbols = set((args.pack / 'nine_symbols.txt').read_text('utf-8'))
    missing = pack_symbols - symbols
    if missing:
        raise SystemExit(f'pack symbols missing from font inventory: '
                         f'{"".join(sorted(missing))!r}')
    cps = sorted(ord(c) for c in symbols if c not in '\r\n\t')
    for size in (16, 20):
        path = args.fonts / f'nine_cjk_{size}.c'
        font = gal_font.parse_font(str(path))
        absent = [cp for cp in cps if gal_font.lookup_glyph_id(font, cp) == 0]
        if absent:
            raise SystemExit(f'{size}px missing U+' +
                             ' U+'.join(f'{cp:04X}' for cp in absent[:10]))
    text = story_text(args.pack)
    absent = text - symbols
    if absent:
        raise SystemExit(f'story characters outside the font: {len(absent)}')
    print(f'PASS font: {len(cps)} codepoints in 16/20px, {len(text)} story characters covered')
    return 0


if __name__ == '__main__':
    sys.exit(main())
