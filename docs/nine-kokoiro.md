<p align="right">
  <strong>English</strong> · <a href="nine-kokoiro.zh_CN.md">简体中文</a>
</p>

# 9-nine- Kokoiro: engineering notes

This document describes the implementation behind the offline Episode 1 reader. It is
intended for review and for reproducing the pack/firmware. User-facing controls and the
content summary are in the root README.

## 1. Fixed input

| Item | Value |
| --- | --- |
| Repository | <https://github.com/xuanyide01/9-nine-rw5> |
| Commit | `6a8907a5ae576f4377abe4b74f15bd9af13efe07` |
| Episode directory | `nine-kokoiro-rw5` |
| Graph | `src/common/graph.js` (74 nodes, chunks, conditions, effects, choices) |
| Story | `src/common/story/**/*.txt` (91 chunks) |
| Runtime semantics | `src/common/engine.js`, `src/common/chunk-codec.js` |
| Art/audio | `src/common/images`, `src/common/faces`, `src/common/ui`, `src/common/opening`, `converted/gallery.json`, `src/common/*-bgm.opus` |

The converter reads these files only. It does not run the upstream preprocessing scripts
and does not reconstruct the uncommitted PC scripts. The graph's own structure is the
authority for branches and the story chunks are the authority for text/visuals.

## 2. Source inventory

| Metric | Value |
| --- | ---: |
| Nodes | 74 |
| Records | 8050 (8049 text + 1 OP marker) |
| Text bytes (UTF-8) | 395,919 |
| Choice groups / options | 5 / 9 |
| Next edges (including choice edges) | 94 |
| State variables | 15 (5 permanent) |
| Chapters | 17 |
| Endings | 2 (`loop1`, `true`) |
| CG groups / variants | 22 / 73 |
| Source OP | 599 frames, 6 fps, 384x216, 99.9 s |
| Source BGM | 2 Opus tracks |

The audit compares every node count, every text/speaker, every edge target/condition/
effect/label, every chapter/ending record and every visual layer against the source.

## 3. Reader semantics

The C reader in `main/nine/nine_vm.c` is an interpreter of the converted graph; it is not
a PC script VM. The contracts replicated from `engine.js`:

- **First matching next**: iterate the node's `nexts` in source order and take the first
  whose condition matches. Choice edges are stored after next edges; a running next with
  an effect is rejected as an unsupported pack expansion.
- **Conditions**: `==`, `!=`, `<`, `<=`, `>`, `>=`; nested `all` is compiled to postfix
  tokens with an AND token whose value is the operand count. A stack of 64 booleans is
  bounded.
- **Effects**: `=` and `+` on int32; overflow is rejected. The permanent-key set is
  encoded in the pack's variable table. In normal play permanent effects go to the
  progress map; in a chapter preview they are skipped.
- **Permanent key merge**: the source's `flags()` exposes only `loop_my` to conditions
  (not every permanent key). The C reader does exactly that. `loop1`, `true`,
  `clear_true` and `titleBg` remain permanent records used for endings, title visuals and
  unlocks.
- **Completion**: `complete[]` is folded into the node effects by the packer, in source
  order, including the derived `loop_my=1`/`clear_true=1` values. In a preview, the
  persistent writes are skipped, so no ending is recorded.
- **Filtered choices**: for episodes whose engine has `visibleChoices`, the packer sets a
  flag. If all options are filtered out, the reader offers every option again instead of
  dead-locking (covered by a synthetic fixture; Ep1 has no conditional options).
- **Preview isolation**: a preview keeps its own run flags but writes no permanent key,
  no automatic progress and no CG/chapter/ending unlock.
- **History**: 24 checkpoints, each packed sparsely (set variables only). `previous()`
  restores the checkpoint but overlays the newest permanent progress, so rewinding never
  revokes an ending.
- **Hop limit**: at most 500 non-text graph hops per locate; a self-loop reports
  `graph hop limit` and never spins forever.
