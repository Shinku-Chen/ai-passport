<p align="right">
  <a href="manosaba.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Manosaba: The Witch Trial of Magical Girls — AI Passport Landscape Port

## Status

| Item | Result |
| --- | --- |
| Software review | **PASS; awaiting device acceptance** (production frozen at full `7f90c4e1…`) |
| Build/identity | PASS: app `7,442,288 B`, full `7,638,896 B`; actual factory@`0x30000`, `749,712 B` free |
| Host tests | **PARTIAL**: gate `PASS=42 FAIL=0 SKIP=1`; only the Windows `install_passport_skills` symlink test is SKIPped |
| Device tests | **NOT RUN / awaiting device acceptance**: no flashing or on-device acceptance |
| Source audit | PASS: 657,333 checks (inherited from software review; not rerun this round) |
| NVS | `nvs@0x9000/0x26000` (152 KiB/38 pages); offline capacity PASS, physical transactions NOT RUN |
- Frozen archive: `build/firmware/7f90c4e15f1bc4f8ff179759123b50b9c63fcdf17c1629200c791e7df538e976/`;
  full `7,638,896 B`, app `7,442,288 B`, factory@`0x30000`, `749,712 B` free.

- Worktree: `D:/Git-Workspace/Shinku-Chen/ai-passport-manosaba`
- Branch: `feature/port-manosaba-landscape`
- Baseline: `3c2e5013a9831c57124ef8f774193f0e06f1f14a`
- Source snapshot: `manosaba-rw5 @ a309a3614837b555741e5d8fd7cfe2b9428e2e49` (read-only)
- In-project review records: `build/review-app-final-7f90/REPORT_FINAL.md`,
  `build/review-app-round2/REPORT_ROUND2.md`,
  `build/review-model/second-round-20261102-141137/SECOND_REVIEW.md`

## Source coverage

`tools/manosaba_audit.py` does not import the packer. It reads the source JSON
and the MNPK binary independently and compares them:

- 458 numeric chunks across 424 scene directories (`0.txt`/`1.txt`/`2.txt`;
  each chunk ≤512 commands)
- 90,243 commands; 684,461 text characters (2,001,703 UTF-8 bytes);
  32,291 unique strings, 671,676 UTF-16 units
- 691 `choice`, 1,522 `offer`, 596 objection links, 13 `map`, 87 `evidence`,
  899 `bookUpdate`, 373 `resetBook`
- 371 chapter entries, 11 routes, 23 areas, 26 speaker names, 168 book keys
  and 307 versions
- 559 images: 62 backgrounds, 172 CGs in 61 groups, 151 portraits, 19 trick
  images, 151 book images, 2 solid-color placeholders and 2 title images. All
  557 source `images/` files are present exactly once, with no extra files
- Every scene's opcode sequence, text/speaker/actor, choice text and targets,
  objection text/targets, jump/call/chapter/book/map/evidence/background/
  portrait references, plus 5,586 `if`/`set` and portrait-pose expression comparisons
  (defaults + random variables, including division-by-zero/Inf/NaN)

Audit report: `build/recheck-audit3.json` (new pack); the previous report is retained.

## Controls

| Page | UP | DOWN | OK | Long OK |
| --- | --- | --- | --- | --- |
| Title | up | down | enter | about |
| Reader | next sentence/page (complete typing first); long = fast-forward until release | history; long = auto toggle | menu; if objection words are present, open the objection list | menu |
| Choices | up | down | **explicit confirm** | menu (keeps the pending choice and cursor on return) |
| Lists | up | down | execute | back to source |
| CG viewer | previous group | next group | next variant | back to list |
| Save/load | up | down | save/load | back |
| Settings | up | down | toggle | back |

- Real `PRESS → RELEASE → CLICK` and `PRESS → LONG → RELEASE` sequences are
  handled explicitly. Reader UP advances on `PRESS`; the trailing `CLICK` does
  not advance twice, and a long press never swallows the next independent
  short press.
- Auto/fast-forward stop at choices, objection words and `pause` boundaries;
  they never select for the player.
