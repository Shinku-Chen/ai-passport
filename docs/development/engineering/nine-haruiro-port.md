<p align="right">
  <a href="nine-haruiro-port.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# 9-nine- Haruiro: offline reader port

This branch (`feature/port-nine-haruiro`) turns the GalGame framework into a
complete offline reader for **9 -Nine-: Episode 3* (`Haruiro`)*, running
on the AI Passport (ESP32-C3, 8 MB flash, no PSRAM, 320×240 ST7789P3, three
buttons). It is one of the six ports prepared for this device; this branch ships
only this title.

## 1. Fixed source and scope

| Item | Value |
| --- | --- |
| Source repository | `https://github.com/xuanyide01/9-nine-rw5` |
| Pinned commit | `6a8907a5ae576f4377abe4b74f15bd9af13efe07` |
| Project | `nine-haruiro-rw5` (the REDMI Watch 5 port of the all-ages Chinese main line) |
| Source entry | `c01a.txt/*start` → graph node `n0` |
| Story package input | `src/common/graph.js`, `src/common/story/**`, `src/common/chunk-codec.js` |
| Engine reference | `src/common/engine.js` (PERSISTENT_KEYS, conditions, effects, order, fallback, preview, hop limit) |

The pinned snapshot is the source of truth. It contains the **non-R18 Chinese
main line**: 78 graph nodes, 10,659 dialogue frames plus one OP marker, 13 choice
nodes, 15 chapters and two ending milestones. The snapshot deliberately excludes
the 11 R18 scenes and all voice tracks; this port neither reconstructs nor
invents that content. "Complete" below means complete relative to this pinned
snapshot.

The original PC script is **not** rebuilt: the already-converted Watch graph is
the authority. The packer only compiles that graph into a compact binary; it does
not interpret the original TJS.

## 2. Content inventory (verified against the source)

| Item | Count / size |
| --- | --- |
| Graph nodes | 78 |
| Frames | 10,660 (10,659 text + 1 OP marker) |
| UTF-8 story text | 560,773 bytes |
| Choice nodes / visible options | 13 / 13 |
| Chapters | 15 |
| Ending milestones | 2 (`loop1`, `true`) |
| State variables | 23 (10 persistent keys + run markers) |
| Visual snapshots | 2,368 |
| Layer records | 9,333 |
| Image assets (deduplicated) | 1,063 |
| CG gallery | 18 groups / 100 variants |
| BGM tracks | 2 (`title`, `opening`) |
| Voice / R18 | none in the pinned snapshot |

## 3. Package format

Two binaries are embedded into the application:

| File | Size (final) | Contents |
| --- | --- | --- |
| `assets/nine/haruiro/nine_script.bin` | 778,099 B | Header/tables/text blocks/strings |
| `assets/nine/haruiro/nine_images.bin` | 5,773,507 B | JPEG/indexed images, OP frames, audio |

`nine_script.bin` (`NINESC2`, little-endian, no native pointers) contains node,
frame, text-block, visual, layer, variable, condition, effect, edge, chapter,
ending and gallery tables plus a bounded string pool. Dialogue is stored in
zlib blocks of at most 8,192 bytes; each frame points to a `(block, offset,
length)` triple and the packer guarantees a NUL inside the block. A whole-file
CRC32 covers every byte.

`nine_images.bin` (`NINEIM1`, version 2) stores:

- JPEG backgrounds/CGs at the **320×180** pack stage (`quality 38`);
- indexed sprites/face crops (256-colour palette, 8-row zlib blocks) so body
  transparency and per-expression face patches survive;
- 493 OP frames at the source 5 fps (full 98.62 s);
- two 16 kHz mono IMA ADPCM tracks;
- three title visuals and the title logo.

Every image record has a CRC over its whole payload. Audio records carry their
own CRC. The boot path validates both packages before any UI is built; a corrupt
package shows a visible notice and leaves the reader inert instead of painting
half-decoded state. The CRC engine is a shared 256-entry table (`nine_crc32.c`).
The earlier per-byte bitwise loop made boot validation a ~6.5 s CPU block (the
Lead device log shows an IDLE-WDT warning); the table keeps every whole-file and
per-asset CRC at ~3.9x host throughput (device estimate ~1.7 s for the ~12.2 MB
boot span, versus 6.5 s). No watchdog is fed, disabled or relaxed. The new image
still needs a Lead device run before that estimate is treated as measured.

## 4. Reader and VM

The VM is a direct interpreter of `engine.js`, not a new script language:

- `locate` applies node effects and completion at the node boundary, then returns
  text, OP, choice or end; ordered `nexts` pick the first matching condition;
