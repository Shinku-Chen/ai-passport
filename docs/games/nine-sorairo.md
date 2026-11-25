<p align="right">
  <a href="nine-sorairo.zh_CN.md">Chinese</a> · <strong>English</strong>
</p>

# 9-nine- Sorairo: port and implementation notes

This page documents the title-specific work on top of the `feature/galgame-framework`
baseline. It is not a generic framework manual; see
[`../development/engineering/galgame-framework.md`](../development/engineering/galgame-framework.md)
for the shared UI and persistence contracts.

## 1. Source and provenance

- Source: `https://github.com/xuanyide01/9-nine-rw5`, commit
  `6a8907a5ae576f4377abe4b74f15bd9af13efe07`.
- The authoritative story is the already-processed Watch graph
  (`src/common/graph.js`) plus its committed text chunks under
  `src/common/story/**`. `converted/graph.json` is used as a cross-check and for
  script/label names; the PC scripts are not rebuilt.
- The packer never writes into the source checkout and never runs the source
  project's PC preprocessing scripts.
- `tools/nine_pack.py` writes a digest over every file under `src/common/` and
  `converted/` into the script header. `nine_pack_open()` exposes it, and the
  audit recomputes it independently. Changing a chunk, image or metadata file
  therefore invalidates the pack/source binding.
- Code in the source repository is MIT; the game text, art and audio are the
  property of Palette / the 9-nine- Project and are included only for personal
  offline use.

## 2. Content and semantics

| Item | Count |
| --- | ---: |
| Nodes | 56 |
| Text frames | 10,385 |
| OP markers | 1 |
| Chapters | 21 |
| Choice groups / options | 4 / 7 |
| Endings (metadata) | 2 |
| Visuals / layers | 2,119 / 8,143 |
| Images | 1102 |
| Gallery groups / variants | 22 / 72 |
| Variables | 15 |

The reader is a small graph interpreter, not the original PC VM:

- `locate()` applies node effects when the cursor reaches `count`, then returns
  text/OP, a choice list, or the next node.
- Choice visibility evaluates `when`; if no option matches, all options are
  offered (the source `visibleChoices` fallback).
- `next` edges are tested in source order and the first match wins.
- Variable storage is allocated from the pack's real key count (15 here, limit
  128), not a fixed 8/32-bit mask. Effects support `=` and `+`; conditions
  support `all`, `==`, `!=`, `<`, `<=`, `>`, `>=`.
- Persistent keys are written to the permanent progress record. `loop1` is the
  one key overlaid by `flags()` for conditions, matching the source exactly.
- Chapter/CG recall runs with `preview=true`: persistent writes and ending
  completion are skipped, so trying a chapter cannot pollute permanent progress.
- History stores 24 checkpoints; graph hops are capped at 500. Save/load records
  carry a version, pack identity, CRC and page.

## 3. Pack format and runtime layout

`tools/nine_pack.py` produces two files, wrapped by
`tools/nine_container.py` into one `assets` partition image:

- `nine_script.bin`: 256-byte header (version 2), tables for nodes, frames,
  zlib text blocks, visuals, layers, variables, conditions, effects, edges,
  chapters (with preview), endings and gallery; a UTF-8 string pool; the source
  digest in the header reserved area.
- `nine_images.bin`: JPEG backgrounds, indexed sprites/face patches with 8-row
  zlib blocks, OP frame records, two audio records and three title images plus a
  logo. Every record is CRC-checked; indexed records are checked against the
  decoded palette indices on demand.
- `nine_assets.bin`: 64-byte container header, script, images. The flash mapper
  checks the container CRC before `nine_pack_open()` sees any state.

The reading layout is full-width: the 16:9 art occupies 320x180 (y 0..179) with no black strip or crop, and the body panel is exactly y 180..239. It shows three complete 16px lines (26 half-width units, 13 CJK glyphs) or two complete 20px lines (20 units, 10 CJK glyphs). The speaker is a fitted semi-transparent pill over the art bottom (max 160px); Auto/notices use the fixed right side of the same row.

The art canvas is 240x135 RGB565 split into five 240x27 bands (64,800 B total,
12,960 B per allocation), displayed at 320x180 through LVGL scaling. This keeps
every contiguous allocation below 32 KB and leaves the dialogue panel and CG/OP
caption area usable. The pack art is 240x135; source art is scaled once at pack
time. JPEG quality is 42; sprites and patches keep RGBA composition and source
anchors.

## 4. UI and controls

The framework's 19 logical screens are retained. Changes are title-specific:

- 320x240 landscape through `bsp_lvgl_set_landscape(true)`.
- Title key visuals 1/2/3 unlock from permanent progress (`loop1`/`loop_sr`,
  `true`/`cf_sr`/`clear_true`); the settings page selects among unlocked art.
- CG browser is paginated (4 groups per page, 22 groups) and the viewer cycles
  the 72 variants; thumbnails are decoded from the real pack, not drawn blocks.
- Story page lists 21 chapters; selecting one shows a chapter transition and
  starts a preview. Music page plays the two real tracks.