- **Save/load**: a save restores the exact node/index/page/run flags and merges the newest
  permanent progress. Decoding is transactional: on a bad CRC, identity, size, key or
  cursor the live state is untouched. A corrupt save is reported, not erased.

## 4. Pack format

Two immutable little-endian files are embedded in the app image. No native pointers or
offsets depend on the load address.

`nine_script.bin` (magic `NINESC1\0`, version 2, 256-byte header, CRC32 over the file):

| Table | Record | Contents |
| --- | --- | --- |
| nodes | 48 B | frame/effect/next/choice ranges, title, chapter, stable id, source label |
| frames | 24 B | kind, visual, text block/offset/length, speaker |
| text blocks | 16 B | zlib stream, raw size <= 4096, CRC32 |
| visuals | 8 B | first layer, layer count |
| layers | 24 B | image, x, y, width, height, opacity |
| variables | 8 B | name, persistent flag |
| conditions | 12 B | opcode, variable, int32 |
| effects | 12 B | set/add, variable, int32 |
| edges | 24 B | target, condition/effect ranges, label |
| chapters | 8 B | title, node |
| endings | 12 B | name, variable, description |
| gallery | 16 B | name, image, group, thumbnail |

`nine_images.bin` (magic `NINEIM1\0`, version 2, 128-byte header):

- 32-byte image records: kind (JPEG / indexed / raw RGB565), size, absolute payload
  offset, payload size, CRC32.
- Indexed assets: palette count, block rows (8), block count, RGBA palette, then zlib
  blocks of palette indices. Block offsets are absolute.
- OP timeline: 12-byte records (image, timestamp, duration).
- Audio: 32-byte records (name, offset, size, duration, rate, channels, CRC32); payload is
  IMA ADPCM blocks (132 bytes / 256 samples).
- Header fields also carry the three title image ids.

The pack is embedded by `main/nine/nine_assets.S.in` (via `gen_assets.cmake`) with a
16-byte aligned section; every image payload is padded to 4 bytes, so RGB565 16-bit loads
on the RISC-V target are aligned. The generated assembly is a build-time custom command,
so regenerating a pack always re-embeds it.

## 5. Display pipeline

- 320x240 landscape; stage 320x180 at y=0, dialogue panel 60 px at y=180.
- The speaker pill sits above/outside the 60 px panel (bottom <= y179), over the
  art, with a compact semi-transparent background fitted to the name plus 8 px
  horizontal padding. It uses the 16 px tier even when body text is 20 px, caps
  at 160 px and ellipsizes longer names; an empty speaker hides the pill and its
  background. There is no numeric page HUD.
- Source 432x243 art uses one 16/27 factor for both axes (x 256/432, y 144/243);
  backgrounds become 256x144, and sprite/patch anchors, offsets and crops use the same
  factor. There is no vertical-only compression and no crop or letterbox.
- Four independent bands of 256x36 RGB565 = 18,432 B each (73,728 B total). JPEG MCU
  output and indexed blocks are decoded directly into the bands. Each band keeps its
  native 256x36 object size, is positioned at y = 0, 45, 90, 135 and drawn with LVGL
  scale 320/256 = 1.25 and pivot (0,0); the four 320x45 results cover 320x180 without a
  center-pivot crop or a seam. The app passes `lv_screen_active()` as the parent;
  production `nine_image_init(NULL)` is rejected, and the real-LVGL regression fills all
  four bands, checks the rendered 0..179 coverage, and decodes every gallery/OP asset.
- JPEG is decoded with TJpgDec. This tree fixes its RGB565 packer: the decoder's MCU
  buffer is B,G,R, so the old conversion produced BGR565 and swapped red/blue. The image
  reference audit caught this before delivery.
- Indexed sprites apply palette alpha multiplied by layer opacity. Faces are cropped from
  the 256x256 atlas with the source anchor math and composited over the body.