- choices are filtered by condition and fall back to **all** options when none
  match, exactly like `visibleChoices`;
- only `loop1` is overlaid from the permanent progress record into the condition
  view; other persistent keys stay in the permanent record (matching `flags()`);
- `applyEffects` sends `PERSISTENT_KEYS` to the permanent record and other keys
  to the run flags; chapter preview (`progression='chapter'`) suppresses every
  persistent write;
- `complete` is folded into persistent effects at pack time, including the
  `clear_true` write for the `true` ending;
- the graph walk is bounded at 500 hops; malformed packs and out-of-range
  cursors return explicit error views;
- 24 state snapshots are kept for `previous`, with no pointers into the text
  cache: text is copied before any other pack access.

Persistence keeps three things separate:

1. **Run variables** (`flag[]`, plus the `flag_set` bitset) travel with a slot or
   the automatic record and are cleared by New Game.
2. **Manual slots** are 30 blobs (`s01`…`s30`), each with node, frame index,
   text page, preview flag, all run/permanent variables and a CRC. Loading
   validates the CRC and episode identity before touching live state. The source
   has 30 slots; the UI renders a four-row moving window.
3. **The permanent record** (`perm`) is written only for persistent effects and
   is merged over a loaded snapshot (except a preview snapshot), matching
   `Object.assign({}, session.progress, this._progress)`. Reaching an ending,
   unlocking the title visuals and clearing the game therefore survive loading an
   older manual slot, while the slot's position and run flags are restored.

The automatic record (`auto`) is throttled (1.5 s) and forced before deep sleep;
chapter preview never writes it.

Startup is a gate, not a best effort: `bsp_lvgl_set_landscape(true)` must succeed
under the BSP lock, `nine_app_init` must build the UI, and a forced
`lv_refr_now()` must submit the first frame **before** the backlight is enabled.
Landscape or init failure paints a centred 16 px CJK error page (the generated
`nine_cjk_16`, not the default Montserrat) and stops. Queue/task/button failures
paint the same kind of error page and roll back the created queue/task, then stop
audio. A text/image decode failure freezes the story at that frame (with a
visible notice); the menu can retry the decode, load a slot or return to the
title, but advancing cannot skip the missing content.

## 5. UI and controls

The framework UI is kept, reflowed for 320×240 and driven by an application layer
(`main/nine/nine_app.c`). The art is drawn on six 256×24 RGB565 bands
(12,288 bytes each) at the top of the screen; the 320×180 art fills the top and
the speaker name sits in a compact fitted semitransparent badge over the art
(bottom at or above y179; empty names hide text and badge), and the dialogue
box occupies y180..239 (60 px). The 256×144 canvas is upscaled 1.25×
to the full 320×180 reading art; the transparent name/auto/notice row overlays
that art (no black strip, no capsule), the OP keeps 320×180 and title/pure CG
keep 320×240. Name badge width adapts to real font metrics with 8 px side padding, caps at
160 px and ellipsizes long names; auto hint and transient notice keep fixed,
non-overlapping slots. The reading page-number HUD is not created (product
request), while real pagination, save/load page numbers and short-press page
turning are unchanged. The gallery has a fourth OP gallery row: it plays the
complete opening timeline and track, returns to the same row on natural end,
short OK skip or long OK back, and never advances the VM or writes NVS. Body text is three 20 px lines at 16 px (two 24 px lines
at 20 px), and pagination uses those real line counts.

| Page | Short OK / UP / DOWN | Long OK |
| --- | --- | --- |
| Start | enter title | — |
| Title | move / select | — |
| Reading | all three advance immediately; a typing page is completed and the same press moves to the next page/line | in-game menu |
| Choice | UP/DOWN select, OK confirms | in-game menu; return keeps the full options and selection |
| Menu / slots / settings / history / gallery / chapters / music | move / select | back |
| CG viewer | UP/DOWN cycle variants, OK back | back |
| Ending | return to title | return to title |

Additional reading gestures:

- A short press always moves the page/line cursor; it never merely completes typing.
- **hold UP**: the long threshold immediately advances one whole sentence,
  skipping the remaining pages of that sentence plus typing and the automatic
  wait. While held, each input tick advances at most one sentence; there is no
  90/120 ms inter-sentence accumulator or catch-up loop. Release stops without a
  short press; choices, ending, OP and corrupt resources stop the hold. A normal
  short press still advances one page.
- **long DOWN**: toggle auto-play. A 1.2 s wait starts only after the page is
  fully typed; a manual short press turns auto off **and still advances**.
