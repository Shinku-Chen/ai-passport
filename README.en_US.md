<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Paper no Mahou Tsukai - AI Passport 320x240 landscape visual-novel reader

A **landscape 320×240** visual-novel reader for AI Passport, ported from the paper-rw5 watch port:
**47 scene scripts / 56,634 ops / 44,521 lines of dialogue, 4 choices, 6 endings, 80 CG groups with
415 images, 341 sprites (163 full blocks + 178 expression patches)**, fully offline.

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

- Tag: `v1.0.0-paper` (branch `feature/port-paper-landscape`)
- Merged image: `FoloToy-AI-Passport-full.bin` (6,354,848 B)
- Application image: 6,289,312 B, version `1.0.0`
- Partition use: **75.56%**, **2,033,760 B** free

## Known limits

- Flash use is 75.56% with 2,033,760 B free; confirm the budget before changing content.

## Porting notes

The reader contract, hardware traps and acceptance checklist shared by all twelve ports: [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port).

## License

This repository is MIT, like upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport); see `LICENSE`. Scripts, art and fonts come from the original game and its community ports and remain with their rights holders; this port is for personal, non-commercial use.
