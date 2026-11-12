<p align="right">
  <a href="galgame-anemoi.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# anemoi — landscape partial-content prototype

This branch runs the public/common part of the Redmi Watch 5 port of *anemoi*
(`xuanyide01/anemoi-rw5`, pinned snapshot
`3e27714c50b860c674a86df90e3df3e4cf6dd254`) on the AI Passport 320×240
landscape panel. Boot goes straight into this work's title screen; reading,
choices, the request board, saves, gallery and the chapter list use the work's
own UI.

**Status.** This is a technical prototype covering the existing public/common
content, not a complete port of the whole work. The candidate pack has **80
scenes / 61,257 instructions**. It keeps the previously shipped 71-scene
prototype and adds only the ranges required by already-reachable flow. Heroine
route chapters, the after-story books and the 070/071/072/073 books are still
not imported; out-of-scope jumps show the volume-boundary page. The prototype
is evidence for the interpreter, renderer, storage and the checked common
routes; it is not evidence that the whole work cannot be ported.

## 1. Scope manifest

`main/anemoi_data/anemoi_scope.json` is a reviewed, data-only source-range
manifest. The packer validates the pinned source commit, scene existence and
range bounds, and rejects overlapping ranges. It cannot add text, flags or a
different source tree.

| Range | Why it is required |
| --- | --- |
| scenes 2, 3, ..., 71-scene base | Previously shipped public/common prototype ranges; no new text |
| scene 26 full `[0,len)` | scene 2 op145/153 calls entry 0/10; the shipped range started at 57 and did not contain entry 0 |
| scene 27 full `[0,len)` | scene 2 op279/287 calls entry 0/10; the shipped range started at 44 |
| scene 28 full `[0,len)` | scenes 19/20/21 call entry 0; the shipped range started at 143 (entry 10) |
| scene 90 full `[0,len)` | scene 2 op1030 calls entry 0; the scene was absent from the shipped pack |
| scenes 167..174 full | scenes 134/135/136 call all eight play scenes at entry 0 |
| scenes 125, 131 `[0,160)` / `[0,149)` | Existing route-chapter heads only; the full books are not imported |
| scenes 133..138 `[1,len)` | Existing Koyomi entry ranges; a13 increments at 134:622, 135:442, 136:483 remain packed |
| scene 175 `[128,476)` | Existing Rikka entry range; not expanded into an unported route |

Explicit exclusions: scenes 205 (070 Spica zero), 206 (071 Dousei-hen) and 262
(073 Silver-hen), plus the 193 unwrapped route chapters. The per-scene reasons
are recorded in the manifest's `reasons` map.

Exact Japanese source labels are kept in the Simplified Chinese peer and the
audit report; this default English file uses romanized names so the repository
language check can enforce English prose.

## 2. Capacity (actual build, not an estimate)

| Quantity | Bytes | Notes |
| --- | ---: | --- |
| `factory` app partition | 8,323,072 | `partitions.csv`, 0x7f0000 |
| Reviewed app image | 8,049,104 | `build-anemoi-A-q24/FoloToy-AI-Passport.bin` |
| Merged full image | 8,114,640 | `build-anemoi-A-q24/FoloToy-AI-Passport-full.bin` |
| Non-pack part of the app | 1,094,302 | app image minus the 6,954,802-byte pack |
| Resource ceiling for a pack | 7,228,770 | 8,323,072 − 1,094,302 |
| Candidate pack | 6,954,802 | sha256 `634c01838d628f5ea2817c0c08c099beb8795f5134659ce186566adb5a770eef` |
| Remaining app/flash headroom | 273,968 | 3% of the app partition |

The merged full image is 8,114,640 bytes, so the 8 MB flash still has 273,968
bytes free. The full image is app image + 65,536 bytes of bootloader/partition
table; the app partition and the flash therefore give the same pack ceiling.

## 3. Encoding candidates and visual comparison

All three candidates use the reviewed 80-scene manifest and the same 16px font.

