<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Tenshi☆Sousou RE-BOOT! — AI Passport portrait reader

A portrait visual-novel reader that brings the Xiaomi Band quick-app fan port of
*Tenshi☆Sousou RE-BOOT!* and its Chinese translation to the
[FoloToy AI Passport](https://ai-passport.folotoy.cn): **61,436 script pages,
61,289 dialogue lines, 1,165,911 characters, 45 chapter points, 13 choice points
and 15 ending points** — six heroine routes (Noa, Amane, Kurumi, Kaguya, Orie,
Fumika), two bad ends, one plain end and six return-to-title screens — fully
offline.

The script, artwork and translation come from
[`hezdaaa/tsxxreboot-miband`](https://github.com/hezdaaa/tsxxreboot-miband),
a Mi Band quick app. This branch re-implements the reader on the ATRI / Starry
Sky Railroad LVGL page system, adds the original branching system (15 endings,
five flag gates, 18 forward jumps and 38 back jumps) and drives it with the
three keys. Status: **released** — tag `v0.1.0-tsxx-reboot`, submitted to the
AI Passport community market (under review).

- Branch: [`feature/tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tsxx-reboot)
- Release: [`v0.1.0-tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-tsxx-reboot)
  — merged image `FoloToy-AI-Passport-full.bin`, **8,135,520 bytes**
- Upstream work: [`hezdaaa/tsxxreboot-miband`](https://github.com/hezdaaa/tsxxreboot-miband)
  — the Mi Band quick-app release this port derives from; it declares no licence,
  and the script, sprites and backgrounds belong to Yuzusoft and the original
  publisher. This port only converts and repacks: the firmware ships the
  converted resource pack and not the original material, which is committed
  under `assets/tsxx-source/` purely as attribution and as the input that
  reproduces the pack. It is a non-commercial fan study; please support the
  official release.
- Asset toolchain:
  [`tools/tsxx_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/tsxx-reboot/tools/tsxx_pack.py)
  (script, images and the branch tables into `main/tsxx_data/tsxx_pack.bin`,
  6.26 MiB) and
  [`tools/tsxx_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/tsxx-reboot/tools/tsxx_font.py)
  (16 px Chinese subset, 3,417 code points).

## Three-key controls

On the title screen UP / DOWN move the cursor, **OK** enters and **OK (hold)**
powers the device down. While reading, **UP**, **DOWN** and a short **OK** all
advance one line (which also reveals a line that is still typing); **UP (hold)**
fast-forwards while held and stops on release; **DOWN (hold)** toggles
auto-reading, which waits 0.7 s after a page finishes and ends on any key press
without counting as idle. **OK (hold)** opens the menu (continue, save, load,
skip chapter, chapter jump, back to title).

Choices use UP / DOWN + OK; on lists **OK (hold)** goes back; on the save screen
**OK** saves or loads and **OK (hold)** deletes a manual slot. Settings switch
the text speed (slow / medium / fast — 50, 28 and 12 ms per character) and
auto-reading. Idle for 45 s dims the backlight, 2.5 min turns it off and 7 min
enters deep sleep; any key wakes the device.

## Features

- **Branching and saves** — five manual slots plus one automatic slot written
  on every scene change (throttled to once every four seconds, forced at choice
  points and before sleep). The title screen offers "continue" from the automatic
  slot; the accumulated choice history is stored with the position, so loading a
  save cannot select the wrong branch.
- **Data-driven original branching** — all 15 ending points, five flag gates
  (11 rules, 54 conditions), 18 forward and 38 back jumps come from
  [`assets/tsxx-source/branch.json`](assets/tsxx-source/branch.json); the packer
  checks every gate condition against a real choice point and the build fails if
  the configuration and the pack disagree.
- **The art layer is 1:1 with the screen** — the canvas is the 240 × 320 panel
  itself, so sprites are stored in canvas coordinates and are never resampled
  when composited; backgrounds and event images stay at 180 × 240 in the pack and
  are scaled up by 4/3 with nearest-neighbour sampling while the text layer is
  drawn at native resolution.
- **Full-screen compositing without PSRAM** — the 150 KB RGB565 canvas uses no
  decode staging buffer: backgrounds, event frames, patches and sprites all
  decode chunk by chunk through the tjpgd callback interface, and sprites carry a
  1 bpp mask that is blitted row by row from Flash.
- **The resource pack is read through two mappings** — the script region at the
  head of the pack (about 1.9 MiB of page tables, text stream, choices and branch
  tables) is mapped read-only for the whole runtime, while images are fetched
  through a 256 KB sliding `esp_partition_mmap` window and decoded as soon as they
  arrive.
- **Parsing, advancing and gate resolution have host tests** — pack reading, the
  advance order (ending point, then jumps and gates, then the next page) and
  pagination do not depend on ESP-IDF or LVGL; a host test walks the entire page
  table with the real pack and then plays all six heroine routes.

## Build and flash

```bash
./tools/validate.sh --firmware   # build, merge and verify the 0x0 image
idf.py build                     # optional incremental app build
# flash build/FoloToy-AI-Passport-full.bin from 0x0 (web flasher or esptool)
```

The packers need Pillow and FreeType in the Python environment ESP-IDF builds
with. `idf.py build` alone does not produce or verify the merged image. Use the
[browser flasher](https://ai-passport.folotoy.cn/tools/web-flasher/) or
`esptool`; the target is an 8 MB Flash, no-PSRAM AI Passport.

## Community cover and release information

- **Community submission:** submitted to the AI Passport community market
  (under review).
- **Community / release cover:** `assets/images/tsxx-reboot-market-cover.png`,
  **1152 × 1536 PNG, 3:4**. It is derived from
  `assets/images/tsxx-reboot-cover.png` (420 × 582) by cropping the centre to
  420 × 560 (11 rows off the top and bottom, an exact 3:4 ratio) and upscaling to
  1152 × 1536 with Lanczos. It is used for the community listing and release
  notes; the image is not duplicated in the archive.
- **Release tag:** `v0.1.0-tsxx-reboot`; merged
  `FoloToy-AI-Passport-full.bin`, **8,135,520 bytes**, from the tag-triggered CI
  build.
- **Pack:** `main/tsxx_data/tsxx_pack.bin`, 6.26 MiB; 16 px Chinese subset with
  3,417 code points.

## Known limitations

- This is a **240 × 320 portrait** reader; there is no landscape mode.
- The body face is a fixed 16 px subset (3,417 code points); there is no 20 px
  body option.
- The target has no PSRAM. The 150 KB canvas and the 6.26 MiB pack are sized for
  the 8 MB, single-app build; adding media needs a new Flash and memory budget.
- There is no voice, music or network path in the firmware. The story runs
  entirely from the converted pack in Flash.
- The script, artwork and characters belong to Yuzusoft and the original
  publisher. This is a non-commercial fan study; the firmware ships the converted
  resource pack, not the original material.

## Experience notes

Reusable engineering notes collected from these releases are under
[`docs/reference/shinku-chen/`](docs/reference/shinku-chen/). See
[`docs/README.md`](docs/README.md), [`AGENTS.md`](AGENTS.md) and
[`docs/contribution/`](docs/contribution/) for the repository's build and
contribution workflow.

## License

The reader source is released under the repository's [MIT licence](LICENSE).
The script, artwork, backgrounds and character material remain the property of
Yuzusoft and the original publisher and are not relicensed by this project.
