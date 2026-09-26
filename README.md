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

A portrait visual novel ported from a Xiaomi Band release: **30 chapters, 4,649
dialogue lines and about 94,000 characters**, read straight through to a single
ending. Status: **in development** — it builds and has been flashed for testing,
but is not published as a release yet.

- Branch: [`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame)
- Asset pipeline: [`tools/gal/`](tools/gal/README.md)

**Controls (three keys):** **UP** advances a line, or reveals the rest of one that
is still typing; **UP (hold)** fast-forwards while held and stops the moment you
let go; **OK** opens the menu (resume, save, load, skip chapter, settings, back to
title); **DOWN** scrolls a line that runs past the panel; **DOWN (hold)** hides the
panel to look at the artwork.

**Modes and persistence:** six manual save slots that include the position within a
paginated line (hold confirm on a slot to delete it), an automatically remembered
last position behind "continue", a chapter jump list, and a settings screen with
text speed (slow / medium / fast / instant), text size (16 px or 20 px) with a live
typewriter preview, and auto-play. The story has one ending; reaching it clears the
resume point.

**Highlights:**

- **Third-party art stays out of the repository** — the artwork and scripts are
  packed at build time into a dedicated 4 MiB `assets` data partition, read from a
  local and untracked source tree. Without it the packer emits a placeholder pack,
  so a fresh clone still configures, builds and boots — which also means a
  CI-built release does not contain the game; release this branch from a locally
  built merged image.
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
  substituted asset is stored once instead of twice (4.41 MiB to 3.59 MiB).
- **Host-tested parser and typesetting** — the pack reader, the advance rules and
  the pagination run without ESP-IDF or LVGL and are covered by 11 host tests,
  including a guard for the 4-byte struct alignment the pack format requires.
- **Eight upstream defects handled** — the source scripts reference eight images
  their own repository does not contain (five are obvious typos); each is
  substituted and reported at pack time instead of drawing a blank frame.

## Notes

- Each application is a separate `feature/*` branch off the upstream baseline.
  Do not merge demo branches wholesale into `main`; port reusable patterns
  instead (see upstream `AGENTS.md`).
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
