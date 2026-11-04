<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Stella of the End + After Story reader

A landscape visual-novel reader for the AI Passport, ported from the Xiaomi
Watch 5 release `xuanyide01/stella-rw5` (commit `4fa1e7b5534aafbcae0c45b732a05615c3b46587`).
The application baseline is `feature/galgame-framework` at
`3c2e5013a9831c57124ef8f774193f0e06f1f14a`.

## Delivery freeze: software review passed, awaiting device acceptance

- Status: **software review passed; device acceptance pending**. Device is **NOT RUN**.
- App: **8,255,424 bytes**, SHA256
  `5851ceeb9bd7597c935a22d00d0c3667e7d437e25264421be843f65bcc98dff2`.
- Full image: **8,320,960 bytes**, SHA256
  `9ef1b9f94543213139dd71f4222c6de8cc6674be4d9fbce90232ec4c29cef1f1`.
- Same-run verified archive:
  `build/firmware/9ef1b9f94543213139dd71f4222c6de8cc6674be4d9fbce90232ec4c29cef1f1`
  (manifest, ELF/MAP and flash_args included).
- Lead-v2 independent evidence: `build/lead-review-v2/app-probe.log`,
  `source-dialogue.log`, `images-probe.log`, `archive-verify.log`, `static.log`
  (11/11 extra app checks, 932/932 image assets, archive verify PASS).
- Gate-fix evidence: `build/gate-fix-20261102/normal-with-source.log`,
  `normal-no-source.log`, `gate-failure-injection.log`, `probe-gate-result.log`.
  Image compilation failure now produces `Host tests: FAIL`, C regressions
  `PASS=19/20 FAIL=1`, and a non-zero exit; the source-audit SKIP also forces
  overall PARTIAL. Normal run: unittest PASS=93 SKIP=21 FAIL=0; C regressions
  PASS=20/20 FAIL=0; source audit PASS.
- Host baseline is **PARTIAL**, not an all-PASS claim. The 21 skips are
  platform-only unittest skips (audio/BLE/Wi-Fi/demo environments); they are
  counted separately from C regressions and source audit.
- App partition headroom is only **67,648 bytes**; adding content/code requires
  a new partition check. No content was removed.
- No real-device or real NVS-adapter acceptance has run. This turn did not
  rebuild firmware; the bytes/SHA above are the reviewed artifact.

## Content coverage

- Main story: 21 chapters, 9,425 dialogue lines, one ending (chapter 21).
- After Story (`Houbou Kibou no Nikki`): 8 scenes (101-107 and 109), 1,378 lines in the
  normal route plus 15 lines in the bonus scene 109; one confirmation choice at
  107:423 targeting 107:424.
- Legacy epilogue scene 108 (157 lines) is kept in the pack for source
  completeness but is not listed in the chapter browser; it is the duplicate
  epilogue the upstream project keeps for old saves.
- Total: 10,975 dialogue ops, 30 scenes, 39 source story chunks, 4 endings,
  1 choice. Every source `.txt` chunk (including multi-chunk scenes 3, 8, 9,
  10 and 13) is included.
- Art: 932 deduplicated assets covering all 1,066 source IDs (aliases included);
  256 gallery groups / 394 variants; 187 after-story assets. No `as_*` asset is
  dropped as "unreferenced".

## Controls

- Reading: UP advances (hold to fast-forward, release stops); DOWN opens
  history (hold toggles auto-read); OK opens the menu.
- Lists: UP/DOWN move, OK confirms, long OK goes back.
- Choice: OK confirms; the cursor resumes at the chosen target.
- Title: continue, start, after story (requires main clear), chapters, load,
  gallery, settings. After the main ending, the after-story entry unlocks.

## Storage and memory

- Pack: 6,722,596 bytes, embedded in the application image.
- Images: 320x180 JPEG at quality 33 for backgrounds/CGs; portrait JPEG at
  quality 54 with a 1bpp alpha mask and original anchor offsets.
- Decode: TJpgDec callback writes MCU blocks directly into the 320x240 RGB565
  canvas; sprites are blended with their mask during the same callback. No
  full-frame decode buffer, no PSRAM.
- Fonts: 2,775 codepoints, 16px and 20px subsets (4bpp), generated from the
  pack text plus UI literals; `tools/stella_font.py --check` passes.
- App partition: 8,255,424 / 8,323,072 bytes; merged image: 8,320,960 bytes
  (67,648 bytes / 1% app-partition headroom). Static DRAM: 289,456 / 321,296 bytes.
  The same MAP gives an 89,184-byte heap from `_heap_start=0x3fcc6ab0` to
  `SOC_ROM_STACK_START-SOC_ROM_STACK_SIZE=0x3fcdc710`. Named/estimated
  allocations are the 153,600-byte canvas and 40,960-byte LVGL pool (static),
  19,200-byte display buffer, 24,320 bytes of task stacks (main/idle/timer/
  esp_timer/event/LVGL/input), queue/TCB estimates, leaving 43,840 bytes before
  unmeasured SPI/panel/I2C/NVS driver allocations. This is link-time evidence,
  not a device heap measurement. The 40,960-byte LVGL pool is a configuration,
  not a device proof.
- NVS: `stella` namespace keys `cfg`, `unlock`, `s0`-`s6`; autosave uses the
  independent `stella_auto`/`auto` entry. Save format v2 stores an explicit
  `story`/`chapter` mode; old v1 blobs are rejected rather than guessing mode.
  A failed `nvs_commit` is reported as a save failure and keeps the old RAM/NVS
  slot; the firmware never erases NVS automatically.

## Build and validation

```bash
python tools/stella_pack.py --source <stella-rw5 checkout> \
    --out assets/stella/stella_pack.bin --report build/stella_pack_report.json
python tools/stella_font.py --font <NotoSansSC-VF.ttf> \
    --pack assets/stella/stella_pack.bin --out-dir assets/fonts
idf.py build
./tools/validate.sh --static
```

`tools/stella_audit.py <source> assets/stella/stella_pack.bin` independently
checks every source chunk, text/speaker, jump, choice target, ending and
gallery/after-story asset; it resolves asset IDs from the pack META alias map,
not from a build report.
`tools/stella_mem_budget.py` derives the DRAM/heap budget from the same MAP;
`tests/test_stella_app_regressions.c` covers return origins, the real
PRESS→RELEASE→CLICK sequence, NVS transaction failure, CG decode failure and
unlock retry; `tests/test_stella_ui_visibility.c` drives the production
`stella_ui.c` against flag-only LVGL objects to check page/row visibility;
`tests/test_stella_image.c` links the production image layer and project
TJpgDec source for geometry, scaling, JPEG corruption and truncated-mask tests.

## Status

Build, firmware layout and host unit tests pass with 21 platform-only skips
(the host baseline is therefore PARTIAL, not an all-PASS claim). Device tests
are NOT RUN. The
project is offline: no Wi-Fi, BLE, audio, voice or movie playback. Source art
and text remain the property of their original rights holders; this is a
personal study port.
