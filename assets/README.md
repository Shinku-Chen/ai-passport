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
| [`../main/fonts/gb2312_14.c`](../main/fonts/gb2312_14.c) and [`gb2312_14.h`](../main/fonts/gb2312_14.h) | LVGL 9 C font source, 14 px, 2 bpp, PLAIN (uncompressed) bitmaps, 7540 glyphs; 3,073,296 bytes (2.93 MiB) and 659 bytes | Complete GB2312 Chinese coverage for the Chinese device screen. Generated with `lv_font_conv` 1.5.3 from Noto Sans SC (SIL OFL 1.1); regenerate with `tools/gen_chinese_font.py`. |

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### GB2312 Chinese font (`lv_font_gb2312_14`)

- Generation command: `python3 tools/gen_chinese_font.py` from the repository root (add `--verify-only` to re-check an existing artifact without converting). The script runs the pinned converter as:

```bash
lv_font_conv --font C:/Windows/Fonts/NotoSansSC-VF.ttf --size 14 --bpp 2 \
  --format lvgl --no-compress --no-prefilter --lv-include lvgl.h \
  --symbols <the 7540 code points listed by the script> \
  --lv-font-name lv_font_gb2312_14 --output main/fonts/gb2312_14.c
```

- Tool version: `lv_font_conv` 1.5.3 (global npm install, `node` 24.18.0). The complete option set is also recorded in the `Opts:` header comment of the generated `.c` file.
- Character inventory: 7540 code points requested, all 7540 glyphs generated. It covers the complete GB2312 character set, that is 6763 Han characters (levels 1 and 2, rows 0xB0-0xF7) plus 682 symbols (rows 0xA1-0xA9: Chinese punctuation, full-width ASCII U+FF01-U+FF5E, units and currency signs, arrows, box drawing, circled numbers, Greek and Cyrillic letters), and 95 printable ASCII code points U+0020-U+007E for status lines and hints.
- Options pinned for this repository: `--no-compress --no-prefilter` keep `bitmap_format = 0` (PLAIN), because the RLE stream that lv_font_conv 1.5.3 emits by default is not decoded by this LVGL 9.6 configuration. `--lv-include lvgl.h` makes both include branches resolve to `#include "lvgl.h"`, matching how `main` includes LVGL: the `lvgl/lvgl.h` form has no matching include directory because the managed component directory is `lvgl__lvgl`, and `main` does not define `LV_LVGL_H_INCLUDE_SIMPLE`.
- Destination and integration: the generated pair lives in `main/fonts/` next to the application that compiles it. `main/fonts/gb2312_14.c` must be listed in the `main` component's `SRCS`, and consumers declare the symbol with `LV_FONT_DECLARE(lv_font_gb2312_14)`. Regenerating at another size produces `gb2312_<size>.c` with the symbol `lv_font_gb2312_<size>`.
- Source and license: the glyphs are rendered from `C:/Windows/Fonts/NotoSansSC-VF.ttf` (Noto Sans SC), which is published under the SIL Open Font License 1.1 and may be redistributed and embedded; the TTF itself is not committed. The font file with the same name is installed by Microsoft Windows or can be downloaded from the Noto project. Do not substitute a field font such as SimHei or Microsoft YaHei: those are licensed with Windows, so a bitmap table derived from them cannot be published with the firmware. Regenerate from any other redistributable font with `--font`.
- Flash and RAM: the generated source is 2.93 MiB, and the linked glyph data is read-only constant data in Flash. Measured cost of enabling it: the application image grew from 1.24 MiB to 1.59 MiB, and 80% of the 8 MB application partition is still free. LVGL decodes individual glyph bitmaps on demand, so the table is not loaded into internal RAM wholesale. This is still significant on an ESP32-C3 with no PSRAM, so re-check `idf.py size-components` after changing the size or bpp.
- Verification: `python3 tools/gen_chinese_font.py --verify-only` re-checks the glyph count, the exported symbol, and the PLAIN bitmap format; the generated source is also syntax-checked with the real `main` compile flags from `build/compile_commands.json`.

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
