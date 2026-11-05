<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Summer Pockets REFLECTION BLUE - AI Passport 320x240 landscape visual-novel reader

Turns the AI Passport into a 320×240 landscape reader for the RW5 Summer Pockets
REFLECTION BLUE source snapshot: 422 scenes, 81,040 dialogue/narration ops,
188 choice groups, 27 map stages, 14 routes / 252 chapter entries, 136 CG groups
(712 variants) and 292 portraits/expressions are all packed. Title, dialogue,
choices, menus, history, 30 manual slots + an independent auto slot, chapters/
routes, CG gallery and progress for all 14 endings are implemented.

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

- Tag: `v1.0.0-summer` (branch `feature/port-summer-landscape`)
- Merged image: `FoloToy-AI-Passport-full.bin` (7,716,528 B)
- Application image: 7,585,456 B, version `1.0.0`
- Partition use: **91.86%**, **672,080 B** free

## Known limits

- Flash use is 91.86% with 672,080 B free; confirm the budget before changing content.
- Content coverage and the trade-offs behind it: [docs/development/engineering/galgame-summer.md](docs/development/engineering/galgame-summer.md).

## Upstream and provenance

- Upstream port repository: [summer-rw5](https://github.com/xuanyide01/summer-rw5) (snapshot `28f8a41c3e77`; converted from the Xiaomi Vela project)
- Application baseline: [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) at `feature/galgame-framework`
- Original work: *Summer Pockets REFLECTION BLUE*; script, art and fonts remain with their rights holders, the upstream projects declare no licence, and this port is for personal, non-commercial use

## Porting notes

The reader contract, hardware traps and acceptance checklist shared by all twelve ports: [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port).

## License

This repository is MIT, like upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport); see `LICENSE`. Scripts, art and fonts come from the original game and its community ports and remain with their rights holders; this port is for personal, non-commercial use.
