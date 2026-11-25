<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# 9-nine- Kokoiro - AI Passport offline reader

A complete offline port of *9-nine- Kokoiro* (Episode 1) to the FoloToy AI Passport:
ESP32-C3, 8 MB flash, no PSRAM, 320x240 landscape, three keys. The firmware embeds the
story, character art, expression patches, CGs, title visuals and two BGM tracks. No
network or external files are required.

This worktree is responsible for this title only. The other three episodes are separate
firmware images and are not included here.

## Pinned input

| Item | Value |
| --- | --- |
| Upstream project | <https://github.com/xuanyide01/9-nine-rw5> |
| Pinned commit | `6a8907a5ae576f4377abe4b74f15bd9af13efe07` |
| Episode directory | `nine-kokoiro-rw5` |
| Authoritative data | `src/common/graph.js` (converted graph) + `src/common/story/**` |
| Semantics source | `src/common/engine.js`, `src/common/chunk-codec.js` |

The original, uncommitted PC scripts are not reconstructed. The converted graph, story
chunks and transcoded assets from that exact commit are the source of truth.

## Content

- **17 chapters**, **74 nodes**, **8049 dialogue frames** (plus one OP marker; 8050 source
  records in total).
- **5 choice groups / 9 options**: the first playthrough skips the choice nodes; the
  second playthrough exposes new options gated by `loop_my`.
- **2 endings**: the first-playthrough loop ending (`loop1`) and the second-playthrough
  true ending (`true`).
- Every line, speaker, choice, condition, effect, ordered `next` and CG variant reference
  comes from the source graph. No story was truncated, no route removed and no
  placeholder art was used to fit flash.
- 15 state variables, including 5 permanent keys. This is not a reduced 8-variable or
  32-bit-flag port.

## Controls

| Screen | Short press | Long press |
| --- | --- | --- |
| Reading | OK / UP / DOWN all advance immediately (skip the typewriter; long lines advance one page unit) | OK = in-game menu; UP = hold to fast-forward; DOWN = auto reading |
| Choice | UP/DOWN move, OK confirms | OK = in-game menu (one long press; returning restores the full options and cursor) |
| Menu / save / load / settings / gallery / chapters | UP/DOWN move, OK runs | OK returns |
| History / about | UP/DOWN scroll | OK returns |
| Title | UP/DOWN move, OK enters | - |
| Chapter transition / ending | any short press continues / returns to title | - |

Additional rules:

- A long press runs only its long-press action; releasing never triggers an extra
  short-press advance.
- One long OK on the reading or choice page opens the menu. Menu/save/settings/history
  return to the same text page or the full option list with its cursor, never to an
  empty GAME page.
- A reading key pressed while auto-play is on stops auto-play **and** advances. Auto is
  never exited by swallowing the advance.
- Holding UP jumps to the next sentence, skipping the typewriter, the auto delay and
  any remaining pages of the current sentence. It advances at most one sentence per
  50 ms control tick (never a busy loop), stops at choices/endings/OP instead of
  choosing for the reader, and stops immediately on release without a short press.
- Pagination is 3 lines at 16 px or 2 lines at 20 px; punctuation start/end rules are
  applied and the full text is paged, never dropped.

## Saves and permanent progress

- **30 manual save slots** (5 rows per page, paged) plus automatic reading progress for
  Continue.
- A slot stores the node, frame index, text page and current-run variables. The record is
  sparse, CRC32-checked and bound to the pack identity.
- **Permanent progress is separate**: `loop_my`, `loop1`, `true`, `clear_true` and
  `titleBg` live in their own NVS record. Loading, rewinding or a chapter preview never
  rolls a completed ending back.
- **A chapter preview never pollutes progress**: entering a chapter from the gallery uses
  an isolated session; it writes no permanent key, no automatic progress and no CG
  unlock.
- Permanent unlocks: 22 CG groups, 17 chapter-read marks and 2 endings. Title visuals
  unlock with the loop/true endings and can be switched in settings.
- A damaged save/progress record is reported on screen; the live state is kept and the
  bad record is never silently applied.

## Gallery

- **CG gallery**: 22 groups / 73 variants, 8 thumbnails per page (3 pages); the full-screen
  viewer switches variants with UP/DOWN.