| Candidate | Layout / palette / JPEG | Pack bytes | Result |
| --- | --- | ---: | --- |
| A-q30 | 0.52 / 32 / q30 | 7,527,695 | Does not fit: 298,925 B over the 7,228,770 B ceiling |
| **A-q24 (selected)** | **0.52 / 32 / q24** | **6,954,802** | Fits with 273,968 B headroom; keeps the shipped layout and palette |
| B | 0.46 / 16 / q25 | 6,470,206 | Fits, but only as a quality comparison; not selected |

A-q24 changes only the scene JPEG quality. It does not drop dialogue, choices,
images or expressions, and it does not change the portrait layout/palette.
The resource metric is the binary SEC_IMAGE row count: release 2,000 ->
candidate 2,065 (+65); kind 0 scene rows +13 (322 -> 335), kind 1 body/face
rows +52 (1,676 -> 1,728), kind 2 solid rows unchanged (2). The 65 new
dialogue glyphs are a separate font metric. Source image keys are not 1:1 with
binary rows, so source-key counts are not used as row counts here.

For reference, the B candidate was also built: app 7,564,512 B / full
7,630,048 B, pack sha256
`5320c42c2b91dbbc376189b0fb1eddf3022599f5afbe25e997af5e93d2206767`.
It fits, but its layout/palette/JPEG changes are a larger visual regression, so
it remains a comparison candidate only.

Audit-only A/B renders of the same background, portrait, face crop and dialogue
page are in `audit/repairs/anemoi-screens/`. PSNR against A-q30: A-q24 45.31
dB, B 37.67 dB. The renderer mirrors the current game canvas; the production C
path draws the body layer only, so the face crop is a separate visual check.

## 4. Font

The app compiles only `gal_cjk_16.c`; 20px is not provided in this volume.
The generated 16px subset has **3,079** glyphs and covers all pack text and UI
literals, including the 65 new dialogue characters. Font provenance:

- `assets/fonts/gal_cjk_16.c`: 2,739,863 B, sha256
  `f8553da2f50e00ed653d7184bd8b0f33b056c3ae251fc632bae6344c998b0e5b`
- `assets/fonts/gal_cjk_symbols.txt`: 9,200 B, sha256
  `7699760f7cba84074d8c58205aca76327b6389ccba78e68c3a801fd2ebef0e18`
- Source font: Noto Sans CJK SC Regular OTF, sha256
  `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`
  (not committed)

Check with:

```text
python3 tools/gal_font.py --check --sizes 16 --out-dir assets/fonts
```

## 5. Normal-route evidence

These are normal NewGame/choice runs, not chapter starts or flag injections.
Saves/progress blobs are only used where explicitly noted.

- **Spica entry:** the scene37 trace now reaches `SCENE 28 cursor=21950`
  (entry 0), then returns to 21; the old scene28 entry0 blocker is gone. The run
  still ends at the unported Aino boundary later in the flow, so this is entry
  coverage, not a full Spica ending.
- **Koyomi 8 play items:** `anemoi-134-natural-walk0..7.log` exercise all eight
  choices at scene134 cursor 43186; item 7 also takes scene174 cursor 50927.
  The paths continue through the packed 167..174 ranges and end at the Koyomi
  route boundary. The a13 increment ops (134:622, 135:442, 136:483) are inside
  the packed ranges.
- **Rikka boundary:** `anemoi-normal-rikka-A-q24.log` reaches the Rikka
  qualification boundary using normal board choices.
- **82 normal menu items:** the reviewed release-pack witness plans were
  translated by source-op offset to the 80-scene pack (the added entry-0 ranges
  shift absolute cursors), then replayed with the production C interpreter.
  Choice labels match the original witness logs, all 82 target items are
  consumed, and every plan reaches `PLAN_CONSUMED target_hit=1`. Some old
  plans ended at a boundary that the new pack correctly continues past, so this
  is item coverage, not an unchanged-terminal claim.

## 6. Yang task gate: SOURCE_DEADLOCK

