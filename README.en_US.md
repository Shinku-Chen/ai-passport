<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Stella of the End - AI Passport 320x240 landscape visual-novel reader

The complete *Stella of the End* story as an AI Passport visual-novel reader: script, backgrounds, sprites and event CGs are packed into the application partition and read offline.

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

- Tag: `v1.0.0-stella` (branch `feature/port-stella-landscape`)
- Merged image: `FoloToy-AI-Passport-full.bin` (8,321,616 B)
- Application image: 8,256,080 B, version `1.0.0`
- Partition use: **99.20%**, **66,992 B** free

## Known limits

### Flash headroom warning

- Only **66,992 B** of flash is free (99.20% used); any change must start with a flash budget check.
- Content coverage and the trade-offs behind it: [docs/reference/shinku-chen/stella/](docs/reference/shinku-chen/stella/).

## Upstream and provenance

- Upstream port repository: [xuanyide01/stella-rw5](https://github.com/xuanyide01/stella-rw5) (snapshot `4fa1e7b5534a`)
- Application baseline: [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) at `feature/galgame-framework`
- Original work: *Stella of the End*; script, art and fonts remain with their rights holders, the upstream projects declare no licence, and this port is for personal, non-commercial use

## Porting notes

The reader contract, hardware traps and acceptance checklist shared by all twelve ports: [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port).

## License

This repository is MIT, like upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport); see `LICENSE`. Scripts, art and fonts come from the original game and its community ports and remain with their rights holders; this port is for personal, non-commercial use.
