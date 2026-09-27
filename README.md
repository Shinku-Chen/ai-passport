<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Saya no Uta — a visual novel reader for the AI Passport

A landscape visual-novel reader that ports the Mi Band 10 fan port of
Nitroplus' *Saya no Uta* ([`liuyuze61/Saya-miband10`](https://github.com/liuyuze61/Saya-miband10))
to the AI Passport: **44 chapters, 3,828 lines of dialogue, three endings**, all
offline. The Mi Band title is a portrait touch app; this branch re-implements the
reading engine in C on LVGL and drives it with the three keys.

- Branch: [`feature/saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/tree/feature/saya-no-uta)

## Layout

```text
┌──────────────────────────────────┐  320 x 240, held in landscape
│                                  │  the background fills the whole screen:
│                        [battery] │  upper 320 x 150 canvas + lower 320 x 90
│                                  │  canvas, both drawn 1:1 (no crop/stretch)
│  speaker name (bottom-left)      │  translucent chip above the dialogue box
├──────────────────────────────────┤
│  up to 4 lines of body text      │  dialogue box 90 px, flush with the bottom
└──────────────────────────────────┘  (16 px font: 19 full-width chars per line)
                                       (20 px font: 15 chars, 3 lines)
```

## Controls

- **Disclaimer screen** — UP / DOWN scroll the text (one line per press, a full screen when
  held). The hint only turns into "press OK to continue" once the text has been read to
  the end; before that OK just pages the text down instead of entering the game.
- **Title / lists** — UP / DOWN move the cursor and stop at the ends (no wrap),
  **OK** selects, **OK (hold)** goes back.
- **Reading** — **OK** opens the menu; UP (short) advances one line of dialogue
  (finishing the typewriter first), UP (hold 1 s or more) fast-forwards at 180 ms per
  line until released, DOWN (short) steps back one page.
- **Auto-play** — hold DOWN for one second to start; it advances one segment every
  900 ms once the current segment has finished typing. **Any key stops it**, choices and endings
  stop it automatically, and the screen never dims, blanks or sleeps while it runs.
- **Choices** — UP / DOWN select, **OK** confirms.
- **Menu** — save, load, skip chapter, back to title, close. Skipping a chapter stops at
  any choice it has not reached yet instead of deciding for you.
- **Settings** — text speed (slow / medium / fast / instant), font size (16 px or
  20 px), about, back.
- **About** — UP / DOWN scroll the text (one line per press, four lines when held),
  **OK** goes back.
- **Save slots** — 5 manual slots plus one automatic slot written on every scene
  change, so "Continue" resumes where you left off. In save mode **OK (hold)** on a
  slot deletes it.

Idle behaviour: 60 s dims the backlight, 3 min turns it off, 7 min enters deep
sleep; any key wakes the device and reopens the reader at the last automatic save.

## Offline data pipeline

Nothing is downloaded at runtime. Two tools generate everything the firmware needs,
and their output is committed so a plain checkout builds:

| Step | Tool | Output |
| --- | --- | --- |
| Script + images | `tools/saya_pack.py` | `main/saya_data/saya_pack.bin` (~3.9 MB): 44 chapters, 473 scenes, 3,828 dialogues, 193 backgrounds, 75 sprites, plus source metadata |
| Font subsets | `tools/saya_font.py` | `assets/fonts/saya_cjk_16.c`, `saya_cjk_20.c` and the character inventory `assets/fonts/saya_cjk_symbols.txt` |

The pack is read straight out of Flash — there is no runtime JSON parsing and no
decompression. Each background is pre-scaled once to a full 320 × 240 frame and stored
as two JPEGs (rows 0–149 and rows 150–239, both 1:1); sprites are scaled to
screen height, pre-cropped to the visible band and stored as JPEG plus a 1bpp mask.
The firmware `mmap`s the pack from the application partition, decodes one scene
(background plus sprite) into a 320 × 150 RGB565 canvas only when the background or
sprite actually changes, and composites the sprite with its mask.

Regenerating only needs the committed `assets/saya-source/` material (scripts,
backgrounds and sprites) plus a redistributable CJK font; the patched release variant
also needs the source port's patch directory, which is not committed. Rebuilding with
the same source commit reproduces the committed `main/saya_data/saya_pack.bin` byte for
byte; see the header of each tool. Fonts are produced by a small in-repo generator instead of
`lv_font_conv` — that tool's last release writes corrupt glyph bitmaps under current
Node.js (same input, byte-identical output with and without `--no-prefilter`, none of
it decodes under LVGL's plain 4bpp reader), and the generator verifies its own output
by re-parsing the C file and comparing every glyph against the rasterization. Font
provenance and license are recorded in [`assets/README.md`](assets/README.md).

Flash budget (ESP-IDF 5.5, app partition 8,323,072 bytes): the community variant uses
about 4.3 MB, leaving 46 % of the partition free; the patched release variant uses
about 4.6 MB, leaving 43 %.

## Variants and the publishing rule

The `main/saya_data/saya_pack.bin` and `assets/fonts/saya_cjk_*.c` committed here are
the **community variant**: they contain nothing from the source port's patch
directory (7 extended chapters plus 22 R18 CGs). The repository's default build and the rule
for community submissions remain the community variant; the fork's own community listing was
switched to the complete (patched) edition at the author's request, and that listing carries an
adult-content notice. The patch's own source files are committed under
`assets/saya-patch/`, so the local release variant is rebuildable from a clone alone;
only the *pack and firmware* built from them stay out of the repository.

The **release variant** carries that patch and is for local flashing only:

```bash
python tools/saya_pack.py --source assets/saya-source \
    --patch assets/saya-patch --commit 4d73ed0 \
    --out build/release/saya_pack.bin
SDKCONFIG_DEFAULTS=sdkconfig.defaults idf.py -B build/release/idf \
    -D SAYA_PACK_FILE=build/release/saya_pack.bin build
```

Fonts are shared: `assets/fonts` holds the union of both variants' characters (including
the glyphs only the patched text uses), and both firmware images embed it. If a new
patch introduces new characters, regenerate that union from both packs:

```bash
python tools/saya_font.py --font <NotoSansSC-Regular.otf> \
    --pack main/saya_data/saya_pack.bin --pack build/release/saya_pack.bin \
    --out-dir assets/fonts
```

`build/` is git-ignored: the patched pack and any firmware built from it must **never be
committed or published to the community**. The font is the union of both variants (glyphs
only, no story content) and is committed; its inventory `assets/fonts/saya_cjk_symbols.txt`
remembers the glyphs the patch used, so regenerating from the community pack alone does
not drop them.

## Releases

- **GitHub Release**: [`v0.1.1-saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.1-saya-no-uta)
  carries two assets: `FoloToy-AI-Passport-full.bin` (the tag-triggered CI build of the
  community variant, sha256 `e90476a5…`) and `FoloToy-AI-Passport-full-patched-r18.bin`
  (a local build of the same commit with the patch, for personal devices only).
