#!/usr/bin/env python3
"""Generate per-title LVGL 16/20px fonts and verify every emitted glyph.

Uses the repository's existing gal_font rasterizer/emitter, no Node conversion.
The read-only source subset font is preferred; --fallback supplies missing UI
glyphs (defaults to Windows Microsoft YaHei when installed). Source font files
are never copied into output.
"""
from __future__ import annotations

import argparse
import json

from pathlib import Path
from PIL import ImageFont
from fontTools.ttLib import TTFont

import gal_font


def cmap(path: Path) -> set[int]:
    with TTFont(path, fontNumber=0) as font:
        return set(font.getBestCmap())


def required_chars(out: Path, source_root: Path) -> set[str]:
    chars = set((out / "nine_symbols.txt").read_text("utf-8"))
    chars.update(gal_font.source_chars(str(source_root)))
    chars.update(map(chr, range(32, 127)))
    chars -= set("\r\n\t")
    return chars


def check(out: Path, source_root: Path) -> int:
    required = required_chars(out, source_root)
    missing_total = 0
    for size in (16, 20):
        path = out / f"nine_cjk_{size}.c"
        if not path.is_file():
            raise SystemExit(f"missing {path}")
        font = gal_font.parse_font(str(path))
        missing = sorted(ch for ch in required
                         if gal_font.lookup_glyph_id(font, ord(ch)) == 0)
        missing_total += len(missing)
        print(f"{size}px: {len(required)} chars, missing={len(missing)}, "
              f"line_height={font['line_height']}")
        if missing:
            print("  " + ",".join(f"U+{ord(ch):04X}" for ch in missing[:20]))
    if missing_total:
        raise SystemExit("nine font coverage failed")
    print("nine font coverage: PASS")
    return 0


def generate(args) -> int:
    chars = required_chars(args.out, args.source_root)
    cps = sorted(map(ord, chars))
    primary = cmap(args.font)
    fallback = cmap(args.fallback) if args.fallback.is_file() else set()
    missing = set(cps) - primary - fallback
    if missing:
        raise SystemExit("Missing font codepoints: " +
                         ",".join(f"U+{cp:04X}" for cp in sorted(missing)))
    report = {"glyphs": len(cps),
              "fallbackGlyphs": len(set(cps) - primary), "sizes": {}}
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "nine_symbols.txt").write_text(
        "".join(map(chr, cps)), encoding="utf-8")
    for size in (16, 20):
        glyphs = gal_font.rasterize(
            str(args.font), size, [cp for cp in cps if cp in primary])
        fallback_cps = [cp for cp in cps if cp not in primary]
        if fallback_cps:
            glyphs.update(gal_font.rasterize(
                str(args.fallback), size, fallback_cps))
        font = ImageFont.truetype(str(args.font), size,
                                  layout_engine=ImageFont.Layout.BASIC)
        asc, desc = font.getmetrics()
        height = size + gal_font.EXTRA_LEADING
        baseline = asc - (asc + desc - height) // 2
        name = f"nine_cjk_{size}"
        path = args.out / f"{name}.c"
        path.write_text(gal_font.emit_font(name, size, cps, glyphs, height,
                                           height - baseline), encoding="utf-8")
        if gal_font.verify(str(path), cps, glyphs):
            raise SystemExit("Emitted glyph verification failed")
        compiled = gal_font.parse_font(str(path))
        bitmap = len(compiled["bitmap"])
        report["sizes"][str(size)] = {
            "bitmapBytes": bitmap,
            "estimatedCompiledBytes": bitmap + (len(cps) + 1) * 16 + len(cps) * 2,
            "sourceBytes": path.stat().st_size,
            "missingGlyphs": 0,
            "verifiedGlyphs": len(cps),
        }
    (args.out / "nine_font_report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path)
    parser.add_argument("--fallback", type=Path,
                        default=Path("C:/Windows/Fonts/msyh.ttc"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=Path("main"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        return check(args.out, args.source_root)
    if not args.font:
        raise SystemExit("--font is required unless --check is used")
    return generate(args)


if __name__ == "__main__":
    raise SystemExit(main())
