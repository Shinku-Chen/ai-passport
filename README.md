<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Saya no Uta — a visual novel reader for the AI Passport

A landscape visual-novel reader that ports the Mi Band 10 fan port of
Nitroplus' *Saya no Uta* ([`liuyuze61/Saya-miband10`](https://github.com/liuyuze61/Saya-miband10))
to the AI Passport: **44 chapters, 3,828 lines of dialogue, three endings**, all
offline. The Mi Band title is a portrait touch app; this branch re-implements the
reading engine in C on LVGL and drives it with the three keys.

- Branch: [`feature/saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/tree/feature/saya-no-uta)

## Layout

```text
┌──────────────────────────────────┐  320 x 240, held in landscape
│  art area 320 x 136              │  background JPEG + character sprite
│                        [battery] │  battery chip in the top-right corner
├──────────────────────────────────┤
│  speaker name                    │  dialogue box, 104 px, opaque
│  up to 4 lines of body text      │  16 px font: 19 full-width chars per line
└──────────────────────────────────┘  (20 px font: 15 chars, 3 lines)
```

## Controls

- **Title / lists** — UP / DOWN move the cursor, **OK** selects, **OK (hold)** goes back.
- **Reading** — **OK** advances (while text is typing, one press shows the whole page),
  **OK (hold)** opens the menu, UP / DOWN step between pages of a long line.
- **Choices** — UP / DOWN select, **OK** confirms.
- **Menu** — save, load, skip scene, back to title, close.
- **Save slots** — 5 manual slots plus one automatic slot written on every scene
  change, so "Continue" resumes where you left off. In save mode **OK (hold)** on a
  slot deletes it.

Idle behaviour: 60 s dims the backlight, 3 min turns it off, 7 min enters deep
sleep; any key wakes the device and reopens the reader at the last automatic save.

## Offline data pipeline

Nothing is downloaded at runtime. Two tools generate everything the firmware needs,
and their output is committed so a plain checkout builds:

| Step | Tool | Output |
| --- | --- | --- |
| Script + images | `tools/saya_pack.py` | `main/saya_data/saya_pack.bin` (~2.6 MB): 44 chapters, 473 scenes, 3,828 dialogues, 193 backgrounds, 75 sprites, plus source metadata |
| Font subsets | `tools/saya_font.py` | `assets/fonts/saya_cjk_16.c`, `saya_cjk_20.c` and the character inventory `assets/fonts/saya_cjk_symbols.txt` |

The pack is read straight out of Flash — there is no runtime JSON parsing and no
decompression. Backgrounds are pre-cropped to 320 × 136 JPEG; sprites are scaled to
screen height, pre-cropped to the visible band and stored as JPEG plus a 1bpp mask.
The firmware `mmap`s the pack from the application partition, decodes one scene
(background plus sprite) into a 320 × 136 RGB565 canvas only when the background or
sprite actually changes, and composites the sprite with its mask.

Regenerating requires a checkout of the source port and a licensed CJK font; see
the header of each tool. Font provenance and license are recorded in
[`assets/README.md`](assets/README.md).

Flash budget (ESP-IDF 5.5, app partition 8,323,072 bytes): application 4.2 MB,
49 % of the partition free.

## Notes

- **Content**: the story contains heavy gore. The app keeps the source port's
  content warning on first boot.
- **Licensing**: *Saya no Uta* is a commercial Nitroplus title. This is a personal
  fan port; the artwork and the Chinese translation come from the public Mi Band
  port, and both the project and the in-app disclaimer ask readers to support the
  original release. Do not republish the generated pack as your own asset.
- **No audio**: the source port ships no audio, and this port adds none.
- **Baseline demo**: the repository's hardware-test menu and `demo_*.c` pages are
  still in `main/` but are not compiled into this application; see the repository's
  [AI guide](docs/development/ai-guide.md) and [fork guide](docs/fork-guide.md).

## Build and validate

```bash
./tools/validate.sh --static     # repository checks, host tests, font coverage
./tools/validate.sh --firmware    # ESP-IDF build + merged-image verification
```

The host test `tests/test_saya_model.c` runs the real pack through the reader logic
(chapter graph, all three endings, pagination round-trips, save encoding), and
`tools/saya_font.py --check` fails the gate if any UI string or script character is
missing from the generated font subsets.
