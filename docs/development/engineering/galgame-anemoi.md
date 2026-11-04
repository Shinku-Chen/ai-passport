<p align="right">
  <a href="galgame-anemoi.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# anemoi — Landscape Port (partial-content technical prototype)

This branch puts part of the Redmi Watch 5 port of *anemoi* (`xuanyide01/anemoi-rw5`, snapshot
`3e27714c50b860c674a86df90e3df3e4cf6dd254`) on the AI Passport 320×240 landscape panel. Boot goes
straight into the work's title screen; reading, choices, request board, saves, gallery and chapter
list are this work's own UI.

**Status — read this first.** This firmware is a **technical prototype covering part of the
source**, not a completed port of the whole work. It contains 71 of 264 source scenes (764,847 of
3,088,939 dialogue UTF-8 bytes, 24.8 %). Heroine route chapters and the after-story arcs are
**not** included; walking into them shows a volume-boundary screen that names the missing
chapter. The prototype is useful because it exercises the real interpreter, the layered
body+face renderer, JPEG/BLOCK decoding and the save/unlock paths on this hardware — it is not
evidence that "the whole work cannot be ported".

The reviewed artifact is the firmware archived under
`build/firmware/bee2d1a2e32780a97511e3acba1dac4c36611619838bef86d2ac8f2c5d3a04a6`
(app 8,191,504 B / full 8,257,040 B). A volume split is one **candidate** next step; nothing in
this branch presents it as a finished whole-work delivery.

## 1. What this volume contains (measured by the packer and the audit)

| Item | Value |
| --- | --- |
| Source scenes / story blocks | 264 scenes / 277 blocks |
| Scenes included | **71** (23 common chapters + the story dispatcher + the board scene + the spare scene + request letters/tasks + the `arrive` epilogue) |
| Instructions included | **54,978**, all matched op-by-op by the independent audit |
| Dialogue text | 764,847 UTF-8 bytes (24.8 % of the source) |
| Portraits | 381 bodies + 1,292 expression crops, pre-scaled to screen pixels and composited 1:1 |
| Backgrounds / CGs | 321 JPEGs at 320×180 |
| Chapter metadata | All 264 source scenes are listed; out-of-volume chapters are labelled and not selectable for play |
| CG gallery | 61 groups (the CGs reachable in this volume) |
| Pack | 7,134,854 B, sha256 `c3f99005…5c292ed4` |

No dialogue, choice, CG or expression was removed from the scenes that are included. The 3 pose
rows that are not stored verbatim (`bs2_ch11_01@7`, `ch11_01@7`, `ch12_01@7`) belong to dress 7 of
actor `ch`, which the source never selects (`$$set_bs_dress_type("ch", 7)` does not occur in any
story block); those names resolve through the documented fallback. The 2 body images used only by
those rows are therefore absent (586 of 588 bodies), and the full-content measurement below still
encodes all 586/2,375 reachable layers.

## 2. Full-content encoding measurements (actual, reproducible)

Two complete 264-scene encodes were produced with the same encoder as the runtime volume. They
are **measurements**, not candidates for flashing: the content of a single pack exceeds the
addressable text-id limit of the runtime format, so these runs use a measurement-only 32-bit
text-id variant (`--wide-ids`); the variant's only difference is field width, recorded below.

| Config | Pack bytes | sha256 | header | text | meta | images | tables/ops |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A: layout 0.52, 32 colours, JPEG q30 | **18,974,835 B (18.10 MiB)** | `d5cf4dca…25bcb49` | 412 | 3,083,657 | 729 | 12,250,003 | 3,640,034 / 210,101 ops |
| B: layout 0.46, 16 colours, JPEG q25 | **16,697,716 B (15.92 MiB)** | `2449f64a…88ebabba` | 412 | 3,083,657 | 729 | 9,972,884 | 3,640,034 / 210,101 ops |

Image payload counts: A = 586 bodies + 2,375 face variants + 1,025 scene images + 3 solids;
B = 586 bodies + 2,540 face variants + 1,025 scene images + 3 solids. Both configs keep every
reachable expression and every CG — no expression was dropped to reach these numbers.

The runs above used the tool default (a timestamped `build/measure-<UTC>-<config>-<scope>/`); the equivalent explicit form is:

```text
python tools/anemoi_pack.py --source <snapshot> --measure --scope all --wide-ids \
    --out-dir build/measure-all-A --layout-scale 0.52 --palette-colors 32 --jpeg-quality 30
python tools/anemoi_pack.py --source <snapshot> --measure --scope all --wide-ids \
    --out-dir build/measure-all-B --layout-scale 0.46 --palette-colors 16 --jpeg-quality 25
```

Raw results are preserved under `build/measure-<UTC>-<config>-<scope>/` (`anemoi_pack.bin`,
`anemoi_pack.json`, `measure.json`); the logs are `build/logs/measure-all-A.log` and
`build/logs/measure-all-B.log`.

**Interpretation.** Under the two configurations measured here, a single pack carrying the whole
source is 2.6× / 2.3× the resource ceiling (see §3). That is a statement about these encodings and
this runtime format — not a proof that no possible strategy could fit the work onto one device.
Other strategies (per-volume packs, a wider-id runtime format, subsampled character art, dropping
the request tasks into separate volumes, etc.) are untested together and remain candidates.

Quality cost of config B relative to A: character layout shrinks from 0.52 to 0.46 (≈ −22 %
portrait area), sprite palettes from 32 to 16 colours (visible banding on gradients, line art
mostly unaffected), backgrounds from q30 to q25 (more blocking). Pack A is the configuration used
by the reviewed volume.

## 3. Capacity accounting (measured, not inferred)

| Quantity | Bytes | How obtained |
| --- | --- | --- |
| `factory` app partition | **8,323,072** | `partitions.csv` (0x7f0000) |
| Application image (reviewed) | 8,191,504 | `build-anemoi/FoloToy-AI-Passport.bin` |
| Non-pack part of the app image | **1,056,650** | app image minus embedded pack (8,191,504 − 7,134,854); includes IDF/LVGL/BSP code, the 3,009-glyph font and alignment |
| Resource ceiling (largest pack that still fits) | **7,266,422** | 8,323,072 − 1,056,650 |
| Pack in the reviewed volume | 7,134,854 | `main/anemoi_data/anemoi_pack.bin` |
| App headroom left | **131,568** | 8,323,072 − 8,191,504 (= 7,266,422 − 7,134,854) |

The 8,323,072 B figure is the whole app partition, not a materials budget after subtracting code
and font. A volume's total is **pack + 1,056,650 B** (conservative: each volume's font subset is
smaller than the 3,009-glyph one embedded now), and it must stay below 8,323,072 B.

### Per-book volume measurements (actual, both configs)

Each row is a real encode of that book's 264-scene subset (`--scope book:NNN`), not an estimate.
"Total" adds the measured non-pack baseline of 1,056,650 B. A playable route volume would also
need the shared dispatcher scene and its own font subset, neither of which is in these packs.

| Book | Scenes | Pack A bytes | Total A | Pack B bytes | Total B | Fits A/B |
| --- | --- | --- | --- | --- | --- | --- |
| 010 common | 23 | 4,491,458 | 5,548,108 | 3,792,363 | 4,849,013 | yes / yes |
| 020 Spica | 36 | 3,699,889 | 4,756,539 | 3,160,976 | 4,217,626 | yes / yes |
| 030 Aino | 28 | 3,542,608 | 4,599,258 | 2,998,746 | 4,055,396 | yes / yes |
| 040 Hiyuki | 42 | 3,554,831 | 4,611,481 | 3,019,439 | 4,076,089 | yes / yes |
| 050 Koyomi | 43 | 4,222,769 | 5,279,419 | 3,510,496 | 4,567,146 | yes / yes |
| 060 Rikka | 30 | 4,476,399 | 5,533,049 | 3,870,222 | 4,926,872 | yes / yes |
| 070 Spica zero | 1 | 1,247,524 | 2,304,174 | 1,017,808 | 2,074,458 | yes / yes |
| 071 dōsei | 34 | 3,806,632 | 4,863,282 | 3,238,358 | 4,295,008 | yes / yes |
| 072 kosodate | 22 | 2,952,498 | 4,009,148 | 2,503,863 | 3,560,513 | yes / yes |
| 073 silver | 1 | 867,419 | 1,924,069 | 710,516 | 1,767,166 | yes / yes |

