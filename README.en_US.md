<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Senren \* Banka

A portrait visual novel ported from the Mi Band fan port of *Senren \* Banka*: the whole
story of the source port plus **92 backgrounds, 123 sprites in several poses each, 570
event illustrations** and the extras it carries, fully offline. The script and art come
from [`hrk666666/Senren-Banka-MiBand-10`](https://github.com/hrk666666/Senren-Banka-MiBand-10)
(a Mi Band 9/10 quick app); this branch re-implements the reader on the ATRI reader's LVGL
page system and drives it with the three keys. Status: **released** — tag
`v0.1.0-senren-banka`, and submitted to the AI Passport community market as
`community-07c775dc` (under review).

- Branch: [`feature/senren-banka`](https://github.com/Shinku-Chen/ai-passport/tree/feature/senren-banka)
- Release: [`v0.1.0-senren-banka`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-senren-banka) — merged image `FoloToy-AI-Passport-full.bin`, 7,997,648 bytes (the image validated on device; CI rebuilds the same sources)
- Upstream work: [`hrk666666/Senren-Banka-MiBand-10`](https://github.com/hrk666666/Senren-Banka-MiBand-10) — the fan port this branch derives from. It declares no licence; its script and artwork are fetched with [`tools/senren_fetch_source.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_fetch_source.py) rather than committed, packed into the firmware, and the upstream project is credited. The port is shared for personal study and exchange only, as the app's first-run notice states.
- Asset toolchain: [`tools/senren_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_pack.py) (image pack 5,263,756 bytes into `main/senren_data/`) and [`tools/senren_scn_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_scn_pack.py) (script pack 1,432,148 bytes) plus [`tools/senren_lvgl_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_lvgl_font.py) (16 px Chinese subset, 3,492 glyphs in code-point order for the sparse-font binary search)

**Controls (three keys):** on lists UP / DOWN move the cursor and **OK** selects. While
reading, **UP / DOWN** advance a line (a press completes the typewriter first, and a line
that does not fit paginates), **UP (hold)** fast-forwards while held and stops on release,
**DOWN (hold)** toggles auto-read, and **OK** (tap or hold) opens the menu — continue, save,
load, skip chapter, back to title. Choices use UP / DOWN + OK and save as soon as they are
confirmed. Short presses register at 180 ms and long presses at 500 ms, passed to the button
component explicitly by the BSP.

**Saves, chapters and idle:** one automatic slot plus five manual slots, with the automatic
one written on every chapter change, every choice and before sleeping, offered as
"continue" on the title screen. Each chapter announces itself as a `Chapter X-X` card
(chapter and section). Idle behaviour: 60 s dims the backlight, 180 s turns it off, 420 s
enters deep sleep, and any key wakes the device; auto-read and fast-forward do not count as
idle time, so a story left running never dims or sleeps by itself.

**Highlights:**

- **A self-written inflate instead of the ROM decompressor** — the reader decodes a whole block into one buffer instead of needing a 32 KB sliding dictionary, so the working buffer stays at 4 KB on a part with no PSRAM to spare.
- **A chunked palette image format** — backgrounds and event illustrations are stored as 240 × 320 JPEG, sprites by pose and bottom-aligned, and event-illustration variants as sparse mask patches against a base picture, each variant stored whichever way is smaller.
- **The story is packed in 3 KB blocks** — the script pack trades about 200 KB of compression ratio for the same 4 KB buffer, and every jump target resolves through chunk and block tables instead of a scan.
- **A real hardware find behind "the menu opens by itself"** — the three keys share one ADC pin behind voltage windows, and a long press whose contact briefly opens sweeps past the widest window and reads as a different key; the BSP now locks the key identity until the voltage returns to the released range, and confirms a press for 40 ms before accepting it. This is board-level logic and belongs upstream.
- **Serial debugging channel** — `SENRENPAGE [title | <chapter> <step>]` captures a frame from the LVGL flush path and streams it back as raw RGB565; `SENRENJUMP`, `SENRENSKIP`, `SENRENSAVE`, `SENRENLOAD`, `SENRENINFO` and `SENRENKEYS` (the last one a ring of the last 32 key events with the ADC voltage each was detected at, which is how the key-window defect above was proven) cover scene jumps, chapter skipping, save and load, state readout and key tracing.

## Notes

- This branch carries one application. A `feature/*` branch README describes only
  its own application; the fork `main` root README is the catalog of every hosted
  project.
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
