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

### Asunabi

A portrait visual novel ported from
[`liuyuze61/Asunabi-miband`](https://github.com/liuyuze61/Asunabi-miband), a Xiaomi
Band release: **30 chapters, 4,649 dialogue lines and about 94,000 characters**,
read straight through to a single ending. Status: **released** as
`v0.1.0-asunabi`, and submitted to the AI Passport community market.

- Branch: [`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame)
- Release: [`v0.1.0-asunabi`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-asunabi) — merged image `FoloToy-AI-Passport-full.bin`, 7,917,142 bytes
- Upstream work: [`liuyuze61/Asunabi-miband`](https://github.com/liuyuze61/Asunabi-miband) — the Xiaomi Band quick-app release this port is based on. No license is declared there; its artwork and chapter scripts ship with the branch under `assets/gal-source/`, with the upstream project credited as the source.
- Asset pipeline: [`tools/gal/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame/tools/gal)

**Controls (three keys):** **UP** advances a line, or reveals the rest of one that
is still typing; **UP (hold)** fast-forwards while held and stops the moment you
let go; **OK** opens the menu (resume, save, load, skip chapter, settings, back to
title); **DOWN** scrolls a line that runs past the panel; **DOWN (hold)** starts or
stops auto-play, which advances 0.9 s after a line finishes and ends on any key
press. A long press registers at 300 ms: the BSP passes the press timing
explicitly instead of using the button component's 1,500 ms default.

**Modes and persistence:** six manual save slots that include the position within a
paginated line (hold confirm on a slot to delete it), an automatically remembered
last position behind "continue", a chapter jump list, and a settings screen with
text speed (slow / medium / fast / instant), text size (16 px or 20 px) with a live
typewriter preview, and auto-play. The story has one ending; reaching it clears the
resume point.

**Highlights:**

- **Third-party art ships with the branch** — the artwork and scripts come from the
  upstream project, which declares no license, and are committed under
  `assets/gal-source/` so a clone builds the complete game. The packer turns them
  into a dedicated 4 MiB `assets` data partition (3.55 MiB used) and fails the
  build rather than emitting a truncated image; without that tree a build still
  configures and boots, showing a placeholder pack instead of the story.
- **Serial screenshots** — the `FAP_SCREENSHOT_V1` command returns the current
  frame over the console as RGB565LE, which the community publisher requires
  before it accepts a submission.
- **Artwork-first UI** — the title backdrop, the option plate and the dialogue
  panel are tinted (30% / 50% / 70%) instead of opaque, so the scene stays visible
  while near-white text stays readable; the panel is never blanked.
- **Full-screen art with no PSRAM** — backgrounds are LVGL indexed images drawn
  straight out of the memory-mapped partition and decoded one scan line at a time
  (about 960 bytes) instead of the 150 KB a frame buffer would need; 83 of the
  chip's 128 flash-MMU pages are in use.
- **The panel is sized from the text size** — four lines times the line height plus
  padding, so a page of four lines fits whole at either 16 px or 20 px. Lines that
  still do not fit are **paginated, not clipped**: the split is computed in a pure
  model with punctuation rules, and the page number is part of the saved position.
- **Hold-to-fast-forward without changing the BSP** — the BSP has no key-release
  event, so fast forward polls the public `bsp_button_read_mv()` and stops as soon
  as the reading leaves the documented voltage window for that key.
- **Asset size cut twice** — character art is cropped to the region that can reach
  the panel and then trimmed to its alpha box (0.82 MiB to 0.24 MiB), and a
  substituted asset is stored once instead of twice (4.41 MiB to 3.55 MiB).
- **Host-tested parser and typesetting** — the pack reader, the advance rules and
  the pagination run without ESP-IDF or LVGL and are covered by host tests,
  including a guard for the 4-byte struct alignment the pack format requires and
  one that keeps every packed asset name in step with the names the firmware looks
  up.
- **Eight upstream defects handled** — the source scripts reference eight images
  their own repository does not contain (five are obvious typos); each is
  substituted and reported at pack time instead of drawing a blank frame.

### ATRI Reader

A portrait visual novel ported from the Mi Band fan port of *ATRI -My Dear Moments-*:
**34 chapters, 1,069 scenes, 12,188 lines of dialogue, five full-body character sprites
and three endings**, fully offline. The story and art come from
[`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI) (the refactored
port of [`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband)), which adds
the full-body `src/common/character/*` sprites the earlier port lacked. The originals are
touch apps for Xiaomi's Vela OS; this branch re-implements the reading engine in C on
LVGL and drives it with the three keys. Status: **released** — tag `v1.0.2-atri-reader`,
submitted to the AI Passport community market as `my-dear-moments-2` (under review).

