<p align="right">
  <a href="galgame-eden.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# eden* Landscape Port — Implementation Notes

This branch (`feature/port-eden-landscape`) turns the galgame framework into a complete, offline
`eden*` reader on the 320x240 landscape panel. It keeps the framework's pagination logic
(`main/gal/gal_text.c`) and display settings (`main/gal/gal_settings.c`) and replaces everything that
is title-specific: content, script execution, picture decoding, persistence, and the UI.

## 1. Scope

| Item | Value |
| --- | --- |
| Source | `xuanyide01/eden-rw5` (`src/common/{assets.js,story,reports}`, `src/common/images/*.jpg`) |
| Chapters / scenes | 21 / 21 |
| Source chunks | 28 files over 21 scenes; scenes 4/5/6/7/11/13/14 have a `1.txt` continuation |
| Script ops | 8,931 (8,860 `text`, 25 `if`, 5 `set`, 13 `jumpScene`, 2 `goto`, 5 `end`) |
| Composed pictures | 2,178 frames from 2,667 JPEGs (1,115 bases + 1,594 patch parts) |
| Picture geometry | 264x165 (16:10), shown 1:1; source frames were 1024x640 scaled to 0.258x by the source port |
| CG gallery | 295 groups, paged 6 per page, unlock bits per group (320 bits reserved) |
| Text | 490,042 UTF-8 bytes in the pack; 2,579 distinct code points in dialogue |
| Player choices | none — the original is a kinetic novel; the choice page exists but no op triggers it |

## 2. Layers

```
main/eden_app.c    state machine: pages, key semantics, history, saves, unlocks, auto/fast-forward
main/eden_ui.c     landscape UI: 15 pages, row lists, typewriter, notice, modal
main/eden_image.c  picture compositing: JPEG base into the canvas, JPEG patch into a strip, blit
main/eden_script.c pure op engine (text/set/if/goto/jumpScene/end) — host-testable
main/eden_pack.c   read-only pack parser — host-testable
main/eden_store.c  NVS: 3 save slots, reading progress, gallery unlocks (versioned blobs)
main/gal/gal_text.c      reused: pagination
main/gal/gal_settings.c  reused: text speed / font tier / auto-play / panel transparency
```

`main/gal/gal_app.c`, `gal_ui.c`, `gal_content.c`, `gal_save.c`, `gal_progress.c` and `gal_unlock.c`
remain in the tree (they are the framework skeleton) but are **not** compiled by this application; the
build list lives in `main/CMakeLists.txt`.

## 3. Pack format (`EDENPK01`)

One little-endian binary, mapped from flash; the firmware never loads it into RAM.

| Section | Contents |
| --- | --- |
| `TEXT` / `TXIDX` | NUL-terminated UTF-8 strings + `u32` offset table |
| `OPS` | 12-byte records `{kind u8, a u8, b u16, c u32, d u32}` |
| `SCENE` | `u32[scene+1]` op range per scene id (1-based) |
| `IMAGE` | 16-byte records `{off u32, len u32, w u16, h u16, content_w u16, content_h u16}` |
| `FRAME` | 6-byte records `{base u16, patch_first u16, patch_count u8, reserved u8}` |
| `PATCH` | 6-byte records `{image u16, x u16, y u16}` |
| `CHAP` | 8-byte records `{scene u16, name u16, op u16, reserved u16}` |
| `GAL` | 6-byte records `{frame u16, group u16, label u16}` |
| `BLOB` | JPEG payloads |

Stored JPEGs are padded to multiples of 8 in both axes because TJpgDec emits whole MCUs; the parser
reports both the stored size (buffer size) and the content size (blit size). Patches wider than 104px
are split into columns by the packer so the decoder scratch strip stays narrow.

## 4. Memory budget

| Consumer | Bytes |
| --- | --- |
| Picture canvas (264x168 RGB565) | 88,704 |
| Patch scratch strip (104x168 RGB565) | 34,944 |
| LVGL heap (`CONFIG_LV_MEM_SIZE_KILOBYTES=64`) | 65,536 |
| LVGL draw buffer (320x40 RGB565) | 25,600 |
| JPEG working buffer | 4,096 |

The canvas is decoded into directly by `esp_jpeg`; patches decode into the scratch strip and are
blitted row by row. There is no full-frame RGBA buffer, no per-picture cache, and no pre-composited
expression matrix.

## 5. Flash budget (this build)

| Item | Bytes |
| --- | --- |
| Resource pack (JPEG q27) | 6,189,252 |
| Title logo (RGB565A8) | 32,670 |
| Fonts 16px + 20px (4bpp, 2,681 code points) | 871,958 (322,521+5,172+21,456 / 493,767+5,172+21,456) |
| Application image | **7,783,760** (app partition 8,323,072, 539,312 free) |
| Merged image | **7,849,296** (limit 8,388,608) |

