<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Asunabi

A portrait visual novel ported to the AI Passport from
[`liuyuze61/Asunabi-miband`](https://github.com/liuyuze61/Asunabi-miband), a Xiaomi
Band release: **30 chapters, 4,649 dialogue lines and about 94,000 characters**,
read straight through to a single ending.

This is the application built on this `feature/asunabi-galgame` branch. The
firmware boots straight into the title screen (no demo menu).

## What it does

- **Reading** — the confirm key advances a line, or reveals the rest of a line
  that is still being typed. A line too long for the panel is paginated, not
  clipped.
- **Title screen** — start a new read-through, continue from the stored position,
  jump to any chapter, or open the settings.
- **Reader menu** — resume, save, load, skip the current chapter, open the
  settings, or return to the title.
- **Save slots** — six manual slots that include the position within a paginated
  line; hold the confirm key on a slot to delete it. The last position is also
  remembered automatically, so "continue" resumes where you stopped.
- **Settings** — text speed (slow / medium / fast / instant), text size (16 px or
  20 px) with a live typewriter preview, and auto-play.
- **Ending screen** — the story has one ending; reaching it clears the resume
  point and offers a return to the title.

## Interaction

Three keys drive the whole app. The battery percentage sits in the top-right
corner and degrades to `--%` when the gauge cannot be read.

| Key | Reading | In a menu |
| --- | --- | --- |
| UP (short) | next line, or reveal the rest of the current one | move the selection up |
| UP (hold) | fast-forward while held; stops the moment you let go | — |
| OK (short) | open the menu | activate the selected row |
| OK (hold) | — | leave the menu |
| DOWN (short) | scroll a line that runs past the panel | move the selection down |
| DOWN (hold) | hide the panel to look at the artwork | — |

## Assets

The artwork and chapter scripts come from
[`liuyuze61/Asunabi-miband`](https://github.com/liuyuze61/Asunabi-miband) and are
**third-party content**. That repository declares no license, so none of it is
redistributed here — attribution is the whole of what this branch can offer in
return, and it is why the material lives outside the repository.

It is kept in a local, git-ignored `assets/gal-source/` tree; the build packs it
into a dedicated 4 MiB `assets` data partition through
[`tools/gal/`](tools/gal/README.md), which is the reusable part of this branch:
a documented pack format, a packer with a visual preview, an inspector, and the
CJK font subset generator.

Two consequences are worth knowing before building or releasing:

- A clone without `assets/gal-source/` still configures, builds and boots — the
  packer emits a small placeholder pack instead. That firmware shows a placeholder
  script and no artwork.
- **A CI-built release therefore does not contain the game.** Release this branch
  from a locally built merged image, not from the tag-triggered CI artifact.

## Firmware / build

This branch replaces the demo menu with the galgame: `main/gal/` (the pack reader
and typesetting model, the memory-mapped asset layer, NVS saves, and the
key-driven UI), a rewired `main/main.c`, a dedicated `assets` data partition
(4 MiB, data subtype `0x40`), and two committed CJK font subsets under
`assets/fonts/`.

Packing the real artwork needs Pillow in the interpreter ESP-IDF builds with:

```bash
"$IDF_PYTHON_ENV_PATH/Scripts/python.exe" -m pip install Pillow   # Windows
python -m pip install Pillow                                       # Linux/macOS
```

Then verify the merged image, which is what a release should ship:

```bash
./tools/validate.sh --firmware      # -> build/FoloToy-AI-Passport-full.bin
```

`./tools/validate.sh --static` runs the host tests, including the pack reader,
the advance rules and the pagination.

## Source

- **Branch**: [`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame)
- **Upstream work**: [`liuyuze61/Asunabi-miband`](https://github.com/liuyuze61/Asunabi-miband) — the Xiaomi Band quick-app release this port is based on. No license is declared there, which is why its artwork and chapter scripts are not redistributed in this repository.
- Asset pipeline: [`tools/gal/README.md`](tools/gal/README.md)