The Yang task-3 board block is gated in scene393 op26 by
`$$check_character_bbs_disp(3)`. For `a12==2`, helper op128 requires
`a2<=15 && a3==1 && a17>=2`. In the source:

- `a17` has exactly one positive writer: scene93 op904 `a17 += 1` (task 0003
  completion);
- `a300` has exactly one writer: scene93 op87 `a300 += 1`;
- `a12` is incremented by scene91..98; scene93 op903 is the task-3 completion
  increment;
- before task 0003 the normal state is `a12=2, a17=0, a300=0`.

Scene393's `a12==2` branch sets the Awayuki task-0003 request, but the helper cannot
return true because `a17>=2` is only written by completing that same task.
The `a17>=1` overlay is not a fix either: pre-task `a17=0` still evaluates
false, and the overlay was rejected in
`audit/repairs/anemoi-overlay-rejection.json`. No compatibility overlay is
applied; `source_overlay` is null in the pack manifest. Yang task 3+ is
therefore marked **SOURCE_DEADLOCK**, not PASS. This is a pinned-source
limitation, not a missing pack range.

## 7. Reproducibility and gates

The packer is invoked with an explicit manifest, not the old heuristic common
closure:

```text
python3 tools/anemoi_pack.py --source <pinned-anemoi-rw5> \
  --scope-manifest main/anemoi_data/anemoi_scope.json \
  --layout-scale 0.52 --palette-colors 32 --jpeg-quality 24
```

The source-controlled `main/anemoi_data/anemoi_pack.json` records the pack
hash/size, source commit, scope-manifest hash, packer arguments, font hashes,
capacity, per-section table, `image_row_kinds` and a `scope_notes` statement
that `included_scenes`/`scope_manifest` are authoritative and source image
keys are not 1:1 with binary rows. `main/anemoi_data/anemoi_pack.bin` remains
a release artifact (gitignored). The optional `--source-overlay` path is an
explicit, hash-recorded compatibility mechanism; it is **unused** here.

### Binary audit

`tools/anemoi_binary_audit.py` reads the source story JSON, the scope
manifest and the binary directly; it does not import the packer's
`Source`/`load_scope_manifest`. It requires the pack scene set to equal the
scope scene set, per-scene exact range lengths and source-op order, and
`checked == scope total == pack op count`. It also checks every static
`if`/`goto`/`call` relocation and every static cross-scene entry reference
against the entry table. The only allowed special mappings are the 2
chapter-head out-of-range returns and the 1 `input_update` -> `ARRIVE` poll;
3 dynamic jump entries are counted but are not claimed as statically resolved.

The current pack reports 80 scenes, 61,257/61,257 ops, 15,620 static branch
targets and 333 static cross-scene entry references. The 333 is a
reference-count check, not every row of the 641-row ENTRY table.
`tests/test_anemoi_binary_audit.py` adds six negatives: a one-op-short range,
a missing/extra pack scene, a changed same-kind jump target, a wrong static
cross-scene entry value, a wrong scope source commit and an out-of-bounds scope
range. Each must fail with the corresponding coverage/target/scope error.

Gate results from this candidate (full output in `audit/repairs/`):

