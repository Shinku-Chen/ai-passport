<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

[`fonts/tsxx_symbols.txt`](fonts/tsxx_symbols.txt) is the character inventory of the
*Tenshi☆Sousou RE-BOOT!* reader on `feature/tsxx-reboot`: 3,417 code points, in the exact
frequency order the resource pack uses for its 1-byte symbol codes. `tools/tsxx_pack.py
--symbols-out` regenerates it; the font subsetter consumes it. The font itself is a
build-time artifact and is not committed here.

## Tenshi☆Sousou RE-BOOT! reader - `tsxx-source/`

Base source material for the visual-novel reader on `feature/tsxx-reboot`, committed
verbatim (22.0 MB, 1,065 files) so a clone can rebuild the resource pack without an
external checkout. It comes from the Xiaomi Band quick-app fan port
[`hezdaaa/tsxxreboot-miband`](https://github.com/hezdaaa/tsxxreboot-miband):

| Directory | Files | Content |
| --- | ---: | --- |
| `script/` | 123 | `scriptData<number>.txt`, the linear page table (61,436 pages) |
| `bcgi/` | 120 | Backgrounds, 336 × 480 JPEG |
| `cimg/` | 185 | Full-height character sprites, PNG with alpha (178 × 715 up to 396 × 649) |
| `evig/` | 637 | Event CGs and scene props, 336 × 480 |

That upstream project declares no license. The script, character art, event CGs and
backgrounds are the property of Yuzusoft and the original publisher; the directory is
kept as the single source of attribution, the pack records the same provenance in its
metadata section, and the fan port is credited as the source of the conversion. The
project is a non-commercial technical study, and a reader needs a legitimately purchased
copy of the original work to make use of it.

The directory mirrors the four subdirectories of the upstream `src/common/`, which is the
layout `tools/tsxx_pack.py` accepts directly:

```bash
# what main/tsxx_data/tsxx_pack.bin contains
python tools/tsxx_pack.py --source assets/tsxx-source --out main/tsxx_data/tsxx_pack.bin \
    --bg-quality 65 --sprite-quality 68 --event-quality 62 --event-width 180 \
    --symbols-out assets/fonts/tsxx_symbols.txt
```

Rebuilding with the same command reproduces the committed
`main/tsxx_data/tsxx_pack.bin` byte for byte. The pack is 6.58 MiB; the 180 px event
width is what keeps it inside the 8 MB Flash budget, and the firmware scales event
frames up to 240 × 320 when it draws them.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