- **Chapter recall**: previews all 17 chapters without writing completion state.
- **OP movie**: the source is 599 frames at 6 fps (99.9 s); the firmware plays 300 frames
  at 3 fps plus the original track, and any key skips it.
- **Music**: the title theme and the OP theme, 8 kHz mono IMA ADPCM, play/stop.

## Display and audio

- A **320x180** stage (source 432x243, exact 16:9; decoded internally at 256x144
  and shown at 1.25x) with a 60 px panel below it.
  The floating speaker pill sits above the panel over the art: fitted
  semi-transparent background, 16 px tier even with 20 px body text, 160 px cap
  with ellipsis, hidden for empty names, and no numeric page HUD. The 320x240 landscape mode is enabled through the BSP
  `bsp_lvgl_set_landscape(true)` call.
- Backgrounds / OP / CGs: JPEG q38 (441 assets, 1,827,874 B).
- Character bodies and face patches: 8-bit palette + alpha (225 assets, 446,231 B),
  preserving multi-character composition, opacity, source layering and the 256x256 face
  atlas anchors.
- Title art: three 320x240 raw RGB565 visuals with the logo baked in; CG thumbnails: 22
  raw 96x51 RGB565 images (25 raw assets, 676,224 B).
- The compositor decodes four 256x36 bands (**18,432 B** each, 73,728 B total) and
  displays them at 1.25x as four 320x45 strips covering 320x180. A failed decode
  clears the canvas, so no stale frame remains visible.
- Audio: two tracks, IMA ADPCM 8 kHz mono. Title 85.4 s / 352,440 B; OP 99.9 s /
  412,236 B.
- Fonts: 16 px and 20 px subsets, **2182 glyphs**, covering all 8049 lines, speakers,
  choices, chapter/ending/gallery names and the UI. 19 glyphs missing from the source
  subset are filled from Noto Sans SC; 0 missing. The source font files are not committed.

## Budget

| Item | Bytes | Notes |
| --- | ---: | --- |
| Story pack `nine_script.bin` | 555,087 | 74 nodes, 8050 frames, 100 text blocks <= 4096 B |
| Image/audio pack `nine_images.bin` | 3,350,479 | 691 images + 2 tracks + 300 OP frames |
| 16 px font (compiled estimate) | 297,240 | 4bpp, 2182 glyphs |
| 20 px font (compiled estimate) | 419,393 | 4bpp, 2182 glyphs |
| App image `FoloToy-AI-Passport.bin` | about 5.34 MB | both packs and both fonts |
| Merged image `FoloToy-AI-Passport-full.bin` | about 5.40 MB | flash from 0x0 |
| factory partition | 8,323,072 | about 2.98 MB free |

- Static DRAM: 171,660 B used of 321,296 B; 149,636 B remain at boot.
- Fixed runtime allocations: four stage bands 73,728 B + 16-line flush buffer 10,240 B +
  input task stack 4,608 B + audio task stack 3,072 B + 24 history entries 4,608 B + I2S
  DMA about 2,880 B.
- Estimated remaining heap: **about 46 KB** (not measured with `heap_caps` on a device;
  see the validation boundary).
- Target LVGL pool: 48 KB. A 32-bit zig build with real LVGL reports 28,700 / 15,968 /
  12,004 used/free/largest after every page; a 40 KB run left only 2,932 B largest.
  The 64-bit host proxy needs 72 KB (45,824 / 18,848 / 12,704).

## Build

ESP-IDF **5.5.3** is required. The generated packs are not committed; build them first:

```bash
# 1) Story and image/audio packs from the pinned source
python3 tools/nine_pack.py \
  --source /path/to/9-nine-rw5 --episode kokoiro \
  --out assets/nine/kokoiro

# 2) 16/20 px fonts (must run after the pack; adds UI and ASCII glyphs)
cd tools && python3 nine_font.py \
  --font /path/to/nine-text.otf \
  --fallback /path/to/NotoSansSC-VF.ttf \
  --out ../assets/nine/kokoiro --source-root ../main

# 3) Static gate (pack/VM/save/font/image/audio/input regressions included)
cd .. && ./tools/validate.sh --static

# 4) Firmware + merged image + independent verification + debug archive
./tools/validate.sh --firmware
```

