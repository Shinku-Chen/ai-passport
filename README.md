<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Starry Sky Railroad and Shiro's Journey — a portrait visual-novel reader

A portrait visual-novel reader that ports the Mi Band 9 Pro fan port of the
commercial Japanese novel *Hoshizora Tetsudou to Shiro no Tabi* and its Chinese fan
translation to the AI Passport. The whole
story runs offline from Flash: **39 chapters, 1,260 scenes, 13,787 lines of dialogue,
one choice and one ending**.

- Branch: `feature/starry-sky-railroad`
- Source of the script, artwork, and translation:
  [`liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P`](https://github.com/liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P)
  (Xiaomi Vela / aiot quick app for the Mi Band 9 Pro), commit `307db0c`.
- The interface and page flow are the ATRI reader's LVGL page system (title, body,
  menu, settings, save slots, ending, about), driven by this title's own script pack
  and save format. The hand-drawn strip renderer it replaced is still in `main/` but
  is no longer compiled.

## Layout

```text
┌────────────────────────────┐  240 x 320, held upright (portrait)
│ [chapter 8]      [battery] │  plain chapter text top-left, battery text top-right
│  art area                  │  one full-screen 240 x 320 background JPEG with the
│  240 x 320                 │  character sprite on it and a speaker name plate over
│  [speaker]                 │  the art, left of the text band
├────────────────────────────┤
│  body text                 │  translucent blue text band over the art, 110 px tall
│                            │  16 px font: 13 full-width chars/line, 5 lines
└────────────────────────────┘
```

The art area is the whole screen. The bottom 110 px carry a translucent blue
gradient band (the text box), and the sprite is drawn *under* that band — exactly
like the source port, so the character's lower body sits inside the text box while
everything above the band is untouched. Only the text rows get a light scrim, which
keeps white text readable no matter what is behind it.

## Release

- **Branch**: [`feature/starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/tree/feature/starry-sky-railroad)
- **Release**: [`v0.1.0-starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-starry-sky-railroad)
  — merged image `FoloToy-AI-Passport-full.bin`, flashed from `0x0`.
