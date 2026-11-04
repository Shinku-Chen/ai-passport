<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# NianNiang Love Story

A **landscape** visual-novel reader for the AI Passport, carrying the complete
port of NianNiang Love Story — the Ren'Py campus comedy that was previously ported to
the Xiaomi Band 10. Everything is on the device: 13 story chunks, 1,248 dialogue
lines, two choice points, four endings, 16 backgrounds, 15 event CGs and all 30
expression sprites. No network, no companion app.

- Upstream: [`hrk666666/NianNiang-MiBand-10`](https://github.com/hrk666666/NianNiang-MiBand-10)
  (HEAD `b963df9`), which is itself built on `hrk666666/Senren-Banka-MiBand-10`.
- Branch: `feature/port-nianniang-landscape`.
- Runtime: ESP32-C3, 8 MB flash, no PSRAM, 320 × 240 landscape, three keys.

## Delivery status (2026-11-02)

**Software review passed; awaiting device acceptance.** Independent reviewer v4 signed off
(N1/N2/N3 closed); software work is frozen here.

- Firmware: app **1,730,272 B** / merged **1,795,808 B**
- app SHA256 `8df14ec9c119b759ad8907ca29f2524dc01ff62fef18fc01377904cba5b04cc0`
- full SHA256 `4f3e731cdcf6a7aa6a738b0ea6fa98f5a901ed7e90a044992a8304be3809fbb2`
- Independent report: `build/review-logs-v4/REPORT-v4.md` (local evidence directory, not committed)
- Still to verify on hardware: real free/largest and the 153,600 B contiguous block, key feel and
  NVS across restarts, the 20 px tier's line height, JPEG decode timing, and deep-sleep wake.
  **Device tests have not run.**

## Controls

| Screen | Keys |
| --- | --- |
| Reading | **UP** advances (completes the typewriter, turns the page, then the next line); **hold UP** fast-forwards while held; **DOWN** opens history; **hold DOWN** toggles auto-play; **OK** opens the menu |
| Lists (title, menu, saves, settings, gallery) | **UP/DOWN** move, **OK** confirms, **hold OK** backs out |
| Saves | **OK** on a slot saves (save page) or loads (load page); **hold OK** returns to the page you came from (no gesture deletes a slot) |
| CG viewer | **UP/DOWN** steps to the neighbouring CG, **OK** returns to the grid |

Physical layout when the device is held sideways: UP is on the right edge, OK in
the middle, DOWN on the left.

## What the port carries

| Item | Count | Notes |
| --- | --- | --- |
| Scenes | 13 | one per upstream `chunkNNN`, in order |
| Dialogue lines | 1,248 | 19,767 characters; every line fits one 16 px page |
| Choice points | 2 | three-way (school gym) and two-way (rooftop); three options jump straight to an ending |
| Endings | 4 | bad end 1 (the police report), bad end 2 (the relationship breaks), normal end (the roommate ending), true end |
| Backgrounds | 16 | 13 referenced by the script; the three night variants are carried but unused (see below) |
| Event CGs | 15 | all referenced; unlocking one adds it to the gallery |
| Sprites | 30 | 28 expressions of the same pose plus 2 for the second character |
| Audio | 0 | the upstream hand-band port has none (the band has no speaker) |

Nothing was dropped to fit: the whole pack is **622,892 bytes**, and the merged
image is **1,795,808 bytes** (1,730,272-byte app = 20.8 % of the 8,323,072-byte
app partition). Bluetooth is compiled out (`CONFIG_BT_ENABLED=n`), which also moves the
start of the heap down by 896 B.

## Art geometry, and why nothing is cropped

The upstream port produced its art with
`Image.open(src).convert("RGB").resize((336, 480))` — a plain anisotropic
stretch of unknown originals, with **no crop**. The original aspect ratio is not
recorded anywhere, so this port does not pretend to restore it:

- every background, CG and the title art is scaled to **168 × 240** and centred
  on the black 320 × 240 canvas (fit, no crop, no further stretch);
- sprites are scaled to 240 px tall (120 px wide), cropped only to their alpha
  bounding box, and drawn bottom-centred over the art;
- the 90 px dialogue panel sits on top of the art with configurable transparency
  (10–55 %, default 30 %), so the lower part of the scene stays visible.

Re-stretching to a guessed 16:9 was measured (it would crop 12.5 % of the width)
and is deliberately **not** used: with the original ratio unknown it can only
produce a wrong picture. See the feasibility report's `H0`/`H1` variants.

## Data pipeline

```
upstream checkout ──tools/nn_pack.py──> assets/nn/nn_pack.bin   (622,892 B)
                              │                ├── SEC_TEXT     (59,574 B, NUL-terminated)
                              │                ├── SEC_IMAGE    (77 images: 16 bg, 15 CG, 15 thumbs,
                              │                │                  30 sprites, 1 title)
                              │                └── SEC_OP       (1,581 × 12 B fixed records)
                              └── assets/nn/nn_charset.txt ──tools/gal_font.py──> assets/fonts/gal_cjk_1{6,0}.c
```

The pack is embedded in the application image (`target_add_binary_data`), so the
firmware reads it as ordinary `const` memory: no partition lookup, no mmap
window, no decompression, no JSON parsing.

Images are **JPEG** for backgrounds/CGs and **JPEG + 1bpp mask** for sprites.
Decoding goes through TJpgDec straight into the 320 × 240 RGB565 canvas
(`main/nn_image.c`): the output callback converts each MCU to RGB565 and, for
sprites, skips pixels whose mask bit is 0. There is exactly one big buffer
(153,600 B), no per-image decode buffer, and no second LVGL layer for sprites.

Regenerating everything (needs the upstream checkout and a CJK font):

```bash
python tools/nn_pack.py --source <NianNiang-MiBand-10 checkout> \
    --out assets/nn/nn_pack.bin --manifest build/nn_manifest.json \
    --charset assets/nn/nn_charset.txt
python tools/gal_font.py --font <NotoSansSC.otf> \
    --chars-file assets/nn/nn_charset.txt --out-dir assets/fonts
python tools/nn_audit.py            # route/asset audit, must print "audit: PASS"
```

## Story graph

The two choice points decide the ending, and a choice that jumps to an ending
never falls through into the next block:

```
chunk001..003 -> chunk004 -[choice]-+ "Push her down now"      -> chunk013@end_1_4_1 -> badend1
                                    + "Get away from me"       -> chunk013@end_1_4_2 -> badend2
                                    + "I'm straight." (empty jump = continue in order)
                                      +-> chunk005..011 -[choice]-+ "Do nothing"   -> chunk013@end_1_11_1 -> normalend
                                                                  + "Kiss her" (empty jump) -> chunk012 -> trueend
```

`tests/test_nn_script.c` walks every branch and asserts that choice 1 maps to
endings {1, 2, continue→choice 2} and choice 2 to {normal end, true end}, that all
four endings are reached exactly once, and that all 1,248 lines are reachable.

## Engine notes

- `main/nn_pack.[ch]` — pack parsing with bounds checks (magic, version, section
  ranges, per-record ranges, jump/choice targets). Malformed packs are rejected at
  `init`; the host test feeds it truncated and corrupted copies.
- `main/nn_script.[ch]` — interpreter over fixed 12-byte ops. Only `SAY` and
  `CHOICE` block; backgrounds, CGs, sprites, chapter titles, effects, jumps run to
  the next blocking op. A run-loop guard turns a jump cycle into a visible error
  instead of a hang.
- `main/nn_canvas.c` — owns the RGB565 canvas and the `lv_image_dsc_t` handed to
  LVGL; background/CG fit-centred, sprites bottom-centred.
- `main/nn_ui.c` — 11 pages on 320 × 240: title, reading, choice, menu, saves
  (5 manual + 1 automatic), settings, history, gallery grid (6 per page), CG
  viewer, ending, about.
- `main/nn_save.[ch]` / `main/nn_save_codec.[ch]` — NVS persistence. Records carry
  scene/op plus the view state, so loading never replays the script; CG and ending
  unlocks live outside the slots. Records are encoded (`NSN1` magic, version,
  payload length, clamped strings) and validated again against the pack before a
  seek, so a corrupt or stale slot is reported instead of restoring nonsense.
  **The reader never calls `nvs_flash_erase()`**: NVS is a shared partition, so a
  failed open degrades to "play now, saves unavailable" with an explicit reason,
  and `tests/test_nn_store_contract.py` enforces that.
- `main/nn_jpeg/` — TJpgDec R0.03 by ChaN, compiled directly (the LVGL wrapper is
  bypassed because it decodes a whole RGB888 image first). The original copyright
  header is retained; only the LVGL configuration include and its `LV_USE_TJPGD`
  guard were removed.

Fixed on the way: the framework's CG/ending bitmaps were 8-slot/32-bit, and its
dialogue/choice model was a linear event list that could not express
"choice → ending". This title uses a scene/label/jump interpreter and a CG list
with its own unlock bits, so neither limit applies.

## Accepted requirements

The decisions this port implements, as confirmed by the user and the project lead:

- **Landscape 320 × 240 by default**, rotated by the panel's MADCTL (no rotation buffer).
- **Keep everything**: story, branches, endings, CGs, sprites and all 30 expressions; no pose
  merging and no expression re-authoring.
- **No crop, no stretch**: the source aspect ratio is unknown, so every image is fitted by height
  and centred; CGs can be viewed full-screen; "restore 16:9" remains a conditional option that
  requires the original ratio to be verified first, and is not used here.
- **Sprites are never shrunk or trimmed because the landscape screen is short**: they stay 240 px
  tall (120 px wide) with the translucent dialogue panel over them.
- Saves: five manual slots plus one automatic slot (written on scene change, before sleep and when
  returning to the title), storing position + view state + unlocks with version/robustness checks.
- The gallery is paged instead of inheriting the framework's 8-tile / 32-bit limits.
- The Chinese font covers the UI, the whole script and speaker names (1,437 glyphs, 16 px and
  20 px tiers, 0 missing).
- Three-key semantics: reading UP advances (hold to fast-forward), DOWN history (hold for auto),
  OK menu; lists use UP/DOWN, OK, and hold-OK to go back.

## Page and key regression coverage

`tests/harness_app.c` runs the **real** `nn_app.c` / `nn_ui.c` / `nn_pack.c` / `nn_save.c` on the
host (LVGL is a thin stub that records object visibility and label text, NVS is an in-memory
implementation, canvas/BSP are stubs) and asserts:

- save page: the footer shows the success notice, hold-OK returns to the menu **without deleting**
  a slot, and repeated hold-OK never deletes anything;
- history (opened by DOWN while reading and by the in-game menu): the overlay is really visible,
  rows carry text, and it no longer reuses the about body;
- settings -> about: the real key path reaches the about page and its back gesture returns to the
  page it came from;
- return-source matrix: title -> settings/gallery/about, reading -> settings/history, menu ->
  save/load/history/settings each return to their own source page;
- save/load restores the exact same line; CG unlocks survive a restart.

`tests/harness_save.c` injects `nvs_set_blob` / `nvs_commit` / `nvs_flash_init` / `nvs_get_blob`
failures and asserts that the old slot and the current reading position are untouched, that the
failure notice is actually visible, that the success notice is visible, and that
`nvs_flash_erase()` is never called.

`tests/test_nn_pack.c` feeds the real pack plus 11 corrupted variants (image past its data area,
overlapping sections, 32 sections, sections pointing into the table, duplicate types, truncation,
bad magic/version/size, record-count overflow) and requires all of them to be rejected.

## Key-event semantics (PRESS / LONG / CLICK / RELEASE)

One physical press produces **PRESS -> RELEASE -> CLICK** (a hold produces
**PRESS -> LONG -> RELEASE**). Every navigation action in `nn_app_key()` runs only on **CLICK**
or **LONG**; PRESS and RELEASE never navigate, and the only use of RELEASE is "UP released =
stop fast-forward". `tests/harness_app.c` drives these real sequences: history, ending, about
and the CG viewer each advance exactly once per full press, and the next press still works.

## Pack validation

`nn_pack_init()` performs full structural validation: section types must be in the known
whitelist (TEXT/META/IMAGE/SCENE/OP/SPEAKER/CHOICE/ENDING; `NN_SEC_COUNT` is only table
capacity), sections may not overlap, duplicate or cover the section table, record counts may
not overflow, image offsets may not leave their section's data area, and **every text
reference** (scene names, choices, ending names/bodies, speakers, SAY/CHAPTER operands) must
lie inside the text section and be NUL-terminated. A bad pack fails at init instead of leaving
a half-usable one; `tests/test_nn_pack.c` covers the real pack plus 17 corrupted variants.

## Memory budget (same-build MAP, corrected after independent review)

- Startup heap: `0x3FCDC710 - 0x3FCA6450` = **221,888 B (216.7 KiB)**
- Dynamic use at runtime, item by item from config/source:

| Consumer | Bytes |
|---|---:|
| main task stack (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`) | 3,584 |
| esp_timer task stack | 3,584 |
| system event task stack | 2,304 |
| LVGL port task stack (`esp_lvgl_port`) | 7,168 |
| input task stack (`main.c`) | 4,096 |
| LVGL flush buffer (240x40x2) | 19,200 |
| canvas (one contiguous block) | 153,600 |
| **Total** | **193,536** |

- Left over: **28,352 B (27.7 KiB)**; about **23-24 KiB** once NVS page caches, LVGL port sync
  objects and fragmentation are taken out.
- **Correction**: the `idle` task uses a static stack (`xTaskCreateStaticPinnedToCore`, already in
  `.bss`/the MAP) and must not be counted as heap; the earlier
  "187,904 / 33,984 B free" figure is retracted because it counted idle and missed the
  LVGL port stack.
- Static (not heap): LVGL pool 65,536 + TJpgDec workspace 6,144 + `nn_app_t` about 1.4 KB
- The single **153,600 B contiguous** block is still unverified on hardware --
  `nn_canvas_init()` logs free/largest, one line from a device settles it, and this document
  does not claim the memory budget passes until then. Wi-Fi stays compiled in (IDF 5.5 hides
  the symbol) but is never initialised, so it costs no runtime heap.

## Known gaps

- The three night backgrounds (`bg_room_night`, `bg_street2_night`,
  `bg_tea_night`) are packed but no op references them — kept so the pack matches
  the upstream asset set.
- Upstream has no audio at all, so there is no BGM/SE.
- Sprite cross-fades (`change`/`fadeout`) and the 25 `blackout` effects are
  rendered as hard cuts / black frames.
