<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Steins;Gate reader for the AI Passport

A landscape visual-novel reader for the AI Passport, ported from the Xiaomi
Watch 5 scene-reader release `steins-rw5` (source snapshot
`28e5b52f11c77fc4781f532e2729a5374f5fd8ea`). The application baseline is
`feature/galgame-framework` at `3c2e5013a9831c57124ef8f774193f0e06f1f14a`.

Everything below describes **this worktree only** (`ai-passport-steins`,
branch `feature/port-steins-landscape`). The first-batch port projects and the
main checkout are read-only references for this branch.

## Delivery state

**Software review: PASS; awaiting device acceptance.**

| Item | Value |
| --- | --- |
| App image | **7,972,400 bytes** at `0x30000`, factory `0x7d0000` = 8,192,000 B, **219,600 B free** |
| App SHA-256 | `a91cb88527ae1e3450208942ab08e3ceb1bf42f1bc85c3d4d4a0e9299e96437d` |
| Merged image | **8,169,008 bytes** |
| Merged SHA-256 | `2020c42f4bdd2030ae417ed8a4db13a7e0cf16376a1d10259c00907e09d34ab5` |
| ELF | 17,986,512 B, SHA-256 `8cff5469e883a3d0d1752ca5e6f8c6e657f3f15f08aa41757cb866bb5044b579` |
| MAP | 4,300,870 B, SHA-256 `bb7ff80ca52928816bcf1907e970fa25b257d9fa0338526a5d56a5bf996b6559` |
| Same-run archive | `build/firmware/2020c42f4bdd2030ae417ed8a4db13a7e0cf16376a1d10259c00907e09d34ab5/` |
| Partition table | NVS `0x9000`/`0x26000` (38 pages), phy `0x2f000`/`0x1000`, factory `0x30000`/`0x7d0000` |
| Resource pack (v5, compressed TXT) | 6,712,980 B, SHA-256 `9fc30e7cc300508e4184b26b9de46c760a91615097ab6e1b14750a92eaab794f` |
| Fonts | story **3,412** codepoints, UI **449**, metrics **3,861** |
| Font SHA-256 | story `a4b51108…`, ui16 `295981c0…`, ui18 `fa1b571e…`, metrics `0046d223…` |
| Build / Host tests / Device | **PASS** / **FAIL 29 PASS, 4 FAIL, 1 SKIP** / **NOT RUN** |

The delivered firmware contains the full main story **and** the phone content:
646 mail entries (609 non-empty bodies), 4,072 typed events over 2,001 rows,
371 call frames over 21 sites, 65 choosable nodes, 136 items, 9 endings, and the
56-key presentation table. The v5 pack stores the 1,190,478 B text blob as
582 independent 2 KiB raw-DEFLATE blocks; the reader decompresses one block at a
time through the ESP32-C3 ROM `tinfl_decompress`.

## Host gate status (actual)

`bash tools/validate.sh --static` → **29 PASS / 4 FAIL / 1 SKIP** (plus 21
sub-case skips). The four failures are pre-existing
`baseline_demo_{audio,low_power,ble,wifi}_runtime` link errors
(`undefined symbol: ui_pixel_screen_create`); every Steins test passes. This is
**not** a fully green gate and is not reported as one.

The single SKIP is the source audit, which did **not** run because `STEINS_SOURCE`
was not set. The independent content audits were executed separately and pass:
plain source content 224/0, v5 33/0, PRES 12/0, font coverage 18/0.

## Static RAM (measured, partial)

`_heap_start = 0x3fccb410`; A = **70,400 B** (`0x3fcdc710`), B = **14,576 B**;
**A + B = 84,976 B** is not the largest contiguous block. `.data` 7,300 B,
`.bss` 231,800 B (includes `s_app` 149,216 B with the 115,200 B canvas).
The decoder keeps 17,108 B of static state (`s_tinfl` 10,992 + `s_win` 4,096 +
`s_work` 2,016 + `s_work_pack` 4), already counted in BSS.

Known dynamic allocations (draw 19,200 + input 6,144 + LVGL 7,168 + main 3,584 +
esp_timer 3,584 = 39,680 B); with an offline 35-item-page one-block cache
(7,672 B) that is 47,352 B, leaving ~23,048 B of A before TCBs, queues, drivers,
allocator metadata and fragmentation. For all 38 NVS pages, Page[38] =
38 × 84 = 3,192 B. At up to five 128 B HashList blocks per page, Page + Hash
totals 3,192 + 38 × 5 × 128 = 27,512 B for all 38 pages; this is a loose
structural bound, not a runtime measurement, and excludes allocator overhead.

### Offline NVS capacity bound (synthetic)

The official ESP-IDF 5.5.3 image for the 38-page layout is
`356ebbcbb66835eac5efe2befcf528407b54ce154a6304264796d9ce08ee1a70`: 31 × 4,096 B
synthetic checkpoints at the hard cap, plus 30 legacy 16 B entries, metadata and
the checkpoint/legacy shadow. It reports **4,293 used / 495 free entries, 3
completely empty pages and 65 content matches**; the older 22-page image raises
the official `InsufficientSizeError`. This is a synthetic coexistence bound — not
a legal-codec layout, not the largest real 31-slot story state — and it does not
prove same-key physical commits, GC or power-loss behaviour. Report:
`build/review-steins-38-20261102T091159Z-43900/REPORT_STEINS_38_HARDCAP.md`.

**Runtime free heap, largest block, high-water mark, physical NVS behaviour,
LCD, glyph rendering, buttons, audio, sleep and frame timing are NOT RUN.**

## History

The earlier `f1fb3029…` firmware (pack `5d97102c…`, 6,900,805 B, app 8,088,096 B)
is a **main-story support build**, not the current phone-complete identity. All
earlier candidates, reviews and the damaged-source incident are retained as
history; their logs and binaries are unchanged.

## Independent reviews

- `build/review-steins1-binding-20261102T095518Z-46350/REPORT.md`
- `build/review-steins-final-20261102T095557Z-46400/REPORT_FINAL_BIND.md`
- `build/review-steins-bss-20261102T100020Z-46821/ERRATA_BSS_SCOPE.md`
- `build/review-mobile-075447-20261102T080910Z-39727/REPORT.md`
- `build/review-pack-reader-final-20261102T084145Z-41819/REPORT.md`
- `build/review-font-20261102T091556Z-44240/REPORT.md`