- Choices and objection words require explicit OK. Menus/save/load/settings
  round-trips preserve the same pending choice and non-zero cursor.

## Architecture

- `tools/manosaba_pack.py` compiles the source `catalog.js`, all 458 chunks,
  `book.txt`, `gallery.txt`, `assets.js` and every image into one read-only
  `MNPK0001` image. Commands are fixed 12-byte records; story text is UTF-16LE;
  expressions are precompiled RPN bytecode; strings are deduplicated; every
  section starts 4-byte aligned (avoiding unaligned u32 accesses).
- `main/gal_pack.c` validates magic/version/section bounds/record counts/
  strings/text/images/scene and jump ranges/expressions/book/gallery at boot;
  malformed packs are rejected before the app starts.
- `main/gal_vm.c` is a C interpreter for `engine.js`/`scene.js`: background
  layers, sprite instances (actor/sub/appearance/pose), focus, conditions,
  call/return, offer/choice, map/evidence/execution, clue images, damage,
  book versions, autosave, resetBook and dynamic jump. New game (`story=true`)
  and chapter entry (`story=false`) map to the source `isDebugDirectJump`
  behavior.
- `main/gal_save.c` stores version/length/CRC-checked saves. It validates the
  complete payload before replacing state; a bad save never changes the live
  state. There are 30 manual slots and one independent autosave slot.
- `main/gal_unlock.c` is a separate versioned global gallery/chapter bitmap.
  It only ORs new bits, never rolls back, and retries dirty writes.
- `main/gal_image.c` decodes with the real `esp_jpeg` (TJpgDec) into a
  320×150 RGB565 canvas and a 160×45 strip displayed at 2×; portraits are JPEG
  plus a 1bpp mask composited pixel by pixel.
- `main/gal_ui.c` / `main/gal_app.c` contain the dedicated dark-rose landscape
  UI and the three-button state machine; the baseline demo/`ui_pixel` visual
  shell is not used.
- `main/main.c` boots straight into the title page; NVS failures degrade to
  "no saving" without erasing; idle dims/screens off/deep-sleeps and forces an
  autosave, unlock and progress flush before sleeping.

## Image policy

- Every background/CG/trick/title image is **aspect-fitted to 320×240**
  (letterboxed, never cropped) and split into a 320×150 top part and a 160×45
  bottom strip. During reading the strip is covered by the opaque dialogue
  band; gallery/title pages show both parts as a complete 320×240 image.
- The title is the original 432×514 watch art; packing removes only the baked
  touch-button footer (decorative, not story art) and fits the landscape
  screen. This does not claim to restore a native landscape original.
- Portraits are aspect-fitted into 150×150, bottom-aligned to the dialogue
  band. All 151 expression variants are kept; no body×face precomposition and
  no upscaling of a single expression.
- JPEG quality: backgrounds/CGs/tricks/book `q=70`, portraits `q=74`, 4:2:0.
  All 557 source images plus 2 title images are in the pack; no full-frame
  RGBA/PNG runtime cache.
- **No cross-expression/cross-image differencing**: per-expression JPEG plus
  mask already fits the budget; differencing would add seams/negative savings.
  No representative pose is used as a difference base.
- Real TJpgDec regression decodes all 559 images at their declared geometry;
  151 portraits are compared pixel-by-pixel against an independent decode
  buffer through the mask; a truncated JPEG fails, and the same resource
  retries successfully.

## Saves and unlocks

- 30 manual slots plus an independent autosave constant. The autosave slot is
  written on chapter transitions, return-to-title and before sleep; `autosave`
  commands only mark a checkpoint and do not hammer NVS.
- A save stores the VM position, page/choice cursor, non-default variables,
  book versions/order, sprites, background layers, return stack, offer
  branches, choice cache and unlock snapshot.
- Load decodes into a temporary VM and replaces the live one only after all
  fields validate. Failure shows a notice and leaves the current state intact.
- Saving requires both `nvs_set_blob` and `nvs_commit` to succeed. Failed
  writes leave old slots/previews intact, and the failure notice is not wiped
  by list refreshes.
