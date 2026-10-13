<p align="right">
  <a href="galgame-framework.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# GalGame Framework Development Guide

This guide covers the generic GalGame framework on the `feature/galgame-framework`
branch: what it can do, how the code is layered, how to bring a new title in, and how
to validate and troubleshoot it.

The framework itself contains **no title content**. The firmware in this repository ships
placeholder content (dialogue, choices, chapters, CGs, tracks) that exercises every page
and mechanic. Bringing in a title means replacing the content layer only; the UI and
interaction stay unchanged.

The design prototype lives at
[galgame-ui-wireframes/prototype.html](../../design/galgame-ui-wireframes/prototype.html):
19 logical screens, each drawn for both 240×320 portrait and 320×240 landscape.

## 1. Capabilities

| Area | Details |
| --- | --- |
| 19 logical screens | Start, title, dialogue, choice, in-game menu, save, load, settings, history, gallery, CG browser, CG recall, story (chapters/routes), music player, confirm modal, chapter transition, ending, about, CG viewer |
| Pagination | Long lines are split by "characters per line × lines per page"; line-start punctuation stays on the previous line; two font sizes (16 / 20px) |
| Reading | Typewriter, hold-to-fast-forward, auto-play (chapter transitions advance automatically without breaking auto mode) |
| Story locks | Unlock bits for CGs / chapters / endings plus story flags; conditional lines, conditional options, conditional chapters |
| Persistence | Settings, 3 manual save slots, automatic reading progress, and story locks each stored in NVS |
| Gallery | CG browser (locked entries show a question mark), CG recall (replay seen scenes), story (chapters), music player |
| Audio | Placeholder square-wave melodies (16 kHz / 16-bit / mono) played from a dedicated task |
| UI | 240×320 portrait; background, CG thumbnails and CG viewer art are drawn in code — no art assets shipped |

## 2. Layout

```
main/main.c              Boot, input task, idle policy (dim / screen off / deep sleep), wake key guard
main/gal/
  gal_content.[ch]       Content layer: lines, event stream, choices, chapters, CGs, recall, tracks, menus, about
  gal_app.[ch]           State machine: page navigation, key semantics, progress and unlock accounting, history
  gal_ui.[ch]            LVGL layer: 19 pages, row-list component, typewriter
  gal_text.[ch]          Pagination (pure logic, host-testable)
  gal_tone.[ch]          Square-wave synthesis (pure logic, host-testable)
  gal_music.[ch]         Playback task + I2S / ES8311 path
  gal_settings.[ch]      Display settings (NVS)
  gal_save.[ch]          3 manual save slots (NVS)
  gal_progress.[ch]      Automatic reading progress (NVS)
  gal_unlock.[ch]        Story locks: unlock bits and flags (NVS)
assets/fonts/            gal_cjk_16.c / gal_cjk_20.c / gal_cjk_symbols.txt (generated, committed)
tools/gal_font.py        CJK subset generation + coverage checks
tests/                   Host tests (pagination, square wave, BSP topics)
docs/design/galgame-ui-wireframes/prototype.html   19-screen prototype (portrait + landscape)
```

## 3. Getting started

### Build and flash

Use ESP-IDF 5.5.3 (see [environment-setup.md](environment-setup.md)):

```bash
source <ESP-IDF-v5.5.3-path>/export.sh
idf.py build                      # incremental build
idf.py -p <port> flash            # flash the app
idf.py -p <port> monitor          # watch logs
```

### Validation gate

```bash
./tools/validate.sh --static      # repository checks + host tests (pagination and square wave included)
./tools/validate.sh --firmware    # ESP-IDF build + merged-image verification
./tools/validate.sh               # full gate (requires ESP-IDF 5.5.3 active)
```

On macOS the host tests need `CC` pointing at a wrapper that maps `--gc-sections`;
see [build-and-test.md](build-and-test.md).

### Fonts

Regenerate the font subsets after any content change (`validate.sh` checks coverage):

```bash
python3 tools/gal_font.py --font <source>.otf --out-dir assets/fonts
python3 tools/gal_font.py --check --out-dir assets/fonts   # check only
```

The character set is collected from string literals under `main/`, so simply re-running
the tool after adding text is enough. The source font itself is not committed.

## 4. Key semantics (↑ / ↓ / OK)