- A long press of any button consumes the CLICK that the BSP may report on the
  same release; a new PRESS clears that latch.

Title visuals 1/2/3 unlock from the source UI's permanent loop/true record:
visual 2 requires `loop1`/`loop_hk` or `true`/`cf_hk`/`clear_true`; visual 3
requires both. The transient `titleBg` milestone (n40 writes 1, n77 writes 2)
never revokes an unlock, so a second New Game reaching n40 keeps visual 3. The
gallery exposes all 18 groups/100 variants (the source has no gating there), 15
chapter previews and both music tracks. Chapter preview starts a `preview` VM and
never touches permanent progress.

Long OK opens the in-game menu on both the reading and the choice pages. Menu
actions never move the story VM or grant permanent progress. Returning from a
reading-page menu restores the exact text sub-page; returning from a choice menu
renders the complete option list with the previous `choice_sel`. Manual save,
load, automatic progress and Continue/restart carry the text sub-page in the
existing save format (no format change); a CRC-valid but out-of-range page is
clamped to the last page by `paginate()`.

## 6. Fonts

`tools/nine_font.py` generates 16 px and 20 px LVGL subsets (4 bpp) from the
source's `nine-text.otf`, using Microsoft YaHei only for the handful of UI
codepoints absent from that font. The character inventory is the union of the
package's story text, all C string literals under `main/`, ASCII and the previous
inventory. Final coverage: **2,425 codepoints** (2,350 story characters), 0
missing, verified by re-reading the emitted LVGL cmap and glyph data.
`tools/nine_font_check.py` independently checks every story character against the
emitted font and runs in the host gate.

## 7. Memory and flash budget

Target: ESP32-C3, 8 MB flash, no PSRAM, ESP-IDF 5.5.3, `factory` app partition
0x7f0000 (8,323,072 B).

### Flash (final firmware)

| Item | Bytes |
| --- | ---: |
| Story script pack | 778,099 |
| Image/audio pack | 5,773,507 |
| Pack total | 5,747,788 |
| Compiled CJK fonts (16+20 px) | 799,119 |
| Flash data total | 6,614,160 |
| Flash code | 560,652 |
| Application image | 8,062,448 / 8,323,072 (96.9 %, 260,624 free) |
| Merged full image | 8,127,984 |

### DRAM (link-time)

| Region | Bytes |
| --- | ---: |
| DRAM total | 321,296 |
| Used | 160,744 (50.0 %) |
| `.bss` | 82,384 |
| `.data` | 7,704 |
| DRAM `.text` | 70,478 |
| Link-time remainder | 160,770 |

Runtime allocations out of that remainder (budget, not a measured device peak):

| Item | Bytes |
| --- | ---: |
| Art canvas: 8 × 256×24 RGB565 | 98,304 |
| LVGL draw buffer: 320×20 RGB565 | 12,800 |
| Input / audio / LVGL / main+timer+idle stacks | ~28,160 |
| Remaining for LVGL objects, NVS, codec buffers | ~21,394 |

The largest single allocation is **12,288 B** (one band); nothing allocates a
32 KB contiguous block. LVGL uses the system heap (`LV_USE_CLIB_MALLOC`) instead
of reserving a 64 KB private pool, so objects and the canvas share one budget.
`-fstack-usage` on the real target flags shows the deepest reader frames:
`nine_vm_save_decode` 1,136 B, `nine_image_show_visual` 800 B,
`nine_app_init` 592 B, `load_slot` 1,072 B, `jpeg_decode` 224 B and `jd_decomp` 48 B; the 8,192 B
input task is not tight. The audio worker uses 544 B.

### Art pipeline and real trade-offs

The pack keeps the **320×180** stage (432×243 source, 16:9), so the package and
the source audit retain the full composition, sprite anchors and face patches.
The C3 has only ~160 KB of link-time DRAM headroom; a 320×180 RGB565 canvas
(115,200 B) plus the stacks and LVGL objects does not fit. The reader therefore
composites into eight bounded 256×24 bands (98,304 B total): the reading/OP use
the first six (256×144 → 320×180), while the title and CG viewer use all eight
(256×192 center-cover → 320×240). The panel upscales 1.25×; the full-screen extra
60 px is covered by title/CG artwork, not a blank strip. This is a real 0.8× downsample of the pack stage (0.59× of the
original 432×243), documented rather than hidden; raising it to 288×162 needs
~19 KB more heap and is not validated. The pack stage can be switched back to
320×180 by changing `NINE_IMAGE_W/H`, the layer scale in `nine_image.c` and the
reference test if more RAM is freed.

