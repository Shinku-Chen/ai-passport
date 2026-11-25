<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Maitetsu · Niiroku Route — offline reader for FoloToy AI Passport

A landscape 320 × 240, three-key port of the RW5 watch port of the Niiroku route from
*Maitetsu* ([`xuanyide01/maitetsu-rw5`](https://github.com/xuanyide01/maitetsu-rw5),
pinned at `9c008406853affe973ceb80f62128696054365e7`) to the FoloToy AI Passport:
four chapters, 1,537 paragraphs, 675 scenes, 35 CGs in six groups, fully offline, plus
the complete 283.88-second title theme.

This repository is a personal fork of [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport).
This branch (`release/maitetsu-20261125`) carries only this title; board facts, the BSP and
the development workflow still come from upstream — see [`AGENTS.md`](AGENTS.md) and
[`docs/README.md`](docs/README.md).

## Scope

- Only the **four Niiroku chapters** present in the RW5 port: Showa 41 Kokura, the
  Yuuichi trip, the Kokura storm, and the epilogue. This is not every route of the
  original game and not another heroine's route.
- The original engine has no branch choices on this route, so the story is linear:
  1,537 paragraphs lead to a single ending.
- Text, artwork, logos and music remain the property of Lose / HIKARI FIELD and their
  rights holders. The repository's MIT license covers the authored code only. See
  [`docs/development/engineering/maitetsu-port.md`](docs/development/engineering/maitetsu-port.md).

## Controls (three keys, landscape)

| Page | Short OK / UP / DOWN | Long OK | Long UP | Long DOWN |
| --- | --- | --- | --- | --- |
| Reading | Advance (while typing the same press completes the page and moves to the next line; long text advances one page at a time) | In-game menu | Jump to the next sentence immediately, keep advancing one sentence per input tick while held, stop on release | Toggle auto-play |
| Lists / menus / settings | Move, OK runs | Back | — | — |

- Fast-forward skips the typewriter and auto-play intervals; it stops by itself at a
  choice or ending page.
- While auto-play is on, a short press still advances; the key is never swallowed.
- The chapter browser and CG recall are **previews**: entering them marks the run as a preview.
  Even reading from chapter 1 through the ending, or saving/loading/restarting mid-way,
  never awards a permanent ending. Only a New Game run that reads all four chapters does.

## Features

- The 19-screen GalGame framework: title, dialogue, menu, three manual save slots,
  automatic progress, history, settings, gallery (CG / recall / chapters / music),
  chapter cards, ending and about.
- Scenes keep the source composition: background plus expression/body patches that were
  composited over the background at build time, shown at an exact 16:9 320 × 180 and
  never cropped.
- The CG browser pages through all 35 CGs (eight per page), including indices 33–35
  that exceed the framework's former 32-bit CG limit.
- The title theme is the complete 283.88 s, 4 kHz mono source as 4-bit IMA ADPCM
  (586 KB), linearly interpolated to 16 kHz on playback — not shortened, not replaced
  by square waves.

## Build

Requires ESP-IDF **5.5.3** and Python 3 (Pillow only when regenerating assets):

```bash
source <ESP-IDF-v5.5.3>/export.sh
idf.py build
```

The media pack and title theme are committed under `assets/maitetsu/`; code-only changes
do not need a regeneration. Regenerating requires the pinned RW5 snapshot:

```bash
python tools/maitetsu_assets.py --source <maitetsu-rw5>
python tools/maitetsu_assets.py --source <maitetsu-rw5> --check
```

## Validation status

- Build: ESP-IDF 5.5.3 `idf.py build` passes; app 7,759,632 bytes of the 8,323,072-byte
  partition.
- Host tests: content reconciliation (1,537 paragraphs), strip decoding (3,994 strips),
  a real LVGL decoder integration test, scene-composition sampling, key / fast-forward /
  auto / preview state machine, save, progress and unlock formats.
- Device tests: **not run this round** (no flashing authorization). Landscape, physical
  keys and audio still need on-device confirmation.

Full evidence, budgets and the remaining unverified items are in
[`docs/development/engineering/maitetsu-port.md`](docs/development/engineering/maitetsu-port.md).