| Page | Short press | Long press |
| --- | --- | --- |
| Start | OK enters the title page | — |
| Title | ↑/↓ move, OK enters | — |
| Dialogue | OK / ↑ / ↓ all advance (or reveal the full page while typing) | OK = in-game menu; ↑ = fast-forward (stops on release); ↓ = toggle auto-play |
| Choice | ↑/↓ select, OK confirms (locked options report the requirement) | OK returns to dialogue |
| In-game menu | ↑/↓ select, OK runs | OK returns to dialogue |
| Save / Load | ↑/↓ pick a slot, OK saves / loads | OK goes back |
| Settings | ↑/↓ select, OK toggles the value | OK goes back |
| History | ↑/↓ scroll | OK goes back |
| Gallery | ↑/↓ pick a category, OK enters | OK returns to the title page |
| CG browser | ↑/↓ pick a thumbnail, OK opens full screen | OK returns to the gallery |
| CG recall | ↑/↓ pick a scene, OK replays it | OK returns to the gallery |
| Music | ↑/↓ pick a track, OK plays / stops | OK returns to the gallery (and stops) |
| Story (chapters) | ↑/↓ pick a chapter, OK starts from it | OK returns to the gallery |
| Chapter transition | any short press continues (auto-play advances it) | — |
| Ending | OK returns to the title page | OK returns to the title page |
| About | ↑/↓ scroll (long press scrolls faster) | OK returns to the title page |
| CG viewer | ↑/↓ switch, OK returns to the thumbnails | — |
| Overwrite confirm | ↑/↓ pick a button, OK runs it | OK cancels |

While auto-play is on, **any key first exits auto-play**; that key press is consumed.

## 5. Typography rules

| Orientation | Resolution | Dialogue panel | 16px tier | 20px tier |
| --- | --- | --- | --- | --- |
| Portrait (skeleton firmware) | 240×320 | 110px, bottom-anchored | 13 chars/line, 5 lines/page | 10 chars/line, 4 lines/page |
| Landscape (BSP API provided) | 320×240 | 90px, bottom-anchored | 19 chars/line, 4 lines/page | 15 chars/line, 3 lines/page |

- All on-screen text uses the dialogue font tier (16 / 20px); there is no 10–12px text.
- The speaker plate sits **outside** the panel's top edge, left-aligned; narration has none.
- The "auto" indicator sits outside the panel's top-right corner as plain text (no border or fill).
- Panel transparency is 10%–55% (5% steps, default 30%) and configurable in settings.
- Line spacing is the font line height plus 1px (20px at the 16px tier, 24px at the 20px tier).

## 6. Pages and navigation

```
Start ──OK──▶ Title
Title ─┬─ Start new game ─▶ Dialogue (progress cleared, starts from the first line)
       ├─ Continue       ─▶ Dialogue (resumes from automatic progress; greyed out when none)
       ├─ Load           ─▶ Load page (greyed out when there is no save)
       ├─ Settings       ─▶ Settings (6 rows when opened here)
       └─ Gallery        ─▶ Gallery
Gallery ─┬─ CG browser  ─▶ Thumbnails ──OK──▶ CG viewer
         ├─ CG recall   ─▶ Seen scenes ──OK──▶ replay that scene (back to dialogue)
         ├─ Story       ─▶ Chapters ──OK──▶ start from that chapter
         └─ Music       ─▶ Tracks ──OK──▶ play / stop
Dialogue ──long OK──▶ In-game menu ─┬─ Back to story
                                    ├─ Save ─▶ Save page (confirm before overwriting)
                                    ├─ Load ─▶ Load page
                                    ├─ History
                                    ├─ Settings (7 rows while reading, extra "back to title")
                                    └─ Back to title
Story ─▶ Choice (locked options greyed out) ─▶ Chapter transition ─▶ … ─▶ Ending ──OK──▶ Title
```

## 7. Bringing in a new title

### 7.1 Replace the content layer

Only `main/gal/gal_content.c` needs editing (declarations in `gal_content.h`):

| Array / macro | Contents |
| --- | --- |
| `gal_content_lines[]` | Dialogue: speaker (empty = narration), text, required flag |
| `gal_content_events[]` | Event stream: `{kind, index, flag written on arrival}` — line / choice / transition / ending |
| `gal_content_choices[]` | Choice groups: prompt line plus options (each may write or require a flag) |
| `gal_content_transitions[]` | Chapter transitions: chapter name, eyebrow, title, subtitle |
| `gal_content_endings[]` | Endings: eyebrow, title, body |
| `gal_content_cgs[]` | CGs: label, caption, unlock event |
| `gal_content_chapters[]` | Chapters: name, summary, progress text, unlock event, required flag |
| `gal_content_recalls[]` | CG recall: scene title, chapter, required CG, replay start event |
| `gal_content_tracks[]` | Music: title, duration text, required CG, note sequence |
| `gal_content_*_menu[]` | Title-page and in-game menu labels |
| `gal_content_about_rows[]`, `GAL_CONTENT_ABOUT`, … | About page |

