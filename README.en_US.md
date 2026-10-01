<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
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
and more. Latest: **v1.5.0**.

- Branch: [`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain)
- Releases: [v1.1.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.1.0), [v1.3.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.3.0), [v1.4.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.4.0), [v1.5.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.5.0)
- Experience notes: [`docs/reference/shinku-chen/voice-keychain/`](docs/reference/shinku-chen/voice-keychain/)

**Controls (three keys):** UP / DOWN to move in a list, **OK** to enter a
directory, select a clip, or play it, and **OK (hold)** for settings (volume,
battery) or to go back.

**Highlights:**

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
- **Quieter deep sleep (v1.4.0)** — the LCD panel, the codec and the fuel gauge are
  shut down before the device enters deep sleep.
- **Directory order (v1.5.0)** — the Yellow Kangaroo pack leads the directory list.

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
computer with three difficulty levels (either side can move first), two players
sharing one device, or **two devices playing each other over Bluetooth LE**.
Latest: **v1.7.0-connect-four**, which adds two-device play over Bluetooth LE.

- Branch: [`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four)
- Release: [v1.7.0-connect-four](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.7.0-connect-four)
- Experience notes: [`docs/reference/shinku-chen/two-device-ble-link.md`](docs/reference/shinku-chen/two-device-ble-link.md)

**Controls:** UP / DOWN move the column cursor (held in landscape, UP is the
right-hand key), **OK** drops a disc, **OK (hold)** returns to the settings
screen. On the settings screen UP / DOWN picks a row and OK changes it (mode:
`HUMAN vs AI` / `AI vs HUMAN` / `TWO PLAYERS` / `LINK PLAY`, level: `EASY` / `MEDIUM` / `HARD`,
preview: `LANDING` / `TOP ROW`); selecting `START` begins a match, or enters the
link screen in `LINK PLAY`. The two computer modes differ only in who moves first.

