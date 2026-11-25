<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# 9-nine- Episode 4: Yuki-iro — AI Passport

An offline reader for *9-nine- Episode 4: Yuki-iro* (Episode 4) on AI Passport:
**320×240 landscape, three keys, fully offline**. The firmware contains the
complete Episode 4 story, every branch and ending record; it needs no network,
account, or PC runtime.

- Upstream watch project: <https://github.com/xuanyide01/9-nine-rw5>
- Pinned commit: `6a8907a5ae576f4377abe4b74f15bd9af13efe07`
- Release source branch: `release/nine-yukiiro-20261125`, based on
  `feature/galgame-framework` `b9909c13605413142fb35f28aabcc494a666fa00`
- Full details, budget and known limits:
  [English](docs/reference/shinku-chen/nine-yukiiro/README.md) ·
  [简体中文](docs/reference/shinku-chen/nine-yukiiro/README.zh_CN.md)

## Content and scope

| Item | Count |
| --- | --- |
| Graph nodes / text frames | 212 / 12,665 (plus one OP marker) |
| Chapters | 23 |
| Choice nodes / records | 47 / 70 (63 reachable under source gates; 7 structurally unreachable) |
| Ending records | 2 (`loop1` mid-story completion, `true` terminal) |
| CG groups / variants | 19 / 108 |
| Runtime variables | 78 |
| Text | 668,498 UTF-8 bytes in 84 zlib blocks |

Artwork comes from the upstream watch project's processed graph, story chunks,
backgrounds/CGs, sprites/faces, title visuals, OP and two BGM tracks. The
uncommitted original PC scripts are **not** rebuilt. PC voice and adult assets
are not included (the watch source is already non-adult and zero-voice).

## Controls (three keys)

- **Short OK / UP / DOWN**: advance. If the current long line has another
  page, move to that **complete page**; otherwise advance to the next line.
  While typing or auto-playing the press still advances immediately and shows
  the destination in full; it never merely completes typing or exits auto.
- **Long OK**: open the in-game menu on the dialogue or choice page; return on
  secondary pages. Returning from Save/Load/Settings/History restores the same
  dialogue page or the full choice list with its prior selection; menu actions
  never advance the VM or grant permanent progress.
- **Long UP**: reaching the long-press threshold immediately advances to the
  next whole sentence, skipping the rest of the current sentence's pages and
  typewriter/auto waits. While held, each input tick advances at most one
  sentence; there is no 90/120 ms accumulation or catch-up. Release stops at
  once. It stops at a choice/ending/OP and never selects for you. A normal short
  press still advances one complete page.
- **Long DOWN**: toggle auto-play; release never adds an extra step.
- **Gallery → OP playback**: short OK starts the real OP frames and opening
  track; short OK skips, natural end returns to the same OP row. Gallery
  playback never advances the live story VM, writes auto/manual saves or
  permanent progress, or unlocks endings/CGs.
- A long-press release never fires an extra short press. Chapter previews never
  write automatic or permanent progress.

## Build

ESP-IDF 5.5.3, ESP32-C3, 8 MB Flash, no PSRAM:

```bash
source <ESP-IDF-v5.5.3>/export.sh
idf.py build
idf.py merge-bin -o "$PWD/build/FoloToy-AI-Passport-full.bin"
python3 tools/verify_firmware.py build
```

Full host checks (the source checkout is read-only and may be outside the repo):

```bash
NINE_SOURCE=<pinned-9-nine-rw5-checkout> CC=<host-cc> ./tools/validate.sh --static
```

`NINE_SOURCE` enables the source-to-pack audit and the source-JS/C-VM trace
differential; without it those two checks report SKIP while the pack, app,
image and font tests still run.

## Verification status

- Build: PASS (ESP-IDF 5.5.3; merged image passes layout verification)
- Host tests: PASS (source-to-pack audit 0 errors; 36 traces / 457,632 steps /
  0 failures; production state machine, strict choice/menu return regression, real image decode against a source-art
  reference, real 32-bit LVGL 19-page rendering (including the Gallery OP entry), font coverage)
- Device tests: **LIMITED USER PASS** (2026-11-24, E firmware: name/body layout; full Gallery OP playback, sound and return; FF release-stop, save/load, font pagination)
- Unverified: every route/ending, long-run stability, power loss and NVS durability, and all hardware items. Do not extend this limited device acceptance to full certification.

A host pass is not full hardware validation; only the listed scope was tested.

## Attribution

Original work: Palette, *9-nine-* Episode 4. The watch project and its assets
are by `xuanyide01` at the pinned repository. This port's reader, packer, C
decoders, tests and documentation are new code in this fork. Upstream code uses
its MIT license; game artwork, script and audio rights remain with their owners.
