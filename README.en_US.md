<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Café Stella and the Reaper's Butterflies · AI Passport Offline Reader

A complete offline port candidate of *Café Stella and the Reaper's Butterflies* for **FoloToy AI Passport** (ESP32-C3, 8 MB Flash, no PSRAM, 320×240 landscape). The firmware keeps the `feature/galgame-framework` page/key/persistence contract but replaces its content and reading layer; it is not a demo or a summary.

> Status: complete offline reader; device acceptance covers the tested Gallery OP (clarity/no overlay/return), ordinary chapters, body text/images, and fast-forward release stop. Full-route, long-run and power-loss behavior are not claimed. All audio/video and source are retained.

## Fixed input

| Item | Value |
| --- | --- |
| Upstream repository | `https://github.com/xuanyide01/cafestella-rw5` |
| Fixed commit | `90ddb9e020970907b61ad4181f25c65267e6e98b` |
| Read-only local source | `C:/tmp/ai-passport-six-20261123-79f0/sources/cafestella-rw5` |
| Target firmware baseline | `Shinku-Chen/ai-passport` `feature/galgame-framework` `b9909c13605413142fb35f28aabcc494a666fa00` |

The authoritative semantics are the repository's processed `converted/graph.json`, `converted/n*.json` and `src/common/engine.js`; branches are not reconstructed from a linear event order.

## Content covered

| Content | Count |
| --- | ---: |
| Story nodes | 923 |
| Dialogue frames | 53,727 (plus one OP marker) |
| Choice groups / options | 69 / 141 |
| Main endings | 7 (bad / normal / Kanna / Natsume / Nozomi / Mei / Suzune) |
| After stories | 5 (Kanna / Natsume / Nozomi / Mei / Suzune) |
| Chapter entries | 189 |
| Gallery CG groups / variants | 121 / 1,631 |
| Source image files | 1,850 (1,729 backgrounds + 121 portrait bases; plus 140 face atlases) |