- Gallery adds an independent OP gallery row: full 623-frame @5 fps + 12 kHz OP
  without story completion; natural/short/long OK returns to the same row and
  browsing stays silent.
- Reading page:
  - short OK/UP/DOWN always advances one complete unit: the next page of a long
    line, otherwise the next sentence. Typing or auto state never consumes the
    press without moving the cursor;
  - long OK opens the menu and does not add a short advance on release;
  - long DOWN toggles auto-play; auto waits for typing and then 900 ms;
  - holding UP fast-forwards every 120 ms, independent of typing and the auto
    delay; releasing stops; a choice/ending stops it;
  - manual input while auto is on still advances and turns auto off, rather than
    being consumed by the auto-off action;
  - text CRCs or image decode failures show a visible error and leave the VM
    cursor unchanged; retrying the frame is possible, but missing content is not
    skipped.
- Choice page: short UP/DOWN moves the selection and short OK confirms; long OK
  **opens the menu directly**, without a prompt/dialogue detour and without
  moving the VM; returning from the menu restores the full choice list and the
  current choice_sel.
- Secondary pages return on long OK. Page changes clear auto/FF/typing state.

The policy is in `main/nine/nine_reader.c`; `tests/test_nine_reader.c` drives it
with a controlled clock and a cursor model that asserts every short key changes a
page/line while typing is active and while auto is on.

Startup order: `bsp_lvgl_set_landscape(true)` must succeed under the LVGL lock; on
failure the backlight stays off and boot stops (no portrait fallback). The
the UI (or a visible error page) is created by `nine_app_init()`, then a fixed
40 ms delay precedes the backlight. That delay is only a scheduling margin, not a
verified LVGL flush-complete signal. Queue, input task and button initialization
failures are rolled back. All UI creation/setters run under `bsp_lvgl_lock()`.

## 5. Persistence

`main/nine/nine_store.c` owns three independent records in the `nine` NVS
namespace:

- three manual slots: metadata + VM save blob, record CRC, pack identity;
- automatic progress: VM state + page, never a permanent-only rollback;
- permanent progress: `loop1`/`loop_sr`/`cf_sr`/`clear_true`/`titleBg` etc.

Loading an old manual slot merges the newest permanent record, so a completed
true ending cannot be rolled back by an older save. Loading a preview save keeps
its preview flag and never writes permanent progress. Corrupt/identity-mismatched
records are marked as damaged and refused, not applied to the live VM.

## 6. Media

- Audio is 12 kHz mono IMA ADPCM (two tracks), decoded in a dedicated task and
  written through `bsp_audio_write()`; no Opus runtime.
- OP video is 623 JPEG frames at 5 fps, 124.66 s. Frames
  are decoded on the LVGL task through the same bounded image path.
- Both are explicit budget choices for 8 MB Flash; no claim of original media
  fidelity.

## 7. Validation

- `tools/validate.sh --static` compiles and runs:
  - real pack + VM traversal (counts, text, choices, true route, loop branch,
    preview isolation, save round-trip and corruption);
  - synthetic conditional/all-effect/first-match/fallback cases;
  - reader clock (short/long/auto/FF/release/page cleanup);
  - NVS slot/progress/permanent records with fault injection via a host stub;
  - IMA media decode and track metadata;
  - production image decoder against a Pillow reference composite (MAE check);
  - corrupt JPEG/background and indexed-sprite frames rejected before the canvas
    is overwritten; the app maps this to a visible error and blocked cursor;
  - frozen source-JS traces for three real choice sequences (30,871 view lines),
    including n4 option 3;
  - font coverage (2,258 glyphs, 0 missing).
- `python tools/nine_audit.py` additionally reconciles every source frame,
  speaker, layer, choice, condition, effect, chapter, ending, gallery variant,
  image tag, OP timing, audio record and the source digest.
- `./tools/validate.sh --firmware` builds with ESP-IDF 5.5.3, merges the
  `assets` partition image, verifies the partition layout and creates the debug
  archive.

Device acceptance remains NOT RUN: physical key timing, landscape MADCTL on the
panel, speaker loudness, deep-sleep wake and sustained heap during a long play
have not been measured on hardware.

## 8. Known limits

- Art is 240x135 displayed at 320x180; small text in the art is softer than the
  source.
- OP uses the complete 623-frame @5 fps source sequence; audio is 12 kHz mono. During OP only, the five art bands use nearest-neighbour LVGL scaling and skip a redundant full-band clear; the static reader/CG paths keep their existing anti-aliased scaling. The OP completion log records `end=natural|skip|error`, frame/render/gap counters, audio feed writes, and LVGL render/flush boundaries; these are port measurements, not glass-response or frame-rate proof.
- The framework's 3 manual slots replace the source port's 30 slots.
- The source graph contains only endings metadata for the loop branch; the loop
  milestone is surfaced as a notice and title-art unlock, while the true ending
  uses the ending page.
- Gallery thumbnails are 48x27 to keep RAM bounded; full images are shown in the
  CG viewer and OP at 320x180.
