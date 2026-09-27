<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Shengzi Cards

A Chinese-character flashcard app: a scrollable card list, a self-test that records
what you already know, and a spelling drill, with learned marks persisted to NVS.
Latest: **v1.0.0**.

- Branch: [`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards)
- Release: [v1.0.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.0)

## Controls

The three keys drive everything, and the status line shows the current mode, the card
position and how many cards are marked learned.

- **UP / DOWN (hold)** cycles the mode: `BROWSE` → `TEST` → `SPELL`.
- **BROWSE** — the card shows its character and pinyin: **UP** goes back one card,
  **DOWN** jumps to a random card.
- **TEST** — the pinyin stays hidden until **OK** reveals it; after the reveal **UP**
  marks the card as known and **DOWN** as unknown, and either choice moves on to a
  random card.
- **SPELL** — a drill pass without marking: the pinyin stays hidden until **OK**, and
  **OK** again moves on to a random card.

## Notes

- This branch carries one application. A `feature/*` branch README describes only
  its own application; the fork `main` root README is the catalog of every hosted
  project.
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
