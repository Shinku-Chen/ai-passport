<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Shengzi Cards

A Chinese-character flashcard memorization app. Three modes — **Browse**
(scroll the character cards), **Self-test** (mark each character learned / not
learned), and **Spell** (see the pinyin and guess the character). A short **OK**
reveals the answer; learned marks persist to NVS. Latest: **v1.0.0**.

- Branch: [`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards)
- Release: [v1.0.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.0)

## Notes

- This branch carries one application. A `feature/*` branch README describes only
  its own application; the fork `main` root README is the catalog of every hosted
  project.
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
