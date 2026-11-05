<p align="right"><a href="shinuyakata.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Shinuyakata reader

This application ports the runtime content of
[xuanyide01/shinuyakata-rw5](https://github.com/xuanyide01/shinuyakata-rw5)
onto the GalGame framework. The base is `feature/galgame-framework` at
`7f04b554`; it already contains upstream `main` at `488beda4` as checked on
2026-11-05. Work lives on `feature/port-shinuyakata`.

## Content and limitations

The pinned source is `76bba0bb0bfcc066ce6a35883d76c5b19981e0cb` (RW5
0.3.7). Its runtime catalog has 198 pages across 99 source scenes, 17,409 text
records, 11 choice groups, 56 CG groups and 311 CG variants. These counts come
from the actual runtime catalog and files; the upstream README's 17,394 text
count is different. Every catalog text record is retained, including source
control/diagnostic text. The port does not invent missing scenes or endings.

From a new game, 196 runtime pages, all 11 choices and eight ending exits are
reachable. Two source pages are unreachable: `n_s303_2` and `n_s601_2_2`.
The latter is explicitly marked an unresolved original exit by the source.
Both remain in the packed data; the port does not turn them into new
playable branches. This is a port of the supplied runtime dataset, not an independent
verification of the original commercial game's completeness.

The source ships no voice, BGM or OP video. Music therefore has an empty state;
no placeholder melodies are played. Original art and script rights belong to
BUG SYSTEM and their respective rights holders. The source port's own code is
MIT; that license does not grant rights to original game materials. The work
contains adult, violent and horror content; the about page retains this notice.

## Interface and controls

The 320 x 240 landscape reader uses the framework's title, dialogue, choice,
menu, settings, history, saves, chapter, gallery, recall and ending pages. The
RW5 stage geometry is scaled proportionally to 320 px wide: backgrounds are
320 x 180 at y=0; centred sprites keep their aspect ratio and share y=196
as their bottom anchor. The translucent dialogue panel occupies the bottom
80 px, with three lines at either font tier; the speaker name is above
it, with a background sized to its text plus padding. Title actions are on the right, CG tiles use four columns and two rows,
and secondary-page lists scroll in their own viewports. Rotation uses the BSP
under the LVGL lock before constructing the UI. The 96 KB LVGL pool holds
the lazily created history, chapter and gallery rows together. Page-boundary
newlines do not create an extra empty text row; overwrite prompts fit both
font tiers.

- Reading: short OK/UP/DOWN reveals the current text or advances one page.
- Long OK opens the menu. Long UP immediately advances and keeps skipping
  without an added delay, crossing chapter cards until release, a choice or
  an ending. Each input-loop turn advances one page so keys remain responsive.
  Long DOWN toggles auto-reading. An action key first stops auto-reading and is consumed.
- Lists: UP/DOWN select, OK confirms, long OK returns. Lists clamp at their ends.
- Gallery: all 56 groups are accessible in windows of eight tiles. In the CG
  viewer, UP/DOWN selects a group's variants; OK or long OK returns.
- Chapters and recalls use bounded windows so every entry is reachable without
  allocating an LVGL widget per story scene.
- Three manual saves plus automatic progress retain event, subpage and all
  route variables. Unlocks are independent. NVS namespaces are title-specific:
  `yksave`, `ykprog`, `ykunlock`; incompatible framework saves are not imported.
- Chapter and CG recall entry points start a fresh route-variable context;
  use a save or Continue to restore an exact playthrough. Saves from portrait or
  the earlier 90 px dialogue layout retain the current line and route variables,
  replaying that line once because its page boundaries have changed. New saves
  retain their exact subpage.

Auto-read waits 900 ms after text finishes, crosses chapter cards and waits for
choices. While enabled, including at a choice, it keeps the screen awake.
Manual reading uses the framework's 60 s dim / 180 s screen-off / 420 s deep
sleep policy. Any key wakes the board.

## Offline rebuilding

The vendored runtime input is under `assets/yakata-source/`; its
`MANIFEST.json` records the upstream commit and per-file SHA-256. It contains
the catalog, story JSON, backgrounds, CGs and transparent sprites used by this
port, plus the upstream license. Raw original engine scripts and audio were
not supplied by the source repository.

```text
python tools/yakata_import.py
python tools/gal_font.py --font <NotoSansSC-Regular.otf> --out-dir assets/fonts
./tools/validate.sh
```

Import requires Pillow. Its deterministic quality-24 JPEG output is
`main/yakata_data/yakata_art.bin` (5,109,188 bytes); generated dialogue tables
and the pure-C route resolver live in `main/gal/`. Inputs and generated data
are retained so an ordinary checkout builds offline. Noto Sans SC is under
the SIL Open Font License; generated subsets, their glyph inventory and the
font generator are retained, while the source font is supplied to the command.

Background/sprite combinations are composited offline and deduplicated by
encoded content. JPEGs are decoded once on image changes using a static 4 KB
TJpgDec workspace. A shared 98,304-byte RGB565 arena holds a 256 x 192 main
image or eight native 64 x 40 thumbnails. A clipped-row decoder expands main
images to 320 x 240; text animation and redraws read cached pixels. This trades
some image detail for bounded RAM use on the no-PSRAM board. The stock JPEG
information callback is unregistered because its 4 KB local buffer overflowed
the LVGL task on hardware. No filesystem partition, Wi-Fi, Bluetooth or network
download is needed. The normal NVS/PHY/factory
partition layout remains in use.

## Validation and device handoff

`tests/test_yakata_content.py` compares every text record, validates image ranges
and group/variant tables, verifies source hashes, and compares the compiled C
route resolver with source-engine conditions/effects over 53,703 transitions.
The normal gate also checks font coverage, repository invariants, BSP fault
paths, host logic, firmware layout and merged-image consistency.

Build and host tests do not prove board rendering. Before hardware acceptance,
check title art, red/blue colour order, sprite placement, both font tiers,
choice routing, manual/automatic save restoration, all secondary-page returns,
CG variants, held fast-forward, and unattended auto-reading with the screen on.
Also test idle sleep and key wake. Flashing requires approval for the exact
verified image; a merged image at `0x0` may reset stored NVS data.

## Verified development build (2026-11-05)

The complete `./tools/validate.sh` gate passed under ESP-IDF v5.5.3. Host application tests cover keys, route-variable restoration through actual NVS encoders, high unlock bits and prolonged auto-mode choice waits. Platform-specific archive/installer cases are skipped on Windows. Offline regeneration of all four content outputs is byte-identical.

- Build: PASS; version `0.1.0-shinuyakata`.
- Host tests: PASS.
- Device tests: NOT RUN.
- Unverified: real rendering, audio absence, key timing, physical idle/sleep/wake and end-to-end on-board save restoration. Only Bluetooth virtual COM4/COM7 ports were detected; no target USB board was detected.
- Merged image: `build/FoloToy-AI-Passport-full.bin`, 7,884,768 bytes, offset `0x0`.
- Application: 7,819,232 / 8,323,072 bytes; 503,840 bytes free.
- Full-image SHA-256: `c2261ec172878e85ddd8c37f797d3e7aa64441f70f5e48587578918edcfdc053`.
- Matching ELF SHA-256: `38c8b917eda479131854f9eb0b38e6d79d1ad5ecaf96e0ef0d78059dbd1e0b33`.
- Verified ELF/MAP and image bundle: `build/firmware/c2261ec172878e85ddd8c37f797d3e7aa64441f70f5e48587578918edcfdc053/`.

At that checkpoint no flash, commit, push or release had been performed.

## Device check (2026-11-05)

After explicit approval, the verified `c2261ec172878e85ddd8c37f797d3e7aa64441f70f5e48587578918edcfdc053` merged image was written at `0x0` through USB COM6 to an ESP32-C3 rev v1.1 with 8 MB flash. esptool verified the written data. No whole-chip erase or rebuild was performed.

A controlled restart and 30-second startup observation confirmed version `0.1.0-shinuyakata`, ESP-IDF v5.5.3 and the matching ELF prefix `38c8b917e`. The display/LVGL and three ADC buttons initialized; the application reported ready at 2.648 s, 168,692 bytes free heap and a 114,688-byte largest block. No panic, watchdog, assertion or reboot loop appeared during that window. Raw serial logs stay local under ignored `build/`.

Build: PASS. Host tests: PASS. Device tests: PASS for flashing and startup only. Unverified: visible artwork/text, physical button navigation, save/load, auto-mode backlight and idle/sleep/wake. User visual acceptance remains pending; successful flashing and clean logs do not establish it.

## Landscape build (2026-11-05)

At this checkpoint: `0.1.1-shinuyakata`, 320 x 240 landscape. The earlier device observations above belong to the portrait build and do not validate this one. The complete gate passed under ESP-IDF v5.5.3. A separate 32-bit native LVGL run rendered all 18 ordinary pages plus the two modal states at both font tiers, including the last menu item, three-way choices and long save names. It reproduced exhaustion with a 64 KB pool; the 96 KB configuration completed the same run, with a measured host peak of 79%. This is layout/host evidence, not board acceptance.

The title, menu, save/load, settings, history, chapters, gallery, CG viewer, transitions, endings and about page were adapted to landscape. The 90 px dialogue panel uses four small-font lines or three large-font lines. Portrait-save migration keeps the current event and route variables, restarting the current line at page zero to prevent skipping text. New landscape saves retain their page indices. Source text, choices and route logic are unchanged. The four content-generator outputs remain byte-reproducible offline.

- Build: PASS.
- Host tests: PASS, including portrait-save migration and 53,703 compiled route comparisons.
- Device tests: NOT RUN for this version.
- Unverified: physical rotation direction, rendering, keys, on-board save migration, automatic reading and idle sleep/wake. USB COM6 is currently absent; only Bluetooth virtual serial ports are listed.
- Merged image: `build/FoloToy-AI-Passport-full.bin`, 8,177,952 bytes at `0x0`. Application: 8,112,416 / 8,323,072 bytes; 210,656 bytes free.
- Full-image SHA-256: `3d888a6cdfef068cbabfe2b7a3bb6805791c69d246d04cd16a963e8869fd6854`.
- Matching ELF SHA-256: `d5ddf4ac3518ff76b413ec782b1fe342e6e5adadeede56adc6ac017403a4b650`.
- Verified image, ELF and MAP bundle: `build/firmware/3d888a6cdfef068cbabfe2b7a3bb6805791c69d246d04cd16a963e8869fd6854/`.

At that build checkpoint, no flash, commit, push or release had been performed for the landscape version.


## Landscape device failure and cache repair (2026-11-05)

The approved `3d888a6c…` landscape build was subsequently flashed through COM6
as an application-only update at `0x10000`, preserving the compatible NVS
partitions. esptool verified the write. Boot identified `0.1.1-shinuyakata`, ELF
prefix `d5ddf4ac3`, and landscape 320 x 240. The user reported slow navigation,
slow screen refresh and a crash in the gallery. **Device tests: FAIL** for that
build. Serial observation captured two stack-protection faults and reboots.
The matching archived ELF resolves PC `0x4204d5be` / `0x4204d5c0` to LVGL
TJPGD `decoder_info()`. Its 4 KB automatic buffer exceeded the remaining stack
in the 7,168-byte LVGL task. Text redraws also repeatedly ran the stock JPEG
area decoder from the start of the picture.

Version `0.1.2-shinuyakata` replaces that path with the bounded cache described
above, avoids rebinding unchanged image sources, and restores scene/thumbnail
contents when pages switch. It keeps stack protection and the existing task
stack sizes. Story data, choices and NVS formats are unchanged. Physical frame
timing, free heap and prolonged gallery stability remain unverified until this
exact repair build is approved and tested on the board.

The separate real-LVGL regression fixture under `tests/yakata_lvgl/` checks
primary colours, clipped repaint output, repeated redraws without JPEG decode,
actual typewriter ticks, same-scene page changes, all imported story images,
all 311 CG variants, all 56 thumbnail groups, reader restoration, both font
tiers and modal bounds. It uses the same 96 KB LVGL pool. It is supplementary
to the normal gate and requires a native C/C++ compiler and the resolved LVGL
dependency. Run it from an ignored build directory so its PPM captures stay local:

```text
cmake -S tests/yakata_lvgl -B build/yakata-lvgl -DCMAKE_BUILD_TYPE=Release
cmake --build build/yakata-lvgl
cd build/yakata-lvgl
./yakata_lvgl ../../main/yakata_data/yakata_art.bin
```


Repair-build verification completed under ESP-IDF v5.5.3:

- Build: PASS (`./tools/validate.sh`, log: `build/gate.log`).
- Host tests: PASS; the supplementary 32-bit real-LVGL fixture also passed all
  820 distinct story images, 311 CG variants, 56 thumbnail groups and repeated
  text redraws without JPEG decode (`build/yakata-lvgl-run.log`); pool peak 77%.
- Device tests: NOT RUN for `0.1.2-shinuyakata`.
- Unverified: physical refresh speed, remaining heap and gallery stability.
- Merged image: 8,180,800 bytes. Application: 8,115,264 / 8,323,072 bytes;
  207,808 bytes free.
- Full-image SHA-256: `9f7431233f1f7ad6d10ed7247d32fd9f9e542e1bd5667fb8d09b1ff5d2ea2dcd`.
- Matching ELF SHA-256: `5d3d7955ace9220b114949f86650fd6aaa10f899c9bab0fc004a0d9e033476eb`.
- Verified image/ELF/MAP bundle: `build/firmware/9f7431233f1f7ad6d10ed7247d32fd9f9e542e1bd5667fb8d09b1ff5d2ea2dcd/`.

COM6 still identifies the same USB board. The partition table is byte-identical
to the flashed `0.1.1` build, allowing an approved application-only write at
`0x10000` while keeping NVS. This repair build has not been flashed, committed,
pushed or published.


## Repair device check (2026-11-05)

After explicit approval, the verified `0.1.2-shinuyakata` bundle
`9f7431233f1f7ad6d10ed7247d32fd9f9e542e1bd5667fb8d09b1ff5d2ea2dcd`
was flashed through COM6 as an application-only write at `0x10000`. esptool
verified the data hash; bootloader, partition table and NVS were not written.
The application hash is
`369690c6350f5d6a76dcd5d62762f9d1640d60da5717c258ea522b0ba1a5572a`.

A controlled reset and 45-second serial observation identified version
`0.1.2-shinuyakata`, ESP-IDF v5.5.3 and matching ELF prefix `5d3d7955a`.
Landscape 320 x 240 and the 98,304-byte pixel arena initialized. Automatic
progress at event 15 / page 1 was still detected. The application reported
ready at 2.241 s with 32,404 bytes free heap and a 16,384-byte largest block.
Four observed main-image decodes took 66–86 ms each. These timings measure
image changes, not text-frame latency. No stack protection fault, panic,
watchdog, assertion or unexpected reset appeared during the observation.
The serial port was closed afterward. Raw logs remain local at
`build/flash-repair-9f743123.log` and `build/boot-repair-9f743123.log`.

The user then confirmed that speed was noticeably improved and the gallery
worked normally. Build: PASS. Host tests: PASS. Device tests: PASS for flashing,
startup and the reported reading/gallery check. Unverified: prolonged operation,
every CG variant on hardware, complete manual save/load, and unattended
screen-on/idle/sleep/wake behavior for this build. No commit, push or release
was performed.


## Immediate fast-forward (2026-11-05)

Version `0.1.3-shinuyakata` removes the 180 ms per-step fast-forward timer.
Long UP advances immediately and subsequent input-loop turns advance without
waiting for typing or chapter-card timing. Chapter cards retain the held skip;
release on either a story page or a card stops it. Choices, endings and menus
stop skip. Normal auto-reading retains its own 900/1,600 ms timing. No busy loop
was introduced, and the story graph, image cache and NVS format are unchanged.

Application tests exercise zero-elapsed-time progression, immediate start,
release on both page types, all 11 choice boundaries, ending/menu stops, a held
skip from New Game across chapter/flow nodes to the first choice, and normal
auto-read timing separately. Device acceptance of this version is pending.


The same `0.1.3` increment makes speaker badges fit the text width plus 8 px
horizontal / 2 px vertical padding, and reduces the dialogue panel from 90 to
80 px. Badges stay above the panel at either font tier; narration hides them.
Both font tiers show three lines per page. Pagination uses the actual label
font and the pinned LVGL line iterator, with page composition kept in a pure-C
helper. This also handles long punctuation runs that could wrap beyond an
approximate character-count boundary. The layout tag is now 2: older saves
replay their current line while retaining event, variables and unlocks.

A 32-bit real-LVGL check passed every page of all 17,409 text records at both
font tiers, including measured speaker-badge widths, panel bounds and unnamed
narration. The existing image-cache/gallery/modal checks also passed. Log:
`build/yakata-lvgl-compact-run.log`.


The combined `0.1.3-shinuyakata` increment passed the complete gate under
ESP-IDF v5.5.3 and independent archive verification:

- Build: PASS (`build/gate-reader-ui.log`).
- Host tests: PASS, including pure renderer-line page composition, immediate
  skip/stop boundaries, both older save-layout tags and the real-LVGL corpus run.
- Device tests: NOT RUN for this build.
- Unverified: physical badge sizing, compact-panel readability, held skip across
  chapters, release response, and on-board restoration of an older save.
- Merged image: 8,180,560 bytes. Application: 8,115,024 / 8,323,072 bytes;
  208,048 bytes free.
- Full-image SHA-256: `4f3b0d6777eb464c3114078f68b024d45ff19a60ceaf4ae06b9695f936f47318`.
- Matching ELF SHA-256: `8080e38226c8fd45a15824128a5ae536d22750142b330d7f1ea181f5f7fe742a`.
- Verified bundle: `build/firmware/4f3b0d6777eb464c3114078f68b024d45ff19a60ceaf4ae06b9695f936f47318/`.

COM6 still identifies the same USB board. The partition table is byte-identical
to the installed `0.1.2` build; an approved application-only write at `0x10000`
can retain NVS. An older saved subpage restarts its current line on first load
under layout tag 2. This build has not been flashed, committed, pushed or released.


## Version 0.1.3 device check (2026-11-05)

With explicit approval, COM6 received the verified `0.1.3-shinuyakata`
application at `0x10000` from bundle
`4f3b0d6777eb464c3114078f68b024d45ff19a60ceaf4ae06b9695f936f47318`.
Application SHA-256: `53b49bd61da254a026358881da655a55d6b68877967d596b8cdeacdaf4b3337d`.
esptool verified the write. Bootloader, partition table and NVS were not written.

A controlled reset and 45-second serial observation confirmed the version,
matching ELF prefix `8080e3822`, landscape 320 x 240 and the 98,304-byte pixel
arena. The earlier automatic progress at event 3501 / page 1 was detected.
The application was ready at 2.163 s, with 32,288 bytes free heap and a
16,384-byte largest block. Gallery/CG navigation produced bounded image-change
decode logs; no panic, stack fault, assertion, watchdog or unexpected reset
appeared. The serial port was closed afterward. Local logs:
`build/flash-013-4f3b0d67.log` and `build/boot-013-4f3b0d67.log`.

Asked to check content-sized speaker badges, the shorter dialogue panel and
immediate held fast-forward across chapters with release-to-stop, the user
confirmed that the result worked. Build: PASS. Host tests: PASS. Device tests:
PASS for flashing, startup and those user-observed changes. Unverified:
prolonged operation, exhaustive CG/font combinations, complete manual save/load
and unattended screen-on/idle/sleep/wake behavior. At the time of this device
check, no commit, push or release had been performed.

## Published release and community submission (2026-11-05)

The public release is [v0.1.3-shinuyakata](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.3-shinuyakata).
Its tag remains fixed at code commit
`9f98ec9c362794f651fada2722d49d11d23228a4`; later documentation commits do
not move the tag.

The [merged firmware download](https://github.com/Shinku-Chen/ai-passport/releases/download/v0.1.3-shinuyakata/FoloToy-AI-Passport-full.bin)
is the accepted local image, 8,180,560 bytes, with SHA-256
`4f3b0d6777eb464c3114078f68b024d45ff19a60ceaf4ae06b9695f936f47318`.
An anonymous public download matched that size and digest. Its matching ELF
SHA-256 is `8080e38226c8fd45a15824128a5ae536d22750142b330d7f1ea181f5f7fe742a`.
The verified bundle remains at
`build/firmware/4f3b0d6777eb464c3114078f68b024d45ff19a60ceaf4ae06b9695f936f47318/`.

The release uses the [official portrait cover](../../assets/images/shinuyakata-cover.png)
from the game's retail package artwork. The
[source record](../../assets/images/shinuyakata-cover-source.json) retains its
origin URLs and image hashes; artwork rights remain with CLEARRAVE / BUG SYSTEM.
The approved game introduction and sole upstream reference,
[xuanyide01/shinuyakata-rw5](https://github.com/xuanyide01/shinuyakata-rw5),
are used in the release and community descriptions.

The community submission is project `2018`, revision `4767`, slug `galgame-4`,
category `games`. Its server status is `pending`: submitted for review, with
public listing still pending. The submitted source URL is
[feature/port-shinuyakata](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-shinuyakata).

Local Build and Host tests remain PASS from the complete gate and corpus
checks above. The duplicate tag-triggered
[Build firmware run 42041421808](https://github.com/Shinku-Chen/ai-passport/actions/runs/42041421808)
was deliberately cancelled to preserve the tested release image; its terminal
state is `completed` / `cancelled`, so this Actions run is not recorded as PASS.
The published asset digest was checked again afterward and remained unchanged.
Device tests remain PASS for the previously accepted 0.1.3 application-only
flash and user checks above. The full image was not flashed again after
publication, and no new device regression is claimed. The remaining checks
listed in the preceding device-check section are still unverified.
