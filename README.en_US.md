<p align="right">
  <a href="README.md">Chinese</a> · <strong>English</strong>
</p>

# 9-nine- Sorairo Sorauta Soranooto - AI Passport Offline Reader

The Chinese story, art, music and complete opening of *9-nine- Episode 2* on a
pocket-sized, three-key reader. After boot, the story, saves, gallery and opening
play from local resources; no network is required.

This branch contains **only this title**. The release version is
`v1.0.0-nine-sorairo`.

## Story and play

The story follows Sora and her friends. Use the three physical keys to advance
dialogue, make choices with UP/DOWN and OK, and move through long sentences page
by page without losing a whole line to one press. From the in-game menu you can
save to one of three slots, load an earlier point, browse history, or change the
font size and pagination.

The gallery collects CG, story recall, chapters, music and a dedicated
**OP 鉴赏 / Opening** row. The opening can be watched without clearing the story;
natural completion, a short press or a long press returns to the same row.
Gallery browsing stays silent, and returning to the title restores the title
music.

## Content

| Item | Count |
| --- | ---: |
| Story nodes | 56 |
| Text frames | 10,385 |
| Chapters | 21 |
| Choice groups / options | 4 / 7 |
| Ending metadata | 2 |
| Images | 1,102 |
| CG groups / variants | 22 / 72 |
| Opening | 623 frames at 5 fps, 124.66 s |
| Audio | 12 kHz mono IMA ADPCM; title and opening tracks |

## Controls

| Page | Short press | Long press |
| --- | --- | --- |
| Reading | OK / UP / DOWN immediately advances one complete unit: the next page of a long sentence, otherwise the next sentence | OK opens the menu; DOWN toggles auto-play; hold UP to fast-forward, release stops immediately |
| Choice | UP/DOWN select, OK confirms | OK opens the menu; returning keeps the complete choices and selection |
| In-game menu | UP/DOWN select, OK runs | OK resumes the story |
| Save / Load | UP/DOWN selects one of three slots, OK saves or loads | OK goes back |
| Settings | UP/DOWN selects a row, OK toggles | OK goes back |
| Gallery / CG / chapters / music / opening / about | UP/DOWN selects or scrolls, OK enters | OK goes up one level |

The title menu contains **New Game**, **Continue**, **Load**, **Settings**,
**Gallery** and **About**. Open the Gallery's independent opening row to watch the
opening; natural completion, a short press or a long press returns to the Gallery.

## Build and reproduce

ESP-IDF 5.5.3, Python 3, Pillow and fontTools are required. The images and fonts
come from the pinned upstream source
(`https://github.com/xuanyide01/9-nine-rw5`, commit
`6a8907a5ae576f4377abe4b74f15bd9af13efe07`):

```bash
python tools/nine_pack.py \
  --source /path/to/9-nine-rw5 \
  --out assets/nine \
  --jpeg-quality 42 --op-fps 5 --audio-rate 12000 \
  --expect-commit 6a8907a5ae576f4377abe4b74f15bd9af13efe07

python tools/nine_font.py \
  --font /path/to/9-nine-rw5/nine-sorairo-rw5/src/common/nine-text.otf \
  --out assets/nine --source-root main

./tools/validate.sh --static
./tools/validate.sh --firmware   # after activating ESP-IDF 5.5.3
```

The build uses `tools/nine_container.py` to generate `nine_assets.bin` and then
merges `FoloToy-AI-Passport-full.bin`. Incremental `idf.py build` alone is not a
substitute for the verified merged image.

## Current verification scope

- **Build: PASS** (ESP-IDF 5.5.3).
- **Host tests: PASS** (pack/VM, branches, key timing, saves, audio, images,
  fonts and source-trace differential).
- **Device tested**: for `9a06ff27…`, the complete Gallery opening's picture,
  audio and return were USER PASS; fast-forward release, save/load, font size and
  pagination were USER PASS.
- **Unverified**: complete on-device traversal of every branch/ending, long-run,
  power-loss and sleep stability, and every page not individually retested. Host
  tests and a scoped device pass are not a whole-device guarantee.

An earlier `9f985c49…` candidate had an OP audio stutter. That old failure is
retained in the development archive and does not describe this tested `9a06ff27…`
opening result.

## Resources and artifacts

| Item | Actual |
| --- | ---: |
| `nine_script.bin` | 737,234 B |
| `nine_images.bin` | 4,366,920 B |
| Script + images | 5,104,154 B |
| `nine_assets.bin` (generated) | 5,104,264 B |
| Assets partition | 6,225,920 B (82.0% used) |
| factory app | 1,474,928 / 2,097,152 B (70.3% used) |
| Merged full image | 7,266,952 B |
| DRAM (link time) | 231,106 / 321,296 B (90,190 B free) |
| Art canvas | 240x135 RGB565, five 240x27 bands, 12,960 B each; displayed at 320x180 |

Release SHA-256:

```text
full merged .bin   9a06ff27a9798137afa1684b89ca45305fcfccea9852c9d88c2f8df28cb6d007
app .bin           2386bf86d5bdebd0d6517c30ed6d881ee0526aacc4ec6cc92a6edee93310ab1b
ELF                2bc20795791e233b0cf7e17c6ec31775c0c7a26b994fe066d761f0c21000c66e
MAP                19f00fe400f281a7334c61b57d07a0216660084997843d0b9b1627d386701c6f
nine_script.bin    e25d48f371a43a60bcc4df2db51c9caaf923e99efaf2df8abcb99e65503e1f8c
nine_images.bin    c05fe781a006d8656d4b66c7f854761a3747f293ce519d27fb2456ef795dec28
```

## Sources and attribution

- Story and processed source: `https://github.com/xuanyide01/9-nine-rw5`, commit
  `6a8907a5ae576f4377abe4b74f15bd9af13efe07`.
- Firmware source branch:
  `https://github.com/Shinku-Chen/ai-passport/tree/release/nine-sorairo-20261125`.
- Upstream firmware project: `https://github.com/FoloToy/ai-passport`.
- The source repository's code is MIT. Game text, art and music remain the
  property of **Palette / 9-nine- Project**. Assets are included for personal
  offline use; this repository does not grant redistribution rights.

## Documentation

- [Port and implementation notes (English)](docs/games/nine-sorairo.md)
- [移植与实现说明（中文）](docs/games/nine-sorairo.zh_CN.md)
- [Development and build rules](AGENTS.md)

Device results apply only to the exact build and scope above; the repository's
implementation notes and tests remain the reference for the complete code
contract.
