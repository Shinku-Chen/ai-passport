<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Mahou Shojo no Majo Saiban - AI Passport 320x240 landscape visual-novel reader

The complete *Mahou Shojo no Majo Saiban* story as an AI Passport 320x240 landscape visual-novel reader: script, backgrounds, sprites and event CGs are packed into the application partition and read offline.

- **All 458 source script files / 424 scenes / 90,243 commands** are packed into
  one read-only flash image and executed by a C VM that mirrors `engine.js` /
  `scene.js`: background layers, sprite instances, focus, conditions,
  `call`/`return`, `offer`/`choice`, map investigations, evidence submission,
  objection words, trial damage, book versions, autosave points, chapter
  entries, and both source and chapter entry modes.
- **Complete art**: 62 backgrounds, 172 CGs in 61 groups, 151 portraits, 19
  trick images, 151 book images, 2 solid placeholders and 2 title images
  (559 total), fitted to 320×240 without
  cropping; portraits use JPEG plus a 1bpp alpha mask.
- Dedicated landscape UI: title, reader, choices, menu, history, save/load,
  settings, gallery/full-screen CG, chapter select, witch book, endings,
  about, and error pages.
- 30 manual slots plus one independent autosave slot; version/length/CRC
  validation, failed writes never replace a valid slot. Gallery/chapter
  unlocks use a separate versioned store that only grows and retries failed
  writes.
- Three-button semantics: UP advances (long press fast-forwards), DOWN opens
  history (long press toggles auto), OK opens the menu/objection list; lists
  use UP/DOWN + OK and long OK to return; choices require explicit OK.

## Controls

| Key | While reading | In lists and choices |
| --- | --- | --- |
| OK (short) | Advance one page; completes the page if it is still typing | Confirm |
| OK (hold) | Open the menu | Go back one level |
| UP (short) | Advance one page | Move the cursor up |
| UP (hold) | Fast-forward while held | - |
| DOWN (short) | Open history | Move the cursor down |
| DOWN (hold) | Toggle auto-read | - |

## Features

- Typewriter text with adjustable speed (instant included)
- Auto-read: advances after a page finishes typing and keeps the screen awake
- Fast-forward: the whole page appears at once instead of typing quickly
- Saves: several manual slots plus an automatic slot written on every scene change
- History, chapter list and a CG gallery
- Idle: dim after 60 s, screen off after 300 s, deep sleep after 1800 s; any key wakes the device and resumes the session

## Build and flash

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o "$PWD/build-release/FoloToy-AI-Passport-full.bin"
python -m esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 \
  build-release/FoloToy-AI-Passport-full.bin
```

Board: ESP32-C3, 8 MB flash, no PSRAM. Flashing and stored-data rules: [`docs/development/engineering/firmware-layout.md`](docs/development/engineering/firmware-layout.md).

## Release

- Tag: `v1.0.0-manosaba` (branch `feature/port-manosaba-landscape`)
- Merged image: `FoloToy-AI-Passport-full.bin` (7,959,888 B)
- Application image: 7,763,280 B, version `1.0.0`
- Partition use: **94.77%**, **428,720 B** free

## Known limits

- Flash use is 94.77% with 428,720 B free; confirm the budget before changing content.
- Content coverage and the trade-offs behind it: [`docs/games/`](docs/games/).

## Porting notes

The reader contract, hardware traps and acceptance checklist shared by all twelve ports: [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port).

## License

This repository is MIT, like upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport); see `LICENSE`. Scripts, art and fonts come from the original game and its community ports and remain with their rights holders; this port is for personal, non-commercial use.
