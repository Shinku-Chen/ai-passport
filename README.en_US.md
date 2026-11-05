<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong> · <a href="README.zh_CN.md">简体中文（独立文件）</a>
</p>

# FoloToy AI Passport — Shinku-Chen's fork

This repository is a personal fork of [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport).
Upstream is the development baseline for [FoloToy AI Passport](https://ai-passport.folotoy.cn):
an **ESP32-C3** wearable with a **240×320** colour display, **three keys**,
**8 MB of flash and no PSRAM**.

The fork hosts several independent applications; each lives on its own `feature/*` branch
and publishes firmware through this repository's
[Releases](https://github.com/Shinku-Chen/ai-passport/releases). Every branch README
describes only its own project — this file is the overview. Board facts, the BSP and the
development workflow come from upstream: see [`docs/README.md`](docs/README.md),
[`AGENTS.md`](AGENTS.md) and [`docs/contribution/`](docs/contribution/).

---

## 1. Visual-novel readers — v1.0.0 batch (12 titles)

Script, sprites, backgrounds, CGs and the CJK font subset are packed into the application
partition, so a plain clone builds a complete, playable firmware with no SD card and no
network access.

| Title | Branch | Release | Orientation | Flash usage |
|---|---|---|---|---|
| anemoi | [`feature/port-anemoi-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-anemoi-landscape) | `v1.0.0-anemoi` | Landscape | 99.01% |
| Steins;Gate | [`feature/port-steins-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins-landscape) | `v1.0.0-steins` | Landscape | 97.35% |
| Steins;Gate 0 | [`feature/port-steins0-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins0-landscape) | `v1.0.0-steins0` | Landscape | 95.47% |
| CLANNAD | [`feature/port-clannad-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-clannad-landscape) | `v1.0.0-clannad` | Landscape | 97.92% |
| Summer Pockets RB | [`feature/port-summer-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-summer-landscape) | `v1.0.0-summer` | Landscape | 91.86% |
| Mahou Shoujo no Majo Saiban | [`feature/port-manosaba-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-manosaba-landscape) | `v1.0.0-manosaba` | Landscape | 94.77% |
| PARQUET | [`feature/port-parquet-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-parquet-landscape) | `v1.0.0-parquet` | Landscape | 86.29% |
| NianNiang Love Story | [`feature/port-nianniang-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-nianniang-landscape) | `v1.0.0-nianniang` | **Portrait** | 21.01% |
| Galgame Master | [`feature/port-galgod-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-galgod-landscape) | `v1.0.0-galgod` | Landscape | 39.48% |
| eden* | [`feature/port-eden-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-eden-landscape) | `v1.0.0-eden` | Landscape | 93.54% |
| Stella of the End | [`feature/port-stella-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-stella-landscape) | `v1.0.0-stella` | Landscape | 99.20% |
| Paper no Mahou Tsukai | [`feature/port-paper-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-paper-landscape) | `v1.0.0-paper` | Landscape | 75.56% |

**Shared features**: typewriter with adjustable speed/font size · auto-advance ·
fast-forward (instant text, no inter-line pause) · save/load slots plus an auto slot ·
history · chapter jump · CG gallery · choices · idle policy (dim 60–90 s, backlight off
300 s, deep sleep 1800 s; never sleeps while auto/fast-forward runs).

**Orientation**: only `nianniang` renders in portrait 240×320; the other eleven are
landscape 320×240. The `-landscape` branch suffix is historical naming only.

**Flash budget**: `anemoi` (82,224 B free) and `stella` (66,992 B free) are close to the
partition limit, `clannad` has 173,056 B free — check the budget before touching them.

---

## 2. Earlier visual-novel ports (9)

- **Asunabi** — [`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame), `v0.1.0-asunabi`
  Portrait; 30 chapters, 4,649 lines (~94k characters), single ending. Six manual save
  slots, chapter jump, text-speed and font-size settings. Ported from a Mi Band quick app;
  assets ship with the branch and credit the upstream project.
- **ATRI -My Dear Moments-** — [`feature/atri-reader`](https://github.com/Shinku-Chen/ai-passport/tree/feature/atri-reader), `v1.0.2-atri-reader`
  Portrait; 34 chapters, 1,069 scenes, 12,188 lines, three endings. Script, backgrounds,
  full-body sprites and the CJK font subset live in one merged image, fully offline.
  Its interaction patterns were reused by most later readers.
- **DRACU-RIOT!** — [`feature/dracu-riot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dracu-riot), `v0.1.0-dracu-riot`
  Portrait; five heroine routes with after-stories and endings, 62 chapter nodes,
  244 event CGs, 85 backgrounds. Ported from a Mi Band fan port; assets belong upstream.
- **limelight lemonade jam** — [`cindy/curious-babbage`](https://github.com/Shinku-Chen/ai-passport/tree/cindy/curious-babbage), `v0.1.0-limelight`
  Portrait; 230 chapter points, 68,229 lines, 991 images, 8 choices; two content packs
  (4.49 + 1.81 MiB) and 3,449 glyph codepoints. Upstream declares no license; personal,
  non-commercial use only.
- **Sanoba Witch** — [`feature/sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch), `v0.1.0-sanoba-witch`
  Portrait; 101 chapters, 53,190 lines (~1.1M characters), five heroine routes and five
  endings.
- **Saya no Uta** — [`feature/saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/tree/feature/saya-no-uta), `v0.1.1-saya-no-uta`
  Landscape; 44 chapters, 3,828 lines, three endings; community port with Chinese text.
- **Senren \* Banka** — [`feature/senren-banka`](https://github.com/Shinku-Chen/ai-passport/tree/feature/senren-banka), `v0.1.0-senren-banka`
  Portrait; 92 backgrounds, 123 sprites and 570 event CGs — the whole story lives in the
  firmware.
- **Hoshizora Tetsudou to Shiro no Tabi** — [`feature/starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/tree/feature/starry-sky-railroad), `v0.1.0-starry-sky-railroad`
  Portrait; 39 chapters, 1,260 scenes, 13,787 lines, one choice and a single ending;
  shipped together with its Chinese fan translation.
- **TSXX RE-BOOT** — [`feature/tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tsxx-reboot), `v0.1.0-tsxx-reboot`
  Portrait; 45 chapters, 61,436 pages, 61,289 lines (~1.16M characters), 13 choices and
  15 endings; 6.26 MiB content pack with 3,417 codepoints.

---

## 3. Audio and connectivity

- **Voice Keychain** — [`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain), `v1.15-voice-keychain`
  A pocket sound-effect player: boots straight into a character list with dozens of packs
  and hundreds of Chinese voice clips (jojo, meme cat, Liu Huaqiang, Haji Mi, Nailong…).
  **UP/DOWN** move, **OK** plays, **long OK** opens settings (volume, battery).
  The firmware is self-contained: the `voicefs` data partition (`0x210000`) is merged into
  the image, so a single flash from `0x0` is enough. Standalone, offline.
- **Pocket Intercom** — [`feature/openclaw-intercom`](https://github.com/Shinku-Chen/ai-passport/tree/feature/openclaw-intercom), `v1.14-intercom`
  Hold **OK** to talk (LED red → green); a companion Android app relays the speech to an AI
  assistant on your LAN and shows the answer on the device and the phone. The device itself
  has no Wi-Fi client, no HTTP and stores no keys; audio goes over BLE as Opus (~3 KB/s).
  Backend can be OpenClaw or any OpenAI-compatible endpoint.
  **Requires the Android app plus a reachable LAN backend.**
- **DLNA Audio Receiver** — [`feature/dlna-receiver`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dlna-receiver)
  Turns the device into a DLNA renderer on the local network, fed by a phone or desktop.
  **Requires a LAN and a casting source.**
- **DLNA Player** — [`feature/netease-dlna-player`](https://github.com/Shinku-Chen/ai-passport/tree/feature/netease-dlna-player)
  DLNA casting playback: boots straight into the player, MP3 decoded on-device. Works with
  NetEase Cloud Music and other casting sources.

---

## 4. Small games and utilities

- **Connect Four** — [`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four), `v1.7.0-connect-four`
  Landscape 10×7 board. **UP/DOWN** pick a column, **OK** drops, **long OK** returns to
  settings; three AI levels or two-player pass-and-play, plus two-device BLE play.
  Standalone, offline.
- **What to Eat Today** — [`feature/cheerful-goodall`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cheerful-goodall)
  A button-driven food roulette: hold a key to cycle recommendations, release to land on
  one; boots straight into the app. **Long OK** switches the render path. Standalone.
- **Tuner** — [`feature/tuner`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tuner)
  Twelve-tone equal-temperament tuner using the onboard microphone; AUTO and MANUAL modes,
  **long OK** opens a debug page with gain adjustment. Standalone.
- **Shengzi Cards** — [`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards), `v1.0.0`
  Flash-card drill for Chinese characters with BROWSE / TEST / SPELL modes and NVS-backed
  progress. Standalone.

---

## 5. Framework and experience docs

- **`feature/galgame-framework`** — shared capabilities and pitfalls of the twelve ports:
  [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port)
  (English + Chinese): requirements, capabilities, twelve "symptom → root cause → fix"
  entries, an acceptance checklist and the branch table.
- **`docs/*` branches** — topic notes (deep sleep and wake, display refresh, CJK font
  subsets, packing budgets, three-key event semantics, linear page tables, compositing
  trade-offs). Bodies live in `docs/reference/shinku-chen/` as `.md` + `.zh_CN.md` pairs.

---

## Three-key controls (readers)

| Key | Reader / main screen | List / choice screen |
|---|---|---|
| **OK short** | Advance to the next line, or confirm | Confirm |
| **OK long** | Open the menu | Go back |
| **UP short** | Advance / move up | Move up |
| **UP long** | Fast-forward (stop on release) | — |
| **DOWN short** | Text history / move down | Move down |
| **DOWN long** | Toggle auto-advance | — |

> Per-app key maps live in each branch README (e.g. Connect Four uses UP/DOWN to pick a
> column; Voice Keychain uses OK to play).

---

## Build and flash

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o <absolute>/FoloToy-AI-Passport-full.bin
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

- The merged image flashes from `0x0` in one go; the version lives in `esp_app_desc` and the
  boot log shows `App version: 1.0.0`.
- If flashing fails with `Packet content transfer stopped`, retry with `--no-stub` at a
  lower baud rate.
- Each branch archives its firmware under `build/firmware/<full sha256>/` (elf, map,
  `flash_args`, `manifest.json`).

## Release conventions

- Tags are `v<version>-<project>` (e.g. `v1.0.0-stella`, `v1.15-voice-keychain`).
  `v1.0.0` alone is the older Shengzi Cards demo, unrelated to this batch.
- Every release ships a self-contained `FoloToy-AI-Passport-full.bin`.
- Branch names do not imply orientation; check the branch README.

## License

Upstream license and contribution rules apply — see [`LICENSE`](LICENSE) and
[`docs/contribution/`](docs/contribution/). Scripts and artwork of the ported titles remain
with their rightsholders and are included for personal, non-commercial use; titles without
an upstream license say so in the corresponding branch README.
