<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# NianNiang Love Story - AI Passport 240x320 portrait visual-novel reader

A **landscape** visual-novel reader that brings the Xiaomi Band port of
NianNiang Love Story to the AI Passport: **13 scenes, 1,248 dialogue lines, two choice
points, four endings, 16 backgrounds, 15 event CGs and 30 expression sprites**,
fully offline. Story and art come from
[`hrk666666/NianNiang-MiBand-10`](https://github.com/hrk666666/NianNiang-MiBand-10)
(itself built on `hrk666666/Senren-Banka-MiBand-10`). This branch is a dedicated
reader: its own script interpreter, image pipeline and every screen.

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

- Tag: `v1.0.0-nianniang` (branch `feature/port-nianniang-landscape`)
- Merged image: `FoloToy-AI-Passport-full.bin` (1,814,064 B)
- Application image: 1,748,528 B, version `1.0.0`
- Partition use: **21.01%**, **6,574,544 B** free

## Known limits

- Flash use is 21.01% with 6,574,544 B free; confirm the budget before changing content.
- Content coverage and the trade-offs behind it: [docs/reference/shinku-chen/nianniang/](docs/reference/shinku-chen/nianniang/).
- This title is **240x320 portrait**; the shared `-landscape` branch suffix is historical and does not describe the layout.

## Porting notes

The reader contract, hardware traps and acceptance checklist shared by all twelve ports: [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port).

## License

This repository is MIT, like upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport); see `LICENSE`. Scripts, art and fonts come from the original game and its community ports and remain with their rights holders; this port is for personal, non-commercial use.
