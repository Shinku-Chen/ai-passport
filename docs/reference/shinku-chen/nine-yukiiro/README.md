<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# 9-nine- Episode 4: Yuki-iro — AI Passport offline port

This is the AI Passport port of *9-nine- Episode 4: Yuki-iro* (Episode 4). It is a
landscape 320×240, three-key, fully offline visual-novel reader. The firmware
contains the complete converted Episode 4 graph, all text, all reachable
branches and both ending records; no network, account, or PC runtime is needed.

## Source and scope

- Upstream watch port: <https://github.com/xuanyide01/9-nine-rw5>
- Pinned commit: `6a8907a5ae576f4377abe4b74f15bd9af13efe07` (Ep4 project
  `nine-yukiiro-rw5`)
- Authority: the committed processed `converted/graph.json`, `graph.js` and the
  143 story chunks. This port does **not** rebuild the original PC scripts.
- Story inventory: 212 graph nodes, 12,666 source records (12,665 text frames
  plus one OP marker), 23 chapters, 47 choice nodes / 70 option records,
  19 CG groups / 108 variants, 2 ending records, 78 runtime variables.
- Text: 668,498 UTF-8 bytes in 84 zlib text blocks (maximum block 8,192 bytes).
- Routes: D02's three-way branch and D20's four-way branch are preserved,
  including their ordered first-match `next` conditions. Seven option records
  are present but structurally unreachable in the source: choosing a branch
  routes past the later node that would offer that same branch again (or the
  branch gate excludes it). They remain in the pack for fidelity; 63 reachable
  option edges are covered by the C/JS differential trace.
- Endings: the `loop1` completion at n112 unlocks the second title visual; the
  `true` completion at n211 reaches the terminal card. A completed run sets
  both permanent bits, matching the source reader.

## What was adapted

- 320×240 landscape, using only the public BSP rotation API.
- The framework's pages, three keys, pagination, persistence and permanent
  ending gates are kept; title art, CG viewer, chapter directory, ending
  gallery, OP movie and two music tracks use the title's real assets.
- The source engine semantics are reproduced in C: permanent-key merge,
  first-match `next`, filtered choices with the all-options fallback, chapter
  preview isolation, 500-hop limit, and stable save cursors. No original PC VM
  or script interpreter is rebuilt.

## Controls

Landscape orientation is fixed. All three keys are logical after rotation.

| Page | Short OK / UP / DOWN | Long |
| --- | --- | --- |
| Start | start the title | — |
| Title | move selection / activate | long DOWN cycles unlocked title art |
| Dialogue | **advance one complete page, else the next line** (while typing or auto, it still advances; destination is shown in full) | OK: in-game menu; UP: threshold immediately advances one whole sentence, then at most one sentence per input tick; release stops; DOWN: auto-play toggle |
| Choice | UP/DOWN move; OK confirms | OK: in-game menu |
| In-game menu | move; OK runs | OK: back to story |
| Save / Load / Settings / History / Gallery / CG list / Chapter / Music / About | move or activate | OK: back |
| CG viewer | UP/DOWN switches; OK returns | OK: back |
| Gallery → OP playback | short OK starts real OP frames + audio; short OK skips; natural end returns to the same OP row | OK: skip/return |
| Ending | OK returns to title | OK returns to title |

The menu remembers the story page that opened it separately from the subpage
back target. Save/Load (including cancel and an invalid/empty slot), Settings
and History return to the same dialogue pagination or the complete choice list
with its prior selection; menu actions never advance the VM or grant permanent
progress. A successful Load restores the saved preview/formal state.

Gallery OP playback uses the same audio-position clock and one-frame-per-tick
renderer as the graph OP. It does not touch the live VM, save slots, preview
state, permanent progress or unlocks; a failed frame or audio start returns
visibly to the Gallery without advancing.