- The release is also submitted to the [AI Passport community market](https://ai-passport.folotoy.cn).
- The firmware answers the serial debug command `STARRYPAGE [title | <chapter> <scene>]`
  with the current frame as raw RGB565. It is captured from the LVGL flush path, so it
  is the real screen (art, sprite, chapter, battery, name plate and text) and is used
  for remote acceptance instead of a phone photo.

## Controls

- **Title / lists** — UP / DOWN move the cursor, **OK** selects, **OK (hold)** goes back.
- **Reading** — **UP / DOWN (short press)** advances one line (while text is typing,
  one press shows the whole page); **UP (hold)** fast-forwards while the key stays down
  and stops the moment it is released; **DOWN (hold)** toggles auto-reading, which
  advances 900 ms after each line finishes typing (any other key cancels it);
  **OK (short press)** opens the menu.
- **Choices** — UP / DOWN select, **OK** confirms.
- **Menu** — save, load, skip chapter, back to title, back to reading.
- **Settings** — text speed (slow / medium / fast), about, power off, back.
- **About** — UP / DOWN scroll the page, **OK** returns.
- **Sprite visibility** — a sprite is drawn only while the character it belongs to is
  the one speaking. The packer derives that owner from who references the sprite, and
  also flags event CGs (`evcg*`) and solid screens (`bg_black/red/white`) as
  "no sprite", so nothing is ever composited on top of an illustration. Narration and
  other speakers hide the sprite instead of leaving the previous face on screen.
- **Save slots** — 5 manual slots plus one automatic slot written on every scene
  change, so "Continue" resumes where you left off. In save mode **OK (hold)** on a
  slot deletes it.

The text box holds five lines at 16 px (13 full-width characters per line). In the
shipped script the average line is 13 characters and the 99th percentile is 43, so
nearly every line fits on one screen; only about 30 of 13,787 lines (the longest is
74 characters) need a second press to see the rest.

Idle behaviour: 60 s dims the backlight, 3 min turns it off, 7 min enters deep
sleep; any key wakes the device and reopens the reader at the last automatic save.
Auto-reading and fast-forward count as activity, so the screen never dims or sleeps
while the app is advancing on its own; the timer resumes once it stops at a choice
or the ending.

The ES8311 codec is initialised and then suspended at boot even though the app plays
no audio: left in its power-on default state it produces an audible idle buzz through
the always-on speaker amplifier.
The first boot shows a one-page attribution and control summary in place of the
source app's tips screen; it is shown once and remembered in NVS. Long-pressing **OK**
on the title page (or picking "power off" in settings) enters deep sleep directly.

## Offline data pipeline

Nothing is downloaded at runtime. Two tools generate everything the firmware needs,
and their output is committed so a plain checkout builds:

| Step | Tool | Output |
| --- | --- | --- |
| Script + images | `tools/starry_pack.py` | `main/starry_data/starry_pack.bin` (~3.3 MB): 39 chapters, 1,260 scenes, 13,787 dialogues, 105 backgrounds, 15 sprites, source metadata |
| LVGL CJK font | `tools/starry_lvgl_font.py` | `assets/fonts/starry_cjk_16.c` (~2.4 MB of C source, 2,708 glyphs) and the character inventory `assets/fonts/starry_cjk_symbols.txt` |

Both are read straight out of Flash — there is no runtime JSON parsing and no
decompression. Backgrounds are pre-scaled and cropped to 240 x 320 JPEG and decoded
straight into the canvas. Sprites are cropped to their opaque bounds and scaled to
fit 168 x 252, then placed against the right edge with the bottom edge at the screen
bottom (so the lower body sits *under* the translucent text band instead of being cut
off); they are stored **losslessly as RGB565 plus a 4bpp alpha mask** together with
their screen position, so the runtime blits them row by row straight from Flash and
never needs an 84 KB sprite decode buffer.

The font is a genuine LVGL 4bpp bitmap font (two cmaps: ASCII as `FORMAT0_TINY`, the
rest as `SPARSE_TINY`), because the UI draws text through LVGL now.

Regenerating needs a checkout of the source port and a licensed CJK font:

```bash
python tools/starry_pack.py --source <source checkout> --out main/starry_data/starry_pack.bin
python tools/starry_lvgl_font.py --font <NotoSansSC-Regular.otf> \
    --pack main/starry_data/starry_pack.bin --out-dir assets/fonts
```

## Rendering architecture

A full 240 x 320 RGB565 canvas is 150 KB and lives in static RAM; the board has no
PSRAM, so a single canvas plus LVGL's own buffers is the whole budget. Every scene is
composed into that canvas, then LVGL draws the pages and text on top:

```text
canvas (static, 240 x 320 RGB565)
  background JPEG   -> decoded straight into the canvas by the ROM TJpgDec via esp_jpeg
  text band         -> translucent blue gradient, blended row by row into the canvas
  sprite            -> RGB565 + 4bpp mask blitted row by row straight from Flash
  text scrim        -> light darkening over the text rows only
  -> LVGL draws boxes, labels, lists, choices and overlays from its own heap pool
```

Page structure, list rows, choice buttons, name plate, chapter progress, page counter,
battery readout, auto-reading indicator, transient notices and the locked page stack
are all the ATRI reader's (`main/atri_ui.c`); `main/atri_image.c` composes the canvas
and `main/atri_app.c` is the state machine. The story graph, resource pack and save
format remain this title's (`main/starry_model.c`, `starry_pack.c`, `starry_save.c`).

Static RAM beyond the canvas: the LVGL heap pool (56 KB, raised from 24 KB because the
page system needs about 90 objects) plus a 19.2 KB DMA draw buffer owned by the BSP.
On device the application partition uses 4.35 MB of 7.96 MB (45 % free), and the
removed strip/sprite/backdrop buffers free roughly 240 KB of DRAM.

Everything that can be tested without hardware is host-tested: the pack parser, the
story graph (walked to the ending), pagination against line wrapping, save encoding,
and the sprite encoding (dimensions, mask length, screen position, right/bottom
edges). The font coverage check confirms every character of the script and the UI
strings has a glyph.

## Known gaps

- **Chapter 7 is absent** in the source data (the chapter files jump from 6 to 8),
  and the source's HE/BE branch flags are never set, so the story runs 1 → 40 to the
  single `FIN` ending. The port reproduces the data as published.
- Because there is only one ending and this title has no per-ending unlock flags, the
  title menu has no "true ending" entry (the ATRI reader had one).
- The text speed setting keeps the three steps the original port used (slow / medium /
  fast); the old 16 px / 20 px font-size toggle was dropped because the new interface
  ships a single 16 px LVGL font.
- Sprite visibility follows the speaker and the packer's owner statistics, so a face
  the source author picked for a *different* speaker is not drawn. In the shipped
  script 3,444 of 11,777 dialogue steps show a sprite; narration, other speakers,
  event CGs and solid screens hide it.
- Latin runs use the source font's proportional advances while the line model budgets
  half-width cells, so a line that mixes a longer English word with CJK can overflow
  the text area by up to one full-width glyph (2 of 19,986 lines in the shipped
  script); the LVGL label wraps within its box.
- No audio: the source port ships none, and this port adds none.
- The repository's baseline hardware-test menu, the `demo_*.c` pages and the old
  strip renderer (`starry_render.c`, `starry_gfx.c`, `starry_font.c`, `starry_ui.c`,
  `starry_app.c`) are still in `main/` but are not compiled into this application;
  see the repository's [AI guide](docs/development/ai-guide.md) and
  [fork guide](docs/fork-guide.md).
- On-device look and feel — text-band tint, sprite contrast, sprite/band overlap,
  page-turn latency and the LVGL pool headroom under the "about" page — has been
  checked on a real device; the whole story line has not been played end to end on
  hardware.

## Licensing

*Hoshizora Tetsudou to Shiro no Tabi* is a commercial title; its artwork and the
Chinese translation come from the public Mi Band port, whose own notice asks readers
to support the original release. This is a personal fan port. The generated pack is a
conversion of that material — do not republish it as your own asset.

## Build and validate

```bash
./tools/validate.sh --static     # repository checks, host tests, font coverage
./tools/validate.sh --firmware   # ESP-IDF build + merged-image verification
```

The static gate runs `tests/test_starry_model.c` (the real pack through the reader
logic), `tests/test_starry_pack.py` (pack structure, including the RGB565 + 4bpp
sprite encoding), `tests/test_starry_app_static.py` (pointer-cast, encoding and
auto-read idle guards), and `tools/starry_lvgl_font.py --check` (every runtime
character has a glyph). The old strip renderer's pixel-level host tests were removed
from the gate together with the renderer itself.
