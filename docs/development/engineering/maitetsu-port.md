<p align="right">
  <a href="maitetsu-port.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Maitetsu Niiroku — AI Passport port

## 1. Scope and input

The input is the RW5 watch port of the Niiroku route from *Maitetsu*:

| Item | Value |
| --- | --- |
| Upstream | [`xuanyide01/maitetsu-rw5`](https://github.com/xuanyide01/maitetsu-rw5) |
| Pin | `9c008406853affe973ceb80f62128696054365e7` (RW5 0.1.3) |
| Input digest | `082babe851a5efd6a4c6ab28bb4363fc1e8bf35448bea32466abe926ee8d07f2` (SHA-256 over the 34 chunks, gallery, UI images, title track and `converted/text.json`) |
| Story | four chapters: Showa 41 Kokura, the Yuuichi trip, the Kokura storm, the epilogue |
| Paragraphs | 354 + 345 + 368 + 470 = 1,537 |
| Scenes | 675 unique composite snapshots across 34 chunks |
| Gallery | 6 groups, 35 CGs (including variant frames) |
| Choices | none: the RW5 engine throws on `choose()` for this route, so the story is linear |
| Title track | 283.88 s, 4 kHz mono 16-bit PCM |

This is the complete content of that RW5 port — **not every route of the original
game**. Nothing in this firmware claims otherwise: the about page, README and this
document all state the four-chapter scope.

The source checkout is treated as read-only. The generator only reads it; every
output is deterministic (no timestamps, stable image order).

## 2. Architecture

The application keeps the branch's GalGame framework: the same 19 logical screens,
three-key interaction, pagination, save/progress split and permanent unlock state.
The content layer is replaced; the following boundaries were extended where the
source data does not fit the skeleton:

| Framework limit | This title | Change |
| --- | --- | --- |
| portrait 240×320 | landscape 320×240 | BSP `bsp_lvgl_set_landscape(true)`; art 320×180 (exact 16:9) above a 60 px dialogue panel |
| 32-bit CG unlock bitmap | 35 CGs | `gal_unlock_t.cg` is now 64-bit; NVS blob v2, v1 blobs migrate |
| 8 CG tiles, first 8 items | 35 CGs | the CG browser pages 8 per page; indices 33–35 are reachable |
| scene drawn from gradients | 675 layered scenes | new media pack + a banded RGB565 JPEG decoder |
| one image per line | background + up to two composited patches | `gal_line_t.scene`; the UI draws background then patches at their rects |
| square-wave music | one 283.88 s track | IMA ADPCM decoder + linear-interpolated 16 kHz playback |
| 3 save slots, run flags | 4-chapter coverage | `run_mask` in save slots and automatic progress |

### 2.1 Media pack

`assets/maitetsu/maitetsu_media.bin` (5,773,642 bytes, `MTMEDIA1`):

- 759 images: 353 backgrounds, 335 composited patches, 35 CGs, 35 64×36 thumbnails,
  1 title key visual.
- Every image is split into independent 30-row JPEG strips. The quantisation and
  Huffman tables are shared once per quality class (four prefixes), so a strip costs
  35 bytes of scan headers plus entropy data. That keeps a whole-image decode buffer
  out of RAM: the device decodes one 320×30 RGB565 strip (19,200 bytes) at a time
  through a custom LVGL image decoder.
- Scenes are stored as *background + patches*. A patch is the sprite composited over
  the background at its exact rectangle, so transparency and expressions are exact and
  backgrounds are shared between scenes (353 images for 675 scenes).
- The pack carries a CRC32 over its tables and one CRC32 per image; a corrupt table
  fails `maitetsu_pack_init`, a corrupt strip fails the first decode of that image.
  A decode failure inside the LVGL task sets a media fault; the input task polls it on
  its next tick, disables reading, and shows the red asset-error panel. Nothing is
  drawn from a stale or half-decoded frame and the story cannot advance.
- The per-image metadata (width, height, first strip, strip count, prefix, CRC) is
  generated into a `const` Flash table. `maitetsu_pack_image()` returns a pointer into
  that table, so the pointer is stable for the process lifetime — the LVGL `info` /
  `open` / `get_area` callbacks never see a shared or recycled temporary. `init`
  compares every generated entry with the pack table and refuses a mismatch.

Quality settings are `background 60 / patch 74 / CG 68 / thumb 74 / title 82`
(Pillow baseline JPEG, 4:2:0). The source backgrounds are 300×169 JPEGs that the RW5
app upscales to 432×243; the target stores them at 320×180, i.e. the same information
at a slightly smaller scale. This is a real recompression — documented here, not a
claim of lossless preservation.

### 2.2 Text and content tables

`tools/maitetsu_assets.py` regenerates `main/gal/gal_content.c` (1,537 dialogue
entries, 1,542 events, 4 transition cards, 1 ending, 35 CGs, 4 chapters, 6 recalls,
1 track) and `main/gal/maitetsu_assets.c` (675 scene rows, 335 patch rows, media
indices). Text is stored as literals, so `tools/gal_font.py` sees every paragraph and
the generated font subset covers all 1,682 required glyphs (16 px and 20 px, 4 bpp).

The generator verifies the source structure before writing anything: chapter counts,
675 unique scene ids, all layer rects inside the 432×243 art band, 35 gallery items in
six groups, and `converted/text.json` matching every paragraph.

### 2.3 Audio

The title theme is the complete source track: 1,135,520 samples at 4 kHz, encoded as
4-bit IMA ADPCM in independent 256-sample blocks (4,436 blocks, 585,588 bytes,
including a 36-byte header and CRC32). Measured reconstruction SNR is 22.16 dB — the
source itself is already band-limited to 2 kHz, so this is the honest cost of the
4-bit representation. Playback decodes block by block and linearly interpolates ×4 to
16 kHz (I2S/ES8311), so the shipped duration is exactly 283.88 s. Nothing is trimmed
and no synthetic tone replaces it. The playback task feeds the I2S DMA in 16 ms slices
and logs one bounded timing line per full pass (`samples / wall-clock ms`); that line
is a device diagnostic for pacing, never a rate change.

## 3. Controls

Landscape 320×240, three keys (logical UP / DOWN / OK; the BSP maps them by level, so
orientation changes do not change the semantics):

| Page | Short OK / UP / DOWN | Long OK | Long UP | Long DOWN |
| --- | --- | --- | --- | --- |
| Reading | Advance one page/line; while the typewriter is running the same press completes the page and advances, never requiring a second press. Long text still advances one pagination unit at a time. | In-game menu | Fast-forward jumps to the next sentence immediately, skipping the rest of the current sentence's pages, the typewriter and the auto interval; while held it advances one sentence per 50 ms input tick and stops on release | Toggle auto-play |
| Chapter card | Continue | — | Fast-forward past it | Toggle auto-play |
| Choice | UP/DOWN move, OK chooses | Back to the reading page | — | — |
| Lists, menus, save/load, settings, gallery | Move / toggle / run | Back | — | — |

Fast-forward stops by itself at choice, ending and other confirmation pages; it never
selects an option. A long press produces only its long-press action: the release event
never adds a short press (covered by tests). Auto-play advances 900 ms after the
current page is fully shown; a short press while auto-play is on still advances and
leaves auto-play on.

### 3.1 Preview versus completion

Entering any chapter from the story browser or replaying a recall starts a **preview**
run (`preview = 1`, `run_mask` reset to that chapter). A preview is an explicit,
persistent run state, not just a coverage count: even reading from chapter 1 through
the ending shows the ending card marked as a preview and never grants the permanent
ending or the derived clear qualification. `preview` travels with manual saves and
automatic progress, so loading, continuing or restarting in the middle of a preview
cannot launder it into a formal run. Only **Start new game** clears `preview`; a formal
run that reaches all four chapters and the ending grants the ending once. Existing
permanent unlocks are never removed. Records written before the preview field existed
(v1–v3) load with `preview = 1`: they keep their text position but cannot award a new
ending.

### 3.2 Resource failures

- Any CRC or strip failure in the LVGL decoder raises a media fault. The key path
  settles it **before** interpreting the press (`input_task` delivers a key before the
  next tick), so a key that arrives after a failed decode can never skip the line that
  failed to display, and the story cannot slip into the ending from an undisplayed
  line.
- While the fault stands, every reading action is refused: advance, fast-forward,
  auto-play, chapter cards, choice execution, chapter/recall entry, New Game, Continue
  and Load. Menu, settings, gallery, about and leaving the reader stay usable.
- The error layer is a screen-level object above every page: it stays visible on the
  title page, CG viewer, CG list and gallery, and page switches keep it on top. If the
  first LVGL lock for showing it fails, the error is kept *pending* and retried on the
  next tick; `media_ok` is never restored by that retry.
- Startup treats orientation and the first frame as hard dependencies: a failed
  `bsp_lvgl_set_landscape()` stops the boot, and a failed first-refresh lock leaves the
  backlight off until the first key press instead of showing an unverified frame.

## 4. Persistence

| NVS namespace | Key | Contents | Format |
| --- | --- | --- | --- |
| `galcfg` | `cfg` | text speed, font tier, auto-play, panel transparency | unchanged |
| `galsave` | `slots` | 3 slots: event, page, chapter, run flags, `run_mask`, `preview` | v4; v1–v3 migrate (preview = 1) |
| `galprog` | `pos` | automatic progress: event, page, run flags, `run_mask`, `preview` | v4; v1–v3 migrate (preview = 1) |
| `galunlock` | `bits` | permanent CG (64-bit) / chapter / ending bits | v2; v1 migrates |

Save loads validate the blob length and version before touching the live state; a
malformed or truncated record is ignored and the previous in-memory state is kept.

## 5. Budget

| Item | Bytes | Notes |
| --- | --- | --- |
| App partition | 8,323,072 | `partitions.csv` factory at `0x10000` |
| Application image | 7,759,632 | `idf.py size` total image 7,759,520 |
| — media pack | 5,773,642 | 759 images / 3,994 strips |
| — title track | 585,588 | 4-bit IMA ADPCM, complete |
| — code, text, fonts | ≈1,388,000 | 1,682 glyphs × 2 sizes at 4 bpp |
| Free in partition | 563,440 | 6.6 % |

Static RAM (`idf.py size`, target ABI): DRAM 211,508 of 321,296 bytes used
(109,788 free), `.bss` 134,296, `.data` 7,324. The pack decoder's strip buffer is
19,200 bytes of `.bss`, the JPEG work pool 4,096 bytes, the LVGL image descriptors
21,504 bytes. The per-image metadata lives in a generated Flash table (12,144 bytes),
so `maitetsu_pack_image()` returns stable pointers and nothing is copied per call. The largest static frame from `-fstack-usage` is `show_line` at
1,104 bytes (1 KiB page buffer), then `gal_save_load` 416 and `maitetsu_pack_band`
256; the input and music tasks use 4 KiB stacks and the LVGL task 7,168 bytes.

## 6. Evidence

Reproduce with the pinned source; logs stay outside the repository.

```bash
# generated files are current (needs Pillow and the pinned checkout)
python tools/maitetsu_assets.py --source <maitetsu-rw5> --check
# independent reconciliation + pack decode + composition samples + audio duration
python tests/test_maitetsu_content.py --source <maitetsu-rw5>
# device-decoder strip test (needs the fetched esp_jpeg component)
cc -std=c11 -Itests/maitetsu_stubs -Imain/gal \
   -Imanaged_components/espressif__esp_jpeg/tjpgd \
   tests/test_maitetsu_pack.c main/gal/maitetsu_pack.c main/gal/maitetsu_assets.c \
   managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c -o /tmp/test_maitetsu_pack
/tmp/test_maitetsu_pack assets/maitetsu/maitetsu_media.bin tests/fixtures/maitetsu-scene-samples.txt
# repository gate
./tools/validate.sh --static
./tools/validate.sh --firmware
```

What the host tests actually verify:

| Test | Coverage |
| --- | --- |
| `tests/test_maitetsu_content.py` | manifest hashes, every image CRC, all 3,994 strips decode (Pillow), 675 scene rows/patch indices, 175 source-composition samples (worst channel delta 22/255), audio CRC + exact 283.88 s, generated C parses back to 1,537 paragraphs / 1,542 events, and with `--source`: every paragraph, the 675 scene ids, gallery counts and `--check` freshness |
| `tests/test_maitetsu_pack.c` | device TJpgDec decodes all 3,994 strips, per-image CRC rejection, corrupt/truncated pack rejection, deterministic repeated decode, all 675 scenes compose, 175 source samples (worst 5-bit delta 5); the per-image metadata is stable across the whole 759-entry scan (no aliasing) |
| `tests/test_maitetsu_decoder.c` | real LVGL build + the production `maitetsu_media` adapter: `info` / `open` / `get_area` / `close`, interleaved 320×180 backgrounds, narrower patches, CGs and 64×36 thumbnails; header colour format, `stride == width*2`, contiguous areas, LVGL path pixel-identical to the pack path, 175 source samples, foreign descriptors rejected, and a corrupted strip raises the media fault without pixels |
| `tests/test_gal_keys.c` | short OK/UP/DOWN each advance (also while the typewriter runs: one press completes the page and advances); long OK opens/leaves the menu without advancing; long DOWN toggles auto, short press while auto still advances; long UP jumps to the next sentence immediately (skipping the rest of a two-page paragraph), then advances one sentence per input tick, release stops, fast-forward keeps the screen awake; a short press still advances one page at a time; page changes clear fast-forward/auto; a chapter-4 preview and a chapter-1 preview read through all four chapters both never grant an ending, including after save/load, while a New Game run does; runtime decode faults stop reading and show the error; corrupt saves are rejected without touching the live position |
| `tests/test_gal_choice_ff.c` | with a minimal branch fixture: fast-forward stops at the choice page, does not choose, long press release adds no step, option flags are written |
| `tests/test_gal_state.c` | save/progress/unlock v1/v2/v3 compatibility, `run_mask` round trip, CG indices 32–34 persist |
| `tests/test_gal_ui_error.c` | real LVGL rendering to a 320×240 frame buffer with the production UI: a corrupted thumbnail raises a real decode fault, the error layer is verified on TITLE / CG viewer / CG list / gallery / reader / menu after page switches (screen-root parent, visible ancestor chain, actual background and text pixels, no blank-white screen), and hiding it restores the page |
| `tests/test_gal_app_flow.c` | table shape (1,537/1,542/675/35/4/6/1), chapter transition positions, boot → title → chapter card → first line |

## 7. Unverified

- **No device test this round.** The firmware was built, merged and verified
  statically; nothing was flashed (flashing was not authorized). Landscape orientation,
  physical key feel, audio output, backlight timing and long-run stability remain
  unverified on hardware.
- The host integration test drives the real LVGL decoder (`info` / `open` /
  `get_area` / `close`) and byte-compares the result, but the on-device draw path
  (partial refresh over the scene) has not been observed on the panel. Startup only
  claims that the first refresh *request* returned (`lv_refr_now` under the LVGL lock);
  panel flush completion is not asserted.
- Landscape and the first frame are hard dependencies: if the LVGL lock or
  `bsp_lvgl_set_landscape(true)` fails, the firmware stops before drawing instead of
  running at the wrong orientation; if the first refresh cannot take the lock, the
  backlight stays off until the first key press.
- ADPCM 22.16 dB SNR is measured offline against the source WAV; perceived quality on
  the ES8311 speaker is unverified.
- Image quality was chosen to fit the 8 MB partition at `background 60 / patch 74`;
  a reviewer may prefer a different trade-off. Changing it only requires regenerating
  the pack and rebuilding.

## 8. Rights

The upstream RW5 project's MIT license covers its authored code. Text, artwork, logos
and music remain with Lose, HIKARI FIELD and their rights holders; MIT grants no
additional rights to those materials. This work does not authorize publication,
distribution or device flashing.
