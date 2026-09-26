<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# FoloToy AI Passport — Shinku-Chen's Fork

This is a personal fork of [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport).
The upstream repository is the development baseline for the [FoloToy AI Passport](https://ai-passport.folotoy.cn)
— an open wearable AI device (ESP32-C3, 240×320 display, three keys, 8 MB Flash, no PSRAM).

This fork carries **several independent applications** built on that baseline. Each
project lives on its own `feature/*` branch and is introduced below. Board facts,
the BSP, and the development workflow come from upstream — see
[`docs/README.md`](docs/README.md), [`AGENTS.md`](AGENTS.md), and
[`docs/contribution/`](docs/contribution/). Released firmware for each project is
attached to this repository's [Releases](https://github.com/Shinku-Chen/ai-passport/releases).

## Projects

### Voice Keychain

A sound-effects keychain that turns the AI Passport into a pocket audio player:
boot straight into the app and play one of **hundreds of Chinese voice clips from
dozens of character packs** — jojo, meme cat, Liu Huaqiang, Haji Mi, Nailong,
and more. Latest: **v1.3.0**.

- Branch: [`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain)
- Releases: [v1.1.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.1.0), [v1.3.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.3.0)
- Experience notes: [`docs/reference/shinku-chen/voice-keychain/`](docs/reference/shinku-chen/voice-keychain/)

**Controls (three keys):** UP / DOWN to move in a list, **OK** to enter a
directory, select a clip, or play it, and **OK (hold)** for settings (volume,
battery) or to go back.

**Highlights (v1.3.0):**

- **Self-contained firmware** — `FoloToy-AI-Passport-full.bin` bakes the
  `voicefs` data partition (at `0x210000`) into one 8 MB image; flash from `0x0`
  and nothing else is needed.
- **Deep-sleep wake fixed** — the GPIO0 wake source was never armed (a pin
  number was passed where a bitmask is required); buttons could not wake the
  device. Now it sleeps after 5 min idle and wakes on any key (verified on device).
- **Reliable list playback** — pressing OK used to stop the current sound but
  not play the selection (a fresh 16 KB Opus-decode stack per play failed under
  heap pressure); replaced with one persistent player task on a static stack.
- Battery percentage refresh every 30 s, plus a voltage-fallback SOC estimate
  when the CW2017 gauge returns `0xFF` after power-up.

### What to Eat Today

A button-driven food roulette that answers the eternal question. Hold **UP** to
run the "what should we eat for lunch?" guide animation, hold **DOWN** to spin
through the food selector, and release to stop on a random pick. Latest: **v1.2.0**.

- Branch: [`feature/cheerful-goodall`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cheerful-goodall)
- Release: [v1.2.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.2.0)
- Experience notes: [`docs/reference/shinku-chen/eat-what/`](docs/reference/shinku-chen/eat-what/)

**Controls:** hold UP / DOWN to run the two animations, release to stop on the
current frame; **OK** toggles LVGL partial vs fast interlaced refresh.
Auto-poweroff after 2 min idle (deep sleep, GPIO0 wake).

### Shengzi Cards

A Chinese-character flashcard memorization app. Three modes — **Browse**
(scroll the character cards), **Self-test** (mark each character learned / not
learned), and **Spell** (see the pinyin and guess the character). A short **OK**
reveals the answer; learned marks persist to NVS. Latest: **v1.0.0**.

- Branch: [`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards)
- Release: [v1.0.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.0)

### Connect Four

A landscape Connect Four for the AI Passport: a **10 × 7 board**, human versus
computer with three difficulty levels (either side can move first), or two players
on one device. Latest: **v1.6.0-connect-four**.

- Branch: [`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four)
- Release: [v1.6.0-connect-four](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.6.0-connect-four)

**Controls:** UP / DOWN move the column cursor (held in landscape, UP is the
right-hand key), **OK** drops a disc, **OK (hold)** returns to the settings
screen. On the settings screen UP / DOWN picks a row and OK changes it (mode:
`HUMAN vs AI` / `AI vs HUMAN` / `TWO PLAYERS`, level: `EASY` / `MEDIUM` / `HARD`,
preview: `LANDING` / `TOP ROW`); selecting `START` begins a match. The two
computer modes differ only in who moves first.

**Highlights:**

- **Landscape 320 × 240 with a dense board** — 70 positions of 26 px discs spaced
  3 px apart, fitted to the panel by a BSP-level MADCTL rotation.
- **Three AI levels** — a wall-clock search budget keeps every move under about a
  second, while EASY and MEDIUM deliberately blunder at a fixed rate so the game
  stays winnable.
- **Sound without assets** — column, drop, win, loss and draw cues are synthesized
  from a sine table; no audio files are stored in flash.
- **Idle deep sleep** — 60 s on the settings screen or 180 s in a match, then any
  key wakes the device (GPIO0 low-level wake, fixed for the ADC-owned pad).
- **Serial screenshots** — the `FAP_SCREENSHOT_V1` command returns the real
  320 × 240 frame, which is how the release cover was captured.

### ATRI Reader

A portrait visual-novel reader that ports the Mi Band fan port of
*ATRI -My Dear Moments-* to the AI Passport: **34 chapters, 1,069 scenes,
12,188 lines of dialogue, five full-body character sprites and three endings**,
fully offline. The story and art come from
[`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI) (the
refactored port of [`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband)),
which adds the full-body `src/common/character/*` sprites the original port
lacked. The originals are touch apps for Xiaomi's Vela OS; this branch
re-implements the reading engine in C on LVGL and drives it with the three keys.

- Branch: this branch. Release not cut yet.

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
goes back. While reading, **UP / DOWN** advance line by line and **UP (hold) /
DOWN (hold)** fast-forwards through whole lines until you let go; **OK** opens the
menu (continue, save, load, skip chapter, back to title). Skipping a chapter runs to
the next chapter and stops at any choice or ending. Choices use UP / DOWN + OK.
The text speed setting cycles through instant / slow / medium / fast, and the about
page scrolls with UP / DOWN. The art area shows the current chapter in the top-left
corner and the page counter in the bottom-right when a line spills over.

**Save slots:** five manual slots plus one automatic slot written on every scene
change, so the title screen can offer "continue". On the slots screen **OK** saves
or loads and **OK (hold)** deletes a manual slot. Idle behaviour: 45 s dims the
backlight, 2.5 min turns it off, 7 min enters deep sleep; any key wakes the device.

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

**Debug aid:** the console (USB-Serial-JTAG) accepts `ATRISHOT <chapter> <scene>`:
the firmware renders that chapter/scene into the art canvas with the normal render
path and streams the raw 240 x 320 RGB565 frame back (`ATRISHOT <w> <h> <bytes>`,
then the pixels, then `ATRISHOT-END`). It is how the drawing pipeline was verified
without a camera; it costs one idle task and does nothing until a command arrives.

**Credits and rights:** the script, images and translation come from the fan ports
`fywmjj/better-mb9p-ATRI` and `liuyuze61/ATRI-miband`, and from
*ATRI -My Dear Moments-* (ANIPLEX.EXE / Frontwing / Makura). This firmware is a
personal, non-commercial port; support the original release.

## Notes

- Each application is a separate `feature/*` branch off the upstream baseline.
  Do not merge demo branches wholesale into `main`; port reusable patterns
  instead (see upstream `AGENTS.md`).
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
