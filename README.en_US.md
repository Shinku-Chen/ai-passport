<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# 9-nine- Haruiro · Offline Reader for AI Passport

This branch turns the AI Passport (ESP32-C3, 8 MB flash, no PSRAM, 320×240
landscape, three buttons) into a **complete offline visual-novel reader** for
*9 -Nine-: Episode 3* (`Haruiro`). Nothing is fetched at runtime: the
story, art, BGM and opening movie are compiled into one full-flash image.

- Branch: `release/nine-haruiro-20261125` — this README covers only this title
- Framework: `feature/galgame-framework` baseline `b9909c13`; its pages,
  three-key interaction, pagination and persistence are kept
- Source: `xuanyide01/9-nine-rw5` @ `6a8907a5ae576f4377abe4b74f15bd9af13efe07`
- Full design, budget and verification:
  [`docs/development/engineering/nine-haruiro-port.md`](docs/development/engineering/nine-haruiro-port.md)

## Scope

The pinned source is the Watch 5 port of the **non-R18 Chinese main line**, and it
is the only authority for this work:

| Item | Count |
| --- | --- |
| Graph nodes / chapters | 78 / 15 |
| Dialogue frames | 10,659 (plus one OP marker) |
| Choice nodes / visible options | 13 / 13 |
| Ending milestones | 2 (branch, true) |
| State variables | 23 (10 persistent keys) |
| Visual snapshots / layers | 2,368 / 9,333 |
| CG gallery | 18 groups / 100 variants |
| BGM tracks / OP frames | 2 / 493 at 5 fps (98.62 s) |

The snapshot itself excludes the 11 R18 scenes and all voice tracks; this port
neither reconstructs nor invents them. "Complete" means complete relative to that
snapshot: every dialogue frame, choice target, both endings and every art
reference are inside the package. All 10,659 frames are reconciled line by line,
and the 10,674-step trajectory matches the source `engine.js` exactly.

## Controls

320×240 landscape. Art occupies 320×180 with a translucent bottom dialogue
overlay a compact, fitted, semitransparent speaker badge above the body box; the body box occupies the lower 60 px with three 16 px lines (two 20 px lines). Reading page numbers are not shown; chapter name top-left, battery top-right.

| Input | Action |
| --- | --- |
| Short OK / UP / DOWN | Advance immediately. A page that is still typing is completed and the same press moves to the next page or line; a press is never swallowed |
| **Long OK** | In-game menu (resume, save, load, history, settings, back to title) |
| **Long DOWN** | Toggle auto-play; 1.2 s after a page is fully typed |
| **Hold UP** | Fast-forward: the long threshold immediately enters the next whole sentence, skipping the remaining pages and typing/auto wait; each held input tick advances at most one sentence with no extra 90/120 ms accumulation; release stops without a short press; choices/ending/OP/resource errors stop |
| Choice | UP/DOWN select, OK confirms; long OK opens the same in-game menu as the reading page, and returning keeps the exact options/selection |
| Secondary pages | UP/DOWN move, long OK returns |

- A manual press during auto-play turns auto off **and still advances one line**.
- A short press always moves the page or line cursor; it never merely completes typing.
- A long press never adds a short press on release; menus, auto and fast-forward
  clear their transient state on page changes.
- Menu actions never move the story VM. Returning from a menu opened on the
  reading page restores the exact text sub-page; returning from one opened on a
  choice restores the complete options and choice selection.
- **30 manual save slots** (four-row moving window) plus automatic progress.
  Run variables, manual/automatic saves and permanent ending qualification are
  independent: loading an old slot restores its position, text sub-page and run
  flags but never rolls back endings or title visuals unlocked later. A saved
  page beyond the frame's current page count clamps to the last page.
- Gallery: 18 CG groups / 100 variants (UP/DOWN cycles variants), 15 chapter
  previews, 2 BGM tracks and an OP gallery. The OP gallery plays the complete
  opening animation and track; natural end, short OK skip and long OK back all
  return to the same OP row without advancing the story. A chapter preview never
  writes permanent progress.
- Title visuals 1/2/3 follow the source UI's permanent loop/true qualification
  (not the transient `titleBg` milestone): either clear unlocks visual 2; both
  unlock visual 3. A second New Game reaching n40 does not revoke visual 3.
- Saves carry a CRC and title identity; a damaged slot reports a validation
  failure and never overwrites live state.

## Build

Requires ESP-IDF 5.5.3:

```bash
./tools/validate.sh --static      # repository checks + all host tests (source trace diff included)
./tools/validate.sh --firmware    # fresh build + merged image + verify_firmware + archive
idf.py -p <port> flash monitor    # only when you want to test on hardware
```

Artifacts: `build/FoloToy-AI-Passport-full.bin` (full-flash image, write at 0x0)
and `build/firmware/<sha>/` (app/full/ELF/MAP archive). This branch does not tag,
publish a release, or touch other titles' branches/tags.

## Assets and budget (final firmware)

| Item | Bytes |
| --- | ---: |
| Story package | 778,099 |
| Image/audio package | 4,969,689 |
| Compiled 16/20 px CJK fonts | 799,119 |
| Application image | 8,062,448 / 8,323,072 (96.9 %, 260,624 free) |
| Merged full image | 8,127,984 |
| Static DRAM | 160,744 / 321,296 (50.0 %) |

- Art: source 432×243 → package stage 320×180 (full composition, sprite anchors
  and face patches) → runtime 256×144 canvas (eight 12,288 B bands) → panel upscale
  1.25× to 320×180. The 256 canvas is a real C3 heap-budget trade-off, quantified
  in the design document.
- BGM: 16 kHz mono IMA ADPCM (1,518,396 B), fully decoded/resampled from the
  source 36 kbps Opus.
- OP: all 493 frames at 5 fps (full 98.62 s), played with the opening track. The source has no
  voice to preserve.
- Largest single allocation: 12,288 B; no >32 KB contiguous block. LVGL uses the
  system heap instead of a private 64 KB pool.

## Verification

| Field | Result |
| --- | --- |
| Build | PASS (ESP-IDF 5.5.3, app 7,254,656 B) |
| Host tests | PASS (`validate.sh --static`) |
| Firmware gate | PASS (layout, merged image, `verify_firmware.py`, archive) |
| Device tests | **NOT RUN** |
| Unverified | physical buttons, panel orientation/legibility, real heap peak, 16 kHz audio loudness, deep-sleep wake, battery, long-run stability |

Software tests are not hardware tests. No flashing, serial monitor or device
operation was performed for this work.

## Attribution and license

Story, art, music and the source font come from the pinned
`xuanyide01/9-nine-rw5` snapshot. Their rights remain with the original rights
holders (Palette / 9-nine- and the Chinese patch authors). The assets are included
only for this offline, non-commercial device port and are not separately licensed
for redistribution. `nine-text.otf` is used only to generate font subsets and is
not embedded. The upstream framework, BSP and TJpgDec retain their own licenses.
