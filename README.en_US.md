<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# anemoi - AI Passport 320x240 landscape visual-novel reader

Part of the Redmi Watch 5 port of *anemoi* on the 320×240 AI Passport panel: layered body + native
face sprites, 320×180 backgrounds/CGs, the request board, 30 save slots plus an auto slot, a CG
gallery and a chapter list. **This build covers 71 of 264 source scenes** (23 common chapters plus
the request letters/tasks; 764,847 of 3,088,939 dialogue bytes); heroine routes and after-story
arcs are not ported and show a volume-boundary screen instead of silently skipping content. Measured with a measurement-only 32-bit text-id variant, a whole-work single pack is 18.97 MB at
0.52/32 colours/q30 and 16.70 MB at 0.46/16 colours/q25 — 2.6× / 2.3× the 7,266,422 B resource
ceiling. That bounds these measured encodings, not every possible strategy; splitting into volumes
is a candidate, not a delivered whole work.

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

- Tag: `v1.0.0-anemoi` (branch `feature/port-anemoi-landscape`)
- Merged image: `FoloToy-AI-Passport-full.bin` (8,306,384 B)
- Application image: 8,240,848 B, version `1.0.0`
- Partition use: **99.01%**, **82,224 B** free

## Known limits

- Only **82,224 B** of flash is free (99.01% used); any change must start with a flash budget check.
- Content coverage and the trade-offs behind it: [docs/development/engineering/galgame-anemoi.md](docs/development/engineering/galgame-anemoi.md).

## Porting notes

The reader contract, hardware traps and acceptance checklist shared by all twelve ports: [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port).

## License

This repository is MIT, like upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport); see `LICENSE`. Scripts, art and fonts come from the original game and its community ports and remain with their rights holders; this port is for personal, non-commercial use.