- If NVS is unavailable the app only reports "this run will not save" and
  never calls `nvs_flash_erase`. The partition table grows NVS to 152 KB
  (38 pages, `nvs@0x9000`) for 31 slots, unlock/progress/settings and at least
  one GC page; `phy@0x2f000`, `factory@0x30000/0x7d0000`.
- Global gallery/chapter unlocks are loaded at boot. Loading a save only ORs
  new bits and marks dirty; title/gallery/ending pages retry dirty writes.

## RAM and flash

From the same ELF/MAP (`build/recheck-build-nvs/FoloToy-AI-Passport.map`):

- app `7,442,288 B` of a `8,192,000 B` app partition (749,712 B free); full `7,638,896 B` of 8 MB
- pack `6,318,856 B`; 16px font with 2,999 glyphs (C source 2,666,593 B)
- `.dram0.data` 7,300 B, `.dram0.bss` 244,856 B; `s_app` 34,728 B
  (pre-NVS-expansion static; includes a reusable 8,472 B `load_vm` and 1,232 B
  book workspace)
- Static image buffers: art 320×150 RGB565 96,000 B + strip 160×45 14,400 B +
  portrait scratch 150×150 45,000 B = 155,400 B. **No full-screen 153,600 B
  contiguous block is allocated.**
- LVGL internal pool 44 KB; TJpgDec work area 4 KB; no full-frame RGBA/PNG cache
- Dynamic items (not double-counted from the MAP): draw 11,520 + input 7,168 +
  LVGL port 7,168 + main 6,144 + esp_timer 3,584 + event 2,304 + FreeRTOS
  timer 2,048 + controls 1,824 = **41,760 B**. Using IDF 5.5.3 registered
  regions: A = `0x3fcdc710-0x3fcce710` = 57,344 B; B = 14,576 B; addresses are
  adjacent but partition/phase and allocator constraints mean A+B is not a
  maximum-contiguous-block guarantee. A-41,760 = 15,584 B.
- NVS 38-page cache adds target `sizeof(Page)=84`: Page array 38×84=3,192 B
  (old 16 pages 1,344 B; 22-page increment 1,848 B); target
  `HashListBlock=128 B`: the official image has 31 item-bearing pages/35
  logical keys -> 3,968 B; the reviewer's conservative 32-used/6-free model
  gives 4,096 B; all-38 worst is 4,864 B. NVS cache total: 7,160 B actual,
  7,288 B conservative, 8,056 B all pages; old 16-page baseline 3,392 B,
  all-38 increment 4,664 B. These are one-HashListBlock-per-page scenarios;
  HashListBlock ENTRY_COUNT=29, so one page can hold up to ceil(126/29)=5
  blocks and this is not a lifecycle worst bound. After named 41,760 B,
  A=15,584 B; after NVS it is
  about 8,424/8,296/7,528 B for 31/32/38 item pages, before drivers, queues,
  TCB, heap metadata and fragmentation. Runtime free/largest, frame time and
  physical input remain **not measured**; Device NOT RUN.

## Build and validation

```bash
python tools/manosaba_pack.py --source <snapshot> --out main/gal_data/manosaba.pack
python tools/manosaba_font.py --font <snapshot>/src/common/manosaba.otf \
    --fallback-font "$WINDIR/Fonts/NotoSansSC-VF.ttf" \
    --pack main/gal_data/manosaba.pack --source-root main --out-dir assets/fonts
idf.py build
idf.py merge-bin -o <absolute>/build/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build
python tools/archive_firmware.py create build --archive-root build/firmware
python tools/manosaba_audit.py --source <snapshot> --pack main/gal_data/manosaba.pack \
    --report build/recheck-audit3.json
```

- Independent source audit: `AUDIT PASS: 657333 checks`
  (`build/recheck-app-nvs-final3/validate-static.log`)
- Route audit: all 371 chapter entries start; new-game DFS plus
  per-row probing of every choice plus 3,000 random playthroughs, 0 execution
  errors; **this round observed** 398/424 scenes; `bad`, `clear` and empty-kind
  (natural end/trial failure) endings reached
