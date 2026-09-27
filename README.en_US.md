<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# ATRI Reader

A portrait visual-novel reader that ports the Mi Band fan port of
*ATRI -My Dear Moments-* to the AI Passport: **34 chapters, 1,069 scenes,
12,188 lines of dialogue, five full-body character sprites and three endings**,
fully offline. The story and art come from
[`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI) (the
refactored port of [`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband)),
which adds the full-body `src/common/character/*` sprites the original port
lacked. The originals are touch apps for Xiaomi's Vela OS; this branch
re-implements the reading engine in C on LVGL and drives it with the three keys.
Status: **released** — tag `v1.0.2-atri-reader`, submitted to the AI Passport community
market as `my-dear-moments-2` (under review).

- Branch: [`feature/atri-reader`](https://github.com/Shinku-Chen/ai-passport/tree/feature/atri-reader)
- Release: [`v1.0.2-atri-reader`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.2-atri-reader) — merged image `FoloToy-AI-Passport-full.bin`, 5,134,544 bytes
- Upstream work: [`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI) and [`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband) — the fan ports this branch derives from. Neither repository declares a licence; their script, backgrounds and sprites are committed with the branch and packed into the firmware, with the upstream projects credited as the source.
- Asset toolchain: [`tools/atri_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/atri-reader/tools/atri_pack.py) (script and images into `main/atri_data/atri_pack.bin`, 3.83 MB) and [`tools/atri_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/atri-reader/tools/atri_font.py) (16 px Chinese subset, 2,771 code points)

```text
┌──────────────────────────────────┐  240 x 320, native portrait; the canvas is
│  art area 240 x 320              │  the whole screen
│  [ch. 3]                  [batt] │  chapter label / battery
│                                  │
│ ┌────────┐                       │  speaker name plate
│ │ Atri   │                       │  full-body sprite, shown only while a
│ └────────┴───────────────────────┤  named cast member is speaking
│  body text, 5 lines x 13 CJK     │  the source's translucent text band is
│                                  │  painted into the canvas, the sprite sits
└──────────────────────────────────┘  above it, a light scrim keeps text readable
```

**Controls:** on lists UP / DOWN move the cursor, **OK** selects and **OK (hold)**
goes back. While reading, **UP / DOWN** advance line by line and **UP (hold)**
fast-forwards through whole lines until you let go; **DOWN (hold)** toggles
auto-read (advances 0.7 s after each line finishes typing, shows a cyan auto marker
bottom-right, any key stops it, and keeps the screen awake while it runs); **OK**
opens the menu (continue, save,
load, skip chapter, back to title). Skipping a chapter runs to
the next chapter and stops at any choice or ending. Choices use UP / DOWN + OK.
The text speed setting cycles through instant / slow / medium / fast, and the about
page scrolls with UP / DOWN. The art area shows the current chapter in the top-left
corner and the page counter in the bottom-right when a line spills over.

**Save slots:** five manual slots plus one automatic slot written on every scene
change, so the title screen can offer "continue". On the slots screen **OK** saves
or loads and **OK (hold)** deletes a manual slot. Idle behaviour: 45 s dims the
backlight, 2.5 min turns it off, 7 min enters deep sleep; any key wakes the device.
Auto-read suspends all three timers and they start over once it stops; waiting on a
choice or an ending is not activity, so the screen still turns itself off there.

**Endings:** the happy and the bad ending are reached through the three choices in
the script; once both are seen the title screen unlocks **the true ending** chapter,
which is how the original Mi Band app gates its extra episode.

**Offline data pipeline.** Nothing is downloaded at runtime; two tools generate
everything the firmware needs and their output is committed, so a plain checkout
builds:

| Step | Tool | Output |
| --- | --- | --- |
| Script + images | `tools/atri_pack.py` | `main/atri_data/atri_pack.bin` (3.83 MB): 34 chapters, 1,069 scenes, 12,188 dialogues, 73 full-screen backgrounds, 14 effect overlays, 5 character sprites, plus source metadata |
| Font subset | `tools/atri_font.py` | `assets/fonts/atri_cjk_16.c` + `assets/fonts/atri_cjk_symbols.txt` (2,771 code points) |

**Highlights:**

- **Portrait, like the source** — the Mi Band frame (336 x 480) is scaled by
  240/336 to 240 x 343 and cropped to the full 240 x 320 screen, so backgrounds,
  sprites and the text band sit where the original puts them.
- **No runtime JSON parsing** — the pack is a flat little-endian binary read
  straight out of flash; text is addressed by `(offset, length)` and the reader
  walks chapter/scene/dialogue tables.
- **~19 KB backgrounds** — backgrounds are cover-scaled, cropped and re-encoded as
  JPEG (q88) at build time; the 72 scene backgrounds plus the title art cost
  1.36 MB, against 4.7 MB of source PNGs.
- **Full-body sprites, speaker-driven** — the five cast sprites (750 x 920 …
  1150 PNGs) are squashed to 240 x 320 exactly like the reference engine's
  `width/height:100%` element, cropped to their alpha box and stored as RGB565 +
  4bpp mask. A sprite is shown **only on lines that carry a speaker name**; the
  viewpoint character (Natsuki) never shows one, and event CGs / black screens stay
  sprite-free because they already draw the cast. Everything else — narration,
  minor characters, scene changes — hides the sprite again.
- **Overlays that cost no RAM** — the 14 effect overlays are stored
  losslessly as RGB565 plus a 4 bpp alpha mask, cropped to their alpha bounding
  box, and composited row by row straight from flash into the canvas. The board
  has no PSRAM and only a few tens of KB of heap left once the canvas and LVGL
  are up, so a JPEG decode buffer for a 240 x 320 overlay would simply not fit;
  this path never allocates.
- **Text band painted in the canvas** — the source draws a translucent blue
  `text_bg` over its full-screen art. Here the same band is alpha-blended into the
  canvas rows (source colour, top-to-bottom ramp) but the sprite is composited
  *above* it, with a light dark scrim over the text rows only: the character stays
  complete instead of being washed out behind the band, and white text keeps its
  contrast.
- **Own font generator** — `lv_font_conv` writes corrupt glyph bitmaps under
  current Node.js (the device showed a screen of noise); the in-repo generator
  rasterises with FreeType, emits the LVGL 9 plain 4 bpp format and re-parses its
  own output pixel by pixel before accepting it.
- **Host-testable story logic** — `tests/test_atri_model.c` walks the real pack:
  pack integrity, the happy / bad / true endings, the choice branch, paging rules
  and save round-trips all run on the host in `tools/validate.sh --static`.

**On-device acceptance switch:** `idf.py -DATRI_BOOT_CHAPTER=<index> build` boots straight
into a chapter (indices follow `CHAPTER_ORDER` in `tools/atri_pack.py`: 31 = b501 happy
ending, 32 = b601 bad ending, 33 = b701 true ending, the last chapter). The default `-1`
keeps the normal title screen.

**Debug aid:** the console (USB-Serial-JTAG) accepts `ATRIJUMP <chapter> [scene]` (moves the
live reading position) and `ATRISHOT <chapter> <scene>`:
the firmware renders that chapter/scene into the art canvas with the normal render
path and streams the raw 240 x 320 RGB565 frame back (`ATRISHOT <w> <h> <bytes>`,
then the pixels, then `ATRISHOT-END`). It is how the drawing pipeline was verified
without a camera; it costs one idle task and does nothing until a command arrives.

**Credits and rights:** the script, images and translation come from the fan ports
`fywmjj/better-mb9p-ATRI` and `liuyuze61/ATRI-miband`, and from
*ATRI -My Dear Moments-* (ANIPLEX.EXE / Frontwing / Makura). This firmware is a
personal, non-commercial port; support the original release.

## Notes

- This branch carries one application. A `feature/*` branch README describes only
  its own application; the fork `main` root README is the catalog of every hosted
  project.
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