Fast-forward has a dedicated whole-sentence transaction: it skips the rest of
the current sentence's pages and typing/auto wait, advances at most one sentence
per input-control tick, has no 90/120 ms accumulation or catch-up loop, and stops
immediately on release, choice/ending, OP, or a resource error. It never selects
an option or advances a failed frame. Long-press release never fires an extra
short action. Auto-play pauses at a choice;
manual advance exits auto and moves the cursor. Chapter previews never write
automatic progress or permanent progress.

### Reading-page geometry

The reading art is the complete 320×180 16:9 image at y0..179; there is no
20px black strip, crop or stretch. The body panel is y180..239 (60px) and fits
three complete 16px lines or two complete 20px lines using the actual font
line height plus 1px leading. The speaker pill is above the panel over the art,
with a compact semi-transparent background, 8px horizontal padding, a maximum
width of 160px and dots for long names. Its bottom is at y179; its real glyph
line plus padding may extend upward but never clips. An empty speaker hides the
label and its background. The notice and Auto indicator have separate slots;
the reader has no "1/1" page-number HUD.

## Resource budget

| Item | Size / format |
| --- | --- |
| Script pack (`nine_script.bin`) | 950,863 bytes; zlib text + graph tables; CRC/episode/source identity |
| Image pack (`nine_images.bin`) | 5,716,981 bytes; 1,167 assets, 2,944 visuals, 11,554 layers |
| Backgrounds / CGs | 320×180 JPEG, quality 38 (downscaled from 432×243); 16:9 assets are letterboxed to 302×170 when shown full-screen |
| Sprites / faces / logo | indexed 192-colour RGBA, alpha preserved; face-atlas patches cropped and scaled |
| OP | 441 frames at 4 fps, 320×180 (source 551 frames at 5 fps), 110.180 s; measured 5 fps pack is 6,272,593 bytes. Losslessly sharing every repeated JPEG header (DQT/DHT/APP/SOF/SOS + EOI) saves 189,740 bytes; the resulting app still exceeds factory by 235,568 bytes, so 5 fps cannot be restored without reducing content/quality |
| Audio | 2 tracks: title 85.417 s, OP 110.184 s; 8 kHz mono IMA ADPCM (not Opus) |
| Fonts | 2,467 codepoints, 16 px and 20 px, 0 missing / 0 blank |
| Art bands | 5 × 23,040 = 115,200 bytes; 320×36 each, full 320×180 art; largest allocation 23,040 bytes |
| LVGL pool | 32,768 bytes; real 19-page 32-bit host run: min free 2,224 B, min largest free block 1,692 B, peak used 93% |
| Application | 8,193,232 bytes of the 8,323,072-byte factory partition (129,840 bytes free) |

The source checkout is about 13 MB on the PC; the device receives
only the two packs and generated fonts. Voice and adult assets from the PC
release are not included (the watch source is already non-adult, zero-voice).

## Build and verification

ESP-IDF 5.5.3, ESP32-C3, 8 MB Flash, no PSRAM:

```bash
source <ESP-IDF-v5.5.3>/export.sh
idf.py build
idf.py merge-bin -o "$PWD/build/FoloToy-AI-Passport-full.bin"
python3 tools/verify_firmware.py build
python3 tools/archive_firmware.py create build --archive-root build/firmware
python3 tools/archive_firmware.py verify build/firmware/<archive>
```

Host/static checks (the source checkout is read-only and may be outside the
repo):

```bash
NINE_SOURCE=<pinned-9-nine-rw5-checkout> CC=<host-cc> ./tools/validate.sh --static
```

`NINE_SOURCE` enables the source-to-pack audit and the source-JS/C-VM trace
differential. Without it those two checks report SKIP; all offline pack, app,
image and font tests still run.

