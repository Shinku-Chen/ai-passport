<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Steins;Gate 0 reader (RW5 source, local technical validation)

A landscape visual-novel reader for the AI Passport, built from the already
converted Xiaomi Watch 5 source snapshot `steins0-rw5` at commit
`dd19f6ce40b348642b17126e25a327846d47367c`. It is a **local technical
validation build**, not a community release: the story, images, title art and
PUA glyph font belong to their rights holders and are not licensed for
redistribution.

## Delivery identity

- Working tree: `D:/Git-Workspace/Shinku-Chen/ai-passport-steins0`
- Branch: `feature/port-steins0-landscape`
- Framework baseline: `3c2e5013a9831c57124ef8f774193f0e06f1f14a`
- App image: **7,942,464 bytes**, SHA256
  `8e0e3e24c7eef711a09751e2bc239a78d02f6b44dfb902a283bc8f223859012c`
  (8,323,072-byte factory partition: **380,608 bytes free**).
- Full merged image: **8,008,000 bytes**, SHA256
  `38098013f7745401ae991955f7d54c205edc8af1fc59e062facedc868e62b335`.
- Same-run archive:
  `build/final-candidate-20261102T090728Z-43806/archive/38098013f7745401ae991955f7d54c205edc8af1fc59e062facedc868e62b335`
  (ELF, MAP, app/full bins, bootloader, partition table, flash_args,
  `manifest.json`); archive create+verify PASS.
- **Software review: PASS; awaiting device acceptance.** The `38098013…`
  archive was independently bound by the final-bind review (9/9 copies, 7/7
  manifest entries, actual partition/full/app, source compile-command/object/
  MAP, pack/font and independent archive verify all PASS). It is the current
  frozen software identity from source app `604291b0…`, app.h `1142546f…`,
  engine `8808f552…`, text `3e32fd6f…`, image `a48078b9…`. The older
  `aabf3025…`/1951 archives remain history and are not current identities.
  **Device review is NOT RUN**, so this is software acceptance only, not a
  device or publication acceptance.

- ELF: **17,330,216 bytes**, SHA256
  `0702f16a2e8d97cf63a464797fa7d211b42c95ddecd1be92a4d5f054c05501d5`;
  MAP: **4,155,368 bytes**, SHA256
  `cb98f76db9eb4b4bb88a1cff3e10598479743b17988f777eaf50db59a6ccff04`.
  Both are in the same archive directory above.
- Resource pack: `assets/steins0/steins0_pack.bin`, **6,815,468 bytes**, SHA256
  `42ec84783cafbdbd52c3ce187cb9dca0cc8e0f36723488ea9f3e189bb223d742`
  (derived from the source game; not committed).
- Generated font source: `assets/fonts/steins0_font16.c`, **2,955,758 bytes**,
  SHA256 `cd29856e69cd72ed6a6b9b16677d98e4ef0c83c57fa3a92f0f31d5c0dc4bc2b7`;
  compiled font object: **444,920 bytes**.
- Pack generator report: `build/steins0_pack_report.json`; independent source
  audit: `build/validate-host-*/source_audit.json`.

## What is preserved

The independent audit (`tools/steins0_audit.py`, a separate parser; it does not
import the packer) recounts the source and compares every dialogue op, speaker,
view/layer, choice text/target/action, condition, ending, chapter entry,
gallery entry and RINE event against the packed bytes:

| Source item | Count |
| --- | --- |
| Story nodes / unique chunk files / chunk references | 41 / 853 / 1,567 |
| Dialogue frames (unique / referenced) | 33,549 / 61,810 |
| Text units / code points (speaker + body) | 52,279 / 681,673 |
| Distinct PUA glyphs | 3,137 |
| Choices / conditionals / ending nodes / distinct endings | 28 / 1 / 12 / 6 |
| Chapter starts / chapter groups | 104 / 14 |
| Gallery groups / variants | 106 / 198 |
| Views / unique layers / layer references | 3,979 / 2,558 / 9,128 |
| Asset ids / unique image files | 1,769 / 1,558 |
| RINE catalog messages / reply windows / choices | 1,385 / 16 / 48 |
| RINE reply sequences / contacts / bases / chapter cards | 504 / 10 / 16 / 14 |
| Source events in the graph | 1,165 |

