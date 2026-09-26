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

### Starry Sky Railroad reader — `fonts/starry_cjk_16.c`

One generated LVGL 4 bpp bitmap font (2,708 glyphs) used by the visual-novel reader on
`feature/starry-sky-railroad`; `fonts/starry_cjk_symbols.txt` is the character inventory
it must cover.

| Item | Value |
| --- | --- |
| Source | Noto Sans SC Regular (OFL-1.1), downloaded 2026-09-26 from the `notofonts/noto-cjk` mirror through jsDelivr (`Sans/SubsetOTF/SC/NotoSansSC-Regular.otf`, 8.3 MB); the source OTF is **not** committed |
| Inventory | 2,708 code points = every UI string under `main/` plus every character of the script pack, including U+3000 (the full-width indent space used 410 times) |
| Format | LVGL 9 bitmap font: 4 bpp PLAIN glyph bitmap, per-glyph descriptors, ASCII as `LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY` and the rest as `SPARSE_TINY`; line height 20, base line 17 |
| Converter | `tools/starry_lvgl_font.py` (Pillow/FreeType rasterization). It re-reads the C file it just wrote and compares every glyph against the rasterization pixel by pixel. |
| Regenerate | `python tools/starry_lvgl_font.py --font <NotoSansSC-Regular.otf> --pack main/starry_data/starry_pack.bin --out-dir assets/fonts` |
| Verify | `python tools/starry_lvgl_font.py --check --pack … --out-dir …` (runs in `tools/validate.sh --static`) |
| Integration | compiled as an ordinary C source through `target_sources()` in `main/CMakeLists.txt` |
| Impact | about 0.32 MB of Flash (322 KB glyph bitmap); no static RAM — the bitmap stays in Flash and LVGL decodes one glyph at a time |

The reader draws text through LVGL, so the font uses LVGL's own layout. Advances are the
source font's natural widths: an ASCII "W" is wider than the half-width cell the
pagination model budgets, and the LVGL label wraps within its box.

`fonts/starry_font16.bin`, `starry_font20.bin` and `starry_symbols.txt` are the previous
hand-rolled `SSRFONT1` packs for the strip renderer that is no longer compiled; they are
kept so the old renderer still builds from a checkout, and the application no longer
references them.

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