- The other 26 scenes were not observed this round: 11 `_TrialInit` (system
  pass-through inside one `read`), 13 `CommonBad01..13`, `System_BadEnd`,
  `System_Title`. This **does not prove** they are unreachable. The
  `CommonBad` zero-padding (`Common/CommonBad{commonBadEndId}`, ID 1 vs
  `CommonBad01`) is **confirmed current source behavior** and still needs
  independent verification; all data remains packed.
- Offline capacity only: 31 save blobs + 3 metadata (unlock/prog/set) +
  namespace. The tool summary `slots=34` means 31 saves + 3 metadata, **not 34
  save slots**. IDF 5.5.3's official `nvs_partition_gen.py` image: used
  `3,824` / free `964` entries, 31 used / 7 free pages. Thirty double-chunk
  saves (`s01..s29`+`auto`) add 30 entries versus the original 3,794 model and
  use one fewer page. Page 30 has 82 free entries; a new maximum BLOB needs
  122, so it must use a new page, leaving 6 free pages. Encoder witness:
  ordinary `257 B`, constructed max `3,278 B`, `GAL_SAVE_MAX=3,824`; the
  31×3,824 image is a synthetic upper, not 31 real story saves. Source-reachable
  worst is unproven; this does not prove physical commit/GC/power-loss. The
  old 64 KiB negative case is rejected. Logs: `build/nvs-capacity-e_3uh9hp/`
  and `build/recheck-app-nvs-final3/validate-static.log`.
- NVS RAM: target `sizeof(Page)=84`, 38-page Page array 3,192 B; 31 actual
  item-bearing pages add 3,968 B, the reviewer 32-used/6-free model adds
  4,096 B, all-38 upper bound 4,864 B. One page can hold up to 5 HashListBlocks
  (126/29), so 7,160/8,056 B are one-block-per-page scenarios, not a lifecycle
  worst bound; see `build/nvs-page-size-nvs.log`.
- Strict full-length traces (not common-prefix only): `test` 411/411,
  `trial04` 179/179 and `trial14` 249/249 with `mismatches=0`; `adv25` has only
  28 map "already investigated" row-text differences and `adv35` one execution
  row-text difference, both compensated by the real app UI. The in-project x4
  strict comparator now actually exports/compares row.value (no hardcoded 0);
  only samples/static entry points were checked, not every UI item or all
  story paths. Logs:
  `build/review-model/second-round-20261102-141137/logs/*.strict.log`.
- Images: 559/559 real decodes pass; 151/151 portrait masks pass pixel-exact
- Saves: full round-trip, CRC/version/length/truncation rejection, failed
  writes leave old slots intact, 31-slot NVS, commit-failure injection,
  unlock version/CRC/merge/retry
- UI: production `gal_app.c`/`gal_ui.c` driven through thin stubs:
  title→new game→reader→choice→menu→save/load→history→gallery→chapters→book→
  settings→about→ending; 0 LVGL lock violations
- Font: `--check` 2,999 codepoints, 0 missing, 0 blank; image/font negative
  gates confirm a broken compile or a missing font exits non-zero
- Complete `tools/validate.sh --static`: all project tests pass; only the
  `install_passport_skills` Windows symlink test is SKIPped, so the overall
  result is `Host tests: PARTIAL` (`build/recheck-app-nvs-final3/validate-static.log`).
- The gate keeps evidence: host temp dirs `build/host-tests.*`, firmware
  `build/firmware-build.*` and negative-test evidence `build/gate-evidence/`
  are retained and their absolute paths printed. `Gate summary: PASS/FAIL/SKIP`
  totals every gate item in the run.

Same-build archive:
`build/firmware/7f90c4e15f1bc4f8ff179759123b50b9c63fcdf17c1629200c791e7df538e976/`
(app/full/ELF/MAP/bootloader/partition/flash_args + manifest).

## Build identity