The packed format now has **32 sections** (`SG0_SEC_COUNT=32`), including the
RINE catalog, reply, choice, sequence, contact, base and chapter-card sections.
No story frame, CG variant, expression layer, RINE event or card was dropped.
Every source image asset id is present, including the title composition
(`@title`), masks and crops; the packer deduplicates only identical parts and
unreferenced payloads.

## Text encoding, layout and font

The source uses a private-use encoding (`U+E000` + SC3 glyph id) and provides
`sg-original.ttf` without a full Unicode mapping. This port keeps the PUA text
and the source glyph shapes; it does **not** invent an OCR/Unicode transcription.
`tools/steins0_font.py` merges PUA glyphs from `sg-original.ttf` with ASCII and
normal Chinese UI glyphs from Noto Sans SC into one 16 px / 4 bpp LVGL font:
**3,733 code points, 0 missing, 0 blank, line height 19, base 2**. This adds
**33 glyphs** over the earlier 3,700-point subset; the compiled font object grew
from 438,156 to 444,920 bytes (**+6,764 bytes**). Widths for PUA Latin and
punctuation come from the source `glyph-widths.js` (`main/steins0_widths.h`), so
the C pagination and font advances agree.

Layout implements UTF-8 codepoint paging, typewriter reveal and closing-
punctuation line-start prohibition. The prohibition now backtracks over a
bounded run of consecutive closing punctuation/opening brackets until a legal
break or `line_start`, so a second `。` cannot fall to the next line start. The
full corpus was laid out in host tests (61,810 texts / 62,100 pages): **0
overlong lines, 0 line starts at U+3002, 0 line starts at the equivalent PUA
period**, with no UTF-8 splits or silent truncation.

## Images, screen and controls

- Logical screen 320×240. Scene art uses the source 432×243 coordinate space
  scaled uniformly into 320×180; the dialogue panel is 320×60 at the bottom.
  Portrait/expression parts are positioned and clipped to the art area.
- Background/CG/title are JPEG (no PNG at runtime); character bodies and face
  patches are JPEG + 1 bpp mask, decoded by the real TJpgDec callback and
  composited per MCU block. Scene views and masks are validated before the
  cache is updated; a failed layer leaves the cache invalid and the next frame
  retries.
- Quality tier: photos 320×180 q38, bodies q48, face patches q58, title q70;
  JPEG 4:2:0, non-progressive. Current image assets total **5,325,033 bytes**:
  photos 483 files/2,732,592 B, bodies 347/1,580,016 B + 184,435 B masks, faces
  728/572,894 B + 76,519 B masks, title 14,161 B, 14 chapter cards 98,506 B, and
  21 RINE stickers 59,958 B JPEG + 5,952 B masks = 65,910 B. The encoded image
  blob is 5,329,007 B. PSNR samples: photos 36.71 dB, bodies 28.07 dB, faces
  24.73 dB. The measured tier was kept to preserve flash headroom. The decoder
  carries each target surface's width/height/stride end-to-end: scene/CG/title/
  card use 320×240, RINE stickers use a real 96×96 target. Guard/canary tests
  cover all 21 stickers, target-boundary RGB/transparent pixels, clear/failure
  paths and corrupt-stamp retry; the old fixed 320-stride write is gone.
- Controls: UP click advances (first completes typing), UP long fast-forwards
  until release; DOWN click opens history, DOWN long toggles auto; OK click
  confirms/advances, OK long opens the menu. Lists use UP/DOWN, OK, long OK to
  return. A trailing CLICK after a LONG is swallowed; the next independent
  short press still works.
- Auto reading is continuous across sentences, pages and chapter cards. A
  manual click/long UP/long OK stops it; the menu/title also stop it. It never
  auto-selects a choice or a pending call and stops at those confirmations.
  Fast-forward stops on release; both auto and fast-forward stop at choices.
- Pages: source title art + six menu entries; game text (speaker, 3 lines,
  page indicator, chapter/date, battery); two-way choice overlay; menu; 30
  manual save slots + separate auto slot; settings (text speed, auto interval,
  clear list, about); chapter select (14 groups / 104 starts, all accessible
  from empty progress); gallery (106 groups / 198 variants, fullscreen CG with
  variant switching, all accessible from empty progress); history (8 entries);
  about; ending; overwrite/return confirmation; chapter cards; RINE phone
  (inbox, sent, contacts, calls, reply and sticker views).

