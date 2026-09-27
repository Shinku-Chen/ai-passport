<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Sanoba Witch

A portrait visual novel ported from the Mi Band fan port of *Sanoba Witch*: **101
chapters, 53,190 lines of dialogue, about 1.1 million characters, five heroine routes
and five endings**, fully offline. The script and art come from
[`hrk666666/Sanoba-Witch-MiBand-10`](https://github.com/hrk666666/Sanoba-Witch-MiBand-10)
(a Mi Band 9/10 quick app, itself derived from the archived
[`futrw4v/Sanoba-Witch-MiBand-9Pro`](https://github.com/futrw4v/Sanoba-Witch-MiBand-9Pro));
this branch re-implements the reader on the ATRI reader's LVGL page system and drives it
with the three keys. Status: **released** — tag `v0.1.0-sanoba-witch`, and submitted to
the AI Passport community market as `community-8e228d9f` (under review).

- Branch: [`feature/sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch)
- Release: [`v0.1.0-sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-sanoba-witch) — merged image `FoloToy-AI-Passport-full.bin`, 5,929,664 bytes
- Upstream work: [`hrk666666/Sanoba-Witch-MiBand-10`](https://github.com/hrk666666/Sanoba-Witch-MiBand-10) — the fan port this branch derives from (its own upstream is the archived `futrw4v/Sanoba-Witch-MiBand-9Pro`). Its code is GPL-3.0; the artwork belongs to Yuzusoft and the Simplified-Chinese text belongs to its fan-translation group. The sources it packs ship with the branch under [`assets/sanoba-source/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch/assets/sanoba-source), with the upstream credited.
- Asset toolchain: [`tools/sanoba_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/sanoba-witch/tools/sanoba_pack.py) and [`tools/sanoba_scn_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/sanoba-witch/tools/sanoba_scn_pack.py) (image pack 2.97 MB and script pack 1.45 MB into `main/sanoba_data/`) plus [`tools/sanoba_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/sanoba-witch/tools/sanoba_font.py) (16 px Chinese subset, 3,458 glyphs, generated in script-table order so nothing is missing)

**Controls (three keys):** on lists UP / DOWN move the cursor and **OK** selects. While
reading, **UP / DOWN** advance a line (one press completes the typewriter, and long lines
paginate with DOWN), **UP (hold)** fast-forwards while held and stops the moment you
release it, **DOWN (hold)** toggles auto-read, and **OK** opens the menu (save, load, jump
to the next choice, auto-play, back to title). Choices use UP / DOWN + OK, and auto-read
is not switched off while a choice is on screen, so answering one resumes reading by
itself. Short presses register at 180 ms and long presses at 500 ms, passed to the button
component explicitly by the BSP.

**Saves, endings and idle:** five manual slots plus one automatic slot written on every
scene change, offered as "continue" on the title screen. Five routes end in five different
endings. Idle behaviour: 60 s dims the backlight, 180 s turns it off, 420 s enters deep
sleep, and any key wakes the device; auto-read and fast-forward do not count as idle
time, so the screen stays lit while they run.

**Highlights:**

- **A compact script pack instead of a raw script dump** — the 5.06 MB node JSON becomes a 1.45 MB pack that decompresses one chunk at a time (3 KB raw per chunk, 4 KB buffer) because the largest free block after boot is under 8 KB; every jump target resolves through a label table instead of scanning a chunk.
- **Three defects that only appear on hardware** — the pack's deflate framing, the chunk-buffer allocation and the main task's stack each showed up as "the story ends immediately"; a boot self-check now prints the first chunk's decompressed size and the first line, so this whole class of failure is visible in the log instead of looking like the end of the story.
- **A layout the panel likes** — 240 × 320 portrait: full-width background, a 240 × 144 SD window in the upper third, and a five-line text band. Backgrounds decode straight into the canvas and an SD image decodes into the row range it occupies, so neither needs a decode buffer.
- **The script's own chapter number** — the corner readout and the transition card show the source numbering (`4-7`), while the save labels and lists use the scenario names.
- **Offline and reproducible** — both packers and the font generator run from the sources committed under `assets/sanoba-source/`; rebuilding either pack is byte-identical to what is in the firmware.
- **Serial debugging channel** — `SANOBAPAGE [title | <scenario> <step>]` captures a frame from the LVGL flush path and streams it back as raw RGB565; `SANOBASHOT`, `SANOBAJUMP` and `SANOBAAUTO` cover scene renders, scene jumps and auto-read checks.
- **No character art and no event CGs** — the upstream port ships neither, so a speaker appears as a name plate and scenes are carried by backgrounds plus chibi SD illustrations; the script, its branches and all five endings are complete.

## Notes

- This branch carries one application. A `feature/*` branch README describes only
  its own application; the fork `main` root README is the catalog of every hosted
  project.
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
