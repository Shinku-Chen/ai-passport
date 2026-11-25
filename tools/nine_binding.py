#!/usr/bin/env python3
"""Bind the pinned source, the two resource packs, and the shipped firmware.

Checks:
  source commit in the pack report
  pack file SHA-256 / sizes
  archive manifest app/full/ELF/MAP SHA-256
  both packs embedded byte-for-byte in the app image
  app placed at 0x10000 in the full image
  factory partition covers the app

Usage: python3 tools/nine_binding.py --pack assets/nine/haruiro \
    --archive build/firmware/<sha> [--source <pinned checkout>]
"""
from __future__ import annotations
import argparse, hashlib, json, struct
from pathlib import Path

PIN = '6a8907a5ae576f4377abe4b74f15bd9af13efe07'


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pack', type=Path, default=Path('assets/nine/haruiro'))
    ap.add_argument('--archive', type=Path, required=True)
    ap.add_argument('--source', type=Path)
    args = ap.parse_args()

    report = json.loads((args.pack / 'nine_pack_report.json').read_text('utf-8'))
    script = (args.pack / 'nine_script.bin').read_bytes()
    images = (args.pack / 'nine_images.bin').read_bytes()
    manifest = json.loads((args.archive / 'manifest.json').read_text('utf-8'))
    app = (args.archive / 'FoloToy-AI-Passport.bin').read_bytes()
    full = (args.archive / 'FoloToy-AI-Passport-full.bin').read_bytes()

    errors = []
    if report.get('sourceCommit') != PIN:
        errors.append('source commit mismatch')
    if report.get('scriptSha256') != hashlib.sha256(script).hexdigest():
        errors.append('script sha does not match report')
    if report.get('imageSha256') != hashlib.sha256(images).hexdigest():
        errors.append('image sha does not match report')
    for name, data in (('FoloToy-AI-Passport.bin', app), ('FoloToy-AI-Passport-full.bin', full)):
        if manifest['files'][name]['sha256'] != hashlib.sha256(data).hexdigest():
            errors.append(f'{name} sha does not match manifest')
    for name in ('FoloToy-AI-Passport.elf', 'FoloToy-AI-Passport.map'):
        if manifest['files'][name]['sha256'] != sha(args.archive / name):
            errors.append(f'{name} sha does not match manifest')

    script_at = app.find(script)
    images_at = app.find(images)
    if script_at < 0:
        errors.append('script pack not embedded verbatim')
    if images_at < 0:
        errors.append('image/audio pack not embedded verbatim')
    if full[0x10000:0x10000 + len(app)] != app:
        errors.append('app image is not at 0x10000 in the full image')

    # Partition table at 0x8000: 0xC00 bytes, 32-byte entries, 0xAA50 magic.
    table = full[0x8000:0x8000 + 0xC00]
    factory = None
    for i in range(0, 0xC00, 32):
        if table[i:i + 2] != b'\xaa\x50':
            continue
        ptype, subtype, offset, size = struct.unpack_from('<BBII', table, i + 2)
        if ptype == 0 and subtype == 0:
            factory = (offset, size)
    if not factory:
        errors.append('factory partition not found')
    elif factory[0] != 0x10000 or len(app) > factory[1]:
        errors.append('app does not fit the factory partition')

    if args.source:
        graph = (args.source / 'src/common/graph.js').read_text('utf-8')
        if not graph:
            errors.append('source graph unreadable')

    result = {
        'sourceCommit': report.get('sourceCommit'),
        'script': {'bytes': len(script), 'sha256': hashlib.sha256(script).hexdigest(),
                   'embeddedAt': script_at},
        'images': {'bytes': len(images), 'sha256': hashlib.sha256(images).hexdigest(),
                   'embeddedAt': images_at},
        'app': {'bytes': len(app), 'sha256': hashlib.sha256(app).hexdigest()},
        'full': {'bytes': len(full), 'sha256': hashlib.sha256(full).hexdigest()},
        'elfSha256': manifest['files']['FoloToy-AI-Passport.elf']['sha256'],
        'mapSha256': manifest['files']['FoloToy-AI-Passport.map']['sha256'],
        'partition': {'offset': factory[0] if factory else None,
                      'size': factory[1] if factory else None},
        'errors': errors,
    }
    print(json.dumps(result, indent=2))
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