## RINE / phone subsystem

The phone UI is implemented rather than deferred. It uses the source's RINE
event stream and the packed RINE sections:

- Inbox/sent/contact/call views, unread/read state, reply windows and reply
  choices, 21 animated stickers, chapter cards and date/contact labels.
- A non-critical call whose `locate` arrives late is automatically deferred to
  `n2@0` while `pendingCall` is retained; accepting it later from the phone
  jumps to `n1@0`. The pending call is cleared by `closeChoice` at the source
  timing, and is preserved by save/load.
- RINE state is part of the save: catalog refs, read bits, replied/window bits,
  pending call and last card survive checkpoint restore without replaying
  messages.
- Reply/sequence rows inherit the mother mail date, so sent and incoming rows
  stay in the same `(base,date,sender)` thread; the source S_reply check yields
  date 101123. `sg0_engine_rine_date` returns the DATE-table index (uint16); the
  app converts it to the full `sg0_pack_date()` value and stores that six-digit
  VALUE as `uint32_t` in `phone_thread_date`, typed thread keys and all
  list/header formatting. There is no uint16 cast of the value; base/message
  ids and DATE indexes retain their existing widths. `unread` counts incoming
  unread only (source S_reply = 6); it does not mark outgoing rows as inbox-read
  or include them in the badge.
- `expireRine` only adds the fallback message when the window is read and not
  replied; unread or already-replied branches do not add it.

Source/encoded size accounting is deliberately kept separate:

| Item | Bytes |
| --- | ---: |
| Source phone + reply body (`phone/*.txt` 49 files + `reply/*.txt` 16 files) | 68,467 (33,600 + 34,867) |
| Encoded RINE fixed records (RMSG/RREPLY/RCHOICE/RSEQ/RCONTACT/RBASE/CARD) | 23,820 |
| Encoded RINE text payload | 48,767 |
| Encoded RINE stickers (21; JPEG + mask) | 65,910 |

The 68,467-byte source body, the 23,820-byte fixed records, the 48,767-byte
RINE text payload and the 65,910-byte sticker payload are different units and
must not be added into a single “117 KB” figure.

## Saves, conditions and unlocks

- NVS namespace `steins0`; manual slots `s00`..`s29` plus the `auto` key:
  **31 slots total** (30 manual + 1 auto). New writes use save **version 3,
  432 bytes**. The first 64 bytes remain the core; the extension carries RINE
  active/read bitmaps (1,408 catalog entries each), replied/window bits,
  pending-call state, last chapter card, **event_node**, **event_cursor** (the
  consumed node-event ordinal, not the story cursor) and an extension CRC16.
  Saving captures the event position; applying it prevents `load_op` from
  replaying already-consumed `rineReset`/`rineReply` events. A v3 event position
  is rejected unless `event_node` equals the saved node and `event_cursor` is
  at most that node's `event_count`.
- Legacy v2 (428 bytes) still decodes its folded RINE state and derives the
  event ordinal from the saved node/story cursor by scanning events without
  applying them. Legacy v1 (64 bytes) has no RINE history: it loads with an
  empty RINE state and a derived event ordinal, and is not rewritten or erased
  without confirmation. The migration never erases NVS.
- Empty, unreadable (`IO`) and damaged/incompatible (`CORRUPT`) slots are
  distinct. A failed `nvs_open`/`nvs_get_blob` is not treated as an empty slot;
  saving over an existing, IO or corrupt slot requires explicit confirmation.
  Cancelling leaves the existing blob byte-for-byte unchanged. `nvs_set_blob`
  and `nvs_commit` must both succeed before RAM slot metadata, preview, or the
  success notice changes. NVS open failure degrades to read-only/no-write; the
  reader never calls `nvs_flash_erase`.
- A save at a choice stores the node end, so loading restores the same pending
  choice; page and story/chapter mode are preserved. The `n0@108` page-1
  regression checks node, cursor, page, view and text after loading.