JPEG quality is q27, not q30: with the complete script the pack has to hold 2,667 images, and the
measured totals are q25 5.71 MB / q27 6.19 MB / q28 6.59 MB / q30 7.80 MB — q30 alone would need
1.55 MB more than the partition has. Measured second-generation quality against the source tiles:
PSNR 30.69 dB mean (26.91 dB worst), MAD 5.37 over 74 sampled frames (the source is itself a q56,
0.258x re-encode, so this is a second lossy generation).

## 5b. Internal RAM budget (no PSRAM)

Numbers below come from this build's MAP (`_bss_start=0x3fc92ad0`, `_bss_end=0x3fcc5718`,
`_heap_start=0x3fcc5720`) and `sdkconfig`. They are **static accounting**, not a measured
high-water mark: the device-side watermark is still unverified.

| Consumer | Bytes | Where |
| --- | --- | --- |
| Picture canvas 264x168 RGB565 | 88,704 | `.bss` (`s_canvas_pixels`) |
| Patch scratch strip 104x168 RGB565 | 34,944 | `.bss` (`s_patch_pixels`) |
| JPEG working buffer | 4,096 | `.bss` (`s_work_buf`) |
| LVGL heap (`CONFIG_LV_MEM_SIZE=65536`) | 65,536 | `.bss` |
| Other static BSS (IDF, LVGL lib, drivers, app state) | 14,664 | `.bss` |
| **Total `.bss`** | **207,944** | MAP |
| LVGL draw buffer 240x40 RGB565 (port allocates at init) | 19,200 | heap |
| Input task stack | 4,096 | heap |
| LVGL port task stack (`ESP_LVGL_PORT_INIT_CONFIG`) | 7,168 | heap |
| main / timer / idle / event task stacks | ~10,200 | heap |
| JPEG decode transient | ~0 extra | reuses the 4 KB work buffer |

Tail of internal SRAM after `.bss`: `0x3FCE0000 - 0x3FCC5720 = 108,768 B` (optimistic upper bound:
it assumes the whole region between `.bss_end` and the top of DRAM is usable). Subtracting the heap
allocations above leaves roughly **68 KB** for LVGL objects, Wi-Fi-less networking buffers, and
stack guard slack — enough for the current UI (≈120 objects) but with no headroom for a second
full-frame buffer. Do not treat "400 KB SRAM" as the budget: `.bss` plus those heap items is the
real constraint.

## 6. Persistence

| Namespace | Key | Contents |
| --- | --- | --- |
| `edensave` | `slots` | 3 slots: scene, op, page, chapter, story variables, preview text |
| `edenprog` | `pos` | automatic reading position (throttled to one write per 2 s) |
| `edenulck` | `bits` | gallery groups (320 bits), chapters (21 bits), endings |
| `galcfg` | `cfg` | framework display settings (text speed, font tier, auto-play, transparency) |

Every blob starts with a `u16` format version; an unknown version is treated as "no record" instead of
being parsed partially.

## 7. Controls

| Page | Short press | Long press |
| --- | --- | --- |
| Splash | OK enters the title | — |
| Title / any list | up/down moves, OK confirms | long OK goes back |
| Dialogue | up advances (completes typing, then pages), down opens history, OK opens the menu | long up fast-forwards until released |
| Settings | up/down moves, OK cycles the value | long OK goes back |
| Save slot | OK saves (second OK when the slot is occupied) | long OK goes back |
| Load slot | OK loads | long OK goes back |

## 8. Validation

```bash
python tools/eden_pack.py --source <eden-rw5> --out main/eden_data/eden_pack.bin   # regenerate
python3 tests/test_eden_artifacts.py                                               # pack/script/decode gate
./tools/validate.sh --static      # repository checks + host tests
./tools/validate.sh --firmware    # ESP-IDF build + merged-image verification
```

`tests/test_eden_artifacts.py` re-reads the generated pack with an independent parser and checks the
script (every op, every jump target, a full 8,860-line playthrough), picture geometry, 18 sampled
frame decodes against the source tiles, font coverage of every code point, pagination capacity, and
the flash budget. `tests/test_eden_pack.c` is the same contract in C for hosts that have a C compiler.

