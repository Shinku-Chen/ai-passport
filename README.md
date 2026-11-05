<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# limelight lemonade jam — AI Passport reader

A portrait visual-novel reader that brings the Xiaomi Band release of
*limelight lemonade jam* to the [FoloToy AI Passport](https://ai-passport.folotoy.cn):
**230 chapter points, 68,229 dialogue lines, about 1.2 million characters,
991 packed images and 8 choice points**, fully offline in an 8 MB ESP32-C3
firmware with no PSRAM.

> Music, friends, and love. From the day he met her, the world grew brighter.

The story and artwork come from
[`skdkzzx/limelight-lemonade-jam-xiaomi-band10`](https://github.com/skdkzzx/limelight-lemonade-jam-xiaomi-band10),
a Vela OS touch app for Xiaomi's Band. This branch re-implements the reading
engine in C on LVGL for the Passport's three keys. Status: **released** as
`v0.1.0-limelight` and submitted to the AI Passport community market (under review).

- Branch: [`cindy/curious-babbage`](https://github.com/Shinku-Chen/ai-passport/tree/cindy/curious-babbage)
- Release: [`v0.1.0-limelight`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-limelight)
  — merged image `FoloToy-AI-Passport-full.bin`, **7,897,824 bytes**
- Upstream work: [`skdkzzx/limelight-lemonade-jam-xiaomi-band10`](https://github.com/skdkzzx/limelight-lemonade-jam-xiaomi-band10)
  — the Xiaomi Band release this port is based on. That repository declares no
  licence; its artwork and script are committed with this branch, packed into
  the firmware, and credited to the upstream project as their source.
- Asset toolchain:
  [`tools/limelight_material_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/cindy/curious-babbage/tools/limelight_material_pack.py),
  [`tools/limelight_script_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/cindy/curious-babbage/tools/limelight_script_pack.py)
  (into `main/limelight_data/`, 4.49 MB + 1.81 MB), and
  [`tools/limelight_lvgl_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/cindy/curious-babbage/tools/limelight_lvgl_font.py)
  (16 px Noto Sans SC subset, 3,449 code points).

## Three-key controls

The chapter is shown at the top left as `chapter-section` (for example `7-2`);
the battery percentage is at the top right. A short press registers at 180 ms
and a long press at 500 ms; the BSP passes those timings explicitly instead of
using the button component's 1,500 ms default.

| Key | Reading | Menu / list |
| --- | --- | --- |
| UP (short) | next line, or reveal the rest of the current one | move the selection up |
| UP (hold) | fast-forward while held; stop on release | — |
| OK (short) | open the menu | activate the selected row |
| OK (hold) | — | leave the list |
| DOWN (short) | next line, or reveal the rest of the current one | move the selection down |
| DOWN (hold) | toggle auto-read (marker at bottom right; any key stops it) | — |

Choices are selected with UP / DOWN and confirmed with OK. The menu holds
continue, save, load, chapter select, CG gallery, settings and back to title.
Idle behaviour: 60 s dims the backlight, 3 min turns it off and 7 min enters
deep sleep; any key wakes the device. Auto-read and fast-forward count as
activity, so the screen stays lit while they run.

## Features

- **Five manual saves plus one automatic save.** The automatic slot is written
  on every scene change and offered as its own row when loading. Hold OK on a
  manual slot to delete it. The saved page position is part of the state.
- **230 chapter markers.** The chapter jump list shows the upstream script's
  `[CHAPTERx-y]` fields as `X-Y`, and the in-game label uses the same form.
- **Full-screen portrait art without PSRAM.** Backgrounds and CGs are stored as
  240 × 214 JPEG and decoded straight into the art canvas; sprites are
  alpha-cut RGB565 with a 1 bpp mask. The dialogue band is translucent, so the
  scene stays visible while text remains readable.
- **Two packed data files, 5.9 MB.** The artwork pack is 4.49 MiB (991
  backgrounds, CGs and sprites) and the script pack is 1.81 MiB (68,229 lines
  in 273 deflate blocks). The ESP32-C3 ROM inflater expands the script, so
  decompression costs no firmware space.
- **Clean text at pack time.** 104 lines carrying inline layout directives
  (`%f`, `$name$`, `#rrggbbaa`) and 7 lines with a leaked translator memo are
  removed by the packer; the dialogue itself is unchanged.
- **Serial acceptance channel.** `LIMEIMAGE <offset>` streams a raw frame,
  `LIMEPAGE` jumps to a dialogue id or named screen, `LIMEAUTO on|off` toggles
  auto-read and `LIMEBTN` reports the button ADC level. Backlight changes are
  logged with the idle time, which is how the "auto-read must not dim" rule is
  verified.

## Build and flash

```bash
./tools/validate.sh --firmware   # build, merge and verify the 0x0 image
idf.py build                     # optional incremental app build
# flash build/FoloToy-AI-Passport-full.bin from 0x0 (web flasher or esptool)
```

The packers need Pillow and FreeType in the Python environment ESP-IDF builds
with. `idf.py build` alone does not produce or verify the merged image. Use the
[browser flasher](https://ai-passport.folotoy.cn/tools/web-flasher/) or
`esptool`; the target is an 8 MB Flash AI Passport.

## Release

`v0.1.0-limelight` ships the merged `FoloToy-AI-Passport-full.bin`
(7,897,824 bytes) as the CI build for this tag. Both packs are committed under
`main/limelight_data/`, so a plain checkout builds the complete story.

## Known limitations

- This is a text-and-art reader: there is no voice, music or network path in
  the firmware. The story runs entirely from the packs in Flash.
- The body face is a fixed 16 px Noto Sans SC subset (13 full-width characters
  per line, five lines per page); there is no 20 px body option.
- The target has no PSRAM. The 5.9 MB of packs and the 240 × 214 art canvas are
  sized for the 8 MB, single-app build; adding large media needs a new memory
  budget.
- The upstream artwork and script declare no licence. They are included as a
  credited fan conversion; the MIT licence below covers the reader code, not
  the third-party story or art. The Noto Sans SC subset is OFL-1.1.

## Experience notes

Reusable engineering notes collected from these releases are under
[`docs/reference/shinku-chen/`](docs/reference/shinku-chen/). See
[`docs/README.md`](docs/README.md), [`AGENTS.md`](AGENTS.md) and
[`docs/contribution/`](docs/contribution/) for the repository's build and
contribution workflow.

## License

The reader source is released under the repository's [MIT licence](LICENSE).
The story, artwork and character material remain the property of their original
creators and are not relicensed by this project.
