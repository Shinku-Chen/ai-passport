<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Manosaba: The Witch Trial of Magical Girls — AI Passport Landscape Reader

This branch ports the Redmi Watch 5 (Vela) adaptation of *Magical Girl no Majo
Shinpan* to the [FoloToy AI Passport](https://ai-passport.folotoy.cn) (ESP32-C3,
320×240 landscape, three buttons, 8 MB Flash, no PSRAM). It is a standalone
application, not a reskin of the upstream demo menu.

- Project notes: [`docs/games/manosaba.md`](docs/games/manosaba.md)
- Branch: `feature/port-manosaba-landscape`
- Source snapshot: `manosaba-rw5 @ a309a3614837b555741e5d8fd7cfe2b9428e2e49`
- Language: Simplified Chinese; complete 16px subset (2,999 glyphs, 0 missing)

## Release

- **Version**: tag [`v1.0.0-manosaba`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.0-manosaba),
  branch [`feature/port-manosaba-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-manosaba-landscape); merged image
  `FoloToy-AI-Passport-full.bin` (app 7,763,280 B / full 7,959,888 B, version `1.0.0`).
- **Layout**: 320x240 landscape.
- **Three keys**: reading - UP advances, hold UP fast-forwards, DOWN opens history, hold DOWN toggles auto-read, OK advances, hold OK opens the menu; in lists and choices UP/DOWN move, OK confirms and hold OK goes back.
- **Features**: multi-slot saves plus an automatic slot, load, CG gallery, chapter list, scrollable history, auto-read and instant fast-forward.
- **Idle**: dim after 60 s, screen off after 300 s, deep sleep after 1800 s; any key wakes the device and resumes the session. Auto-read and fast-forward never idle out.
- **Known limits**: flash use is 94.77% with 428,720 B free; still confirm the budget before changing content.

## Implemented

- **All 458 source script files / 424 scenes / 90,243 commands** are packed into
  one read-only flash image and executed by a C VM that mirrors `engine.js` /
  `scene.js`: background layers, sprite instances, focus, conditions,
  `call`/`return`, `offer`/`choice`, map investigations, evidence submission,
  objection words, trial damage, book versions, autosave points, chapter
  entries, and both source and chapter entry modes.
- **Complete art**: 62 backgrounds, 172 CGs in 61 groups, 151 portraits, 19
  trick images, 151 book images, 2 solid placeholders and 2 title images
  (559 total), fitted to 320×240 without
  cropping; portraits use JPEG plus a 1bpp alpha mask.
- Dedicated landscape UI: title, reader, choices, menu, history, save/load,
  settings, gallery/full-screen CG, chapter select, witch book, endings,
  about, and error pages.
- 30 manual slots plus one independent autosave slot; version/length/CRC
  validation, failed writes never replace a valid slot. Gallery/chapter
  unlocks use a separate versioned store that only grows and retries failed
  writes.
- Three-button semantics: UP advances (long press fast-forwards), DOWN opens
  history (long press toggles auto), OK opens the menu/objection list; lists
  use UP/DOWN + OK and long OK to return; choices require explicit OK.

## Build

ESP-IDF 5.5.3 and Python 3 (Pillow for assets/fonts) are required.

```bash
python tools/manosaba_pack.py --source <manosaba-rw5> --out main/gal_data/manosaba.pack
python tools/manosaba_font.py --font <manosaba-rw5>/src/common/manosaba.otf \
    --fallback-font "$WINDIR/Fonts/NotoSansSC-VF.ttf" \
    --pack main/gal_data/manosaba.pack --source-root main --out-dir assets/fonts
idf.py build
idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build
```

Host tests: `CC="$PWD/tools/cc-host.sh" MANOSABA_SOURCE=<manosaba-rw5> \
bash tools/validate.sh --static`. On this Windows machine the
`install_passport_skills` symlink test is skipped, so the full static gate is
PARTIAL; all C tests in this project, the independent source audit, and the
real-TJpgDec image regression are actually executed.

## Current validation

- Software review: **PASS; awaiting device acceptance** (production frozen at full `7f90c4e1…`)
- Build: PASS (app 7,442,288 B, full 7,638,896 B; actual factory@0x30000, 749,712 B free; NVS@0x9000/0x26000)
- Host tests: **PARTIAL** (gate PASS=42 FAIL=0 SKIP=1; only the Windows `install_passport_skills` symlink test is SKIPped)
- Device tests: **NOT RUN / awaiting device acceptance** (no flashing or on-device acceptance)
- Source audit: PASS (657,333 checks; 458 files / 90,243 commands / 5,586
  expression comparisons)
- Archive: `build/firmware/7f90c4e15f1bc4f8ff179759123b50b9c63fcdf17c1629200c791e7df538e976/` (old `a14d…`/`99b…` kept)

## License and provenance

The upstream repository and BSP keep their own licenses. This port is an
independent implementation; the original game's script, art, characters, and
trademarks belong to their respective owners. The project does not claim a
right to redistribute original assets. The font source is the snapshot's
Noto Sans Mono CJK SC (SIL OFL 1.1); five missing glyphs come from the system
Noto Sans SC (SIL OFL 1.1). See [`docs/games/manosaba.md`](docs/games/manosaba.md).
