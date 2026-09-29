<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

| File | Format and size | Use and source |
| --- | --- | --- |
| [`fonts/intercom_cjk_16.c`](fonts/intercom_cjk_16.c) | LVGL 9 C font source, 16 px, 4 bpp, PLAIN (uncompressed) bitmaps, 7540 glyphs, 900,691 bytes of linked bitmap data; 6,622,442 bytes (6.32 MiB) of source | Complete GB2312 Chinese coverage for the intercom device screen. Rasterised with Pillow (FreeType) from Source Han Sans SC Normal (SIL OFL 1.1); regenerate with `tools/intercom_font.py`. |
| [`fonts/intercom_cjk_symbols.txt`](fonts/intercom_cjk_symbols.txt) | Code-point list, one `U+XXXX` per line, ascending and unique; 52,780 bytes | The 7540 covered code points, so a copy-coverage guard can compare against the font without parsing the `.c`. Regenerated together with the font. |

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### GB2312 Chinese font (`lv_font_intercom_cjk_16`)

- Generation command: `python3 tools/intercom_font.py` from the repository root (`--check` re-verifies an existing artifact without regenerating). The script rasterises with Pillow/FreeType and writes the LVGL 9 `fmt_txt` format itself:

```bash
python3 tools/intercom_font.py                 # generate, then compare pixel by pixel
python3 tools/intercom_font.py --check         # verify the committed artifact only
python3 tools/intercom_font.py --font <static CJK font>
```

- Why a hand-written rasteriser instead of `lv_font_conv`: that tool stopped at 1.5.3 (2021) and, under Node 24, writes 4 bpp bitmaps that the LVGL 9 PLAIN decoder reads back as noise, which shows up as garbled text on the device. The generator is ported from the `feature/sanoba-witch` branch (`tools/sanoba_font.py`), which validated the same approach on device; the two branch-specific parts (character set and UI constants) are the intercom's own.
- Source and license: the glyphs are rendered from `SourceHanSansSC-Normal.otf` (Source Han Sans SC Normal), published under the SIL Open Font License 1.1, which permits redistribution and embedding. Auto-detection prefers the copy shipped with the LVGL component, `managed_components/lvgl__lvgl/scripts/generators/built_in_font/SourceHanSansSC-Normal.otf`; the font file itself is never committed. Do not substitute a Windows-bundled field font such as SimHei or Microsoft YaHei — a bitmap table derived from them cannot be published with the firmware — and do not switch to the variable `NotoSansSC-VF.ttf`, whose too-light default instance is what made the previous 14 px / 2 bpp table look blurry on screen.
- Character inventory: 7540 code points requested, all 7540 glyphs generated (the source font covers all of them). It covers the complete GB2312 character set, that is 6763 Han characters (levels 1 and 2, rows 0xB0-0xF7) plus 682 symbols (rows 0xA1-0xA9: Chinese punctuation, full-width ASCII U+FF01-U+FF5E, units and currency signs, arrows, box drawing, circled numbers, Greek and Cyrillic letters), and 95 printable ASCII code points U+0020-U+007E for status lines and hints. `fonts/intercom_cjk_symbols.txt` lists the same set.
- Format pinned for this repository: 16 px, 4 bpp, PLAIN (uncompressed) bitmaps, `line_height = 20`, `base_line = 3`. Glyphs are packed as continuous nibbles with every glyph byte-aligned, so `stride` stays 0; ASCII uses the compact `FORMAT0_TINY` cmap and all other code points the `SPARSE_TINY` cmap. `main/oc_ui.c` is laid out for the resulting 20 px line box.
- Destination and integration: `main/CMakeLists.txt` compiles the generated source with `target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/intercom_cjk_16.c")`, and `main/oc_ui.c` declares the symbol with `LV_FONT_DECLARE(lv_font_intercom_cjk_16)`. No header is generated; the `.c` is self-contained and includes only `"lvgl.h"`.
- Flash and RAM: the bitmap payload is 900,691 bytes (880 KiB) and is linked as read-only data in Flash (DROM); the 6.32 MiB source text is build input, not image content. Measured cost of this change: the application image grew from 1,669,472 bytes (1.59 MiB) to 2,218,384 bytes (2.12 MiB), that is +548,912 bytes, and 73% of the 8 MB application partition is still free. LVGL decodes individual glyph bitmaps on demand, so the table is not loaded into internal RAM wholesale. This is still significant on an ESP32-C3 with no PSRAM, so re-check `idf.py size-components` after changing the size or bpp.
- Verification: `python3 tools/intercom_font.py --check` re-rasterises the source font and compares every glyph pixel by pixel against the committed `.c`; it also checks the exported symbol, the glyph count, the PLAIN bitmap format, the ASCII `FORMAT0_TINY` range, the line height and the code-point coverage. The regenerated font was additionally built into the firmware with ESP-IDF 5.5.3 (`idf.py -B build build`).

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