- Title visuals (3) and CG thumbnails (22) are raw RGB565 in flash and are handed to LVGL
  as image descriptors; no decode or RAM copy is needed. The title visual has the source
  logo baked in at the landscape position. Gallery CGs and every OP frame are packed at
  the same 256x144 16:9 geometry and fitted to the 320x180 stage; the CG info bar starts
  at y=180 and the OP page uses the stage without a second canvas.
- A failed preflight decode clears the canvas to black before returning failure, so the
  previous scene is never presented as the current line.
- LVGL's flush buffer is 320x16 (10,240 B, single buffer). Startup verifies the
  landscape call under the BSP lock and refuses to continue unless LVGL reports
  320x240. The backlight gate arms under the lock, requests a real screen invalidate,
  and opens only after a full refresh cycle (FLUSH_START then REFR_READY; not physical completion).
  It retries three times; a lock failure or timeout leaves the panel dark and refuses
  to report ready. All UI init and setters run under `bsp_lvgl_lock()` from the input
  task, not only from the LVGL task. The framework UI is reflowed
  for landscape: title menu right-aligned, 6-row menus, 5-row save/load pages with edge paging, 5-row
  chapter/history windows, 3x3 CG tiles, 60 px dialogue panel.
- One long OK on GAME or CHOICE opens the menu directly. Menu, Save/Load, Settings
  and History return to the same text page or to the full CHOICE list with its
  `choice_sel`; they never leave GAME with a CHOICE VM view. Entering TITLE from
  START does not overwrite the auto record loaded for Continue; leaving an active
  reading/menu session still saves the current point.

## 6. Text and fonts

- Text is stored in 100 zlib blocks of at most 4096 raw bytes; a frame is a block +
  byte offset + length and is NUL-terminated inside the block. The reader inflates one
  block into a 4096-byte cache and copies the line before the next call.
- Pagination uses `gal_text_pages` with 36 half-width units per line / 3 lines at 16 px,
  and 28 units / 2 lines at 20 px. Line-start punctuation is kept on the previous line;
  the full text is paged. The 16 px label is 57 px high inside the 60 px panel; the
  20 px label is 46 px high. Maximum source line is 80 characters (two pages at 16 px,
  three at 20 px).
- The typewriter runs on the LVGL timer. A short press is a real advance: it reveals the
  current pagination unit and moves to the next unit/line in the same press; it is never
  consumed only to finish the animation. Fast-forward is sentence-granular: at most one sentence per 50 ms control
  tick, skipping the typewriter, auto interval and the current sentence's
  remaining pages. It stops at choices/endings/OP; release adds no short press.
  A normal short key still advances one pagination unit.
- Fonts are generated by `tools/nine_font.py` (which uses the repository's own FreeType
  rasterizer and LVGL font emitter). 2182 glyphs at 16 px and 20 px, 4bpp. The source
  subset is `nine-text.otf`; 19 codepoints missing there are rasterized from Noto Sans
  SC. Each emitted glyph is read back through LVGL's cmap semantics and compared to the
  raster image (0 mismatches, 0 missing). `tests/test_nine_assets.py` re-checks that
  every character reachable from the pack and all ASCII are in the symbol set.

## 7. Audio

- Both source Opus tracks are decoded offline with ffmpeg to 8 kHz mono PCM and encoded
  as IMA ADPCM blocks (256 samples per 132-byte block).
- Playback runs in a dedicated 3 KB-stack task; the decoder produces 512-byte PCM blocks
  and writes them through `bsp_audio_write()`. The audio task never touches LVGL or the
  image decoder.
- The OP page uses the same task; the audio position selects OP frames (3 fps), and a
  timestamp table drives the slideshow. Skipping or finishing stops the task and returns
  to the story/gallery.
- Record CRC and block headers are validated at boot; a corrupt track fails the media
  init and the UI reports audio as unavailable instead of playing noise.

## 8. Persistence

