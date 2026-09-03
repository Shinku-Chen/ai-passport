<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# FoloToy AI Passport — Shinku-Chen's Fork

This is a personal fork of [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport).
The upstream repository is the development baseline for the [FoloToy AI Passport](https://ai-passport.folotoy.cn)
— an open wearable AI device (ESP32-C3, 240×320 display, three keys, 8 MB Flash, no PSRAM).

This fork carries **several independent applications** built on that baseline. Each
project lives on its own `feature/*` branch and is introduced below. Board facts,
the BSP, and the development workflow come from upstream — see
[`docs/README.md`](docs/README.md), [`AGENTS.md`](AGENTS.md), and
[`docs/contribution/`](docs/contribution/). Released firmware for each project is
attached to this repository's [Releases](https://github.com/Shinku-Chen/ai-passport/releases).

## Projects

### Voice Keychain

A sound-effects keychain that turns the AI Passport into a pocket audio player:
boot straight into the app and play one of **hundreds of Chinese voice clips from
dozens of character packs** — jojo, meme cat, Liu Huaqiang, Haji Mi, Nailong,
and more. Latest: **v1.3.0**.

- Branch: [`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain)
- Releases: [v1.1.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.1.0), [v1.3.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.3.0)
- Experience notes: [`docs/reference/shinku-chen/voice-keychain/`](docs/reference/shinku-chen/voice-keychain/)

**Controls (three keys):** UP / DOWN to move in a list, **OK** to enter a
directory, select a clip, or play it, and **OK (hold)** for settings (volume,
battery) or to go back.

**Highlights (v1.3.0):**

- **Self-contained firmware** — `FoloToy-AI-Passport-full.bin` bakes the
  `voicefs` data partition (at `0x210000`) into one 8 MB image; flash from `0x0`
  and nothing else is needed.
- **Deep-sleep wake fixed** — the GPIO0 wake source was never armed (a pin
  number was passed where a bitmask is required); buttons could not wake the
  device. Now it sleeps after 5 min idle and wakes on any key (verified on device).
- **Reliable list playback** — pressing OK used to stop the current sound but
  not play the selection (a fresh 16 KB Opus-decode stack per play failed under
  heap pressure); replaced with one persistent player task on a static stack.
- Battery percentage refresh every 30 s, plus a voltage-fallback SOC estimate
  when the CW2017 gauge returns `0xFF` after power-up.

### What to Eat Today

A button-driven food roulette that answers the eternal question. Hold **UP** to
run the "what should we eat for lunch?" guide animation, hold **DOWN** to spin
through the food selector, and release to stop on a random pick. Latest: **v1.2.0**.

- Branch: [`feature/cheerful-goodall`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cheerful-goodall)
- Release: [v1.2.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.2.0)
- Experience notes: [`docs/reference/shinku-chen/eat-what/`](docs/reference/shinku-chen/eat-what/)

**Controls:** hold UP / DOWN to run the two animations, release to stop on the
current frame; **OK** toggles LVGL partial vs fast interlaced refresh.
Auto-poweroff after 2 min idle (deep sleep, GPIO0 wake).

### Shengzi Cards

A Chinese-character flashcard memorization app. Three modes — **Browse**
(scroll the character cards), **Self-test** (mark each character learned / not
learned), and **Spell** (see the pinyin and guess the character). A short **OK**
reveals the answer; learned marks persist to NVS. Latest: **v1.0.0**.

- Branch: [`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards)
- Release: [v1.0.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.0)

## Notes

- Each application is a separate `feature/*` branch off the upstream baseline.
  Do not merge demo branches wholesale into `main`; port reusable patterns
  instead (see upstream `AGENTS.md`).
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
