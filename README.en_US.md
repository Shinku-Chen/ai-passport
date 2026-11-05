<p align="right"><a href="README.md">简体中文</a> · <strong>English</strong></p>

# Shinuyakata GalGame

An offline landscape reader for AI Passport, adapting the RW5 runtime content of *Shiniyuku Kimi, Yakata ni Mebuku Zouo*. Read the story, choose branches, save your place and revisit unlocked CGs.

![Official package artwork](assets/images/shinuyakata-cover.png)

Current version: **0.1.3-shinuyakata**. This branch maintains this reader; see the [other applications](https://github.com/Shinku-Chen/ai-passport/tree/main) on the fork's main branch.

## Download and install

- [Release: v0.1.3-shinuyakata](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.3-shinuyakata)
- [Merged firmware: FoloToy-AI-Passport-full.bin](https://github.com/Shinku-Chen/ai-passport/releases/download/v0.1.3-shinuyakata/FoloToy-AI-Passport-full.bin)
- [Official web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)

For first installation, flash the merged image at `0x0`. A full-image write may clear stored data. To retain saves when upgrading a development build, use compatible component images under the [firmware and stored-data policy](docs/development/engineering/firmware-layout.md). Older portrait or taller-dialogue saves keep their event and route variables, replaying their current line under the new pagination.

## Reading and controls

- On the title screen, UP/DOWN selects an action and OK starts or continues. No network setup is needed.
- During reading, short OK/UP/DOWN completes animated text, then advances a page.
- Long OK opens the menu. Hold UP to start immediate fast-forward across chapters; release, choices and endings stop it.
- Long DOWN toggles auto-reading. It keeps the screen awake, including at choices. An action key first stops auto-reading.
- The menu offers three manual save slots, loading, history, font size, text speed and dialogue transparency. Automatic progress is used by Continue.
- The gallery includes unlocked CGs, recalls and chapter entries. In a CG viewer, UP/DOWN changes variants and OK returns.

Speaker badges fit their text. The 80 px dialogue panel shows three lines at either font tier. The supplied dataset has 198 runtime pages, 17,409 text records, 11 choice groups and 56 groups / 311 CG variants.

## Content and validation

The title contains adult, violent and horror content. The source dataset supplies no voice, BGM or OP video, so this is a silent reader. Unresolved/unreachable source nodes are retained; completeness of the original commercial game is not independently guaranteed. See the [reader notes](docs/assets/shinuyakata.md).

Build and Host tests: PASS. On-device checks of 0.1.3 confirmed reading/gallery behavior, speaker badges, the compact dialogue panel and immediate fast-forward. Prolonged operation, exhaustive route coverage and sleep/wake still need a full physical regression. The release uses the verified local image, SHA-256:

`4f3b0d6777eb464c3114078f68b024d45ff19a60ceaf4ae06b9695f936f47318`

## Build and attribution

Activate ESP-IDF 5.5.3 and run from the repository root:

```bash
./tools/validate.sh
```

This creates and verifies `build/FoloToy-AI-Passport-full.bin`. Generated content, font subsets and pinned materials are retained; a normal build does not require reimporting them. See the [offline rebuilding notes](docs/assets/shinuyakata.md#offline-rebuilding) to regenerate data.

- Base: [GalGame framework](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework), `7f04b554`.
- Runtime source: [xuanyide01/shinuyakata-rw5](https://github.com/xuanyide01/shinuyakata-rw5), pinned to `76bba0bb0bfcc066ce6a35883d76c5b19981e0cb`.
- Original game/art: BUG SYSTEM / CLEARRAVE. The cover uses official retail package art; materials retain their original rights.
- Original source-port code: MIT. Noto Sans SC font: SIL Open Font License. Those licenses do not grant rights to the commercial materials.

[Reader and release notes](docs/assets/shinuyakata.md) · [Contribution conventions](docs/contribution/commit-and-pr.md)