Every option, condition, route, ending and post-clear gate follows the source `engine.js` `locate/choose/flags/matches/complete` semantics. Conditions (including `checkAnyClear` and the source's default-zero `kan_flag`) and `flagRules` derived values are preserved. The source `kan_flag == 3` branch is unreachable in the source graph; the port keeps it unreachable instead of inventing a different story.

## Controls (320×240 landscape, three keys)

| Page | Short OK / UP / DOWN | Long press |
| --- | --- | --- |
| Start | Short OK enters the title | — |
| Title | Up/down moves, OK enters | — |
| Reading | All three keys advance one sentence/page; typing and auto advance immediately | Long OK opens the in-game menu; long UP fast-forwards; long DOWN toggles auto |
| Choice | Up/down selects, OK confirms | Long OK returns to dialogue without choosing |
| Menu / settings / save / load / gallery / chapters | Up/down moves, OK runs | Long OK goes back |
| History / about | Up/down scrolls | Long OK goes back |
| OP / transition / ending | OP: short press skips; transition/ending: short press continues/returns | OP long OK does not choose |

- All three short keys **advance immediately** while typing or in auto: to the next page for a long line, otherwise the next line. They never just finish the current page or merely leave auto and swallow the press.
- A manual short press during auto-play **leaves auto and advances immediately** (skips typing and the sentence wait).
- Long UP skips typing and auto waits and keeps advancing; release stops immediately; choices, transitions and endings stop it without choosing for the player.
- A long press only fires its long action; release does not add a short action. Opening/closing menus, choices, auto and fast-forward clear mutually exclusive state.

## Visuals and assets

- Source stage 432×243 is packed at **224×126** (exact 1:1.777 aspect); at runtime three **224×42 RGB565** bands are scaled by LVGL to **320×180**.
- Backgrounds use JPEG q10; the seven title backgrounds use q18; portraits/face atlases use q18 plus a **2 bpp alpha mask** (4 levels), with transparent RGB filled from the nearest opaque edge and 4:4:4 JPEG, preserving transparency, variants, expression anchors and stage positions. There is no full-screen RGB565 canvas.
- Gallery CGs reuse the same background images; thumbnails decode at 32×18 and display at 2×.
- The Chinese fonts are generated from the source OTF as 16 px / 20 px 4 bpp subsets covering **every character actually present in the pack and UI** (3,401 codepoints; `tools/gal_font.py --check` reports 0 missing and 0 blank glyphs). No system fallback font is used.
- A real UI software-display smoke (production `cafe_ui.c` + LVGL 9.5, 320×240, 64 KB pool) covers 30 screens: title, 16/20px lines, menu, save/load, gallery, CG list, chapter routes/list, history/recall, CG viewer, OP first/mid frames, music, audio error, settings and CJK fatal; output `contact-sheet.png`. 64-bit host: 58,120 B total, 11,376 B free at end, 48,832 B peak, fragmentation 23. 32-bit WoW64 (production UI/fonts/image path, audio HAL stub): 56 KB configured pool, 30 screens PASS, 53,484 B total, 23,096 B free, 21,576 B largest free, 32,156 B peak; 64 KB pool, 61,676 B total, 31,288 B free, 29,768 B largest, 32,156 B peak. These are x86 hosts, not ESP32-C3 device evidence.

## Audio/video implementation (user selected: complete audio and video)

The user selected keeping all source audio/video at the current image quality; q18/BG12/BG14 did not replace it.

- Title BGM: real source `title-bgm.opus` transcoded to 4 kHz mono IMA ADPCM, 516,096 B, 1,027,198 samples = 256.7995 s; all decoded samples retained.
- OP BGM: real source `opening-bgm.opus` to 8 kHz mono IMA ADPCM, 373,760 B, 743,959 samples = 92.995 s. Playback performs the source's 92.4 s pre-EOF guarded stop; the video continues to 95.46 s of silent tail. This is not “all samples played”, and the file is not stretched or truncated.
- OP video: source 477 frames @5 fps / 95.460 s mapped to 287 frames at 224×126 q15 @3 fps, 620,030 B; source frame `floor(k*5/3)`, last frame held to 95.460 s.
- The OP video region `y=0..179` has no app title/hint capsules: the title/eyebrow objects are hidden, leaving only a short control hint in the bottom `180..239` strip. Normal chapter transitions still show their titles; source-native cards are untouched.
- OP JPEGs use production `cafe_image_render_op` to scale into the full 224×126 stage and reuse the existing three 224×42 bands (56,448 B total) and `s_stage`; no full 224×126/320×180 RGB565 allocation and no extra LVGL transform cache.
- Audio faults are settled before any key/long-timer action; the triggering press/release is consumed, retry keeps the original OP frame/time and seeks audio to the same position, and stop uses an active-busy acknowledgement with an observable timeout. The audio task doubles only the 4 kHz title track; the OP track is already 8 kHz. Both feed the codec/I2S at 8 kHz, 16-bit mono. A write failure stops playback and enters the existing resource fault; there is no busy wait. Title music follows title/menu/gallery/settings lifecycle; the OP marker starts audio/video, and finish or short-press skip advances the VM once; the body/auto state resumes afterward.
- Music gallery lists and plays the real title/OP tracks; Settings has sound on/off and volume. On the OP page, short press skips; playback ends automatically at about 95.46 s. Long OK does not choose for the player.
- Images: the existing 1,997 story images (BG q10; portraits/atlas/title q18) are retained; OP adds 287 images, total 2,284 image records. New media are not counted as story-image edits.

## Actual Flash / DRAM budget

| Item | Bytes |
| --- | ---: |
| `cafe.pak` (text + metadata + all images) | 6,424,952 |
| Image data (including OP 620,030) | 4,084,761 raw B (3,360,348 after dedup) |
| Audio IMA / OP JPEG (subset, not additive) | 1,509,886 (audio 889,856 + OP 620,030) |
| Text zlib data | 1,694,803 |
| App partition (factory) | 8,323,072 |
| Firmware app bin | 8,246,672 |
| Firmware free | 76,400 (1%; ~75 KB below the earlier 190 KB estimate) |
| Merged image (bootloader/partition table included) | 8,312,208 |

| Artifact | SHA-256 |
| --- | --- |
| `assets/cafe/cafe.pak` | `39130be057c33ef7252b1fd6fd8526937aa4bf4c225467513890cdd7e6ce7c46` |
| `FoloToy-AI-Passport.bin` | `d05c9a248269570f6cbcdeef409821671368aceb2f5801493ecb4683342915ac` |
| `FoloToy-AI-Passport-full.bin` | `d706c5a67a2feeb0d757d54af4ba72753f6e62e854633977e37d28b143dff7d4` |

- Static DRAM: 239,250 / 321,296 B used, 82,046 B free; the audio PCM/decode static buffers (~5.1 KB) are included. Runtime bands add 56,448 B and the input task stack 4 KB, leaving about 21.6 KB dynamic.
- `-fstack-usage` with the real RISC-V target flags: the critical chain is `show_frame_page` 1,120 B, and `cafe_app_key` + `handle_short` + `advance_reading` + `show_frame` about 1.4 KB. The image chain is `render_sprite_band` 304 B + `decode_jpeg` 176 B, inside the 4 KB input stack.
- Bands, masks and the JPEG work area are bounded static/≤32 KB single blocks; no full 320×180 RGB565 (115,200 B) dynamic allocation.

## Build

Requires ESP-IDF **5.5.3** (this machine: `C:\Espressif\frameworks\esp-idf-v5.5.3`):

```bash
source <ESP-IDF-5.5.3>/export.sh
idf.py -B build-cafe build
idf.py -B build-cafe merge-bin -o FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build-cafe
python tools/archive_firmware.py create build-cafe --archive-root build/firmware
```

Reproduce the pack and fonts:

```bash
python tools/cafe_pack.py                 # writes assets/cafe/cafe.pak and reports
python tools/gal_font.py --font <cafestella-text.otf> \
    --out-dir assets/fonts --source-root assets/cafe
```

## Verification

```bash
CC=gcc ./tools/validate.sh --static       # repo checks + all host tests + font coverage
./tools/validate.sh --firmware            # requires activated ESP-IDF 5.5.3
```

Classification: `--static` executed PASS. `./tools/validate.sh --firmware` was executed in Git Bash and is **FAIL (environment)** (`idf.py` unavailable; ESP-IDF 5.5.3 rejects MSys/Mingw; log `reports/cafestella/logs/firmware-gate-attempt.log`). Native PowerShell equivalent build/merge/verify/archive commands each PASS (`reports/cafestella/logs/firmware-{build,merge,verify,archive,archive-verify}.log`). Device tests are **NOT RUN**.

Host regressions include a startup/lock/backlight/input-task ordering contract (`tests/test_cafe_lifecycle_contract.py`), 160 source-`engine.js`/C-VM traces (141 options, 7 endings, 5 after stories, skip/previous; 436,182 commands comparing node/index/progress/derived flags/all `when` results), a controlled-clock three-key state machine, save/bad-blob handling, the landscape app state machine, and Pillow reference decode/composite for 7 frames (0–5 portraits, face variants). These are **host** evidence, not device acceptance.

## Copyright

All story, art, music and character rights for *Café Stella and the Reaper's Butterflies* belong to **YUZU SOFT**. This repository contains only port code, conversion tools and downsampled assets generated for offline reading; support the official release and do not redistribute original assets.