| Command | Result | Log |
| --- | --- | --- |
| `ANEMOI_SOURCE=... bash tools/validate.sh --static` | PASS | `audit/repairs/anemoi-full-static-final.log` |
| `ANEMOI_SOURCE=... python3 tests/test_anemoi_scope.py` | PASS | `audit/repairs/anemoi-scope-final.log` |
| `python3 tools/gal_font.py --check --sizes 16` | PASS | `build/anemoi-gate-static/font.log` |
| `ANEMOI_SOURCE=... ANEMOI_RELEASE_PACK=... bash tools/anemoi_gate.sh build/anemoi-gate-final4` | PASS (26/0/0) | `audit/repairs/anemoi-gate-final4.log` |
| `python3 tools/anemoi_binary_audit.py --source ... --baseline-pack ...` | PASS (61,257/61,257; 15,620 targets; 333 refs) | `audit/repairs/anemoi-binary-audit.log` |
| `python3 tests/test_anemoi_binary_audit.py` | PASS (positive + 6 negatives) | `audit/repairs/anemoi-binary-audit-negatives.log` |
| `idf.py -B build-anemoi-A-q24 build` | PASS | `audit/repairs/anemoi-idf-final-build2.log` |
| `idf.py -B build-anemoi-A-q24 merge-bin` | PASS | `audit/repairs/anemoi-merge-final.log` |
| `python3 tools/verify_firmware.py build-anemoi-A-q24` | PASS | `audit/repairs/anemoi-verify-final.log` |
| `python3 tools/archive_firmware.py create build-anemoi-A-q24 --archive-root build/firmware` | PASS | `audit/repairs/anemoi-archive-create.log` |
| `python3 tools/archive_firmware.py verify build/firmware/4000d07b...` | PASS | `audit/repairs/anemoi-archive-verify.log` |
| 82-item translated production-C replay | PASS | `audit/repairs/anemoi-normal-items-translated/receipt.json` |

Device tests are **NOT RUN**: no USB/flash operation was performed. Runtime
free/largest block and frame time are not measured on hardware.

The shared static chain now invokes `tools/anemoi_gate.sh` itself. An unset
`ANEMOI_SOURCE` or a missing pack is a hard failure, not a skip. On Darwin the
host-only demo runtime tests use the native `-dead_strip` linker option instead
of GNU `--gc-sections`.

## 8. RAM and heap budget (final MAP)

Flash headroom is not RAM. The final ESP32-C3 MAP/ELF reports:

- `.dram0.data` 7,300 B; `.dram0.bss` 240,968 B.
- Static bss includes the canvas 115,200 B, LVGL pool 65,536 B, application
  state 32,832 B, block buffer 8,224 B, tinfl state 10,992 B, and
  palette/row/LVGL-global storage 2,208 B; the remaining bss is about 5,976 B.
- `_data_start = 0x3fc90e00`, `_heap_start = 0x3fccd7e0`; the reserved
  static-data region is 248,288 B.
- Heap region A (`_heap_start..APP_USABLE_DRAM_END 0x3fcdc710`) is 61,232 B.
- Heap region B (`0x3fcdc710..SOC_DIRAM_DRAM_HIGH 0x3fce0000`) is 14,576 B;
  the ROM reserved tail is merged only after the scheduler starts. Heap at
  boot is therefore 75,808 B (A+B).
- Named dynamic reservations: LVGL draw buffer 19,200 B (240×40×2), LVGL task
  stack 7,168 B, input task 6,144 B (4,096 B fallback on allocation failure),
  main task 8,192 B, timer task 3,584 B, event task 2,304 B = 46,592 B.
- Source anchors: draw buffer `components/bsp/src/bsp_display_lvgl.c:97`;
  LVGL task `managed_components/espressif__esp_lvgl_port/include/esp_lvgl_port.h:68`;
  input task `main/main.c:283-288`; main stack `sdkconfig.defaults:38`;
  timer/event `build-anemoi-A-q24/config/sdkconfig.h:488,512`;
  `APP_USABLE_DRAM_END` `components/heap/port/esp32c3/memory_layout.c`;
  ROM stack `components/soc/esp32c3/include/soc/soc.h:205-206`.
- Remaining before LVGL objects, NVS, drivers and fragmentation: 14,640 B in
  region A; 29,216 B in A+B. This is a static link budget, not a runtime
  measurement; device free/largest block and frame time are NOT RUN.

## 9. Known gaps

1. Yang task 3+ is a source deadlock; no natural completion is claimed.
2. Scenes 205/206/262 and the 193 route chapters are outside the shipped
   prototype; the boundary page is intentional.
3. Spica/Koyomi/Rikka evidence covers the packed qualification/play ranges and
   route boundaries, not full endings of unimported books.
4. The 20px font variant is not compiled in this volume.
5. Device acceptance is NOT RUN.