| Artifact | Bytes | SHA-256 |
| --- | --- | --- |
| app | 7,442,288 | `81905d3c6f90dc24322132e41fbe0c4fc747bd3619249af9c3bb1a94fd688f5c` |
| full | 7,638,896 | `7f90c4e15f1bc4f8ff179759123b50b9c63fcdf17c1629200c791e7df538e976` |
| ELF | 17,258,540 | `116b3ac91d219f57f782f010da06f23c2bb327856e44209a0709498742318ff3` |
| MAP | 4,148,310 | `da803cfbfcb8b7d7a175f45cf875d5d0d9b0774bc1dad1a876c31fb19e8c2918` |
| pack | 6,318,856 | `e96268ff7c829507236767afb7ea014059d975b1068ad61ab3c896913ca813ea` |

A separate clean build from `sdkconfig.defaults` and fresh merge/verify
produced the identified sizes; `build/recheck-build-nvs` is this round's
evidence. The previous `build/clean-repro` predates this source revision.

## This repair round

- Pack: link count plus the fixed return row is capped at 31; nonzero sections
  must not overlap, must leave the directory, and duplicate types/oversized
  ranges are rejected; declared TEXT ranges reject NUL/unpaired surrogates.
- Runtime: objections fail closed on bad frames; the sprite pool is 96 (source
  paths observed 33), `sceneBodies` uses stable order/x sorts, and overflow is
  explicit. Fatal damage keeps the cursor and sets `end_kind=trialFail`.
- Text/UI: conversion failures leave a valid NUL-terminated prefix; the
  longest 1,205 B book entry pages through a 1,232 B shared workspace; previews
  use the source 20-code-point plus ellipsis rule.
- State: checkpoints are independent of the visible page; save v2 persists the
  real `isDebugDirectJump` and current bodies; failed loads leave the live VM
  untouched. NVS reads distinguish NOT_FOUND/IO_ERROR/CORRUPT/OK and occupied
  unreadable slots require overwrite confirmation.
- Content: all 61 gallery groups / 172 CGs and all 371 chapter entries open
  from first launch, matching `index.ux`; unlock metadata is retained.
- Packer only treats exact `#000000` as black, uses `[-,]` actor bases, and
  emits `trialFail`. BSP flush is 24 lines (19,200→11,520 B); target RISC-V
  stack usage is at most 704 B (`render_history`), VM at most 320 B.

## Known trade-offs

- No voice, music, video, 3D trial camera or live animation, matching what the
  watch source already omitted; portraits keep the 151 main expression frames.
- The title art loses only the baked touch-button footer; the 432×514 original
  composition is not claimed to be restored.
- Only a 16px font is included; 20px does not fit the 8 MB budget, so settings
  only show available options.
- The watch source (`index.ux`) exposes all gallery/chapter entries. The port
  now matches that entry behavior: all 61 gallery groups / 172 CGs and all 371
  chapter entries open from first launch. Unlock bits are retained as records
  but never gate content.
- The source "next chapter" skip feature is not implemented; chapter select and
  normal reading replace it.
- The `CommonBad` dynamic-jump zero-padding is confirmed current source
  behavior; this round did not reach the 13 scenes, so they are not claimed
  unreachable pending independent verification. Source text is not changed and
  all data is packed.
- Five glyphs (U+2191, U+2193, U+6808, U+68DA, U+76CF) are absent from the
  source OTF and come from the system Noto Sans SC (SIL OFL 1.1); no commercial
  font is used.
- Device boot, key timing, real free RAM, frame time, NVS wear and screen
  framing are not verified.

## Provenance and license

- Original game and watch port: see the source snapshot README, `LICENSE` and
  `VALIDATION.md`; script/art/characters/trademarks belong to their owners.
- This port is an independent implementation. The upstream BSP and repository
  keep their own licenses.
- Font: snapshot `manosaba.otf` (Noto Sans Mono CJK SC, SIL OFL 1.1); five
  missing glyphs come from Noto Sans SC (SIL OFL 1.1). The generated C font is
  produced by `tools/manosaba_font.py` and keeps the original glyph license.