**Two-device link play:** both devices set `MODE = LINK PLAY` and press `START`;
each broadcasts and scans at the same time, so there is no host/join choice — the
device with the larger BLE address connects out, and the two reach opposite roles
on their own. The screen reports `SEARCHING...` / `CONNECTING...` / `HANDSHAKE...`
and the match starts once both sides have exchanged a `HELLO`; the connecting
device plays first, the first player alternates on every rematch, and only the
player whose turn it is can drop a disc (`YOUR TURN` / `PEER TURN`). A stop-and-wait
layer (per-packet sequence number, piggybacked acknowledgement, retransmission)
plus a per-move ply check keeps the two boards in step, and
[`tools/c4_peer.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/connect-four/tools/c4_peer.py)
plays the same protocol from a PC over BLE. The link needs about 73 KB of heap,
and the serial screenshot tool reserves a whole 320 × 240 frame, so the default
build ships `LINK PLAY` and leaves the screenshot tool to the opt-in
`idf.py -DC4_ENABLE_SCREENSHOT=ON build`.

**Highlights:**

- **Landscape 320 × 240 with a dense board** — 70 positions of 26 px discs spaced
  3 px apart, fitted to the panel by a BSP-level MADCTL rotation.
- **Three AI levels** — a wall-clock search budget keeps every move under about a
  second, while EASY and MEDIUM deliberately blunder at a fixed rate so the game
  stays winnable.
- **Sound without assets** — column, drop, win, loss and draw cues are synthesized
  from a sine table; no audio files are stored in flash.
- **Two-device play without a phone** — symmetric peer discovery over BLE
  (`components/bsp/bsp_ble_link.c`) plus a host-tested message layer
  (`main/c4_link_proto.c`), about 73 KB of heap measured on the board.
- **Idle deep sleep** — 60 s on the settings screen or 180 s in a match, then any
  key wakes the device (GPIO0 low-level wake, fixed for the ADC-owned pad).
- **Serial screenshots** — the `FAP_SCREENSHOT_V1` command returns the real
  320 × 240 frame, which is how the release cover was captured; it is now an
  opt-in development build, because it cannot coexist with the BLE link on a
  board with no PSRAM.

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

### Starry Sky Railroad and Shiro's Journey

A portrait visual novel ported from the Mi Band fan port of *Hoshizora Tetsudou to
Shiro no Tabi* and its Chinese fan translation: **39 chapters, 1,260 scenes, 13,787
lines of dialogue, one choice and one ending**, fully offline. The script, art and
translation come from
[`liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P`](https://github.com/liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P)
(a Mi Band 9 Pro quick app); this branch re-implements the reader on the ATRI
reader's LVGL page system and drives it with the three keys. Status: **released** —
tag `v0.1.0-starry-sky-railroad`, and submitted to the AI Passport community market
as `community-0d8223f7` (under review).

- Branch: [`feature/starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/tree/feature/starry-sky-railroad)
- Release: [`v0.1.0-starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-starry-sky-railroad) — merged image `FoloToy-AI-Passport-full.bin`, 4,627,696 bytes
- Upstream work: [`liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P`](https://github.com/liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P) — the fan port this branch derives from. It declares no licence; its script, backgrounds and sprites ship with the branch and are packed into the firmware, with the upstream project credited as the source.
- Asset toolchain: [`tools/starry_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/starry-sky-railroad/tools/starry_pack.py) (script and images into `main/starry_data/starry_pack.bin`, 3.44 MB) and [`tools/starry_lvgl_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/starry-sky-railroad/tools/starry_lvgl_font.py) (16 px Chinese subset, 2,708 code points)

**Controls (three keys):** on lists UP / DOWN move the cursor, **OK** selects and
**OK (hold)** goes back. While reading, **UP / DOWN** advance one line (while text is
still typing, one press shows the whole page) and **UP (hold)** fast-forwards while
held and stops the moment you release it; **DOWN (hold)** toggles auto-reading, which
advances 0.9 s after a page finishes typing and stops on any other key; **OK** opens
the menu (continue, save, load, skip chapter, back to title). Choices use UP / DOWN +
OK, and skipping a chapter plays the chapter-transition card into the next chapter.
Short presses register at 180 ms and long presses at 500 ms, passed to the button
component explicitly by the BSP.

**Saves, endings and idle:** five manual slots plus one automatic slot written on every
scene change, which the title screen offers as "continue"; on the slots screen **OK**
saves or loads and **OK (hold)** deletes a manual slot. The story has a single choice and
a single ending. Idle behaviour: 60 s dims the backlight, 3 min turns it off, 7 min enters
deep sleep, and any key wakes the device; auto-reading and fast-forward do not count as
idle time.

**Highlights:**

- **A sprite appears only while its own character is speaking** — the packer derives each sprite's owner from which named speaker references it, so 3,444 of 11,777 dialogue steps draw one; event illustrations and solid-colour scenes are flagged to never get a face composited on top.
- **The sprite sits under the text band** — the translucent blue text box is composited over the character, exactly like the source port, so the lower body stays inside the panel while everything above the band remains untouched.
- **Full-screen compositing without PSRAM** — backgrounds are decoded straight into the 240 × 320 canvas, sprites use lossless RGB565 with a 4 bpp mask blitted row by row from Flash (no sprite decode buffer at all), and the panel is fed from a 40-line partial buffer.
- **Offline data pipeline** — nothing is downloaded at runtime; the output of `tools/starry_pack.py` and `tools/starry_lvgl_font.py` is committed, so a plain checkout builds.
- **Serial debugging channel** — `STARRYPAGE [title | <chapter> <scene>]` captures a frame from the LVGL flush path and streams it back as raw RGB565, which is how every layout check in this project was made; `STARRYJUMP` jumps straight to a chapter.

### Sanoba Witch

A portrait visual novel ported from the Mi Band fan port of *Sanoba Witch*: **101
chapters, 53,190 lines of dialogue, about 1.1 million characters, five heroine routes
and five endings**, fully offline. The script and art come from
[`hrk666666/Sanoba-Witch-MiBand-10`](https://github.com/hrk666666/Sanoba-Witch-MiBand-10)
(a Mi Band 9/10 quick app, itself derived from the archived
[`futrw4v/Sanoba-Witch-MiBand-9Pro`](https://github.com/futrw4v/Sanoba-Witch-MiBand-9Pro));
this branch re-implements the reader on the ATRI reader's LVGL page system and drives it
with the three keys. Status: **released** — tag `v0.1.0-sanoba-witch`, and submitted to
the AI Passport community market as `community-8e228d9f` (under review).

- Branch: [`feature/sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch)
- Release: [`v0.1.0-sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-sanoba-witch) — merged image `FoloToy-AI-Passport-full.bin`, 5,929,664 bytes
- Upstream work: [`hrk666666/Sanoba-Witch-MiBand-10`](https://github.com/hrk666666/Sanoba-Witch-MiBand-10) — the fan port this branch derives from (its own upstream is the archived `futrw4v/Sanoba-Witch-MiBand-9Pro`). Its code is GPL-3.0; the artwork belongs to Yuzusoft and the Simplified-Chinese text belongs to its fan-translation group. The sources it packs ship with the branch under [`assets/sanoba-source/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch/assets/sanoba-source), with the upstream credited.
- Asset toolchain: [`tools/sanoba_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/sanoba-witch/tools/sanoba_pack.py) and [`tools/sanoba_scn_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/sanoba-witch/tools/sanoba_scn_pack.py) (image pack 2.97 MB and script pack 1.45 MB into `main/sanoba_data/`) plus [`tools/sanoba_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/sanoba-witch/tools/sanoba_font.py) (16 px Chinese subset, 3,458 glyphs, generated in script-table order so nothing is missing)

**Controls (three keys):** on lists UP / DOWN move the cursor and **OK** selects. While
reading, **UP / DOWN** advance a line (one press completes the typewriter, and long lines
paginate with DOWN), **UP (hold)** fast-forwards while held and stops the moment you
release it, **DOWN (hold)** toggles auto-read, and **OK** opens the menu (save, load, jump
to the next choice, auto-play, back to title). Choices use UP / DOWN + OK, and auto-read
is not switched off while a choice is on screen, so answering one resumes reading by
itself. Short presses register at 180 ms and long presses at 500 ms, passed to the button
component explicitly by the BSP.

**Saves, endings and idle:** five manual slots plus one automatic slot written on every
scene change, offered as "continue" on the title screen. Five routes end in five different
endings. Idle behaviour: 60 s dims the backlight, 180 s turns it off, 420 s enters deep
sleep, and any key wakes the device; auto-read and fast-forward do not count as idle
time, so the screen stays lit while they run.

**Highlights:**

- **A compact script pack instead of a raw script dump** — the 5.06 MB node JSON becomes a 1.45 MB pack that decompresses one chunk at a time (3 KB raw per chunk, 4 KB buffer) because the largest free block after boot is under 8 KB; every jump target resolves through a label table instead of scanning a chunk.
- **Three defects that only appear on hardware** — the pack's deflate framing, the chunk-buffer allocation and the main task's stack each showed up as "the story ends immediately"; a boot self-check now prints the first chunk's decompressed size and the first line, so this whole class of failure is visible in the log instead of looking like the end of the story.
- **A layout the panel likes** — 240 × 320 portrait: full-width background, a 240 × 144 SD window in the upper third, and a five-line text band. Backgrounds decode straight into the canvas and an SD image decodes into the row range it occupies, so neither needs a decode buffer.
- **The script's own chapter number** — the corner readout and the transition card show the source numbering (`4-7`), while the save labels and lists use the scenario names.
- **Offline and reproducible** — both packers and the font generator run from the sources committed under `assets/sanoba-source/`; rebuilding either pack is byte-identical to what is in the firmware.
- **Serial debugging channel** — `SANOBAPAGE [title | <scenario> <step>]` captures a frame from the LVGL flush path and streams it back as raw RGB565; `SANOBASHOT`, `SANOBAJUMP` and `SANOBAAUTO` cover scene renders, scene jumps and auto-read checks.
- **No character art and no event CGs** — the upstream port ships neither, so a speaker appears as a name plate and scenes are carried by backgrounds plus chibi SD illustrations; the script, its branches and all five endings are complete.

### Senren \* Banka

A portrait visual novel ported from the Mi Band fan port of *Senren \* Banka*: the whole
story of the source port plus **92 backgrounds, 123 sprites in several poses each, 570
event illustrations** and the extras it carries, fully offline. The script and art come
from [`hrk666666/Senren-Banka-MiBand-10`](https://github.com/hrk666666/Senren-Banka-MiBand-10)
(a Mi Band 9/10 quick app); this branch re-implements the reader on the ATRI reader's LVGL
page system and drives it with the three keys. Status: **released** — tag
`v0.1.0-senren-banka`, and submitted to the AI Passport community market as
`community-07c775dc` (under review).

- Branch: [`feature/senren-banka`](https://github.com/Shinku-Chen/ai-passport/tree/feature/senren-banka)
- Release: [`v0.1.0-senren-banka`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-senren-banka) — merged image `FoloToy-AI-Passport-full.bin`, 7,997,648 bytes (the image validated on device; CI rebuilds the same sources)
- Upstream work: [`hrk666666/Senren-Banka-MiBand-10`](https://github.com/hrk666666/Senren-Banka-MiBand-10) — the fan port this branch derives from. It declares no licence; its script and artwork are fetched with [`tools/senren_fetch_source.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_fetch_source.py) rather than committed, packed into the firmware, and the upstream project is credited. The port is shared for personal study and exchange only, as the app's first-run notice states.
- Asset toolchain: [`tools/senren_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_pack.py) (image pack 5,263,756 bytes into `main/senren_data/`) and [`tools/senren_scn_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_scn_pack.py) (script pack 1,432,148 bytes) plus [`tools/senren_lvgl_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_lvgl_font.py) (16 px Chinese subset, 3,492 glyphs in code-point order for the sparse-font binary search)

**Controls (three keys):** on lists UP / DOWN move the cursor and **OK** selects. While
reading, **UP / DOWN** advance a line (a press completes the typewriter first, and a line
that does not fit paginates), **UP (hold)** fast-forwards while held and stops on release,
**DOWN (hold)** toggles auto-read, and **OK** (tap or hold) opens the menu — continue, save,
load, skip chapter, back to title. Choices use UP / DOWN + OK and save as soon as they are
confirmed. Short presses register at 180 ms and long presses at 500 ms, passed to the button
component explicitly by the BSP.

**Saves, chapters and idle:** one automatic slot plus five manual slots, with the automatic
one written on every chapter change, every choice and before sleeping, offered as
"continue" on the title screen. Each chapter announces itself as a `Chapter X-X` card
(chapter and section). Idle behaviour: 60 s dims the backlight, 180 s turns it off, 420 s
enters deep sleep, and any key wakes the device; auto-read and fast-forward do not count as
idle time, so a story left running never dims or sleeps by itself.

**Highlights:**

- **A self-written inflate instead of the ROM decompressor** — the reader decodes a whole block into one buffer instead of needing a 32 KB sliding dictionary, so the working buffer stays at 4 KB on a part with no PSRAM to spare.
- **A chunked palette image format** — backgrounds and event illustrations are stored as 240 × 320 JPEG, sprites by pose and bottom-aligned, and event-illustration variants as sparse mask patches against a base picture, each variant stored whichever way is smaller.
- **The story is packed in 3 KB blocks** — the script pack trades about 200 KB of compression ratio for the same 4 KB buffer, and every jump target resolves through chunk and block tables instead of a scan.
- **A real hardware find behind "the menu opens by itself"** — the three keys share one ADC pin behind voltage windows, and a long press whose contact briefly opens sweeps past the widest window and reads as a different key; the BSP now locks the key identity until the voltage returns to the released range, and confirms a press for 40 ms before accepting it. This is board-level logic and belongs upstream.
- **Serial debugging channel** — `SENRENPAGE [title | <chapter> <step>]` captures a frame from the LVGL flush path and streams it back as raw RGB565; `SENRENJUMP`, `SENRENSKIP`, `SENRENSAVE`, `SENRENLOAD`, `SENRENINFO` and `SENRENKEYS` (the last one a ring of the last 32 key events with the ADC voltage each was detected at, which is how the key-window defect above was proven) cover scene jumps, chapter skipping, save and load, state readout and key tracing.

### Saya no Uta

A landscape visual-novel reader ported from the Mi Band 10 fan port of Nitroplus'
*Saya no Uta* ([`liuyuze61/Saya-miband10`](https://github.com/liuyuze61/Saya-miband10)):
**44 chapters, 473 scenes, 3,828 lines of dialogue and three endings**, all offline.
The Mi Band title is a portrait touch app; this branch re-implements the reading
engine in C on LVGL and drives it with the three keys. Status: **released** — tag
`v0.1.1-saya-no-uta`, listed on the AI Passport community market as
`community-10803507`.

- Branch: [`feature/saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/tree/feature/saya-no-uta)
- Release: [`v0.1.1-saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.1-saya-no-uta) — `FoloToy-AI-Passport-full.bin` (the community variant, built by CI) plus a local build of the patched edition for personal devices
- Data pipeline: [`tools/saya_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/saya-no-uta/tools/saya_pack.py) (script and images into `main/saya_data/saya_pack.bin`, about 3.9 MB) and [`tools/saya_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/saya-no-uta/tools/saya_font.py) (16 px and 20 px Chinese subsets, generated in-repo)

**Controls (three keys):** the first-boot warning scrolls with UP / DOWN and only offers
"press OK to continue" once the text has been read to the end. On lists UP / DOWN move
the cursor without wrapping, **OK** selects and **OK (hold)** goes back. While reading,
**OK** opens the menu, **UP** advances a line (completing the typewriter first), **UP
(hold 1 s)** fast-forwards at 180 ms per line until released, and **DOWN
(short)** steps back one page. Holding **DOWN** for a second toggles auto-play, which
advances 900 ms after each segment finishes typing and stops on any key, at a choice or at
an ending. The menu carries save, load, skip chapter, back to title and close; skipping a
chapter stops at a choice it has not reached yet. Settings offer text speed (slow / medium /
fast / instant) and font size (16 px or 20 px).

**Saves and idle:** five manual slots plus one automatic slot written on every scene
change, which the title screen offers as "continue"; in save mode **OK (hold)** deletes a
slot. Idle behaviour: 60 s dims the backlight, 3 min turns it off, 7 min enters deep sleep,
and any key wakes the device at the last automatic save; auto-play keeps the screen awake.

**Highlights:**

- **Backgrounds decoded straight from flash** — each background is pre-scaled once to a
  full 320 × 240 frame and stored as two 1:1 JPEGs (rows 0–149 and 150–239), sprites as
  JPEG plus a 1 bpp mask; the pack is memory-mapped, so there is no runtime JSON parsing
  and no decompression.
- **Two editions, one repository** — the committed pack is the community variant; the
  patched complete edition is rebuilt locally from `assets/saya-patch/` and its pack and
  firmware are never committed or published.
- **A font generator instead of `lv_font_conv`** — that tool's last release writes corrupt
  glyph bitmaps under current Node.js, so the repository generates the subsets itself and
  re-parses every glyph against its rasterization before writing the C files.
- **Content and licensing** — the story contains heavy gore and keeps the source port's
  content warning; *Saya no Uta* is a commercial Nitroplus title, so this is a personal fan
  port whose generated pack must not be republished as an asset.

### AI Passport Pocket Intercom

Turn the AI Passport into a pocket AI intercom: **hold OK to speak → the phone app acts as the
middleman — it hands your words to the backend AI and brings the answer back to both the device
screen and the phone**. The backend can be **OpenClaw**, an **OpenAI-compatible Hermes** endpoint,
or any other OpenAI-compatible API. The device itself never touches the network: audio travels over
Bluetooth between device and phone only. Status: **released** — tag `v1.8.0-intercom`, submitted to
the AI Passport community market (`community-82cbed79`, under review).

- Branch: [`feature/openclaw-intercom`](https://github.com/Shinku-Chen/ai-passport/tree/feature/openclaw-intercom)
- Release: [`v1.8.0-intercom`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.8.0-intercom) — `FoloToy-AI-Passport-full.bin` (built by CI); for newer builds, pick the latest release whose name contains `intercom` on the [Releases page](https://github.com/Shinku-Chen/ai-passport/releases)
- Phone app (separate repository): [`Shinku-Chen/ai-passport-openclaw-android`](https://github.com/Shinku-Chen/ai-passport-openclaw-android) — Android 8+, download the signed `app-release.apk` from its Releases
- Wire protocol: [`docs/development/engineering/intercom-wire-protocol.md`](https://github.com/Shinku-Chen/ai-passport/blob/feature/openclaw-intercom/docs/development/engineering/intercom-wire-protocol.md) (English and Chinese)

**Controls (three keys):** hold **OK** to talk — the screen turns red while it is getting ready and
green once you can speak; release to send. A short **OK** press only lights the screen, a long
**UP** press opens settings (brightness / device info), and **UP / DOWN** scroll the history.

**Highlights:**

- **Opus uplink on the device** — 16 kHz capture, 60 ms frames, DTX, about 3 KB/s (a tenth of PCM), encoded on static stacks on a PSRAM-less ESP32-C3 to dodge heap fragmentation.
- **The phone is the gateway** — the app does speech recognition, talks to the backend (websocket RPC with an ed25519 device identity for OpenClaw; OpenAI-compatible HTTP for Hermes and custom endpoints), and sends the answer back to the device. The device parses no HTTP and stores no keys.
- **Press-to-red, ready-to-green** — the screen turns red the instant the key goes down and green once the gateway is ready; measured about 280–290 ms. The recognition channel is pre-warmed, so there is nothing to wait for.
- **Closed-loop gateway approval** — OpenClaw requires each device to be approved first; until then the app shows `Waiting for gateway approval … (deviceId …)`. Approve it on the console's Devices page or run `openclaw devices approve <deviceId>`, and the app continues by itself.
- **Reply collection** — gateway status lines, streaming fragments and tool output are handled separately; the body comes from the terminal message and is corrected from `chat.history`, so the device shows exactly one body bubble per turn.
- **No speech synthesis** — this version does not do TTS: answers are text on the device screen and in the app, and the device never reads them aloud.


## Notes

- Each application is a separate `feature/*` branch off the upstream baseline.
  Do not merge demo branches wholesale into `main`; port reusable patterns
  instead (see upstream `AGENTS.md`).
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
