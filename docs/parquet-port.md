<p align="right">
  <a href="parquet-port.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# PARQUET reader for AI Passport (landscape 320x240)

A complete offline port of the RW5 (REDMI Watch 5) visual-novel reader
`parquet-rw5` to the FoloToy AI Passport: ESP32-C3, 8 MB flash, no PSRAM,
320x240 landscape, three buttons. The firmware boots straight into the work's
own title screen; it does not enter the hardware demo menu.

Status: **software review accepted; production code frozen, awaiting device
acceptance. Device tests = NOT RUN.** The work gate passes; the repository-wide
`tools/validate.sh --static` still reports baseline failures on this Windows
host (details below). Lead review evidence: `build/lead-review-v4/probe-ui.log`,
`probe-badpack.log`, `negative-maintained.log`, `pack-stack.log`,
`archive-verify.log` (gitignored local evidence).

## Source and scope

- Read-only source snapshot: `parquet-rw5` @ `8ab38afe77c6a4d282447c7e13135b577a13303e`
  (KiriKiri Z script + converted RW5 assets). Metadata used for packing and
  auditing is tracked under `assets/parquet-source/`; the images and the source
  font stay outside the repository.
- Current source content, all preserved: **107 scenes / 434 script chunks /
  11,205 ops / 11,098 text ops / 2 choice nodes (4 options) / 4 END scenes**
  (`SIDE STORY`, `PARQUET`, and both `AFTER STORY` endings) / 54 speakers /
  37 chapters in 3 routes / 37 gallery groups with 367 CG images.
- Entries follow the source: `sideInitial = 1`, `flowScene = initial = 5`
  (START enters the pre-story once, the main story afterwards), `afterInitial = 93`
  (AFTER STORY appears only after `sf.parquet_main_clear`). Chapter browsing
  starts at the source chapter scenes and does not write story flags.
- Source fact worth knowing: scene **99** is a second choice node
  (`dummyselect2`) that no entry, chapter or jump reaches; it is kept in the
  pack and covered by a direct-entry regression instead of being deleted.
- Script text is compared op-by-op against the source snapshot by
  `tools/parquet_audit.py` (SHA-256 of the whole text stream matches), so the
  "all current source content" claim is auditable rather than asserted.

## Layout and controls

- 320x150 RGB565 art canvas on top (background + up to 3 portraits + cut-in),
  a 90 px opaque dialogue band below: speaker, chapter label, three text lines.
  Title/menu/list/gallery/CG/ending pages are separate LVGL layouts, not the
  demo shell.
- `another` view (695 text ops) uses a landscape-specific redesign: the art
  area stays 320x150 and a small "ANOTHER VIEW" badge marks the shot. The source
  RW5 layout instead shifts the art down 24 px and shrinks it to 262 px high;
  this port deliberately does not reproduce that vertical composition, so the
  two are not pixel-equivalent (content and backgrounds are unchanged).
- Keys: **UP** advance (hold = fast-forward, release stops), **DOWN** history
  (hold = auto mode), **OK** menu (long OK also opens the menu). Lists:
  **UP/DOWN** move, **OK** confirm, **long OK** back. A click emitted after a
  long press is swallowed; the next independent short press is not affected.
- Choices require an explicit OK; auto/fast-forward stop at a choice. Opening
  the menu or save/load/settings from a pending choice keeps the pending choice
  and its cursor.
- Typewriter time is accumulated in the UI (the input task ticks every 50 ms
  while the slowest speed is 55 ms/char), so slow text advances on 50 ms ticks
  and on irregular short ticks without losing the remainder.
- The auto slot is written from any sub-page that still has an effective reading
  checkpoint (menu/save/load/history/settings included), so going to sleep while
  in a menu keeps the same sentence, page and pending choice; a failed write is
  retried with a 2 s backoff and never overwrites the manual slots.
