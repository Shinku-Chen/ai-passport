<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Galgame reader framework (pending/galgame-framework)

This branch carries the shared reader framework used by the AI Passport visual-novel
ports. It is not a flashable application; it collects the page, interaction,
pagination, save and packing contracts that the application branches reuse.

## What it provides

- **One three-key contract**: short OK / UP / DOWN advances or completes text;
  long OK opens the menu; long UP fast-forwards; long DOWN toggles auto-read.
  Lists and choices use short UP / DOWN to move and short OK to confirm.
- **Pagination and dialogue panel**: fixed-pixel layout with full-width / half-width
  units; the translucent panel is flush with the bottom edge and the page counter
  never covers the body.
- **Saves and progress**: several manual slots plus an automatic slot, with run
  state kept separate from permanent clear records.
- **Offline packs**: script, images and fonts are packed into the application
  partition, so a clone builds a complete title with no network or SD card.
- **Banded rendering**: art decodes into horizontal bands so memory does not grow
  with the full screen; JPEG uses the ROM decoder, sprites use RGB565 plus a
  1 bpp mask.
- **Standard screens**: menu, save/load, history, chapters, gallery, settings and
  about.

## Titles

The twelve v1.0.0-batch ports share this framework; `galgod` and `manosaba` carry
their own reader / UI layer. The 2026-11-25 batch keeps the same page, key and
persistence contract.

## Documentation

- [Galgame reader porting notes](docs/reference/shinku-chen/galgame-port/README.md)
  — shared requirements, framework capabilities, twelve hardware-only defects
  and the pre-release checklist.
- [Newer galgame port lessons](https://github.com/FoloToy/ai-passport/blob/main/docs/reference/shinku-chen/newer-galgame-port-lessons.md)
  — source pinning, preview isolation, banded budgets and story-graph
  reconciliation from the later eight ports.

## Status

This is a `pending/*` branch and is not published to the community as an
application. It is a development baseline, not a ready-to-flash firmware; use the
matching `feature/*` branch for a playable title.

## Upstream

- Repository: [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport)
- Baseline: `upstream/main`, maintained under the fork's `feature/*` /
  `pending/*` convention.