This does **not** mean "split into 10 volumes and ship". Two open points must be measured before
that claim: (a) a route volume that replays the common prologue needs the **union** of common and
route assets, which is larger than either measured pack; (b) the dispatcher + font subset must be
added per volume. Both are next-step measurements, not conclusions.

## 4. Image pipeline

- **Portraits**: each `POSE` record carries the body image, the expression crop, the crop's
  x/y/w/h in the body canvas, original size and layout. The packer applies the source
  `portraitBox()` geometry and bakes both layers at their screen-pixel size (layout 0.52 → source
  432×286 maps onto 320×212). Runtime composites 1:1 and never rescales, so small faces are never
  enlarged to full bodies, and the 469 face atlases (a duplicate of the same crops) are not stored
  at all.
- **BLOCK payload**: palette (≤32 colours with per-index alpha), PNG-style per-row filters, one
  independent zlib stream per 16 rows. Decoding needs a `16×(w+1)` block buffer plus the tinfl
  state; bad blocks and out-of-range palette indices are rejected before touching the canvas.
- **Backgrounds / CGs**: 320×180 q30 4:2:0 JPEG, decoded MCU-by-MCU into the canvas, centred and
  never cropped. **TJpgDec R0.03 as configured here emits BGR565** (red = 0x001F); the output
  callback swaps R/B, pinned by the `jpeg_split.jpg` fixture.
- **Known visual gap**: the source resolves a background name through
  `$$get_bg_time_control_name()` (name + `_01.._05/_80` by `$time_control`). The prototype stores
  the literal name from the script and looks it up directly, so **53 of the 226 background names
  in this volume would show the base image instead of the time-of-day variant**. Fixing this is a
  runtime change (apply the same suffix rule in the `OP_BG` handler); it is not in the reviewed
  binary.

## 5. Storage and state

- 30 manual slots + one independent auto slot + one versioned global-unlock record.
- Every record carries magic, version, length and CRC32; decoding validates field ranges and
  leaves the caller's state untouched on any failure.
- A save is reported successful only when both `nvs_set_blob` and `nvs_commit` succeed; the old
  slot and the visible list stay unchanged on failure. A failed NVS init only degrades
  persistence to “this session”; the code never calls `nvs_flash_erase()`.

## 6. Controls and UI

| Screen | Short press | Long press |
| --- | --- | --- |
| Reading | UP / OK: advance (completes typing first); DOWN: history | UP: fast-forward (stops on release); DOWN: toggle auto-play; OK: in-game menu |
| Choice | UP / DOWN move; OK: explicit confirm | OK: menu (selection and cursor survive the round trip) |
| Lists | UP / DOWN move; OK: enter / execute | OK: back to the source page |
| CG | UP / DOWN change image; OK: back to list | OK: back to list |

The state machine only acts on CLICK (delayed confirmation) and LONG; PRESS never navigates, so a
press cannot trigger both navigation and a trailing CLICK, and the independent short press after a
long press is still its own CLICK. Auto-play stops when a choice appears. The UI model is plain C
(screen id, rows, rectangles, selection) and the host tests assert on that model.

## 7. RAM accounting (same MAP build)

| Region / item | Bytes | Notes |
| --- | --- | --- |
| SRAM alias window | `0x3fc80000`..`0x3fce0000` | 384 KiB total (not all heap) |
| Static data+bss region | 232,160 | `_data_start 0x3fc90e00` .. `_heap_start 0x3fcc98e0` |
| `.dram0.bss` | 224,840 | includes canvas, block buffer, tinfl, app state, IDF/LVGL statics |
| Heap region A | **77,360** | `_heap_start` .. `APP_USABLE_DRAM_END 0x3fcdc710` |
| Heap region B (ROM-reserved tail) | **14,576** | `0x3fcdc710` .. `SOC_DIRAM_DRAM_HIGH 0x3fce0000`; registered as the last heap block at init |
| Heap at boot (A+B) | **91,936** | from `_heap_start`, not from `_bss_end` (which would give 91,944 with 8 B of alignment) |
| LVGL pool | 65,536 | `CONFIG_LV_MEM_SIZE_KILOBYTES=64`, static |
| Canvas `s_pixels` 320×180×2 | 115,200 | static `.bss`, not malloc |
| Portrait block buffer | 8,224 | 16 rows × (255+1) |
| tinfl state | 10,992 | static `.bss` (ROM miniz) |
| Application state | 17,752 | interpreter + slot metadata + view state |