- Font size can be switched between **16 px and 20 px** in Settings; both
  subsets are complete for the whole script + UI (2,589 code points, 0 missing
  glyphs). Nothing that is not implemented is shown as an option.

## Script pack (`parquet_scn.bin`, 535,634 B)

- Scene table (12 B), 16 B fixed-size ops, 12 B visual tuples (2,623 unique
  background/body/cut-in combinations), chapter/route/ending/gallery tables and
  one string blob. Text is compressed per 32-op block (361 blocks, 552,959 B
  raw → 315,531 B zlib), so jumping into any chapter only inflates one block
  (max 2,370 B).
- `pq_pack_init` validates every table as an ordered span: each segment must
  start at/after the header, stay inside the blob, use overflow-checked lengths
  and never overlap another non-empty segment (zero-length tables are accepted
  only with a legal start). Text blocks use subtractive bounds
  (`comp_off <= size && comp_len <= size - comp_off`), so a wrapped
  `comp_off + comp_len` is rejected at init and again at load; no lookup is
  dereferenced before validation.
- Op semantics mirror `src/common/engine.js`: `text` stops on the line and
  carries its own visual snapshot (background, up to three body ids, cut-in,
  `another` view, cut-in opacity); `jumpScene` moves to scene+op; `choice`
  applies the source `effect` variable and jumps; `end` applies the current
  scene's `clearFlag` in story mode.
- Every text op is decoded at runtime and checked against its declared length
  and NUL position; a corrupt block is reported, never silently skipped.

## Image pack (`parquet_img.bin`, 5,187,277 B)

- 1,510 entries: 402 JPEG backgrounds/CG/title images, 138 palette BLOCK images
  (36 shared bodies + 101 cut-ins + logo), 970 expression PATCH images.
- Backgrounds and CG are re-encoded with `fit` inside 320x150 (no cover crop);
  the 367 gallery images are the same payloads, shown complete with letterboxing.
  JPEG quality 62 keeps 399 unique backgrounds at 2,835,630 B.
- Portraits keep the source's body/expression split: 979 used portrait ids map
  to **36 shared bodies** (one BLOCK each, not one per alias) plus **970
  expression layers**. Layers are cropped to their alpha bounding box and stored
  as RGB565 + 8-bit alpha PATCH blocks (1,409,240 B) instead of duplicated
  97x148 PNGs. Anchors come from the source `faceAnchor` scaled by the same
  150/262 factor for all portraits; one/two/three characters keep that scale.
- Cut-ins are 101 palette BLOCKs at 108 px tall, drawn with the source's 200/255
  opacity over a dimmed background. A failed background or portrait draws an
  explicit error colour, keeps the CG locked, and the next render retries.

## Storage and unlock semantics

- 30 manual save slots + one separate auto slot in NVS (`pqsave`). A save stores
  scene, text op, page, mode (text/pending choice), pending cursor, variable
  table, preview and chapter label; it is validated by version, total length,
  CRC32, scene/op bounds, variable ids and NUL termination. A failed write
  leaves the old slot and the visible state unchanged and reports a visible save-failure notice.
- Auto slot is used on scene change, before sleep and when returning to the
  title. NVS is never erased; if NVS cannot be opened the app degrades to
  read-only behaviour and says so.
- Global progress (`pqmeta`) holds the four `sf.*` flags (side/main/rin/tsu),
  37 CG-seen bits and 3 ending bits. It is loaded at boot, written with 2 s
  backoff retries on every page, and merged add-only when loading an older save
  (a load never clears a flag the global record already has).
- Loading a save replays the saved text op, which restores the same sentence,
  page, background, portraits and cut-in; loading a pending-choice save restores
  the same two options and the pending cursor.

## Memory account (same ELF/MAP as the archived build)

From `build/fw-parquet-fresh/FoloToy-AI-Passport.map`:

| Item | Bytes |
| --- | ---: |
| DRAM segment (`dram0_0_seg`) | 321,296 |
| IRAM image copy inside DRAM | 69,120 |
| `.dram0.data` | 7,316 |
| `.dram0.bss` (includes 24,576 LVGL pool, 10,992 shared tinfl, 8,192 text block, 4,096 JPEG pool, 4,096+2,048 patch buffers, 1,688 BLOCK row buffers) | 65,344 |
| `_heap_start` → `0x3fcce710` | **179,504** |

Dynamic allocations (internal 8-bit heap): canvas 96,000 (allocated first in
`app_main`), LVGL draw buffer 19,200 (240x40x2 in the BSP), input task stack
8,192, LVGL task stack 7,168, main/esp_timer/event stacks 3,584+3,584+2,304,
NVS page cache + handles ≈8,400, heap metadata ≈2,000 → **150,432 B**, leaving
≈**28.4 KiB**. The idle task stack (1,536) is a FreeRTOS static stack already
counted in the MAP and is not double-counted here. NVS and metadata figures are
estimates; the real free/largest block and frame time still require a device
measurement.

## Verification

Tracked gate: `tools/parquet_gate.sh` (also called from `tools/validate.sh`).

1. `tools/parquet_verify.py --fixtures tests/parquet/fixtures` — parses both
   packs independently, compares every op/text/gallery entry against the source
   snapshot (11,098 texts, SHA-256 match) and emits per-image expectations from
   its own BLOCK/PATCH decoders and Pillow JPEG decode.
2. `tools/tests/parquet_host_build.sh` — builds and runs the production C code
   on the host: **109,324 checks** plus **18 bad-pack checks** (pack structure, every op, four route/ending
   walks, orphan scene 99, chapter entries, save/load/corruption/failure,
   global progress, all 1,510 images decoded and compared — lossless formats
   byte-exact, JPEG by mean/max grid error) and **168 UI checks** driving the
   production UI with real PRESS/RELEASE/CLICK/LONG sequences (title, reader,
   history, menu, save overwrite, load at choice, key guard, gallery, settings,
   font switch, AFTER STORY unlock). Both are 0 failures.
   A native bad-pack regression (`tests/host/test_pack_negative.c`, 18 checks)
   mutates in-memory copies only: a wrapped text-block offset is rejected at
   init, segment overlap/header intrusion is rejected in both packs, zero-length
   segments keep legal boundaries, and alias-shared sprite payloads are still
   accepted. The normal pack must still load and the legal end-of-data block
   boundary must still be accepted.
   Regressions explicitly cover the 50 ms/55 ms slow-typewriter cadence and
   irregular short ticks, sleeping from menu/history/settings (same sentence,
   page, pending choice) with auto-save retry, and restoring a pending-choice
   save together with its own background/portraits/cut-in after visiting a
   different scene.
3. Negative gate: an image test with a deliberately missing header must fail to
   compile and the gate must exit non-zero (`tools/tests/parquet_negative_gate.sh`).
4. `tools/parquet_audit.py` — independent source audit (no packer import): 22
   checks over every op, chapters, routes, endings, gallery, portrait alias
   dedup and reachability; 0 failures.
5. Font coverage check (16 px + 20 px, 2,589 code points, 0 missing) and
   `tools/check_repo.py`.

Firmware: ESP-IDF v5.5.3, clean isolated build with the tracked
`sdkconfig.defaults`, `merge-bin`, `verify_firmware.py`, `archive_firmware.py
create` + `verify`.

Repository-wide gate (per-case accounting, unique `build/validate-*` output
dirs, no deletion):

- `tools/validate.sh --firmware` passed for the frozen build (fresh clean build,
  merge, layout verify, archive verify). It is not re-run by this docs-only
  round because the firmware must not be rebuilt.