`tests/test_eden_source_audit.py` is the source-equivalence gate: it re-walks every chunk of the
source tree with its own reader (never the packer's `Source` class) and compares all 8,931 ops
one by one — kind, text, speaker, frame id, and jump targets translated from scene-relative to
absolute. It also pins the seven multi-chunk scenes: the seam op must be readable and every
target that lands in the second chunk must resolve to the same absolute op.

`tests/test_eden_app.c` compiles the **production** `eden_app.c` / `eden_ui.c` / `eden_store_nvs.c`
against `tests/app_stubs` (LVGL object model, NVS, JPEG, BSP) and drives real key events. It pins the
paths that a data-only audit cannot see:

- page origin: title→settings and title→load return to the title, in-game menu→settings returns to
  the menu (a page stack, not a hardcoded target);
- overwrite failure: with `nvs_commit` injected to fail, the RAM slot keeps the old preview and the
  old save is still loadable;
- decode failure: a failing patch neither unlocks the CG, nor unlocks it on a re-render of the same
  frame; once the fault is cleared a successful decode unlocks it;
- fast forward: a short press advances exactly one line, and long-press followed by another key,
  a page change, or the release stops it;
- LVGL locking: no UI write happens while `bsp_lvgl_lock()` is not held;
- unlock persistence: a failed unlock write backs off instead of hammering flash, retries on every
  page (title / gallery / CG list / ending, where there is no dialogue line at all), and lands once
  the store recovers; the reload path is verified by reading the record back.
- CG navigation: moving the selection in the list does not push the same page, closing the full-screen
  CG pops back to the list, and the list itself pops to the gallery with one long press.

The NVS stand-in now stages `nvs_set_blob` writes and only applies them on a successful
`nvs_commit`, so "commit failed" really means "flash still holds the old record" inside the harness.
That is still a **stand-in**: it proves the app/store contract (RAM keeps the old slot, nothing is
reported as saved, a reload sees the old record), not real-flash durability across a power cut.

`tests/test_eden_image.c` runs the real `eden_image.c` against the JPEG stub and checks the failure
semantics: patch failure falls back to the complete base frame, base failure clears the canvas, and
out-of-bounds patches / mismatched JPEG headers are rejected without touching the current frame.

Host C tests run through `tools/cc-zig.sh` when no system compiler exists (`pip install ziglang`);
the gate fails outright when no compiler is available. The gate reports `passed/failed/skipped`
counts and prints `Host tests: PARTIAL` whenever anything was skipped — it never prints PASS while
a group was skipped.

## 9. Known limits

- No player choices in the source: the choice page is present but unreachable.
- No music: the source repository ships track names only, no audio payloads.
- CG thumbnails are replaced by a paged list plus a full-screen viewer (six decoded thumbnails would
  cost about 64 KB of PSRAM this board does not have).
- Camera panning (`reports/camera-commands.json`, 24 commands) is stored as discrete camera frames by
  the source port; the reader shows the resulting frames and does not animate the pan.
- On-device rendering, input timing and NVS behaviour are **not** verified yet; see the delivery
  report for the separate Build / Host tests / Device tests / Unverified fields.

## 10. Delivery status — software review passed, pending device acceptance

This revision is **frozen** for on-device acceptance. No production code, pack, or firmware was
rebuilt for this note; the hashes below are the reviewed build.

| Item | Value |
| --- | --- |
| Application image | 7,783,760 B, SHA256 `5b39da679672375120e5c2f4351f12144989820b903ac795afc72e2e2947ff9b` |
| Merged image | 7,849,296 B, SHA256 `a8922b1b8a105bafd8425b608028544cbaf0607820e0fc0180ab25f1004d179d` |
| Same-run archive | `build/firmware/a8922b1b8a105bafd8425b608028544cbaf0607820e0fc0180ab25f1004d179d/` |

Lead-side evidence (in this project, unchanged by the worker):

- `build/lead-review-v4/static.log` — independent `tools/validate.sh --static`: 42 passed /
  0 failed / 2 skipped, Host tests PARTIAL.
- `build/lead-review-v4/archive-verify.log` — independent archive verify, PASS.
- `build/lead-review-v4/probe_app.c` + `probe_app.log` — assertion-corrected C probe over the real
  app/UI/image/store/adapter path: maintained 105 + 11 added = 116 checks, 0 failures.

Two earlier probe assertions from `build/lead-review-v3/` were withdrawn as faulty (ASCII `????`
against a full-width `？？？` label, and an assertion that contradicted its own "return to gallery"
message); the original evidence is preserved and the erratum is recorded in the Lead's review log.

**This round did not repeat a clean-directory rebuild**: the accepted hashes come from the reviewed
`idf.py -B build` build plus `merge-bin` / `verify_firmware.py` / `archive_firmware.py`. The earlier
clean-directory rebuild equality (round 2) is the reproducibility evidence on record.

Device tests: **NOT RUN**. Landscape orientation, real pagination/glyph coverage at both font tiers,
BSP key timing, on-device RAM watermark, JPEG decode cost, NVS durability across power loss,
deep-sleep wake, and battery display all remain to be accepted on hardware. The static RAM accounting
in §5b is not a pass result.
