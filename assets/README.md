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

### Saya no Uta reader — `fonts/saya_cjk_16.c`, `fonts/saya_cjk_20.c`

Two generated LVGL subsets (4 bpp, uncompressed) used by the visual-novel reader on
`feature/saya-no-uta`; `fonts/saya_cjk_symbols.txt` is the character inventory they
must cover.

| Item | Value |
| --- | --- |
| Source | Noto Sans SC Regular (OFL-1.1), downloaded 2026-09-26 from the `googlefonts/noto-cjk` mirror through jsDelivr; the 8.3 MB source OTF is **not** committed |
| Inventory | 2,787 code points = every UI string in `main/*.c` plus every character of the script pack |
| Converter | `tools/saya_font.py` (Pillow/FreeType rasterization, emits the LVGL 9 bitmap format directly). It verifies itself by re-parsing the C file it just wrote and comparing every glyph against the rasterization pixel by pixel. |
| Regenerate | `python tools/saya_font.py --font <NotoSansSC-Regular.otf> --pack main/saya_data/saya_pack.bin --out-dir assets/fonts` |
| Verify | `python tools/saya_font.py --check --pack … --out-dir …` (runs in `tools/validate.sh --static`) |
| Integration | compiled into the `main` component through `target_sources()` in `main/CMakeLists.txt` |
| Impact | about 0.94 MB of Flash; no static RAM (glyphs stay in Flash) |

Not generated with `lv_font_conv`: its last release (1.5.3, 2021) writes corrupt glyph
bitmaps under current Node.js — with the same OTF and options, `--no-compress` and
`--no-compress --no-prefilter` produce identical bytes that decode to noise under LVGL's
plain 4bpp reader, while LVGL's own Montserrat fonts decode correctly with the same
reader. On the device this showed up as completely garbled text. The in-repo generator
avoids the Node dependency entirely and fails loudly instead of emitting bad glyphs.

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
