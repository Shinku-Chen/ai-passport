<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong> · <a href="README.zh_CN.md">简体中文（独立文件）</a>
</p>

# FoloToy AI Passport — Shinku-Chen's fork

This repository is a personal fork of [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport).
Upstream is the development baseline for [FoloToy AI Passport](https://ai-passport.folotoy.cn):
an open-source wearable built on an **ESP32-C3** with a **240×320** colour display,
**three keys**, **8 MB of flash and no PSRAM**.

This fork hosts several independent applications, each on its own `feature/*` branch,
with firmware published in this repository's
[Releases](https://github.com/Shinku-Chen/ai-passport/releases). Board facts, the BSP
and the development workflow come from upstream — see [`docs/README.md`](docs/README.md),
[`AGENTS.md`](AGENTS.md) and [`docs/contribution/`](docs/contribution/).

## 1. Visual-novel readers — v1.0.0 batch (12 titles)

Each branch turns the device into a three-key visual-novel reader: script, sprites,
backgrounds and CGs are packed into the application partition, so a plain clone builds
a complete, playable firmware with no SD card and no network access.

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
| Paper no Mahou Tsukai | [`feature/port-paper-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-paper-landscape) | `v1.0.0-paper` | **Portrait** | 75.56% |

All twelve share the same feature set: three-key reading (short OK advances, long OK
opens the menu, long UP fast-forwards, long DOWN toggles auto-advance), a typewriter
effect, save/load slots, history, chapter jump, CG gallery, auto-advance, instant text
under fast-forward, and no dim/sleep while auto or fast-forward is active.

> Branch names keep the `-landscape` suffix even for the two portrait titles; they are
> left unchanged to avoid breaking existing references. The orientation above is what
> the firmware actually renders.

## 2. Visual-novel readers — earlier ports

- **Asunabi** — `feature/asunabi-galgame` (`v0.1.0-asunabi`): portrait, 30 chapters,
  4,649 lines (~94k characters), single ending; six manual save slots, chapter jump and
  text-speed/font-size settings.
- **ATRI -My Dear Moments-** — `feature/atri-reader` (`v1.0.2-atri-reader`): portrait,
  34 chapters, 1,069 scenes, 12,188 lines, three endings, fully offline. This project
  also defined the reader interaction patterns reused later.
- **DRACU-RIOT!** — `feature/dracu-riot` (`v0.1.0-dracu-riot`): portrait, five heroine
  routes with their after-stories and endings.
- **limelight lemonade jam** — `feature/limelight` (`v0.1.0-limelight`): portrait, 230
  chapter points and 68,229 lines from the commercial release.
- **Sanoba Witch** — `feature/sanoba-witch` (`v0.1.0-sanoba-witch`): portrait, 101
  chapters, 53,190 lines (~1.1M characters), five routes and five endings.
- **Saya no Uta** — `feature/saya-no-uta` (`v0.1.1-saya-no-uta`): landscape, 44
  chapters, 3,828 lines, three endings.
- **Senren \* Banka** — `feature/senren-banka` (`v0.1.0-senren-banka`): portrait; story,
  backgrounds, sprites and event CGs all live in the firmware.
- **Hoshizora Tetsudou to Shiro no Tabi** — `feature/starry-sky-railroad`
  (`v0.1.0-starry-sky-railroad`): portrait, includes the Chinese fan translation.
- **TSXX RE-BOOT** — `feature/tsxx-reboot` (`v0.1.0-tsxx-reboot`): offline three-key
  reading.

## 3. Audio and connectivity

- **Voice Keychain** — `feature/voice-keychain` (`v1.15-voice-keychain`): a pocket audio
  player with hundreds of Chinese voice clips from dozens of character packs; UP/DOWN
  move through folders, OK plays, long OK opens settings (volume, battery).
- **Pocket Intercom** — `feature/openclaw-intercom` (`v1.14-intercom`): hold OK to talk;
  a companion Android app relays speech to an AI assistant on the LAN and brings the
  answer back to the device screen and the phone (two-device BLE link plus voice uplink).
- **DLNA Audio Receiver** — `feature/dlna-receiver`: turns the device into a DLNA
  renderer on the local network.
- **DLNA Player** — `feature/netease-dlna-player`: DLNA casting playback, usable with
  NetEase Cloud Music and other casting sources.

## 4. Small games and utilities

- **Connect Four** — `feature/connect-four` (`v1.7.0-connect-four`): a 10×7 board in
  landscape, three AI levels or two-player pass-and-play.
- **What to Eat Today** — `feature/cheerful-goodall`: hold a key to cycle through
  recommendations, release to land on one.
- **Tuner** — `feature/tuner`: pitch detection and tuning indication from the onboard
  microphone.
- **Shengzi Cards** — `feature/shengzi-cards`: a flash-card drill for Chinese characters.

## 5. Documentation and experience branches

- `feature/galgame-framework` carries the shared capabilities and pitfalls of the
  twelve ports under `docs/reference/shinku-chen/galgame-port/` (English + Chinese).
- `docs/*` branches record topic-specific lessons (deep sleep and wake, display refresh,
  font subsets, packing budgets, three-key event semantics) under
  `docs/reference/shinku-chen/`.

## Three-key controls

| Key | Reader / main screen | List / choice screen |
|---|---|---|
| **OK short** | Advance to the next line, or confirm | Confirm |
| **OK long** | Open the menu | Go back |
| **UP short** | Advance / move up | Move up |
| **UP long** | Fast-forward (stop on release) | — |
| **DOWN short** | Text history / move down | Move down |
| **DOWN long** | Toggle auto-advance | — |

Auto-advance and fast-forward never dim or sleep the display. Idle policy: dim after
60 s (90 s for summer and galgod), backlight off after 300 s, deep sleep after 1800 s;
any key wakes the device.

## Build and flash

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o <absolute>/FoloToy-AI-Passport-full.bin
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash @flash_args
```

The version lives in `esp_app_desc`; the boot log shows `App version: 1.0.0`.
If flashing fails with `Packet content transfer stopped`, retry with `--no-stub`
at a lower baud rate.

## Flash budget

`anemoi` (82,224 B free), `stella` (66,992 B free) and `clannad` (173,056 B free)
are close to the partition limit. Check the budget before changing those branches.

## License

Upstream license and contribution rules apply — see [`LICENSE`](LICENSE) and
[`docs/contribution/`](docs/contribution/).
