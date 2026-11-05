<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Galgame Master - AI Passport 320x240 landscape visual-novel reader

A complete landscape (320×240) port of the GalGod galgame: all 18 chapters / 5,457 lines / 119,410 characters, 15 choice
groups including the scored quiz with the `bad_end_flag >= 7` branch, 4 endings
plus afterstory, all 55 sprite expressions, 38 backgrounds, 34 CG/SD references (33 unique after
de-duplication) with a paged gallery, three save slots plus autosave, history and settings.

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
- Idle: dim after 90 s, screen off after 300 s, deep sleep after 1800 s; any key wakes the device and resumes the session

## Build and flash

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o "$PWD/build-release/FoloToy-AI-Passport-full.bin"
python -m esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 \
  build-release/FoloToy-AI-Passport-full.bin
```

Board: ESP32-C3, 8 MB flash, no PSRAM. Flashing and stored-data rules: [`docs/development/engineering/firmware-layout.md`](docs/development/engineering/firmware-layout.md).

## Release

- Tag: `v1.0.0-galgod` (branch `feature/port-galgod-landscape`)
- Merged image: `FoloToy-AI-Passport-full.bin` (3,351,360 B)
- Application image: 3,285,824 B, version `1.0.0`
- Partition use: **39.48%**, **5,037,248 B** free

## Known limits

- Flash use is 39.48% with 5,037,248 B free; confirm the budget before changing content.
- Content coverage and the trade-offs behind it: [`docs/games/`](docs/games/).

## Porting notes

The reader contract, hardware traps and acceptance checklist shared by all twelve ports: [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port).

## License

This repository is MIT, like upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport); see `LICENSE`. Scripts, art and fonts come from the original game and its community ports and remain with their rights holders; this port is for personal, non-commercial use.