- Global unlock is a separate 34-byte versioned blob (6 ending bits, 104
  chapter bits, 106 gallery bits, flags, CRC). It is loaded at boot, merged
  (union) from loaded saves, marked dirty, retried on a 5 s throttle and forced
  before sleep. Save slots also carry an unlock snapshot.
- Chapter/CG **access is not gated by unlock**. From empty progress, all 104
  chapter starts and all 106 gallery groups (198 variants) are reachable
  directly from the title UI, matching source `openRoutes`/`openChapters` and
  `galleryList`/`viewCg`. Unlock/chapter/gallery bits remain metadata for
  read/reached statistics; they are not used to hide content.
- The single source condition `n8` uses the source semantics: ending 0
  (`prComplete`) routes to `n13`, otherwise `n14`; ending 5 also records
  ending 3. Chapter mode reaches END through the same `finishGame`-equivalent
  path and writes ending progress. Every branch is reachable; host walk covers
  both condition outcomes, all 41 nodes, all 6 endings and all 104 chapter
  entries.

## Capacity

Lead's local offline NVS capacity proof (not a project file) covers the real
production key set: NVS 24 KiB; baseline **507 used / 249 free entries** (1
wholly erased page) and conservative shadow **546 used / 210 free entries** (1
wholly erased page). It includes 31 records × 432 B (`s00..s29` + `auto`),
unlock 34 B and cfg 4 B. The shadow is an offline simultaneous-placement upper
bound, not a physical GC/power-loss transaction; A/B are not a heap contiguous
block. No physical NVS, heap or stack acceptance is implied.

## RAM budget

The numbers below come from the current archived ELF/MAP, not the previous
1951 build. The linker's DRAM segment and heap start are reported separately;
the low address tail is **not** treated as measured runtime heap.

```text
dram0_0_seg        321,296 B  (0x4e710)
.dram0.data          7,300 B
.dram0.bss         240,352 B
.dram0.dummy        69,120 B  (linker fill/IRAM overlap, not heap)
.iram0.text         68,768 B  (IRAM, shown for completeness)
_heap_start       0x3fccd570
segment end       0x3fcce710
link-time tail       4,512 B  (0x11a0; not a runtime free-heap claim)
```

Static buffers inside the image include the 320×240 RGB565 canvas (153,600 B),
96×96 stamp canvas (18,432 B), LVGL pool `work_mem_int` (32,768 B configured),
chunk inflate buffer (4,608 B), mask decompression (7,168 B), JPEG work pool
(4,096 B) and `s_app` (191,228 B; all of the above canvas/state is inside it).
The display uses one 240×40×2 = 19,200 B DMA draw buffer, not double-buffered.
Dynamic initialisation includes the 4,096 B input task stack (+TCB/queue),
7,168 B LVGL task stack (+TCB) and 3,584 B main-task stack. Main-task release
lifetime is not explicitly measured. NVS cache/driver allocations, task TCBs,
fragmentation, LVGL port heap and runtime largest/free blocks are not counted;
the link-time 4,512 B tail is not heap. No PSRAM or full-frame RGBA/PNG cache
is used.

A same-MAP budget view is A **61,856 B** / B **14,576 B**; known dynamic
**37,632 B** leaves A **24,224 B** before TCB/queue/NVS/driver/fragmentation.
These are pre-allocation budgets, not measured heap or a contiguous block. The
static pools above are already in BSS; the link-time 4,512 B tail is not heap.

Actual runtime heap, free/largest block, high-water mark, NVS allocation
behavior and frame time are **NOT RUN**; they must not be inferred from the
link-time tail. Unread behavior for different action sequences and natural
auto-card reproduction remains **PARTIAL**; the same-action S_reply check gives
incoming-only unread 6 and is not compared to probe counts that include
outgoing or opened-thread read state.

## Validation status

- **Build/merge/verify/archive: PASS.** Isolated ESP-IDF 5.5.3 build from
  tracked `sdkconfig.defaults` (LVGL 9.5.0 pinned, 32 KiB pool), absolute-path
  `idf.py merge-bin`, `verify_firmware.py`, archive create+verify and independent
  archive verify all passed. App **7,942,464 / 8,323,072 B**; full image
  **8,008,000 B**. The first wrapper merge attempt was **PARTIAL** only because a
  relative `-BuildDir` produced a relative output path (`FileNotFoundError`);
  the target compile/link succeeded and the absolute merge retry passed.