- **Community market**: submitted as "Saya no Uta (Community)" (the Simplified Chinese peer
  records the exact localized title) with the patch-free community merged image; project 672,
  revision 1413, slug `community-10803507`, awaiting review. The earlier submission is no longer
  listed in the creator centre, so this release was submitted as a new play. The submission text and publish metadata are
  archived in
  [`docs/reference/shinku-chen/saya-no-uta/`](docs/reference/shinku-chen/saya-no-uta/README.md),
  and the Simplified Chinese peer records the exact localized titles.
- The publishing workflow and its checks are described in
  [`docs/development/release/publish-to-community.md`](docs/development/release/publish-to-community.md).

## Notes

- **Content**: the story contains heavy gore. The app keeps the source port's
  content warning on first boot.
- **Licensing**: *Saya no Uta* is a commercial Nitroplus title. This is a personal
  fan port; the artwork and the Chinese translation come from the public Mi Band
  port, and both the project and the in-app disclaimer ask readers to support the
  original release. Do not republish the generated pack as your own asset.
- **No audio**: the source port ships no audio, and this port adds none.
- **Baseline demo**: the repository's hardware-test menu and `demo_*.c` pages are
  still in `main/` but are not compiled into this application; see the repository's
  [AI guide](docs/development/ai-guide.md) and [fork guide](docs/fork-guide.md).

## Build and validate

```bash
./tools/validate.sh --static     # repository checks, host tests, font coverage
./tools/validate.sh --firmware    # ESP-IDF build + merged-image verification
```

The host test `tests/test_saya_model.c` runs the real pack through the reader logic
(chapter graph, all three endings, pagination round-trips, save encoding), and
`tools/saya_font.py --check` fails the gate if any UI string or script character is
missing from the generated font subsets.
