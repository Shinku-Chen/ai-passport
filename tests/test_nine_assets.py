#!/usr/bin/env python3
"""Static asset/report contract for the committed 9-nine- build inputs.

Does not need the external source checkout: it validates the generated pack,
the pack report binding, and the generated font reports. The deeper source
audit is tools/nine_audit.py and needs the pinned source.
"""
from __future__ import annotations
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / 'assets/nine/kokoiro'
SOURCE_COMMIT = '6a8907a5ae576f4377abe4b74f15bd9af13efe07'
NONE = 0xffffffff
fails = 0


def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print('FAIL:', msg)


def main():
    script = (PACK / 'nine_script.bin').read_bytes()
    images = (PACK / 'nine_images.bin').read_bytes()
    report = json.loads((PACK / 'nine_pack_report.json').read_text('utf-8'))
    check(script[:8] == b'NINESC1\0', 'script magic')
    check(images[:8] == b'NINEIM1\0', 'image magic')
    check(struct.unpack_from('<I', script, 8)[0] == 2, 'script version')
    check(struct.unpack_from('<I', images, 8)[0] == 2, 'image version')
    check(struct.unpack_from('<I', script, 12)[0] == len(script), 'script size')
    check(struct.unpack_from('<I', images, 12)[0] == len(images), 'image size')
    zero = script[:16] + b'\0\0\0\0' + script[20:]
    check((zlib.crc32(zero) & 0xffffffff) == struct.unpack_from('<I', script, 16)[0],
          'script CRC')
    zero = images[:16] + b'\0\0\0\0' + images[20:]
    check((zlib.crc32(zero) & 0xffffffff) == struct.unpack_from('<I', images, 16)[0],
          'image CRC')
    check(report['sourceCommit'] == SOURCE_COMMIT, 'pinned source commit')
    check(report['scriptBytes'] == len(script), 'report script bytes')
    check(report['imageMediaBytes'] == len(images), 'report image bytes')
    check(report['scriptSha256'] == hashlib.sha256(script).hexdigest(), 'report script sha')
    check(report['imageSha256'] == hashlib.sha256(images).hexdigest(), 'report image sha')
    check(report['nodes'] == 74, '74 nodes')
    check(report['frames'] == 8050, '8050 frames')
    check(report['textFrames'] == 8049, '8049 text frames')
    check(report['textBytes'] == 395919, 'source text bytes')
    check(report['variables'] == 15, '15 variables')
    check(report['chapters'] == 17, '17 chapters')
    check(report['endings'] == 2, '2 endings')
    check(report['galleryGroups'] == 22, '22 CG groups')
    check(report['galleryVariants'] == 73, '73 CG variants')
    check(report['opening']['sourceFrames'] == 599 and report['opening']['outputFrames'] == 300,
          'OP frame conversion is declared')
    check(report['opening']['durationMs'] == 99910, 'OP duration')
    check(report['audio'][0]['name'] == 'title' and report['audio'][0]['rate'] == 8000,
          'title audio')
    check(report['audio'][1]['name'] == 'opening' and report['audio'][1]['bytes'] > 0,
          'opening audio')
    check(report['imageKinds']['raw565'] >= 25, 'raw title/thumbnail assets')

    # Every image payload CRC and indexed block decompresses to the declared size.
    icount, ioff = struct.unpack_from('<2I', images, 20)
    check(icount == report['images'], 'image count matches report')
    for i in range(icount):
        kind, w, h, off, size, crc, _, _ = struct.unpack_from('<8I', images, ioff + i * 32)
        payload = images[off:off + size]
        check((zlib.crc32(payload) & 0xffffffff) == crc, 'image CRC %d' % i)
        if kind == 1:
            pal, rows, blocks = struct.unpack_from('<3I', payload, 0)
            check(blocks == (h + rows - 1) // rows, 'indexed blocks %d' % i)
            table = 12 + pal * 4
            for k in range(blocks):
                boff, blen = struct.unpack_from('<2I', payload, table + k * 8)
                raw = zlib.decompress(images[boff:boff + blen])
                check(len(raw) == w * min(rows, h - k * rows), 'indexed raw %d/%d' % (i, k))


    symbols = (PACK / 'nine_symbols.txt').read_text('utf-8')
    # Every character that can reach the 16/20px fonts must be in the symbol set
    # (the generator verifies each symbol's glyph pixels). This covers all story
    # text, speakers, choices, chapter/ending/gallery names without the source.
    str_off, str_size = struct.unpack_from('<2I', script, 132)
    def pstr(off):
        end = script.index(bytes([0]), str_off + off, str_off + str_size)
        return script[str_off + off:end].decode('utf-8')
    fcount, foff = struct.unpack_from('<2I', script, 44)
    bcount, boff = struct.unpack_from('<2I', script, 52)
    chars = set()
    for i in range(fcount):
        kind, visual, block, toff, tlen, speaker = struct.unpack_from('<6I', script, foff + i * 24)
        if kind == 0:
            bo, bc, br, _ = struct.unpack_from('<4I', script, boff + block * 16)
            raw = zlib.decompress(script[bo:bo + bc])
            check(len(raw) == br, 'text block raw %d' % block)
            chars.update(raw[toff:toff + tlen].decode('utf-8'))
        chars.update(pstr(speaker))
    ecount, eoff = struct.unpack_from('<2I', script, 100)
    for i in range(ecount):
        to, _, _, _, _, label = struct.unpack_from('<6I', script, eoff + i * 24)
        chars.update(pstr(label))
    ccount, coff = struct.unpack_from('<2I', script, 108)
    for i in range(ccount):
        title, _ = struct.unpack_from('<2I', script, coff + i * 8)
        chars.update(pstr(title))
    ncount, noff = struct.unpack_from('<2I', script, 116)
    for i in range(ncount):
        name, _, desc = struct.unpack_from('<3I', script, noff + i * 12)
        chars.update(pstr(name)); chars.update(pstr(desc))
    gcount, goff = struct.unpack_from('<2I', script, 124)
    for i in range(gcount):
        name, _, _, _ = struct.unpack_from('<4I', script, goff + i * 16)
        chars.update(pstr(name))
    forbidden = set(map(chr, (13, 10, 9)))
    # UI strings/error messages live in main/*.c; include their literal characters
    # so the gate fails on a missing glyph even without the external source.
    sys.path.insert(0, str(ROOT / 'tools'))
    from gal_font import source_chars
    chars |= source_chars(str(ROOT / 'main'))
    missing = sorted(c for c in chars if c not in forbidden and c not in symbols)
    check(not missing, 'story/UI glyph coverage: ' + ''.join(missing[:20]))

    font = json.loads((PACK / 'nine_font_report.json').read_text('utf-8'))
    check(font['glyphs'] == len(symbols) == 2182, 'font glyph count')
    check(font['fallbackGlyphs'] >= 0, 'fallback count present')
    for size in ('16', '20'):
        check(font['sizes'][size]['missingGlyphs'] == 0, 'missing glyphs %s' % size)
        check(font['sizes'][size]['verifiedGlyphs'] == font['glyphs'], 'verified %s' % size)
        check((PACK / ('nine_cjk_%s.c' % size)).is_file(), 'font C %s' % size)
    # ASCII is required for the UI (digits, %, /, ...).
    for cp in range(32, 127):
        check(chr(cp) in symbols, 'ASCII U+%04X' % cp)

    print('fails=%d' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
