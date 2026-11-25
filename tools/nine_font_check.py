#!/usr/bin/env python3
"""Coverage check for the per-title 16/20px fonts (nine_cjk_*)."""
from __future__ import annotations
import argparse
import sys
import unicodedata
from pathlib import Path

import gal_font


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--source-root', type=Path, default=Path('main'))
    a = ap.parse_args()
    chars = set((a.out / 'nine_symbols.txt').read_text('utf-8'))
    chars.update(gal_font.source_chars(str(a.source_root)))
    chars.update(map(chr, range(32, 127)))
    chars -= set('\r\n\t')
    cps = sorted(map(ord, chars))
    ok = True
    for size in (16, 20):
        path = a.out / f'nine_cjk_{size}.c'
        if not path.exists():
            print(f'FAIL: missing {path}')
            ok = False
            continue
        font = gal_font.parse_font(str(path))
        missing = [cp for cp in cps if not gal_font.lookup_glyph_id(font, cp)]
        blank = []
        for cp in cps:
            if cp <= 0x20 or unicodedata.category(chr(cp)).startswith('Z'):
                continue
            got = gal_font.decode_glyph(font, cp)
            if got and not any(got[5]):
                blank.append(cp)
        print(f'{size}px: {len(cps)} codepoints, missing {len(missing)}, blank {len(blank)}')
        if missing:
            print('  missing:', ' '.join(f'U+{cp:04X}' for cp in missing[:20]))
            ok = False
        if blank:
            print('  blank:', ' '.join(f'U+{cp:04X}' for cp in blank[:20]))
            ok = False
    print('PASS: nine font coverage' if ok else 'FAIL: nine font coverage')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
