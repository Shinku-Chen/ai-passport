<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# FoloToy AI Passport — Shinku-Chen's fork

This repository is a personal fork of [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport).
Upstream is the development baseline for [FoloToy AI Passport](https://ai-passport.folotoy.cn):
an **ESP32-C3** wearable with a **240×320** colour display, **three keys**,
**8 MB of flash and no PSRAM**.

The fork hosts multiple independent applications. Published applications live on
`feature/*` branches; unpublished ones stay on `pending/*`. Firmware is published
through this repository's [Releases](https://github.com/Shinku-Chen/ai-passport/releases).
Every application branch README describes only its own project; this file is the
overview. Board facts, the BSP and the development workflow come from upstream:
see [`docs/README.md`](docs/README.md), [`AGENTS.md`](AGENTS.md) and
[`docs/contribution/`](docs/contribution/).

---

## 1. Visual-novel readers (28)

### 1.1 v1.0.0 batch (13)

| Title | Branch | Latest release | Orientation | Flash | Notes |
|---|---|---|---|---|---|
| anemoi | [`feature/port-anemoi-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-anemoi-landscape) | `v1.0.2-anemoi` | Landscape | 99.01% | Common route, 71/264 scenes; body and face layers |
| Steins;Gate | [`feature/port-steins-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins-landscape) | `v1.0.1-steins` | Landscape | 97.35% | Complete script, backgrounds, sprites and event CGs |
| Steins;Gate 0 | [`feature/port-steins0-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins0-landscape) | `v1.0.1-steins0` | Landscape | 95.47% | 104 chapter entries and six endings |
| CLANNAD | [`feature/port-clannad-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-clannad-landscape) | `v1.0.1-clannad` | Landscape | 97.92% | 212 scenes / 195 chapters; per-speaker faces |
| Summer Pockets RB | [`feature/port-summer-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-summer-landscape) | `v1.0.0-summer` | Landscape | 91.86% | 422 scenes, 81,040 lines, 14 routes |
| Mahou Shojo no Majo Saiban | [`feature/port-manosaba-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-manosaba-landscape) | `v1.0.0-manosaba` | Landscape | 94.77% | Source engine commands run on a C virtual machine |
| PARQUET | [`feature/port-parquet-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-parquet-landscape) | `v1.0.0-parquet` | Landscape | 86.29% | Two-band canvas, chapter auto-continue, de-duplicated sprites |
| NianNiang Love Story | [`feature/port-nianniang-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-nianniang-landscape) | `v1.0.0-nianniang` | **Portrait** | 21.01% | 13 scenes, four endings, four render bands |
| Galgame Master | [`feature/port-galgod-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-galgod-landscape) | `v1.0.0-galgod` | Landscape | 39.48% | Strip-canvas pivot fix and content-based sprite de-duplication |
| eden* | [`feature/port-eden-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-eden-landscape) | `v1.0.0-eden` | Landscape | 93.54% | One scaled image fills the panel; no large contiguous heap |
| Stella of the End | [`feature/port-stella-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-stella-landscape) | `v1.0.0-stella` | Landscape | 99.20% | 21 chapters; tightest partition in the batch |
| Paper no Mahou Tsukai | [`feature/port-paper-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-paper-landscape) | `v1.0.1-paper` | Landscape | 75.56% | Fixed BGR-to-RGB565 path; CGs fill the panel |
| Shinuyakata | [`feature/port-shinuyakata`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-shinuyakata) | `v0.1.3-shinuyakata` | **Portrait** | 98.29% | 198 pages, 17,409 text records, 56 CG groups; silent reader |

Only `nianniang` and `shinuyakata` are portrait 240×320; the rest are landscape
320×240. The `-landscape` suffix is historical and does not describe orientation.

### 1.2 2026-11-25 batch (6)

| Title | Branch | Release | Orientation | Flash | Notes |
|---|---|---|---|---|---|
| Café Stella and the Reaper's Butterflies | [`feature/cafestella-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cafestella-20261125) | `v1.0.0-cafestella` | Landscape | 99.87% | 923 nodes, seven endings, five after stories, full audio and OP |
| Maitetsu · Niiroku Route | [`feature/maitetsu-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/maitetsu-20261125) | `v1.0.0-maitetsu` | Landscape | 94.02% | Four Niiroku chapters, 35 CGs, complete 283.88 s title theme |
| 9-nine- Kokoiro | [`feature/nine-kokoiro-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/nine-kokoiro-20261125) | `v1.0.0-nine-kokoiro` | Landscape | 64.94% | 17 chapters, two endings, 30 manual save slots |
| 9-nine- Sorairo | [`feature/nine-sorairo-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/nine-sorairo-20261125) | `v1.0.0-nine-sorairo` | Landscape | 87.31% | 21 chapters, 22 CG groups, gallery opening |
| 9-nine- Haruiro | [`feature/nine-haruiro-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/nine-haruiro-20261125) | `v1.0.0-nine-haruiro` | Landscape | 97.66% | 15 chapters, 13 choices, two endings |
| 9-nine- Yukiiro | [`feature/nine-yukiiro-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/nine-yukiiro-20261125) | `v1.0.0-nine-yukiiro` | Landscape | 99.23% | 23 chapters, 47 choice nodes, two endings |

### 1.3 Earlier ports (9)

| Title | Branch | Latest release | Notes |
|---|---|---|---|
| Asunabi | [`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame) | `v0.1.0-asunabi` | Portrait; 30 chapters, 4,649 lines, one ending, six manual saves |
| ATRI -My Dear Moments- | [`feature/atri-reader`](https://github.com/Shinku-Chen/ai-passport/tree/feature/atri-reader) | `v1.0.2-atri-reader` | Portrait; 34 chapters, 1,069 scenes, 12,188 lines, three endings |
| DRACU-RIOT! | [`feature/dracu-riot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dracu-riot) | `v0.1.0-dracu-riot` | Portrait; five routes, 62 chapter nodes, 244 event CGs |
| limelight lemonade jam | [`feature/curious-babbage`](https://github.com/Shinku-Chen/ai-passport/tree/feature/curious-babbage) | `v0.1.0-limelight` | Portrait; 230 chapter nodes, 68,229 lines, eight branches |
| Sanoba Witch | [`feature/sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch) | `v0.1.0-sanoba-witch` | Portrait; 101 chapters, 53,190 lines, five routes and endings |
| Saya no Uta | [`feature/saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/tree/feature/saya-no-uta) | `v0.1.1-saya-no-uta` | Landscape; 44 chapters, 4,078 lines, three endings |
| Senren * Banka | [`feature/senren-banka`](https://github.com/Shinku-Chen/ai-passport/tree/feature/senren-banka) | `v0.1.2-senren-banka` | Portrait; 92 backgrounds, 123 sprites, 570 event CGs |
| Starry Sky Railroad | [`feature/starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/tree/feature/starry-sky-railroad) | `v0.1.0-starry-sky-railroad` | Portrait; 39 chapters, 1,260 scenes, 13,787 lines, one ending |
| Tenshi☆Sousou RE-BOOT! | [`feature/tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tsxx-reboot) | `v0.1.1-tsxx-reboot` | Portrait; 45 chapters, 61,436 pages, 61,289 lines, 15 endings |

---

## 2. Sound and connectivity

| Application | Branch | Latest release | Notes |
|---|---|---|---|
| Voice Keychain | [`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain) | `v1.15.1-voice-keychain` | Character sound packs and hundreds of Chinese clips; self-contained `voicefs` partition |
| Pocket Intercom | [`feature/openclaw-intercom`](https://github.com/Shinku-Chen/ai-passport/tree/feature/openclaw-intercom) | `v1.14-intercom` | Hold OK to talk; a companion Android app relays to OpenClaw or an OpenAI-compatible backend |

---

## 3. Small games and utilities

| Application | Branch | Latest release | Notes |
|---|---|---|---|
| Connect Four | [`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four) | `v1.7.0-connect-four` | Landscape 10×7, three AI levels, two-player and two-device BLE |
| What to Eat Today | [`feature/cheerful-goodall`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cheerful-goodall) | `v1.2.0` | Button-driven food roulette; release to land on a recommendation |

---

## 4. Unpublished (`pending/*`)

| Application / capability | Branch | Notes |
|---|---|---|
| DLNA Audio Receiver | [`pending/dlna-receiver`](https://github.com/Shinku-Chen/ai-passport/tree/pending/dlna-receiver) | Turns the device into a LAN DLNA renderer |
| DLNA Player | [`pending/netease-dlna-player`](https://github.com/Shinku-Chen/ai-passport/tree/pending/netease-dlna-player) | DLNA casting player with on-device MP3 decoding |
| Tuner | [`pending/tuner`](https://github.com/Shinku-Chen/ai-passport/tree/pending/tuner) | Twelve-tone equal-temperament tuner using the onboard microphone |
| Shengzi Cards | [`pending/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/pending/shengzi-cards) | BROWSE / TEST / SPELL flash-card drill with NVS progress |
| Galgame Framework | [`pending/galgame-framework`](https://github.com/Shinku-Chen/ai-passport/tree/pending/galgame-framework) | Shared page, key, pagination and persistence framework for the readers |
| Two-device BLE link | [`pending/ble-link-transport`](https://github.com/Shinku-Chen/ai-passport/tree/pending/ble-link-transport) | Reusable BLE discovery, connection and stop-and-wait transport |

---

## 5. Framework and experience docs

- **Galgame framework**: [`pending/galgame-framework/README.md`](https://github.com/Shinku-Chen/ai-passport/blob/pending/galgame-framework/README.md)
  — shared reader pages, three-key interaction, pagination, saves and pack
  conventions; the branch also carries
  [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/pending/galgame-framework/docs/reference/shinku-chen/galgame-port).
- **Experience archive**: upstream [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)
  holds paired English/Chinese notes on display refresh, deep sleep, CJK fonts,
  packing budgets, three-key semantics, compositing and memory trade-offs.
- **`docs/*` branches** carry documentation, archive and experience increments
  that are not upstream yet; they do not ship application firmware.

---

## Three-key controls (readers)

| Key | Reading / main screen | Lists and choices |
|---|---|---|
| **OK (short)** | Advance a line or confirm | Confirm |
| **OK (hold)** | Open the menu | Go back one level |
| **UP (short)** | Advance / move up | Move up |
| **UP (hold)** | Fast-forward while held | — |
| **DOWN (short)** | History / move down | Move down |
| **DOWN (hold)** | Toggle auto-read | — |

> Each application's exact keys are in its branch README (for example Connect
> Four uses UP/DOWN to pick a column, and Voice Keychain uses OK to play).

---

## Build and flash

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o <absolute-path>/FoloToy-AI-Passport-full.bin
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

- The merged image is written once from `0x0`; no separate partition flash is
  needed. The version is stored in `esp_app_desc` and appears in the boot log as
  `App version: 1.0.0`.
- If flashing reports `Packet content transfer stopped`, retry with `--no-stub`
  and a lower baud rate.
- Each branch archives its firmware under `build/firmware/<full sha256>/`
  (ELF, map, `flash_args`, `manifest.json`).

## Versioning and releases

- Release tags use `v<version>-<project>` (for example `v1.0.0-stella` or
  `v1.15.1-voice-keychain`); the older `v1.0.0` tag is the Shengzi Cards demo
  release, not the 13-reader batch.
- Each release asset is a self-contained `FoloToy-AI-Passport-full.bin`.
- Branch names do not describe orientation; the `-landscape` suffix is historical.
  The branch README is authoritative.

## License

The upstream licence and contribution conventions apply; see [`LICENSE`](LICENSE)
and [`docs/contribution/`](docs/contribution/). The scripts and artwork of each
ported title remain with their original rights holders and are used for personal,
non-commercial purposes only; titles without an upstream licence statement are
noted in their branch README.