- **C/Python suites: game-specific PASS, overall FAIL.** The current
  `tools/validate.sh --static` run exits **1** and reports
  `Host tests: FAIL (suites PASS=34/36 FAIL=2 SKIP=0; source_audit=PASS)`.
  All **23/23 C regressions** pass, including **9/9 Steins;Gate 0 C suites**
  (pack, text, story, app, UI, RINE, all-open, IO replay, event save); **4/6
  Python gates** pass. Independent source audit PASS against the current 32-section
  pack (6,815,468 B, SHA `42ec8478…`). The app-level event-save regression
  covers n0@243 and n31@730 save/load/continue/auto, v2 folded migration and
  invalid event-node/ordinal/CRC/version rejection. The app UI preserves
  `11.23` for the R1 thread/reply/sent rows, v3 uses 432 B with event ordinals
  4/6, and v1/v2 migration plus page-1 round trips are covered. `unread` for
  different action sequences and natural auto-card reproduction remain PARTIAL.
  The script no longer uses a failure-count whitelist: a non-zero result is
  always FAIL.
- **Baseline Python gates: actual FAIL, not PASS/PARTIAL.** `test_check_repo`
  has **5 actual failures** and `test_install_passport_skills` has **3 actual
  failures** on this Windows host. They are counted as two failed suites in
  the overall FAIL; there is no 30/30 claim for the combined run. Logs:
  `build/validate-host-20261102-152002-35404/test_check_repo.log` and
  `.../test_install_passport_skills.log`; classification maintenance logs:
  `build/gate-classification-*/`.
- **Classification maintenance test: PASS.** The static gate runs
  `tests/test_validate_classification.sh` (+ shared
  `tools/validate_summary.sh`). It keeps FAIL above PARTIAL and verifies that
  historical-looking 5/3 counts, compile/run non-zero, and failure+missing-source
  all remain FAIL; skip-only is PARTIAL and a clean run is PASS.
- **Independent software review: PASS; device acceptance pending.** References:
  `build/review-final-bind-20261102T093438Z-45265-b44fc5d7/REPORT.md`,
  `build/review-phone-date-20261102-154228/REPORT.md`,
  `build/review-app/v14-20261102T080736Z/REPORT-v14.md`,
  `build/review-app/v18-20261102T090420Z/REPORT-v18.md`,
  `build/review-app/v15-20261102T082519Z/REPORT-v15.md`, and
  `build/review-sg0-pack-font-supplement-20261102T064045Z/REPORT.md`.
  Project-internal v18 documents the Lead-local capacity result; that result is
  not reclassified as a physical transaction PASS.
- **Device tests: NOT RUN.** Runtime heap/largest block/high-water/frame time,
  real NVS commit/retry, LCD/button timing, physical PUA rendering and all
  interaction acceptance remain unverified.

## Known gaps / deliberate deferrals

- OP movie, title music, BGM/SFX/voice are not included.
- Only the 16 px font is provided; no 20 px setting is offered.
- This is not a hardware, review or distribution acceptance. Do not publish the
  pack or the game-font-derived C file; regenerate them locally from a legally
  held source copy.
- RINE phone UI, late-call handling, chapter cards, all-open chapter/CG access,
  the 32-section pack and the new save layout are implemented in this candidate;
  they are not deferred gaps.

## Reproduction

```bash
# 1. Build the pack (needs Pillow) and audit it independently.
python tools/steins0_pack.py --source <steins0-rw5 checkout> \
    --out assets/steins0/steins0_pack.bin --report build/steins0_pack_report.json
python tools/steins0_audit.py <steins0-rw5 checkout> \
    --pack assets/steins0/steins0_pack.bin --json build/steins0_source_audit.json

# 2. Regenerate the merged 16 px font and width header (needs the source fonts).
python tools/steins0_font.py --pack assets/steins0/steins0_pack.bin \
    --pua-font <checkout>/src/common/sg-original.ttf \
    --ui-font <licensed Noto Sans SC OTF> \
    --widths <checkout>/src/common/glyph-widths.js --out-dir assets/fonts

# 3. Gate (static + firmware) with ESP-IDF 5.5.3 activated.
STEINS0_SOURCE=<checkout> ./tools/validate.sh
```
