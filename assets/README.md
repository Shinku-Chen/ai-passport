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

| File | Format | Use, source and license |
| --- | --- | --- |
| [`fonts/gal_font_16.c`](fonts/gal_font_16.c), [`fonts/gal_font_20.c`](fonts/gal_font_20.c) | Generated LVGL `lv_font_t` sources, 4 bpp | Body and speaker text for the galgame in [`main/gal/`](../main/gal/). Generated from **Noto Sans SC** (`NotoSansSC-VF.ttf`, SIL Open Font License 1.1, which permits redistribution of the derived bitmap font). Neither the variable font nor the converter is needed to build: the generated sources are committed. |

Coverage is the union of every codepoint in the packed chapter scripts and every
non-ASCII character in [`main/gal/gal_strings.h`](../main/gal/gal_strings.h) --
2096 codepoints plus printable ASCII. Regenerate after changing either input:

```bash
python tools/gal/gen_font.py
```

Flash cost is about 87 KiB (16 px) plus 120 KiB (20 px) of bitmaps and glyph
descriptors, in read-only data. Nothing is copied to RAM at runtime; see
[`tools/gal/README.md`](../tools/gal/README.md) for the whole pipeline.

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