Replace `GAL_CONTENT_TITLE` / `SUBTITLE` / `LOGO_SUB` / `VERSION` with the title's own data.

### 7.2 Story locks

`gal_unlock` stores three kinds of state as bitmasks (up to 32 each):

- Lines: an unmet `require_flag` makes the event **skip itself** (that is how branches work).
- Options: an unmet `require_flag` greys the row out and reports "requirements not met";
  `flag_set` is written when the option is chosen.
- CGs / chapters: unlocked on reaching `unlock_event`; chapters may also require a flag.
- Events can carry `flag_set` themselves (e.g. a transition writes "reached chapter 2",
  an ending writes "cleared").
- Flag constants live in `gal_content.h` as `GAL_FLAG_*`; titles add or remove their own.

Unlock state persists across saves; save slots only store a position and never copy it.

### 7.3 Fonts

Regenerate after adding text (see §3). `--check` already runs inside `validate.sh`, so
missing glyphs fail the gate.

### 7.4 Art and audio

- Backgrounds, CG thumbnails and the CG viewer are drawn as gradients in `gal_ui.c`;
  swapping them for `lv_image` plus image assets does not require touching list rows,
  buttons or the dialogue panel.
- The music player currently synthesises square waves in `gal_tone`, driven by
  `gal_music` (task plus I2S). To ship real audio, replace the playback loop in
  `gal_music.c`: decoded PCM still goes through `bsp_audio_write()` with the format set
  by `bsp_audio_set_format()`.
- Titles with landscape art can use the BSP landscape API and ship 320×240 crops.

## 8. Persistence

| NVS namespace | Key | Contents | Written when |
| --- | --- | --- | --- |
| `galcfg` | `cfg` | Text speed, font tier, auto-play, panel transparency | A setting changes |
| `galsave` | `slots` | 3 manual save slots (event + page + chapter) | Saving from the save page |
| `galprog` | `pos` | Automatic reading progress (event + page) | Throttled after advancing (≤ once per 2s), forced before deep sleep |
| `galunlock` | `bits` | CG / chapter / ending unlock bits and flags | On change (reaching an unlock event, writing a flag) |

Progress is throttled to limit NVS wear; `gal_app_flush()` forces a write at key moments
such as entering deep sleep.

## 9. Tests

| Test | Coverage |
| --- | --- |
| `tests/test_gal_text.c` | Pagination: line filling, page splits, offsets, explicit breaks, boundaries |
| `tests/test_gal_tone.c` | Square wave: period and duty cycle, silence, block-leading ramp, invalid arguments |
| `tests/test_key_guard.c` and friends | Wake key guard, BSP fault injection and sleep/wake recovery |

When adding pure logic (state machines, pagination, timing), keep it independent of
ESP-IDF / LVGL, add a host test, and wire it into `tools/validate.sh`.

## 10. Troubleshooting

- **Boxes or garbled text on screen**: the font subset is missing glyphs. Run
  `tools/gal_font.py --check` and regenerate. Also remember that row-list items are created
  lazily, so a font-tier switch must update existing rows too (`gal_ui_set_font_size` does).
- **Text changed but the UI did not**: regenerate the fonts and rebuild; `--check` fails
  the gate otherwise.
- **The device disappears from the computer**: after 420 seconds idle it enters deep sleep
  and USB power drops. Press any key to wake it (it reboots afterwards).
- **No sound**: check the ES8311 and I2S path (`bsp_audio_*`); playback failures are logged
  on the serial console.
- **NVS**: unlock and progress state is written only when it changes; do not write
  high-frequency state directly to NVS.
- **Concurrency**: LVGL is not thread-safe. Any non-LVGL task touching the UI must hold
  `bsp_lvgl_lock()`. Button callbacks must stay non-blocking (slow work belongs in tasks,
  as the music player does).

## 11. Delivery

Report firmware changes as separate fields, as required by the repository:

```text
Build:      PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: remaining board, instrument, or manual checks
```

Flashing does not require backing up the firmware already on the device, and it does not
preserve user data; export anything you need to keep beforehand.