Other measured trade-offs:

- JPEG quality 38 at the pack stage; indexed sprites/faces use 192 colours with
  8-row zlib blocks (max block 2,080 B).
- OP: all 493 source frames at 5 fps (full 98.62 s, 1,989,799 B).
- BGM: source 36 kbps Opus (895,865 B) → 16 kHz mono IMA ADPCM (1,518,396 B).
  The 8 kHz variant (759,264 B) was rejected: it is audibly muffled and the
  flash budget has room for 16 kHz. Voice is absent from the source snapshot.

## 8. Build and run

```bash
# ESP-IDF 5.5.3 environment
./tools/validate.sh --static      # repository checks + all host tests
./tools/validate.sh --firmware    # fresh build, merged image, verify, archive
idf.py -p <port> flash monitor    # device flashing is NOT part of this work
```

Assets are regenerated with the pinned source (read-only):

```bash
python tools/nine_pack.py --source <nine-haruiro-rw5> --episode haruiro \
    --out assets/nine/haruiro --jpeg-quality 38 --op-fps 3
python tools/nine_font.py --font <nine-text.otf> --out assets/fonts --source-root main
python tools/nine_font_check.py
python tools/nine_audit.py --source <nine-haruiro-rw5> --pack assets/nine/haruiro
```

`tools/nine_host_tests.sh` is wired into `validate.sh --static`. It runs:

| Test | What it proves |
| --- | --- |
| `test_nine_trace` | 10,674-step trajectory identical to the pinned `engine.js` (text, node/frame, choice labels, permanent digest) |
| `test_nine_synth` | source-vs-C differential for ordered conditions, `all`, filtering fallback, additive effects, preview suppression, 500-hop error |
| `test_nine_vm_state` | save roundtrip, per-byte corruption rejection, permanent-record merge, preview, 24-step history |
| `test_nine_app` | 129 checks of the key state machine with a controllable clock: three-key advance, long OK menu on reading and choice, menu return restores the exact choice, choice save/load stays on CHOICE, long DOWN auto, whole-sentence fast-forward (one tick per sentence, no 90 ms accumulator), release stop, choice stop, long-release suppression, auto+manual advance, page-1 save/load/continue/restart, out-of-range page clamp, corrupt slot, ending and old-save permanent merge, and the second-run n40 title-visual rule |
| `test_nine_badpack` | corrupt story/image packages show visible errors; a bad story never starts, a missing visual freezes advancement |
| `test_boot_contract` | landscape result is required; CJK error page; first frame flushed before backlight; queue/task/button rollback; UI entry points hold the BSP lock |
| `test_nine_audio` | both 16 kHz IMA tracks validate and decode to non-silent PCM |
| `test_nine_image` | 44 Pillow reference composites (JPEG ±5, indexed exact), all 1,063 assets validated, largest allocation 12,288 B |
| `nine_font_check` | 2,425 codepoints present in both emitted fonts; all 2,350 story characters covered |
| `nine_audit` | independent source↔pack reconciliation of every frame, edge, effect, visual layer, chapter, gallery record, OP frame and audio CRC |

The source-trace and synthetic fixtures are committed under
`tests/nine_host/fixtures`; their header carries the script CRC32, so a stale
fixture fails the test instead of silently passing.

## 9. Verification status

| Field | Result |
| --- | --- |
| Build | PASS (ESP-IDF 5.5.3, app 8,062,448 B) |
| Host tests | PASS (`validate.sh --static`, including the host tests above) |
| Firmware gate | PASS (`validate.sh --firmware`: layout, merged image, `verify_firmware.py`, archive) |
| Device tests | **NOT RUN** |
| Unverified | physical buttons, panel orientation/legibility, real free-heap peak, audio loudness/codec at 16 kHz, deep-sleep/wake, battery, long-run stability |

Software success is not a hardware test. No COM port, monitor, reset or flash
operation was performed for this work.

## 10. Attribution and license

- The reader code, packer, audit and tests in this branch are provided under the
  repository's license.
- The story, artwork, music and the source font come from the pinned
  `xuanyide01/9-nine-rw5` snapshot. Their rights remain with the original
  rights holders (Palette / 9-nine- and the Chinese patch authors). They are
  included only for this offline, non-commercial device port; this branch does
  not grant any right to redistribute them separately.
- `nine-text.otf` is used as the subset source and is not copied into the
  firmware; only the generated bitmap fonts are embedded.
- The upstream framework, BSP and TJpgDec retain their own licenses (TJpgDec is
  ChaN's R0.03, see `main/nine/tjpgd/LICENSE.txt`).