Heap consumers that are **not** statically reserved: LVGL draw buffer `240×40×2 = 19,200`
(`bsp_display_lvgl.c:89`, `BSP_LCD_W=240`), the LVGL port task stack `7,168`
(`esp_lvgl_port.h:68`), the application input task `6,144`, plus NVS page cache and driver
allocations at init (sizes not measured here). Known planned heap use is therefore ≈32.5 KiB
before NVS/drivers, i.e. roughly 59 KiB of the 91,936 B remains. **Runtime free/largest block and
frame time are not measured on hardware**; this section is a static accounting, not a PASS.

## 8. Known gaps in this prototype (do not read as "common route fully verified")

1. **Helper return values are dropped.** The packer stores a helper's `return value` payload in
   the micro-op's `c` field with `flags = 0`, while `run_helper()` tests `op.flags & 1`; every
   value-returning helper (`$$reverse_flag`, `$$get_record_flag`, `$$get_bg_time_control_name`,
   `$$chop_bg_blur_name`, …) currently returns 0. This is why `dynamicChoice.enabled` conditions
   built from `$$reverse_flag` collapse. The fix is one condition in `anemoi_script.c` (read `c`
   or set `flags` in the packer); it is **not** in the reviewed binary because this round is
   evidence-only.
2. **Time-of-day background suffix is not applied** at runtime (53/226 names in this volume,
   §4). The variants are in the pack; only the resolution step is missing.
3. **The automated walk stops at the first request board by test design**, not because the common
   route was walked end-to-end. `tests/test_anemoi_script.c` selects the first board item and
   then stops; the only long walk in this round is that test plus the chapter-start sweep
   (23 chapters each reaching a dialogue frame). The earlier "2.9 M lines" observation came from a
   debugging walker that blindly re-picked option 0 in a three-approach request puzzle
   (scene 31 `020_スピカ0002`, source ops 212–411: sing / lure / smell, each setting `a:125..127`
   and looping back until all three are set) — a walker-policy loop, not a source loop, and it is
   not part of the gate.
4. **Mini-game operations are flag-only.** The source has 15 `skipGame` instructions (rps 1,
   quiz 1, race 13). `story-game-flags.js` resolves rps to `a:409..411 = 10/0/0`, quiz to
   `a:412 = 10`, and leaves snake/race untouched; our interpreter implements exactly that and
   has **no mini-game UI**, matching the watch edition's no-mini-game design. The host test only
   asserts that every native name in the pack has a C dispatch — that is coverage of names, not
   proof of matching semantics, and this document does not claim semantic completeness.
5. **Device tests are NOT RUN** (no hardware authorisation): runtime heap, frame time, button
   feel, and on-device JPEG/BLOCK decode speed are unverified.

## 9. Build, gate and reproduction

```text
# content pack for the reviewed volume (writes into main/anemoi_data only)
ANEMOI_SOURCE=<snapshot> python tools/anemoi_pack.py --scope common \
    --layout-scale 0.52 --palette-colors 32 --jpeg-quality 30

# measurements (always an isolated directory; the tool refuses main/anemoi_data)
ANEMOI_SOURCE=<snapshot> python tools/anemoi_pack.py --measure --scope all --wide-ids \
    --out-dir build/measure-all-A \
    --layout-scale 0.52 --palette-colors 32 --jpeg-quality 30

# this work's gate (host C suites + independent audit + negative cases)
ANEMOI_SOURCE=<snapshot> bash tools/anemoi_gate.sh build/anemoi-gate

# firmware (PowerShell; IDF rejects MSYS paths under Git Bash)
idf.py -B build-anemoi -D SDKCONFIG=build-anemoi/sdkconfig build
idf.py -B build-anemoi merge-bin -o <abs>/build-anemoi/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build-anemoi
python tools/archive_firmware.py create build-anemoi --archive-root build/firmware
```

`--measure` never writes into `main/anemoi_data`: it defaults to
`build/measure-<UTC>-<config>-<scope>/`, prints that path, and aborts if the output directory
resolves into the formal data directory. `tests/test_anemoi_artifacts.py` runs the measurement and
asserts that the formal pack, manifest and font are byte-identical before and after.