Evidence in this worktree: pack-to-source audit 0 errors; 36 source-JS traces /
457,632 steps / 0 failures; production reader integration and strict choice/menu return regression PASS; all 1,057 pack
assets decode and the source-art composite mean absolute channel-sum error is
13 (max 62); font coverage 2,467/2,467. Device acceptance is a **LIMITED USER
PASS** on the E firmware (name/body layout; full Gallery OP playback, sound and
return; FF release-stop; save/load and font pagination). No whole-game, all-route,
long-run, power-loss, NVS or full hardware claim is made.

## Failure and startup behavior

- Image-layer allocation or bundle binding failure is a fatal, CJK-visible
  unreadable page. It is not a text-only mode; New, Continue, Load, chapter
  entry, short reading keys, auto and fast-forward are refused.
- A per-frame image decode failure keeps the text visible but blocks cursor
  movement, unlocks, permanent progress, automatic progress and manual saves.
  A short press retries only that frame; a later press can advance after the
  frame is restored. OP failure is fatal; title/CG failure is a visible notice.
- Startup order is input queue → button callbacks → input task → ready gate →
  backlight. Any failure rolls back the callback/task/queue and leaves the
  backlight off. First-frame `lv_refr_now` is a refresh request/logical cycle,
  not a claim that the physical panel flush completed.
- Boot uses a 4 KiB main task stack; the auto-save scratch buffer is heap-backed instead of a 1120-byte boot stack local, and the first-frame draw chain is exercised by the target `.su` budget. The image-pack CRC scan is table-driven and yields periodically while retaining full-pack and per-asset CRC rejection.
- The target and real-LVGL test use a 32 KiB pool. Lists that never show a
  right-hand value no longer allocate that label, so the 8-row CG window and
  7-row settings fit; the real 19-page run's minimum free pool was 2,224 B.

## Known limits

- The OP is 4 fps (441 real source frames; full 110.180 s) and the two BGM
  tracks are 8 kHz IMA ADPCM to fit the 8 MB no-PSRAM target. The measured
  5 fps pack is 6,272,593 bytes; even losslessly sharing all repeated JPEG
  headers saves 189,740 bytes, leaving the app 235,568 bytes over factory. It
  cannot be restored without reducing content or quality. This is a deliberate
  downscale from the watch port's 320×180/5 fps OP and 36 kbps Opus tracks, not
  a claim of bit-exact media.
- OP and single-asset CG/title frames use a one-resource path: one strict asset
  CRC/bounds check, one JPEG decode writing the existing five bands, one pixel
  clear and one invalidation. The previous path decoded every OP frame twice
  (validate, then render) and checked its CRC three times. A failed decode
  clears all bands under the LVGL lock and publishes no half frame.
- Sprites are 192-colour indexed; backgrounds/CGs are JPEG q38.
- Seven option records are unreachable by the source graph's own gating; they
  are retained but cannot be selected in a faithful run.
- Only the `true` terminal card is reachable; `loop1` is the mid-story permanent
  completion that unlocks title art 2.
- A 32 KiB pool is not enough for the untrimmed framework: the menu page had 684 B free and refresh could not complete. The reader now skips value labels on lists that never show one, which makes the eight-row CG window and seven-row settings fit in 32 KiB (real host run: min free 2,224 B).
- The reader exposes no back key, so the VM keeps one checkpoint rather than the
  source engine's 24-entry undo ring. This saves 24 full variable snapshots on a
  no-PSRAM C3; text history (24 rows) is separate and complete up to the row budget.
- Device acceptance is limited to the checked E firmware scope above. Build,
  merge, layout, archive, host-state and clock-driven input results are host
  evidence; every route/ending, long-run stability, power loss and NVS
  durability, and all hardware items remain unverified.

## Attribution

Original work: Palette, *9-nine-* Episode 4. The converted watch project and
its assets are by `xuanyide01` (see the pinned repository). This port's reader,
packer, C decoder, tests and documentation are new code in this fork. The
upstream project's MIT license covers its code; the game artwork, script and
audio rights remain with their respective owners. No commercial redistribution
is implied.