- `tools/validate.sh --static` on this Windows host: **PASS 18, FAIL 2, SKIP 4**.
  The 4 SKIPs are the known baseline demo runtime link differences
  (`undefined symbol: lv_* / ui_pixel_*`). The 2 FAILs are unmodified baseline
  self-tests, not this work: `tests/test_check_repo.py` asserts forward-slash
  diagnostic paths (Windows emits backslashes) and
  `tests/test_install_passport_skills.py` needs directory-symlink privilege
  (WinError 1314). They are reported as FAIL, not silently skipped.
- The work's own gate inside that run is PASS (109,324 C checks, 18 bad-pack
  checks, 168 UI checks, 22 audit checks), and a tracked negative harness
  (`tools/tests/validate_negative.sh`) proves a broken work-C compile or run
  makes the gate summary FAIL with a non-zero exit.
- Compiler discovery is built into the tracked scripts (ZIG_CC/zig/
  `python -m ziglang cc`/CC); no ignored personal wrapper under `build/bin` is
  required.

## Build identity

- `FoloToy-AI-Passport.bin` **7,179,360 B**
  SHA-256 `2b634e0f3c499f2dd61acd3ed6203ce8ab66b7ba9516c3b6a3c7861dc28a7724`
- `FoloToy-AI-Passport-full.bin` **7,244,896 B**
  SHA-256 `c5167b5dc92e77aef4f2b53816e060821ec18f8ef2c801dfbbb72c24c1fdea98`
- ELF SHA-256 `6701796901372aa691e403e77d21df6d4671a124dc5048c18c84cb8646acdb20`,
  MAP SHA-256 `e82b458bf605b01c4cb91354755965a5cf3f6114d1f0cb4d887d7070b62c0115`
- Archive: `build/firmware/c5167b5dc92e77aef4f2b53816e060821ec18f8ef2c801dfbbb72c24c1fdea98/` (full/app/ELF/MAP/bootloader/partition
  table/flash_args/manifest), verified by `archive_firmware.py verify`.
- Packs: `parquet_img.bin` 5,187,277 B
  SHA-256 `b6eef1efb14a65c56c026c08768c4fea581bea96d36751586d92217747114d89`;
  `parquet_scn.bin` 535,634 B
  SHA-256 `e092ae53db6d181df8235e6c16ca1fe591a9ab6c902368007a99c641b5fc9ad5`.

## Known limitations / unverified

- **Device tests = NOT RUN**: panel orientation, touch-free key feel, actual
  free heap/largest block, frame time, palette/JPEG quality on the panel and
  deep-sleep wake are unverified.
- The source gallery was ungated; this port unlocks a gallery group when its CG
  is first displayed (documented deliberate UI policy, keeps content intact).
- Chapter browsing intentionally does not write story flags or endings.
- Scene 99 has no entry in the source flow; it is reachable only by a direct
  entry (covered by tests), not from the title or chapter list.
- No music/voice/animation: the RW5 snapshot ships none of those assets.
- The title menu has no EXIT item because the firmware exposes no application
  exit API; the idle policy (dim/screen-off/deep sleep) handles power.
- Battery level is not shown; it is optional and would need layout space.

## Reproduce

```bash
# packs (needs the read-only RW5 source tree for images/font; snapshot metadata is tracked)
python tools/parquet_pack.py --source assets/parquet-source \
    --images <parquet-rw5>/src --out main/parquet_data

# fonts (Noto Sans SC / Source Han Sans, SIL OFL 1.1; the subset is not committed as a font file)
python tools/gal_font.py --font <NotoSansSC-VF.ttf> --out-dir assets/fonts --sizes 16,20

# full gate
bash tools/parquet_gate.sh

# firmware (PowerShell with ESP-IDF 5.5.3 activated)
idf.py -B build/fw-parquet-fresh build
idf.py -B build/fw-parquet-fresh merge-bin -o build/fw-parquet-fresh/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build/fw-parquet-fresh
python tools/archive_firmware.py create build/fw-parquet-fresh --archive-root build/firmware
```
