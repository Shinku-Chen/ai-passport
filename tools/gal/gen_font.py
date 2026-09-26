#!/usr/bin/env python3
"""Generate the Chinese font subsets used by the galgame reader.

The readable text is the union of:
  * every codepoint the chapter scripts use (build/gal/gal_charset.txt), and
  * every non-ASCII codepoint in main/gal/gal_strings.h,

so menu labels can never fall outside the subset. A UI string added without
regenerating this font renders as placeholder boxes, which is why the strings
live in one scanned header rather than inline in the UI code.

The generated `.c` files are committed: CI has neither the font converter nor the
source OTF, and a build must not depend on either. Regenerate only when the
scripts or the UI strings change, and commit the result.

Usage:
    python tools/gal/gen_font.py \
        --charset build/gal/gal_charset.txt \
        --font "C:/Windows/Fonts/NotoSansSC-VF.ttf"

Noto Sans SC is used under the SIL Open Font License 1.1, which permits
redistribution of the derived bitmap font.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

# Sizes the reader offers. Each is a separate subset, so adding one costs flash.
SIZES = (16, 20)

BPP = 4
ASCII = (0x20, 0x7E)


def codepoints_from_charset(path: pathlib.Path) -> set[int]:
    text = path.read_text(encoding="utf-8")
    return {ord(ch) for ch in text}


def codepoints_from_header(path: pathlib.Path) -> set[int]:
    """Every non-ASCII character inside the header's string literals.

    Scanning the whole file is intentional: the file is a table of UI strings and
    single-line comments, and any Chinese that appears there is either a string or
    documentation of one. ASCII is covered by the fixed range instead.
    """
    text = path.read_text(encoding="utf-8")
    return {ord(ch) for ch in text if ord(ch) > 0x7E}


def build_symbols(codepoints: set[int]) -> str:
    """The exact character set, as a string.

    `--symbols` is far more compact than range notation for Chinese: 2096
    scattered codepoints need ~14.5 KiB of ranges but only ~6 KiB of characters,
    which keeps the argument clear of Windows' command-line limits.
    """
    # ASCII is always included and is cheaper to express as one range.
    return "".join(chr(value) for value in sorted(codepoints) if value > ASCII[1])


def find_converter(explicit: str | None) -> list[str]:
    """Resolve a way to run lv_font_conv.

    Prefers invoking the JavaScript entry point with `node` directly: the
    installed `.cmd` shim routes arguments through cmd.exe, which truncates a
    command line at ~8 KiB and would reject a CJK subset.
    """
    if explicit:
        return explicit.split()

    node = shutil.which("node") or shutil.which("node.exe")
    if node:
        npm = shutil.which("npm") or shutil.which("npm.cmd")
        if npm:
            probe = subprocess.run([npm, "root", "-g"], capture_output=True, text=True)
            if probe.returncode == 0:
                entry = pathlib.Path(probe.stdout.strip()) / "lv_font_conv" / "lv_font_conv.js"
                if entry.is_file():
                    return [node, str(entry)]

    for candidate in ("lv_font_conv", "lv_font_conv.cmd"):
        on_path = shutil.which(candidate)
        if on_path:
            return [on_path]

    for candidate in ("npx", "npx.cmd"):
        npx = shutil.which(candidate)
        if npx is None:
            continue
        probe = subprocess.run([npx, "--no-install", "lv_font_conv", "--version"],
                               capture_output=True, text=True)
        if probe.returncode == 0:
            return [npx, "--no-install", "lv_font_conv"]

    raise SystemExit(
        "lv_font_conv not found; install it (npm install -g lv_font_conv@1.5.3) "
        "or pass --lv-font-conv")


def generate(converter: list[str], font: pathlib.Path, symbols: str, size: int,
             output: pathlib.Path, name: str) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    command = converter + [
        "--font", str(font),
        "--range", f"0x{ASCII[0]:X}-0x{ASCII[1]:X}",
        "--symbols", symbols,
        "--size", str(size),
        "--bpp", str(BPP),
        "--format", "lvgl",
        "--no-compress",
        "--lv-font-name", name,
        "--lv-include", "lvgl.h",
        "--output", str(output),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"lv_font_conv failed for size {size}")
    # lv_font_conv reports glyphs it could not find; those would render as boxes.
    for line in (result.stdout + result.stderr).splitlines():
        if "missing" in line.lower() or "not found" in line.lower():
            print(f"  warning (size {size}): {line.strip()}")


def check_no_stray_chinese(source_dir: pathlib.Path, allowed: pathlib.Path) -> None:
    """Fail if a firmware source outside the strings header carries Chinese.

    Coverage is the scripts plus gal_strings.h, so a Chinese literal anywhere else
    compiles happily and then renders as placeholder boxes on the device. Comments
    are stripped first because only string literals reach the display.
    """
    offenders = []
    for path in sorted(source_dir.glob("*.[ch]")):
        if path.resolve() == allowed.resolve():
            continue
        text = path.read_text(encoding="utf-8")
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
            if any(ord(ch) > 0x7E for ch in literal):
                offenders.append(f"{path.name}: {literal}")
    if offenders:
        raise SystemExit(
            f"Chinese string literals outside {allowed.name} would not be covered "
            "by the generated font subset:\n  " + "\n  ".join(offenders))


def main() -> int:
    parser = argparse.ArgumentParser(description="generate the galgame font subsets")
    parser.add_argument("--charset", default="build/gal/gal_charset.txt")
    parser.add_argument("--strings", default="main/gal/gal_strings.h")
    parser.add_argument("--font", default="C:/Windows/Fonts/NotoSansSC-VF.ttf",
                        help="licensed CJK source font (SIL OFL)")
    parser.add_argument("--out-dir", default="assets/fonts")
    parser.add_argument("--lv-font-conv", default=None,
                        help="override the converter command, e.g. 'npx --no-install lv_font_conv'")
    args = parser.parse_args()

    charset_path = pathlib.Path(args.charset)
    if not charset_path.is_file():
        raise SystemExit(f"{charset_path} not found; run tools/gal/pack_assets.py first")

    font = pathlib.Path(args.font)
    if not font.is_file():
        raise SystemExit(f"source font {font} not found")

    codepoints = codepoints_from_charset(charset_path)
    strings_path = pathlib.Path(args.strings)
    codepoints |= codepoints_from_header(strings_path)
    check_no_stray_chinese(strings_path.parent, strings_path)
    symbols = build_symbols(codepoints)
    converter = find_converter(args.lv_font_conv)

    print(f"font subset: {len(codepoints)} codepoints + ASCII ({len(symbols)} symbols)")
    total = 0
    for size in SIZES:
        name = f"gal_font_{size}"
        output = pathlib.Path(args.out_dir) / f"{name}.c"
        generate(converter, font, symbols, size, output, name)
        written = output.stat().st_size
        total += written
        print(f"  {name}: {written / 1024:.0f} KiB of C source -> {output}")
    print(f"total generated source: {total / 1048576:.2f} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
