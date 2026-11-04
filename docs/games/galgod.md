<p align="right">
  <a href="galgod.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# GalGod (Kami ni Naritai Galgame Ryouiki Daijin) for AI Passport

A complete, offline visual-novel port of the GalGod galgame (by the original
studio behind the band port)
to the FoloToy AI Passport, in **landscape 320×240**, based on the
`feature/galgame-framework` baseline (`3c2e5013`).

- Branch: `feature/port-galgod-landscape`
- Content source: `hrk666666/GalGod-MiBand-10` @ `ebe085d2` (script + naming)
- Art source: `mcpotato1123/galgod-band` v2.0.0 (672×480 backgrounds, 854×480 CG,
  143×380 sprites — the band port's own 336×480 files are already cropped and are
  **not** used for art)
- License: application code is GPL-3.0. **The script and all artwork are not
  covered by that license** and remain the property of the original author; this
  port is a non-commercial personal conversion only.

## What is implemented

| Area | Status |
| --- | --- |
| Story | All 5,457 lines / 119,410 characters from all 29 source chunks |
| Chapters | 18 chapter cards, chapter unlock tracking, chapter list screen |
| Branches | 15 choice groups / 53 options, including the one conditional option |
| Quiz + counter | 13 quiz questions with the real integer counter (`bad_end_flag >= 7`) |
| Endings | 4 endings tracked (Bad End, Two People's World, Return to Everyday, The End), afterstory via `true_end_finish` |
| Jumps | 71 jumps kept as runtime gotos, including 3 backward jumps (no expansion, no skipping) |
| Sprites | All **55** faces/expressions kept; three slots (left/center/right), two characters on screen at once where the source does it |
| Backgrounds | 38 files, 36 unique after md5 de-duplication |
| CG / SD | 34 referenced images (15 event CG + 19 SD); 33 unique after content de-duplication; full-frame letterboxed (no side crop); gallery with 5 pages of 8 |
| Reading UI | Typewriter, pagination (19 chars × 4 lines at 16 px, 15 × 3 at 20 px), auto mode, fast-forward (hold UP), history (64 lines), save preview |
| Saves | 3 manual slots + 1 auto slot: position, chapter, variables, unlock bitmap, preview; version + CRC; overwrite asks for confirmation |
| Global unlocks | Separate versioned NVS record (CRC) merged on load, never decreased by an older save, persisted throttled with retry after failure |
| Failure handling | A CG is unlocked only after its JPEG decoded; a decode failure keeps the previous frame cached, shows a toast and retries on the next page turn |
| Settings | Text speed, font size (16/20 px), auto delay, dialogue panel opacity |

Not present: **audio**. The source band port never shipped music, voice, or SFX
nodes; the pack contains no audio section, and the port does not invent one.

## Controls (three keys)

| Screen | UP | DOWN | OK |
| --- | --- | --- | --- |
| Dialogue | short: next page (finishes typing first); **hold: fast-forward** (release stops) | history | menu |
| Choice | previous option | next option | confirm |
| Lists (menu/slots/settings/browse/endings) | previous row | next row | activate; **long press: back** |
| History / chapters | scroll up | scroll down | back |
| Gallery | previous tile (crosses pages) | next tile | open CG; **long press: back** |
| CG viewer | previous unlocked CG | next unlocked CG | back |
| Ending | — | — | back to title |

## Content pipeline

```
python tools/galgod_pack.py --script <band-src/common/scn> \
    --images <upstream-src/common/img> --out main/gal_data/gal_pack.bin \
    --manifest build/pack-manifest.json
python tools/galgod_font.py --pack main/gal_data/gal_pack.bin \
    --font <NotoSansSC-Regular.ttf> --out-dir assets/fonts
python tools/galgod_verify.py main/gal_data/gal_pack.bin
python tools/galgod_audit.py --script <band-src/common/scn> --pack main/gal_data/gal_pack.bin
```

`galgod_verify.py` checks every cross-reference inside the pack.
`galgod_audit.py` deliberately does **not** import the converter: it re-parses all
29 source chunks, builds a source interpreter and a pack interpreter, and then
compares dialogue order *and speaker names*, every option's witness landing
target, all 71 jumps (including the cross-chunk sprite-clear flag), the sprite
slot/alias mapping and the chapter order.

### Pack layout (`GALPK001`, little endian)

| Section | Content |
| --- | --- |
| TEXT | UTF-8 blob, every string NUL-terminated and de-duplicated |
| NAME / LINE / CHOICE / OPTION / ACTION / COND | Fixed-size records |
| EVENT | `{kind, a, b, c, d}` — LINE, BG, CG, SPRITE, CHOICE, GOTO, VAR, CHAPTER, ENDING, END, UNLOCK_END |
| IMAGE | 36-byte records: two JPEG halves + optional 1-bpp masks |
| CHAPTER / ENDING / GALLERY / TITLE / META | Tables and provenance |

Images: backgrounds are cover-cropped to 320×240 and split into a 320×150 top
JPEG plus a **160×45** strip JPEG drawn at 2× behind the dialogue panel. CGs are
scaled to full width (320×180) and letterboxed, so the complete frame is kept.
Sprites are scaled to 240 px height (90 px wide), split the same way and carry a
1-bpp alpha mask. All JPEGs are baseline 4:2:0, decoded with `esp_jpeg` (TJpgDec)
into caller-provided buffers — no whole-frame RGBA decode, no PSRAM.

## Firmware layout and budget

| Item | Bytes |
| --- | ---: |
| App binary | 3,284,048 |
| `factory` partition | 8,323,072 (39.4 % used) |
| Merged image (`full.bin`) | 3,349,584 |
| Resource pack | 1,767,140 (124 images: 1,306,052) |
| Static draw buffers | 96,000 + 14,400 + 28,800 |
| DRAM at link time | 227,774 / 321,296 (70.9 %), ~93 KB left for the heap |

## RAM budget (estimate from the same build's MAP; no device measurement yet)

| Item | Bytes | Source |
| --- | ---: | --- |
| DRAM region `dram0_0_seg` | 321,296 | MAP: origin `0x3fc80000`, length `0x4e710` |
| Static `.data` + `.bss` + IRAM | 227,774 | `idf.py size` |
| Heap span at link time (conservative) | 93,088 | MAP: `_heap_start = 0x3fcb7b70`, region end `0x3fcce710` |
| BSP flush buffer 240×24×2 | 11,520 | heap, allocated by `bsp_lvgl_init()` |
| LVGL task stack | 7,168 | `ESP_LVGL_PORT_INIT_CONFIG()` default |
| Input task stack | 4,096 | `main.c` |
| `esp_timer` task stack | 3,584 | `CONFIG_ESP_TIMER_TASK_STACK_SIZE` |
| System event task stack | 2,304 | `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` |
| LVGL objects + label text (estimate) | 15,000–25,000 | pages/lists/history rows |
| **Estimated free after boot** | **≈ 35–45 KB** | arithmetic only |
| Real high-water mark | **NOT RUN** | needs on-device `heap_caps_get_free_size` |

The 93,088 B figure is the conservative span this link leaves between `_heap_start`
and the end of `dram0_0_seg`; it is a budgeting baseline, not a measured heap and
not the only region the allocator may serve from (driver allocations, reclaimable
buffers and fragmentation are decided at runtime). The nominal ESP32-C3 top
(`0x3fce0000`, ~165 KB from `_heap_start`) is outside this link's region, so do
not budget against 400 KB, 165 KB or the linker remainder as if it were free heap.

## Build and validation

```bash
# ESP-IDF 5.5.3
# Windows (maintained entry points, not ad-hoc build/ scripts)
powershell -File tools/idf.ps1 -B build build
powershell -File tools/gate-firmware.ps1        # build + merge + verify + archive

# Host gate (this machine has no system cc; tools/cc-host.sh wraps Zig clang)
CC="$(pwd)/tools/cc-host.sh" GALGOD_SOURCE=<band-src/common/scn> ./tools/validate.sh --static
```

On Windows the firmware half of `validate.sh` cannot run inside Git Bash (ESP-IDF
refuses MSYS), so `tools/gate-firmware.ps1` performs the same steps natively:
clean build in a fresh `build/firmware-gate/run-*` directory, merge,
`verify_firmware.py`, `archive_firmware.py`. Nothing is deleted, so every run
leaves its logs and artifacts behind.

Host tests cover: pagination (`test_gal_text`), pack structure (`test_gal_pack`),
VM semantics (`test_gal_model`), all real routes through the UI entry point
(`test_gal_routes`: 405 states / 329 choice stops / 53 options / ending mask 0xF),
image composition with JPEG/LVGL stubs (`test_gal_image`), save/settings with an
in-memory NVS and fault injection (`test_gal_store`), corrupt packs
(`test_gal_badpack`) and the real app+UI key flow (`test_gal_app`: menu from a
pending choice, save/load at a choice, toast visibility, history rows,
fast-forward cancellation).

## Unverified / known limits

- No on-device run yet (flashing is out of scope for this task); display timing,
  JPEG decode latency and key feel still need device acceptance.
- The dialogue panel covers the bottom 90 px of the frame; CGs are letterboxed
  rather than cropped, backgrounds are cover-cropped (95.2 % of the frame kept).
- Two sprites are supported at the left/center/right slots; the source never uses
  three at once.
- The 2 sprites and 1 background that the script never references are still packed
  (completeness), so the pack reports 3 unused images.

## Delivery status: frozen (software review passed, awaiting device acceptance)

**Status: software review passed; device acceptance pending (Device tests NOT RUN).**

| Item | Value |
| --- | --- |
| App binary | 3,284,048 B — `cc8e350d300109c39551f8b975a14373a828e1cf869d43587d6b47b7a57486d9` |
| Merged image `full.bin` | 3,349,584 B — `f2fe13166b9bc86321bb72036c46b63c46edce776f86a8533294c0532706ef0e` |
| ELF / MAP | `build/firmware-gate/run-20261102-040848/FoloToy-AI-Passport.{elf,map}` |
| Archive (same build) | `build/firmware/f2fe13166b9bc86321bb72036c46b63c46edce776f86a8533294c0532706ef0e/` |
| Lead review logs | `build/lead-review-r3/probe_app.log`, `probe_image.log`, `archive-verify.log` |

This version is frozen: no further changes to production code, configuration or
assets. "Software review passed" does not mean on-device heap, display timing or
NVS behaviour is verified; those remain open device-acceptance items. Earlier
archives (`3a2bc8a9…`, `8fe3b642…`, `bc92e302…`, `b48f4633…`, `98170bb3…`) and all
review probes and logs are kept.