NVS namespace `nine`:

| Key | Contents |
| --- | --- |
| `cfg` | text speed, font tier, default auto, transparency, sound, title visual |
| `unl` | 16-byte permanent bitset: CG groups 0-31, chapters 32-63, endings 64-65 |
| `prg` | permanent progress map (sparse key/value + set bits), CRC32 + pack identity |
| `auto` | automatic reading progress (same sparse save record), overwritten with a 1-byte invalid record when an ending is reached |
| `s00`..`s29` | 30 manual slots (sparse save records) |
| `snd` | last sound toggle |

A save record is 32 bytes of header + 6 bytes per set variable + counts + CRC32. It is
bound to the script CRC (pack identity). A slot blob is at most 220 bytes; 30 slots plus
auto/progress/unlock/config need about 9 KB of the 24 KB NVS partition. Slots are
scanned once at boot for labels; a slot whose header is damaged is shown as occupied but
`damaged`, and loading it reports the error without touching the live session.

## 9. Memory and flash budget

| Region | Bytes |
| --- | ---: |
| Flash: script | 555,087 |
| Flash: images + audio | 3,350,479 |
| Flash: 16 px font (estimated compiled) | 297,240 |
| Flash: 20 px font (estimated compiled) | 419,393 |
| Flash: app image | 5,339,504 (0x517970) |
| Flash: merged image | 5,405,040 |
| factory partition | 8,323,072 (about 2.98 MB free) |
| DRAM static used / total | 171,660 / 321,296 |
| DRAM free at boot | 149,636 |

Runtime fixed allocations: bands 73,728 + flush 10,240 + input stack 4,608 + audio
stack 3,072 + history 4,608 + I2S DMA about 2,880 = 99,136 B. The remaining heap is
estimated at about 46 KB after NVS/audio initialization. This is a static estimate; it
is not a device `heap_caps` measurement.

The target LVGL pool is 48 KB. A 32-bit zig host build with the real LVGL sources and
the real UI reports: after init used 26,540 / free 18,396 / largest 17,984; after every
page used 28,700 / free 15,968 / largest 12,004. A 40 KB run also passed but left only
2,932 B largest after all pages, so 48 KB is retained for target-config margin. The
64-bit host needs 72 KB for the same UI (used 45,824 / free 18,848 / largest 12,704);
that is an 8-byte-pointer proxy, not a target result.

`-fstack-usage` on the real target build reports, for the worst chains:

- input task: 48 B, `nine_app_key` 16 B, `handle_click` 32 B, `load_slot` 1,360 B,
  `nine_vm_save_decode` 1,136 B -> about 2.6 KB, with a 4,608-byte task stack.
- largest page builder: `fill_slots` 640 B; `fill_chapter_rows` 512 B.
- reading/advance: `nine_app_tick` 16 B -> `handle_view` 48 B -> `load_text_frame` 80 B
  -> `nine_image_show_visual` 800 B -> `jpeg_decode` 224 B -> `jd_decomp` 48 B.
- audio worker: 544 B on a 3,072-byte stack.

The 24-entry history buffer is heap-allocated at init from the pack's key count
(24 x 192 B here) and freed only at deinit.

## 10. Validation

Local evidence (this worktree, Windows host; absolute paths in the delivery report):

| Command | Result |
| --- | --- |
| `./tools/validate.sh --static` (with `CC=gcc`) | PASS |
| `./tools/validate.sh` (static + IDF 5.5.3 firmware) | PASS |
| `idf.py ... build` / `merge-bin` | PASS; app 0x517970, merged 5,405,040 |
| `python3 tools/verify_firmware.py build/gate-firmware` | PASS |
| `python3 tools/archive_firmware.py create ...` | PASS |
| `python3 tools/nine_audit.py ...` | PASS; 8049 texts, 94 edges, 20850 layers |
| `nine_source_trace.cjs` vs `nine_trace` (8 scenarios) | PASS; about 27.5k view/state lines |
| `python3 tools/nine_image_audit.py ...` | PASS; 209 frames, 24 source-composite frames |
| `python3 tests/test_nine_assets.py` | PASS |
| `-fstack-usage` target build | recorded above |
| `tests/nine_realgl/run_gate.sh` (32-bit 48 KB real LVGL; 64-bit 72 KB proxy) | PASS |
| `python3 tools/nine_shots.py` (host real-LVGL PNG/HTML) | PASS; normal/multi/CG/20px/menu |