On this machine `tools/validate.sh --static` needs `CC=tools/cc-zig.sh` (no system `cc`) and then
still fails on the **baseline** demo runtime link (`test_demo_audio_runtime` misses LVGL symbols),
which this branch does not touch; this work's own suites pass in `tools/anemoi_gate.sh`.

## 10. Licence

The source port of *anemoi* is a personal fan work. This branch is for local technical validation
only and does not distribute the generated content pack; neither the pack nor the firmware archive
is committed. The bundled TJpgDec licence is in `main/anemoi/tjpgd/LICENSE.txt`.

## 11. v3 fixes (this revision) and the reviewed identity

The independent audit v1/v2 (see `build/review-independent/`) found three blocking defects. This
revision fixes them; the package and firmware were rebuilt, and the old `bee2d1a2…` archive plus all
measurement/review evidence are kept.

| Defect | Root cause | Fix |
| --- | --- | --- |
| All variables unresolvable | `write()` froze the text pool (`build_text()`) before serialising `SEC_VAR`, so 136 variable-name ids pointed past the text pool (`var[1]=17622:(null)`) | variable names are registered before the freeze; `text_id()` now raises if anything is added after the freeze; `anem_pack_open()` rejects dangling ids (VAR/text/table/expr/op) and the old pack is now refused |
| Board diverged after frame 4,532 (scene 68 vs source 62) | `$$set_date` (a helpers.js function) was never compiled: the packer skipped every name present in its own native table, so only C stubs ran; `a:1`/`$time_control` were also absent from VAR because only scripts referenced them | helpers are compiled in preference to native stubs; native-only names (`a:1`, `$time_control`, …) and the dynamic `z:201..450` space are registered in VAR; runtime-computed strings (`left/right/mid`, `tostr`) get a scratch pool instead of silently returning id 0 |
| Scene 31 choice showed one wrong label and looped | `run_helper()` reset the arena per micro-op, clobbering the caller's `labels`/`enabled` arrays | helper evaluation uses a stack discipline (save/restore `arena_used`) and never resets inside a nested call; dynamic `["v","z",expr]` keys compile to a real dynamic lookup |
| Board listed requests the source drops | source `$$set_bbs_request_data` drops items whose `preview`/`scene` names are not in the catalog | same filter in C (by source name) |
| Time-of-day backgrounds always `_01` | `$time_control` missing, and `exists_image` treated a bare base alias as existing | VAR schema fixed; namemap marks pure base aliases so `exists_image` answers like the source, and `OP_BG` resolves base → base+suffix |

Verified after the fix (production C against the source engine transcript, same policy):

- `build/anemoi-verify/20261102-043808/`: identical transcript for the first **6,521** comparable
  lines (through day 13 / board #7), where the prototype reaches a **volume boundary** (request
  letters for scenes whose entry-0 prefix is not packed); the source continues to `END` after
  10,961 lines. This is the documented boundary, not a fix claim for the rest.
- `tests/test_anemoi_semantics.c` (in the gate): `$$reverse_flag`, `$$get_bg_time_control_name`,
  `$$chop_bg_blur_name`, `$$set_date` values; first board `a:1=7, a:2=12, a:3=1` with the four
  source items and `choose → scene 62`; scene 31 choices 3 → 2 → 1 → `END` with `a:125..127=1`;
  a `$time_control=2` background resolving to `bg999_03`.
- Independent packing data check: pack `7,163,111 B`, sha256
  `8df8f768bb95cc1c89763cb533debffede4dca506ee1e62ec214638620d46fa3` (old `c3f99005…` and
  `5fa57cba…` packs kept as evidence).
- Firmware: app **8,223,536 B** sha256 `9006a8ab…`, full **8,289,072 B** sha256 `ab784f32…`,
  ELF sha256 `6bdd7951…`, MAP sha256 `0347f467…`; archive
  `build/firmware/ab784f3248c756c5921731ed5d2cd96b6d459cd1599d1073c2877b1049909ac8`
  (verify PASS). App headroom 99,536 B.

Remaining gaps (unchanged claims): request **letters** (the ten-op prefix before `entry 10`) are not
packed, so board jumps land on the task and later boards reach the volume boundary; mini-games stay
flag-only (15 `skipGame`: rps 1 / quiz 1 / race 13); gallery is 61 entries / 14 groups (source 490 /
89); 20 px font is not offered; device tests are NOT RUN.