- Branch: [`feature/atri-reader`](https://github.com/Shinku-Chen/ai-passport/tree/feature/atri-reader)
- Release: [`v1.0.2-atri-reader`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.2-atri-reader) — merged image `FoloToy-AI-Passport-full.bin`, 5,134,544 bytes
- Upstream work: [`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI) and [`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband) — the fan ports this branch derives from. Neither repository declares a licence; their script, backgrounds and sprites are committed with the branch and packed into the firmware, with the upstream projects credited as the source.
- Asset toolchain: [`tools/atri_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/atri-reader/tools/atri_pack.py) (script and images into `main/atri_data/atri_pack.bin`, 3.83 MB) and [`tools/atri_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/atri-reader/tools/atri_font.py) (16 px Chinese subset, 2,771 code points)

**Controls:** on lists UP / DOWN move the cursor, **OK** selects and **OK (hold)** goes back.
While reading, **UP / DOWN** advance line by line and **UP (hold)** fast-forwards through
whole lines until you let go; **DOWN (hold)** toggles auto-read (advances 0.7 s after each
line finishes typing, shows a cyan auto marker bottom-right, any key stops it, and keeps the
screen awake while it runs); **OK** opens the menu (continue, save, load, skip chapter, back
to title). Skipping a chapter runs to the next chapter and stops at any choice or ending.
Choices use UP / DOWN + OK. Press timing is 120 ms for a short press and 300 ms for a long
press; the text speed setting cycles through instant / slow / medium / fast, and the about
page scrolls with UP / DOWN.

**Saves, endings and idle:** five manual slots plus one automatic slot written on every
scene change, which the title screen offers as "continue"; on the slots screen **OK** saves
or loads and **OK (hold)** deletes a manual slot. The three choices in the script lead to the
happy or the bad ending, and seeing both unlocks **the true ending** chapter — the same gate
the original Mi Band app uses. Idle behaviour: 45 s dims the backlight, 2.5 min turns it off,
7 min enters deep sleep, and any key wakes the device; auto-read does not count as idle time.

**Highlights:**

- **Sprites follow the speaker** — a full-body sprite appears only on a line that carries a speaker name and hides as soon as that character stops talking; event CGs, black screens and the viewpoint character never show one, and sprites stand at the right edge like the original artwork.
- **Full-screen compositing without PSRAM** — backgrounds are decoded straight into the canvas, and sprites plus effect overlays use lossless RGB565 with a 4 bpp alpha mask cropped to the alpha bounding box, leaving more than 40 KB of free heap.
- **Offline data pipeline** — nothing is downloaded at runtime; the output of `tools/atri_pack.py` and `tools/atri_font.py` is committed, so a plain checkout builds.
- **Serial debugging channel** — `ATRISHOT <chapter> <scene>` renders any scene into the art area and streams the raw 240 × 320 frame back, which is how this release's cover and every layout check were captured; `ATRIJUMP` jumps straight to a chapter.

### limelight lemonade jam

A portrait visual novel ported from the Xiaomi Band release of *limelight lemonade jam*:
**230 chapter markers, 68,229 lines of dialogue, about 1.2 million characters, 991 packed
images and 8 choice points**, fully offline. The story and art come from
[`skdkzzx/limelight-lemonade-jam-xiaomi-band10`](https://github.com/skdkzzx/limelight-lemonade-jam-xiaomi-band10),
a touch app for Xiaomi's Vela OS; this branch re-implements the reading engine in C on LVGL
and drives it with the three keys. Status: **released** as `v0.1.0-limelight`, submitted to the AI
Passport community market (under review).

- Branch: [`cindy/curious-babbage`](https://github.com/Shinku-Chen/ai-passport/tree/cindy/curious-babbage)
- Release: [`v0.1.0-limelight`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-limelight) — merged image `FoloToy-AI-Passport-full.bin`, 7,897,824 bytes
- Upstream work: [`skdkzzx/limelight-lemonade-jam-xiaomi-band10`](https://github.com/skdkzzx/limelight-lemonade-jam-xiaomi-band10) — the Xiaomi Band release this port is based on. That repository declares no licence; its artwork and script are committed with the branch, packed into the firmware, and credited to the upstream project as their source.
- Asset toolchain: [`tools/limelight_material_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/cindy/curious-babbage/tools/limelight_material_pack.py) and [`tools/limelight_script_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/cindy/curious-babbage/tools/limelight_script_pack.py) (into `main/limelight_data/`, 4.49 MB + 1.81 MB) and [`tools/limelight_lvgl_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/cindy/curious-babbage/tools/limelight_lvgl_font.py) (16 px Chinese subset of Noto Sans SC, 3,449 code points)
- Merged image: `FoloToy-AI-Passport-full.bin`, 7,431,456 bytes

**Controls:** **UP / DOWN** advance line by line, **UP (hold)** fast-forwards while held and
stops the moment you let go, **DOWN (hold)** toggles auto-read (advances once the line
finishes typing, shows an auto marker bottom-right, any key stops it), **OK** opens the menu
(continue, save, load, skip chapter, CG gallery, back to title), **OK (hold)** goes back from
a list, and choices are picked with UP / DOWN and confirmed with OK. Press timing is 180 ms
for a short press and 500 ms for a long press, which the BSP passes explicitly instead of
using the button component's 1,500 ms default.

**Saves, chapters and idle:** five manual slots plus one automatic slot written on every
scene change (offered on its own row when loading); hold OK on a manual slot to delete it.
The chapter jump list shows all 230 markers as `X-Y`, taken from the `[CHAPTERx-y]` field in
the upstream script, and the in-game label shows the same `X-Y`. Idle behaviour: 60 s dims
the backlight, 3 min turns it off, 7 min enters deep sleep, and any key wakes the device;
auto-read and fast-forward count as activity, so the screen stays lit while they run.

**Highlights:**

- **Sprites sit at the bottom-right of the screen, behind the dialogue band** — the art canvas covers rows 0-213, so the rows below that are drawn by a second LVGL image object pointing into the decode buffer (no extra RAM) and are then covered by the translucent band, matching the original layout.
- **Two packed images, 5.9 MB, without PSRAM** — backgrounds and CGs are stored as 240 × 214 JPEG, sprites as alpha-cut RGB565 with a 1 bpp mask, and the script is UTF-8 compressed in 250-entry blocks that the ESP32-C3 ROM `tinfl` inflater expands, so decompression costs no flash.
- **The upstream text is cleaned at pack time** — 104 lines carrying inline layout directives (`%f`, `$name$`, `#rrggbbaa`) and 7 lines with a leaked translator memo are removed by the packer, which leaves the dialogue itself unchanged.
- **Serial acceptance channel** — `LIMEIMAGE <offset>` streams the raw frame, `LIMEPAGE` jumps to a dialogue id or a named screen, `LIMEAUTO on|off` toggles auto-read and `LIMEBTN` reports the button ADC level; every backlight change is logged with the idle time, which is how the "auto-read must not dim" rule is verified.

## Notes

- Each application is a separate `feature/*` branch off the upstream baseline.
  Do not merge demo branches wholesale into `main`; port reusable patterns
  instead (see upstream `AGENTS.md`).
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