Host-test skip groups are preserved: `test_check_repo` ran 28 with 4 skipped,
`test_archive_firmware` ran 26 with 5 skipped, and `test_install_passport_skills` ran 18
with 12 skipped; every skip is the Windows-symlink platform guard. CMake compiler-check
skips are build probes, not test cases, and are not counted here.

The source/VM diff scenarios are `first-run`, `true-run`, `bad-run`, `mixed-run`,
`alt-run`, `previous`, `skip`, `previews` (the last three exercise rewind, chapter skip
and chapter previews). Their union covers all 8050 source records and both endings.

The image reference has two levels:

- **Pack reference**: decode the pack's real JPEG/indexed/raw assets with Pillow and
  composite the exact layer list; worst mean absolute channel error over 209 frames is
  7.268, p99 25, max 57 (JPEG decoder differences and alpha rounding; tolerance mean
  <= 7.5, p99 <= 27).
- **Source reference**: re-decode the source art at the same target geometry and
  composite; worst mean 16.987, p99 58 (JPEG q38 + 192-color palette; tolerance mean
  <= 18, p99 <= 60). The 256x144 internal stage has a higher edge-pixel fraction than
  the previous 320-wide proxy, which is why the bounds are slightly wider; this is a
  coarse provenance check, not a lossless claim.

## 11. Reproduce

```bash
python3 tools/nine_pack.py --source <pinned-checkout> --episode kokoiro \
    --out assets/nine/kokoiro
(cd tools && python3 nine_font.py --font <nine-text.otf> \
    --fallback <NotoSansSC-VF.ttf> --out ../assets/nine/kokoiro --source-root ../main)
python3 tools/nine_audit.py --source <pinned-checkout> --episode kokoiro \
    --pack-dir assets/nine/kokoiro --out build/nine/audit-kokoiro.json
python3 tests/nine_host/make_filter_pack.py
# compile the host tools, then:
./build/nine/nine_trace.exe assets/nine/kokoiro/nine_script.bin \
    tests/nine_host/scenarios/first-run.txt
node tests/nine_host/nine_source_trace.cjs <nine-kokoiro-rw5> \
    tests/nine_host/scenarios/first-run.txt
python3 tools/nine_image_audit.py --source <pinned-checkout> \
    --pack-dir assets/nine/kokoiro --render build/nine/nine_render.exe \
    --out build/nine/image-audit.json
./tools/validate.sh
```

Do not reuse stale build directories after changing the pack: the build-time assembly
generator re-runs when either `.bin` changes, and `validate.sh --firmware` uses a fresh
directory by default.

## 12. Known gaps

- No video decoder: OP is a 3 fps image sequence with the original track (300/599 source
  frames, same 99.9 s duration). This is a deliberate, documented reduction, not a claim
  of full video support.
- Audio is 8 kHz mono IMA ADPCM; source Opus is not played bit-exactly.
- Voice and R18 content were never part of the source project, so they are absent.
- The 16:9 stage is decoded internally at 256x144 and shown at 1.25x as 320x180; title
  art is center-cropped to landscape, character art is quantized to 8-bit palettes and
  backgrounds to JPEG q38. All content is retained; there is no vertical compression.
- Chapter transitions have no dedicated graph nodes; the framework transition page is
  retained but not used by normal reading.
- Device behavior, real heap, audio output and NVS wear are NOT RUN/UNVERIFIED.