`nine_pack.py` rejects a source checkout whose HEAD differs from the pinned commit.
Regenerate the fonts after every pack rebuild: the packer rewrites `nine_symbols.txt`, and
the font tool expands it to the 2182 glyph set including UI and ASCII.

Manual build:

```bash
source /path/to/esp-idf-v5.5.3/export.sh
idf.py -B build/fw -D SDKCONFIG=build/fw/sdkconfig build
idf.py -B build/fw merge-bin -o "$PWD/build/fw/FoloToy-AI-Passport-full.bin"
python3 tools/verify_firmware.py build/fw
```

Full-chip flash of the merged image (overwrites existing firmware and data):

```bash
esptool.py --chip esp32c3 -p <PORT> -b 460800 \
  --before default_reset --after hard_reset write_flash 0x0 \
  build/fw/FoloToy-AI-Passport-full.bin
```

> This round did not authorize any device, COM, monitor or flash operation, so only the
> reviewable build artifacts and commands are delivered.

## Validation

| Item | Result |
| --- | --- |
| Build (ESP-IDF 5.5.3, C3, 8 MB) | **PASS** |
| Host tests (static gate, including this title's C regressions; 4/5/12 Windows symlink platform skips) | **PASS** |
| Merged image / `verify_firmware.py` / debug archive | **PASS** |
| Source graph vs C VM trajectory diff (8 scenarios, about 27.5k views/states) | **PASS** (local evidence) |
| Source-to-pack audit (8049 lines, 94 edges, 20850 layers) | **PASS** |
| Image reference composite (209 frames, real JPEG/palette/raw decode) | **PASS** (tolerances in the doc) |
| Production real-LVGL startup (no `NINE_IMAGE_HOST`; 32-bit 48 KB pool / 64-bit 72 KB proxy) | **PASS** (local evidence) |
| Host real-LVGL screenshots (normal/multi/CG/20px/menu) | **PASS**; `build/nine/shots/index.html` (not device captures) |
| Device tests | **NOT RUN** (flashing/serial forbidden this round) |

**Not verified**: real-device landscape/rounded corners, key feel and long-press/release
timing, real-device free heap and largest block, audio level/noise, NVS power-loss/wear,
idle dim/sleep/wake. A passing software gate is not a device pass.

## Known limitations

- **The OP is not video**: the ESP32-C3 has no video decoder, so the firmware plays a
  3 fps image sequence plus the original track. The source 6 fps/599 frames become
  3 fps/300 frames; the 99.9 s duration is preserved.
- **Audio is 8 kHz mono IMA ADPCM**, transcoded offline from the source Opus tracks; it is
  not original 44.1/48 kHz quality.
- The source watch project contains no voice and no R18 content; this firmware contains
  none either.
- The title visuals are center-cropped from the source 432x514 portrait art to 320x240
  landscape; the stage is an exact 16:9 320x180 adaptation of the source art.
- 8-bit character palettes (up to 192 colors) and JPEG q38 are deliberate flash/RAM
  trade-offs, not lossless.
- The framework chapter-transition page exists, but this graph has no transition nodes;
  normal reading simply updates the chapter label.
- Remaining runtime heap is a static estimate, not a device `heap_caps_get_free_size()`
  reading.

## Copyright and provenance

- *9-nine- Kokoiro* is copyright **Palette**. This port uses only the processed text and
  transcoded assets in the public RW5 project above; it does not include game
  installation files.
- The upstream RW5 project's `package.json` declares MIT; game asset rights are separate.
  Confirm asset permission before public distribution.
- Repository code follows the FoloToy AI Passport license and contribution rules. The
  source fonts (`nine-text.otf`, Noto Sans SC) are not committed; only the generated
  glyph subsets are.

## Documents

- Engineering notes (Chinese): [`docs/nine-kokoiro.zh_CN.md`](docs/nine-kokoiro.zh_CN.md)
- Engineering notes (English): [`docs/nine-kokoiro.md`](docs/nine-kokoiro.md)
- Framework and hardware baseline: [`docs/README.md`](docs/README.md), [`AGENTS.md`](AGENTS.md)
