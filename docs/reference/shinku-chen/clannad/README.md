<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# CLANNAD reader for the AI Passport

A landscape visual-novel reader ported from the Xiaomi Watch 5 release
[`xuanyide01/clannad-rw5`](https://github.com/xuanyide01/clannad-rw5), snapshot
`2c67720807db9d9761c22ddbc1f11e6a90cc2adc` (source README: 0.2.0). The
application baseline is `feature/galgame-framework` at
`3c2e5013a9831c57124ef8f774193f0e06f1f14a`.

## Delivery status

- **UI-startup candidate; software review and device acceptance pending.**
  This candidate is not the previously accepted `21e3fe93…` build. The only
  production change is the title-canvas clear plus the LVGL pool log described
  below; pack/font/configuration are unchanged. **Device tests: NOT RUN.**
- App image: **8,149,744 bytes**, SHA256
  `3b3a979af17fd849818ec30062bf9264e5da0780aa030f5abba8228b87074525`.
- Merged image: **8,215,280 bytes**, SHA256
  `263a4846aa2699d6a39d4df51c742c5c9bdda04d98bb56daef44e416c5017767`
  (flash at `0x0`; keep the merged-image data caveat in
  [`firmware-layout.md`](../../../development/engineering/firmware-layout.md)).
- Same-run archive:
  `build/firmware/263a4846aa2699d6a39d4df51c742c5c9bdda04d98bb56daef44e416c5017767/`
  (ELF, MAP, app, merged, bootloader, partition table, `flash_args`, manifest).
- ELF SHA256 `ab8ac5a4888d68933a0a0f5563cb660565ae5efdc7488e4626fe39fd2d053a5d`;
  MAP SHA256 `fd0685194f5b18781ab4aaedb29361991498b980cba6c8bbc969fc0850729428`.
- App partition headroom: **173,328 bytes** (`8,323,072 - 8,149,744`).
- Pack: **7,168,054 bytes**, SHA256
  `594affb1db5da33842556cfaabab0ffe31923f9c1ca9d28ae485e46b2a928dd6`.

The firmware is offline: no Wi-Fi, BLE, audio, voice or video playback.

## Independent review evidence

- Source/engine: `build/review-model/final-20261102T045914Z/REPORT.md`
- Storage: `build/review-store/final-20261102T045911Z/REPORT.md`
- Image: `build/review-app/final-20261102T045921Z/REPORT.md`
- Final 12 KiB identity/stack binding and MAP:
  `build/review-app/final-12k-20261102T063832Z/REPORT.md`

The three earlier reports and the 12 KiB report bind the accepted
`21e3fe93…` identity. They do not cover this UI-startup candidate; the new
candidate requires a final identity/stack recheck. Device acceptance is not part
of any of those software reviews.

## Source coverage

The current watch snapshot is preserved op by op; this is not the PC release and
no PC-only content is claimed.

| Item | Count |
| --- | --- |
| Scenes / chunks | 212 / 411 |
| Story ops | 150,288 |
| Dialogue ops | 101,488 |
| Choice sites | 309 (302 static + 7 dynamic) |
| Chapter entries / routes | 195 / 19 |
| Explicit `end` ops | 30 |
| Gallery groups / images | 68 / 174 |
| Unique image assets | 920 (356 story backgrounds, 174 gallery, 386 sprites, 2 title, 2 UI) |
| Sprite files for source IDs | 386 files cover 1,083 body IDs (the source itself merged expression variants) |
| Font | 16 px, 3,662 codepoints, 0 missing |

Seventeen scenes (`9035`, `9054`, `9070`-`9083`, `9600`, `9999`) are not listed in
the chapter browser but are retained and reached through common-route calls.
Every scene entry table is stored up to the highest index used by the script
(32); the source's own entry values are compared for all 212 scenes.

The independent interpreter in [`tools/clannad_audit.py`](../../../../tools/clannad_audit.py)
sweeps every chapter entry with two deterministic choice policies (390 runs),
plus both new-game paths. The production C VM reproduces all 390 transcripts
(text/choice counts, end site and FNV-1a hash). Source exploration enumerates
105 distinct runtime termination points (explicit `end` ops and empty-stack
`return`s; these are not 105 distinct original endings). The 390 policy runs
cover 98 of them and seven explicit choice paths cover the remaining seven.
A further five source choice self-loops are bounded-truncated; they are neither
endings nor port hangs, so this is not 110 endings. After Story and true-ending
progress are exercised separately through the app's global `g[]` regression.

## Screen and images

The logical screen is 320x240. The art window is 320x168 (4:3 backgrounds are
fully fitted at 224x168, centred, with theme-coloured side bars); the dialogue
panel is the lower 72 px and shows a speaker row plus three 16 px lines.

| Asset | Encoding |
| --- | --- |
| Story background | 224x168 JPEG, quality 40 (4:3 kept, no crop) |
| Gallery CG | 320x240 JPEG, quality 30 |
| Character sprite | alpha-trimmed, height 160, JPEG quality 55 + 1 bpp mask |
| Title | 320x240 crop of the source 432x514 painting, quality 45 |
| Logo / subtitle | alpha-trimmed JPEG quality 75 + 1 bpp mask |

The 386 sprite files share 107 exact-content 1 bpp masks (286,550 bytes total).
No additional body/expression merging was performed beyond the source's own
aliasing. The art window means the full-screen CG viewer is a complete 4:3 fit at
224x168 rather than a 320x240 fill; this is the explicit memory trade-off below.

JPEG decoding uses the component TJpgDec through a per-MCU callback that writes
directly into the 320x168 RGB565 canvas; no full-frame JPEG/RGBA buffer and no
PSRAM. A damaged JPEG, a declared-size mismatch, a truncated mask or a missing
asset fails the draw, shows a notice, keeps the cache invalid and allows retry.
The in-place canvas is cleared completely and invalidated before the failure is
returned; a failed later title/CG component cannot leave a half frame visible.

## Controls

- Reading: UP advances (typing completes the page first); hold UP to
  fast-forward, release stops. DOWN opens history; hold DOWN toggles auto-read.
  OK opens the menu.
- Choice: UP/DOWN move, OK confirms, long OK opens the menu. The pending choice
  and its non-zero cursor survive menu/save/load/settings round-trips.
- Lists: UP/DOWN move or scroll, OK executes, long OK returns to the source page.
- CG: UP/DOWN switch variants, OK returns to the gallery.
- Title: new game, continue (auto save), load, After Story (requires the source
  condition), CG, chapters/routes, settings.

## Storage and progress

- 30 manual slots in NVS namespace `clannad` (`s00`-`s29`), plus an independent
  auto slot in `clannad_auto/auto`. Auto saves use the current VM TEXT/CHOICE
  checkpoint even while MENU/HISTORY/SETTINGS/SAVE/LOAD/title is displayed;
  they are written on scene changes, every 18 advanced lines, on returning to
  the title and before sleep. A failed auto write keeps `auto_dirty` and retries
  with a bounded timer; sleep/title flush forces the retry.
- A save stores the exact scene/cursor/page/choice cursor, images, variables,
  counters, call stack, a preview and CRC32. Blobs are compressed with a
  fixed-Huffman DEFLATE encoder; decompression uses the same inflate as the
  story pack. Measured save blobs are 152-287 bytes compressed (661 bytes raw
  worst observed), so 30 slots plus auto/progress/config fit the 24 KB NVS
  partition with room for garbage collection. Slots distinguish EMPTY, VALID,
  CORRUPT and IO_ERROR. A failed read is never shown as empty and SAVE re-reads
  before writing; only a proven-empty slot saves without confirmation, while a
  valid slot always opens the cancel-default confirm. A failed NVS write keeps
  the previous slot and the current state and reports the failure; the firmware
  never erases NVS automatically.
- Bounded capacity expansion (2026-11-12): the VM table is 512 slots with
  a 3/4 probe ceiling; the 49-g + 74-choice witness peaks at 195 live variables.
  The save raw budget is 3 KiB (worst observed 1,415 bytes; true-ending
  checkpoint 1,069 bytes). The progress codec is bounded at 128 g[] entries /
  1 KiB (67-entry true-ending record 414 bytes; old 64-entry blobs remain
  readable). Zero-valued entries are explicit overlays, not omissions. The
  30-slot measured compressed saves above remain the ordinary save case; the
  larger bounds are not a claim that 128/512 entries fit every theoretical
  state.
- Global `g[]` progress is stored separately (`clannad/prog`) and applied over a
  loaded save so old saves cannot roll back light orbs or After Story flags.
  Chapter browsing applies the saved `g[]` values on entry (the source's
  `selectBrowse` calls `withProgress(newState(id), progress)`), but writes no
  progress back because `progress_sync` remains story-only. True ending and
  After Story are derived from the source's `g[45]` and source flag set.
- There is no RTC on this board; slots show chapter, mode, preview and occupancy
  rather than a wall-clock time.

## RAM (same-build MAP evidence)

`tools/clannad_mem_budget.py` reads the MAP of this exact ELF:
static DRAM data+BSS is 290,992 bytes; the post-BSS heap region is 91,568
bytes. The usable end is IDF's `SOC_ROM_STACK_START - SOC_ROM_STACK_SIZE`
(the same IDF used for the build), not the optimistic `0x3fce0000`; runtime
free/largest and fragmentation are not measured. The display draw buffer is sized from the panel's physical width:
240 x 40 lines x 2 bytes = 19,200 bytes (single buffer; not the 320-pixel LVGL
landscape width). Named dynamic items are that 19,200-byte draw buffer, 31,488
bytes of heap task stacks and ~1,600 bytes of queue/TCB estimates, leaving
39,280 bytes before unmeasured SPI/panel/I2C/NVS driver allocations and
fragmentation. The LVGL port task stack is 7,168 bytes; the idle task stack is
already static in the MAP and is not subtracted again.
Static named buffers include the 107,520-byte 320x168 canvas, 28,672-byte story
scratch, ~14,584-byte VM, 1,040-byte app list-row scratch and the 32,768-byte
LVGL pool. This is link-time evidence, not a device heap or frame-time
measurement.

### Input-task stack

The input task stack is **12,288 bytes**. The earlier 2,064/2,096 B values are
the inflate read/validation chains, not the whole-VM worst. The final
conservative expression audit uses a real root depth of 8 and an all-offset
upper bound of 9. Its parent chain is

`input_task 80 + app_key 48 + load_slot_blob 240 + run_frame 144 + vm_run 224 = 736 B`.

At the target ELF's actual `eval_expr_end` frame of 768 B, `9 * 768 + 736 =
7,648 B`; the independent reviewer's private 784 B proxy gives `7,792 B`. This
leaves 4,640/4,496 B of the 12 KiB input stack before unmeasured
libc/FreeRTOS/scheduler peaks. The real root-8 estimate is 6,880 B, but root
field coverage is deliberately treated as conservative; neither is device
evidence. The main task remains 4,096 B. The production fixes still keep the
DEFLATE distance tree at 30 symbols and the 1,040-byte list-row scratch in
`clannad_app_t`. Target `-fstack-usage` reports:

| Function | Target frame |
| --- | --- |
| `inflate_dynamic` | 1,200 B |
| `inflate_fixed` | 1,008 B |
| `refresh_page` | 272 B |
| `run_frame` | 144 B |
| `clannad_vm_run` | 224 B |
| `eval_expr_end` | 768 B |
| `load_slot_blob` | 240 B |
| `input_task` | 80 B |

The separate 2,064 B read and 2,096 B load/validate chains remain below this
whole-expression budget. These are compile-time frames; runtime high-water,
free/largest heap and frame timing are **NOT RUN**.

## Build and validation

```bash
python3 tools/clannad_pack.py --source <clannad-rw5> \
    --out assets/clannad/clannad_pack.bin --report build/clannad_pack_report.json
python3 tools/clannad_font.py --font <NotoSansSC-VF.ttf> \
    --pack assets/clannad/clannad_chars.txt --source-root main --out-dir assets/fonts
./tools/validate.sh --static        # with CLANNAD_SOURCE set for the source audit
python3 tools/clannad_paths.py --source <clannad-rw5>   # regenerate the 7 explicit paths
./tools/validate.sh --firmware      # ESP-IDF 5.5.3
```

Recorded host evidence:

- `tests/test_clannad_vm.c`: 194 checks, all 390 chapter policies + new-game
  paths, text/choice save validation/apply, preview truncation/UTF-8 rejection,
  cross-scene and call-stack saves, progress, paging and DEFLATE. The 6729
  witness checks cursor 114 with saved `g2=1/g32=0/g0=5` versus 203 without
  progress, then the local `g32=1/g0=4` changes.
- `tests/test_clannad_image.c`: 2,825 checks, all 920 assets decoded with the
  real TJpgDec, mask transparency, corrupt/truncated/wrong-size rejection and
  failure-instant clear/invalidate for scene, CG and title composition.
- `tests/test_clannad_stack_contract.py` and the `clannad-frame-budget` compile
  case keep the 12 KiB input stack, app-owned row scratch and 30-symbol distance
  table from regressing; target frames are in `build/stack-probe-12k/`.
- `tests/test_clannad_app.c`: 137 checks driving the production app with real
  PRESS/RELEASE/CLICK and LONG sequences, text/choice save/load, auto restore,
  six subpage checkpoint flushes, save-IO retry/confirm, write failure, bad
  saves, progress reload/retry, fast-forward-at-choice, image failure and bad
  pack.
- `tools/clannad_audit.py --pack`: 150,288 ops, 411 chunks, 212 scenes, 19
  routes, 195 chapters, 68 groups and 174 gallery entries compared with the
  source; result `passed`.
- The gate's negative test compiles a deliberately broken image regression and
  requires a non-zero exit.

The host gate is **PARTIAL: 30 PASS / 0 FAIL / 27 baseline platform SKIP**.
Every required CLANNAD C regression passes (194 VM / 2,825 image / 138 app
checks); the 27 skips are Windows symlink tests because Developer Mode is
unavailable: 18 in `test_install_passport_skills`, four in `test_check_repo`,
and five in `test_archive_firmware`. These are baseline/platform limitations,
not device or runtime results; CLANNAD-specific cases have no skips. The gate counts
PASS/FAIL/SKIP per case and exits non-zero if any C compile or run fails.
Evidence is written to a unique `build/host-tests/<timestamp>-<pid>/` directory
and is never deleted; firmware validation uses a unique directory under
`build/firmware-validation/` instead of a deleting trap.

## UI startup audit (candidate)

- **Title canvas clear (fixed):** the title page is transparent over the art
  canvas. `draw_title_art` now calls `clannad_image_clear` before composing the
  background/logo/subtitle, so the 48 px side bars use the theme clear instead
  of zeroed BSS or a previous frame. The app regression checks both side bars
  against the clear baseline.
- **All pages are created eagerly:** title/game/choice/list/CG/message pages and
  the notice exist from startup; page switching only changes hidden flags and
  does not delete objects. A host LVGL real-library smoke (64-bit upper bound,
  `LV_MEM_SIZE=32768`, no display) measured about 100% pool use / 88 B free after
  UI creation; this is **not** a target measurement (target is 32-bit and uses a
  reduced LVGL config). The target pool is not proven insufficient, but has no
  measured margin.
- `clannad_app_init` now logs `lv_mem_monitor` total/used/free/largest after UI
  creation for the next device run. This is diagnostic only; runtime pool
  high-water remains **NOT RUN**.
- Empty notice text is hidden; the notice starts hidden and is foregrounded only
  for non-empty text. No inactive-page deletion/task lifetime was introduced.
- Main task is 4,096 B (not 3,584 B in this checkout); `clannad_app_init` target
  frame is 96 B, `clannad_ui_create` 64 B, and the input task remains 12,288 B.
  The earlier 7,648 B expression/stack budget remains valid for the unchanged VM.

## Review fixes and remaining evidence

- **Text-save preview cursor (P0)**: `clannad_vm_save_validate()` now advances
  `r.pos` past the preview before reading `name_len`; the old 73-byte and 115-byte text blobs
  generated before the fix validates/applies after it (0 -> 1), while empty-
  preview choice saves remain valid. Field truncation and bad UTF-8 are still
  rejected without changing the VM.
- **Chapter progress**: the real 6729 chapter entry with saved
  `g2=1/g32=0/g0=5` reaches the source-visible cursor 114 (old no-progress path:
  203); chapter-local `g32=1/g0=4` changes do not alter the global progress
  blob. Source witness: `src/pages/index/index.ux:183` and
  `story/6729/0.txt:113`.
- **Autosave checkpoint and slots**: six subpage/title flush paths save the
  current reading cursor; occupied-but-unreadable slots show IO_ERROR/CORRUPT,
  never empty, and cannot be overwritten without a successful re-read plus the
  cancel-default confirmation.
- **Image failure**: the failure-instant canvas is byte-identical to the clear
  baseline (`diff_vs_clear=0`) for the bad second mask and a real truncated
  JPEG; A -> bad B -> A restores the exact A hash.
- **Not verified**: device stack high-water, runtime free/largest and
  fragmentation, frame timing, physical NVS/GC/power loss, physical LCD and
  glyph rendering, physical buttons, sleep, and 30-slot capacity on device.
  No real multi-page text was found in this corpus (101,488 dialogue ops,
  max_pages=1); multi-page page-index behavior is therefore untested rather
  than claimed.
- Evidence roots are preserved under `build/recheck-save/`,
  `build/recheck-image-fail/`, `build/recheck-store/`, `build/stack-probe-12k/`
  and `build/firmware/`; old archives `64f553a2…`, `6570063f…`, `73223756…` and `21e3fe93…` remain.

## Known limitations

- The source watch port already merged expression variants: 1,083 body IDs map
  to 386 images. This port preserves those 386 files and does not claim the
  missing PC expressions.
- The source contains no PC-exclusive scenes; no such content is reconstructed.
- Audio is omitted. The source only ships the title track; the story itself has
  no BGM/voice/video in this snapshot.
- Only the 16 px font is shipped; the 20 px setting is not offered because it
  does not fit the 8 MB image with the complete text/CG/sprite set.
- The CG viewer uses the same 224x168 art window as reading (complete 4:3 fit),
  not a full-screen 320x240 fill. This is required to keep the static canvas
  within the C3's DRAM when the complete sprite/CG set is present.
- The title art is a 320x240 crop of the source's 432x514 painting; logo and
  subtitle are drawn from the source UI assets.
- Device acceptance remains NOT RUN: stack high-water, free/largest, physical
  NVS/GC/power-loss, LCD/glyphs, physical buttons, JPEG frame timing, sleep and
  destructive capacity checks are all unverified. The static 4,640 B stack
  margin does not replace those measurements.

## Licensing

The port code is MIT. CLANNAD text, images, music and third-party assets remain
the property of their rights holders and are not covered by MIT. The source
snapshot's `LICENSE` and `THIRD_PARTY_NOTICES.md` are retained in the source
checkout; the font is Noto CJK under SIL OFL 1.1.