### RAM after the fix (same MAP)

`_heap_start = 0x3fccd3b0`, `.dram0.bss = 239,904 B`. Provable allocator region A
(`_heap_start..APP_USABLE_DRAM_END 0x3fcdc710`) = **62,304 B**; the ROM startup-stack tail B
(`0x3fcdc710..0x3fce0000`) = **14,576 B**, merged only after the scheduler starts
(`heap_caps_enable_nonos_stack_heaps`). Named dynamic items 41,984 B (draw 19,200 + LVGL task 7,168 +
input 6,144 + main 3,584 + timer 3,584 + event 2,304) leave **20,320 B (A)** / **34,896 B (A+B)**
before LVGL objects, NVS, drivers and fragmentation. Device free/largest is NOT RUN.

### Variants vs duplicates (image counts)

381 bodies / 1,292 expression crops in this volume are **baked display variants**, not a second copy
of the source images: the source has 588 distinct body files and 2,221 distinct crops, and each crop
is baked at the screen size of the body it is paired with (a face used at two scales yields two
payloads). The 469 face atlases are not packed at all. Body/face counts are therefore not comparable
to source file counts directly; the audit maps each payload back to (source id, polygon) in
`measure.json`-style references.

## 12. Common closure measurement (letters + every task, recursive)

The v3 candidate (pack `8df8f768…`, firmware `ab784f32…`) still did **not** contain the full
common route. `compute_scope()` appended the entry-10 task ranges of every request scene but never
re-queued them, so any `callScene`/`native` dependency *inside* those tasks was never scanned, and
the ten-op letter prefix (`[0, entry10)`) was not packed at all. Wiring a true fixed-point closure
(`common-full`: flow scene, all request letters, all entry-10 tasks, their scenes/board previews,
recursively, until no new range appears) gives:

| Scope | Scenes | Ops | Pack | vs ceiling 7,262,647 B |
| --- | --- | --- | --- | --- |
| v3 candidate (`common`, tasks but no closure) | 71 | 55,049 | 7,163,111 B | +99,536 B headroom |
| common + tasks, true closure (`common`) | 133 | 115,930 | 11,502,736 B | **over by 4,240,089 B** |
| common + letters + tasks (`common-full`) | 133 | 116,064 | **11,505,505 B** | **over by 4,242,858 B** |

Letters are cheap (text +1,015 B, ops +1,608 B); the volume is the tasks and their dependencies.
Section deltas from the candidate to `common-full`: text +946,858, ops +732,180, blob +2,476,083,
pose +34,584, namelist +21,860, expr +22,946, image +13,824, tables together ≈ +96 KB.

Lossless dedupe cannot close a 4.24 MB gap: the candidate's 2,000 image rows point at 1,945 distinct
payloads (5 shared), and the closure adds 864 more payloads; no background/body is byte-identical.

Therefore the honest status is: **a full common route to the source END does not fit the current
8 MB partition at any encoding measured so far**, and the 6,521-line match reported in §11 is a
prefix of an *under-scoped* pack, not a near-complete route. The `X|` boundary stop (source name of the chapter reached) is not a "volume
boundary": it is where the under-scoped pack ran out of reachable content.

Strict comparison is now a maintained tool (`tools/anemoi_compare.py`): it requires equal stop
counts, per-stop kind/speaker/text equality, board count + every item (`preview:scene:entry:lock`),
choice count + every label, and both sides to end in `E` with equal `lines/choices/boards`; a
boundary marker instead of `END`, a truncated file, one changed character, a wrong end count, an
extra board item or a changed choice label are all rejected (`--selftest` covers all six). On the
candidate it correctly fails:

```text
FAIL | terminator | source END missing on the C side
STATS | oracle_stops=11009 c_stops=6522 oracle_T=10961 c_T=6497 oracle_B=23 c_B=8 oracle_C=24 c_C=16
```

Options that would fit, in the order the constraints allow: (1) split by volume (the per-book
measurements in §7 are the candidates, not a decision), (2) reduce the art budget (scale/palette/
quality), which changes declared fidelity, (3) drop content (explicitly not allowed). None of these
is announced as the plan; the numbers above are the decision input.
