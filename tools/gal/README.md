<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Galgame Asset Pipeline

These tools turn the upstream visual novel
[`liuyuze61/Asunabi-miband`](https://github.com/liuyuze61/Asunabi-miband) into the
read-only `assets` flash partition consumed by `main/gal/*`.

That upstream repository declares no license, so its artwork and chapter scripts
are never committed here. The pack format, the tools and the generated fonts are
this repository's own work; the material they consume is not.

## Where the source material goes

The artwork and chapter scripts are third-party content and are **not tracked by
this repository**. Put a local copy of the upstream project's `src/common`
directory at:

```text
assets/gal-source/common/
├── bg/       background and effect artwork (PNG)
├── fg/       character bodies and expression patches (PNG)
├── *.txt     chapter scripts (JSON, one file per chapter, named 1..N)
└── text_bg.png, logo.png
```

That path is git-ignored. The layout matches the upstream project's `src/common`
directory, so a copy of it can be dropped in unchanged.

When the directory is absent the packer emits a small placeholder pack instead,
so a fresh clone still configures, builds and boots. The placeholder needs no
image library.

## Tools

| Tool | Purpose |
| --- | --- |
| `gal_format.py` | The single source of truth for the on-flash layout. Also generates `main/gal/gal_format.h`, which carries `_Static_assert` guards so the C and Python sides cannot drift silently. |
| `pack_assets.py` | Converts artwork and scripts into `gal_assets.bin`. |
| `gen_font.py` | Generates the CJK font subsets under `assets/fonts/`. |
| `inspect_pack.py` | Reads a pack back: layout, alignment, per-image accounting, chapter dump, single-image export. |

### Packing

```bash
python tools/gal/pack_assets.py --source assets/gal-source/common --out build/gal
python tools/gal/inspect_pack.py build/gal/gal_assets.bin
python tools/gal/inspect_pack.py build/gal/gal_assets.bin --chapter 3 --lines 4
python tools/gal/pack_assets.py --preview 8 --preview-out scene8.png
```

Packing the real artwork needs Pillow. Install it into the interpreter ESP-IDF
builds with, otherwise the firmware build stops at the packer:

```bash
"$IDF_PYTHON_ENV_PATH/Scripts/python.exe" -m pip install Pillow   # Windows
python -m pip install Pillow                                       # Linux/macOS
```

`--preview` composites one chapter's first illustrated scene at panel
resolution, which is how the dialogue panel geometry and the sprite origin were
chosen; use it before changing any layout constant.

### Fonts

```bash
python tools/gal/gen_font.py
```

The subset is the union of every codepoint in the packed scripts and every
non-ASCII character in `main/gal/gal_strings.h`, so a UI string can never fall
outside it. **Adding a Chinese literal anywhere else in the firmware renders as
placeholder boxes** -- put it in that header and regenerate.

The generated `.c` files are committed: CI has neither the converter nor the
source font, and a build must not depend on either. Source font and licence are
recorded in [`assets/README.md`](../../assets/README.md).

The converter is invoked through `node` on the installed script rather than the
`lv_font_conv` shim, because the shim routes arguments through `cmd.exe`, which
truncates a command line at about 8 KiB and rejects a CJK subset.

## Pack format

```text
gal_pack_header_t        magic 'GALA', version, image and chapter counts, offsets
gal_image_entry_t[]      one per image, including its screen offset
name blob                NUL-terminated image names
gal_chapter_entry_t[]    offset and length of each chapter
image payloads           [256 x lv_color32_t palette][one index byte per pixel]
chapter payloads         header + scene records + dialogue records + text pools
```

Points that matter when changing anything here:

- Images use LVGL's indexed `I8` layout. `LV_BIN_DECODER_RAM_LOAD` is off, so
  LVGL decodes one scan line at a time and a full-screen background costs about
  one scan line of RAM rather than a 150 KB frame buffer. This board has no PSRAM.
- Every offset the firmware casts to a struct is 4-byte aligned, and the reader
  rejects a pack that violates it. A misaligned struct load is undefined
  behaviour, and on this target the compiler is entitled to assume the alignment
  the type requires.
- Chapter text and speaker names are pooled per chapter and NUL-terminated.

## Screen geometry

The source is 336x480 and the panel is 240x320, so the packer scales by
`240/336` and drops the extra rows. Character art is cropped to the region that
can actually reach the panel before its alpha bounding box is taken; skipping
that crop more than doubles the sprite cost.

[`main/gal/gal_layout.h`](../../main/gal/gal_layout.h) is the source of truth for
the on-screen layout. The packer reads it back for `--preview`, so the composite
it renders cannot drift from what the firmware draws. The only geometry it keeps
its own copy of is the sprite origin, because that value is baked into the stored
sprite offsets.

The dialogue panel matches the Saya reference port: flat, fully opaque, full
width, a single 2 px accent rule along its top edge, and no rounding. The
upstream project's own panel artwork is not packed at all, because it is a
near-white plate carrying a repeating ornament that competes with the picture at
any usable opacity. Dropping it also saves 37 KiB.

Panel height is a measurement rather than a preference, and it is **derived from
the text size** rather than fixed: the panel is `4 x line_height + 6`, so a page of
four lines always fits whole at either size.

| text size | line height | panel height | panel top | artwork visible |
| --- | --- | --- | --- | --- |
| 16 px (small) | 19 px | 82 px | 238 | 214 px (67%) |
| 20 px (large) | 23 px | 98 px | 222 | 198 px (62%) |

At 16 px the 220 px text block fits 13 characters per line, so a page holds:

| text area | panel height | fits | dialogue lines that need a second page |
| --- | --- | --- | --- |
| 3 lines | ~64 px | 39 chars | 175 (3.76%) |
| **4 lines** | **82 px** | **52 chars** | **8 (0.17%)** |
| 5 lines | 100 px | 65 chars | 2 (0.04%) |
| 6 lines | ~118 px | 78 chars | 0 |

The panel runs to the bottom edge, where the display driver's rounded-corner mask
shapes it. At the default 16 px it is 82 px tall, about half the original 152 px.
This differs from the Saya reference port, which keeps its panel fixed and drops
to three lines per page at the larger size; here four lines are always shown,
which is what sizing the panel from the line height buys. The
eight lines that do not fit are **paginated, not clipped**: `gal_text_pages()`
splits them in the pure model, the reader shows one page at a time, and the page
number is part of the saved position. That is the same arrangement the Saya port
uses, including its rule that a closing mark is never left at the start of a
line. `units_per_line` is deliberately conservative (26 units for a 27.5 unit
block) so the model never places more on a line than LVGL can actually fit.

The speaker name sits outside the panel, in the lower-left of the artwork.

## Upstream defects

The upstream scripts reference eight images that its own repository does not
contain. Five are obvious typos. Left alone the original renderer draws a blank
frame, so the packer substitutes the closest existing asset and reports every
substitution:

```text
line2            -> line21           truncated filename
zev_ask_c03_01   -> ev_ask_c03_01    stray leading "z"
zev_ask_c03_11   -> ev_ask_c03_11    stray leading "z"
ev_ask_c03_03    -> ev_ask_c03_01    no such asset
ev_ask_c01_06    -> ev_ask_c01_03    no such asset
a0014h           -> a0015h           missing expression
ask_z2a0100      -> ask_z1a0100      missing body variant
ask_z2b0100      -> ask_z1b0100      missing body variant
```

A substituted reference keeps its own name in the script but resolves to the
borrowed asset, so the asset is stored once instead of twice. Change the table at
the top of `pack_assets.py` if you disagree with a substitution.

## Flash budget

The `assets` partition is 4 MiB (`partitions.csv`). The packer is given
`--max-bytes` by `main/CMakeLists.txt` and fails the build rather than producing
a truncated partition image, so the partition size and the pack size cannot drift
apart. The current pack is about 3.6 MiB.
