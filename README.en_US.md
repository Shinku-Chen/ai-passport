<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong> · <a href="README.zh_CN.md">简体中文（独立文件）</a>
</p>

# FoloToy AI Passport — Shinku-Chen's fork

This repository is a personal fork of [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport).
Upstream is the development baseline for [FoloToy AI Passport](https://ai-passport.folotoy.cn):
an open-source wearable built on an **ESP32-C3** with a **240×320** colour display,
**three keys**, **8 MB of flash and no PSRAM**.

This fork hosts several independent applications on top of that baseline, each living
on its own `feature/*` branch. Board facts, the BSP and the development workflow come
from upstream — see [`docs/README.md`](docs/README.md), [`AGENTS.md`](AGENTS.md) and
[`docs/contribution/`](docs/contribution/).

## Galgame ports (12 titles, v1.0.0)

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

> Branch names all use the `-landscape` suffix, including the three titles that actually
> render in portrait. They are kept as-is to avoid breaking existing references; the
> orientation in this table matches the firmware.

### Three-key controls

| Key | Reader page | List / choice page |
|---|---|---|
| **OK short** | Advance to the next line (complete the current line first) | Confirm |
| **OK long** | Open the in-game menu | Go back |
| **UP short** | Advance | Move up |
| **UP long** | Fast-forward (stop on release) | — |
| **DOWN short** | Text history | Move down |
| **DOWN long** | Toggle auto-advance | — |

Auto-advance and fast-forward never dim or sleep the display. Idle policy: dim after
60 s (90 s for summer and galgod), backlight off after 300 s, deep sleep after 1800 s;
any key wakes the device.

### Build and flash

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o <absolute>/FoloToy-AI-Passport-full.bin
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash @flash_args
```

The release version is written into `esp_app_desc`; the boot log shows
`App version: 1.0.0`. If flashing fails with
`Packet content transfer stopped`, retry with `--no-stub` at a lower baud rate.

### Flash budget

`anemoi` (82,224 B free), `stella` (66,992 B free) and `clannad` (173,056 B free)
are close to the partition limit. Check the budget before changing those branches.

## Lessons learned

The pitfalls collected while porting the 12 titles — LVGL 9 label wrap semantics,
tjpgd's BGR output, `lv_image_set_src` resetting the object size, banded rendering
versus heap fragmentation, and when to swallow the click that follows a long press —
are documented on the `feature/galgame-framework` branch under
`docs/reference/shinku-chen/galgame-port/`.

## Other branches

The fork also carries a voice keychain, small games and reader experiments, each on its
own `feature/*` branch.

## License

Upstream license and contribution rules apply — see [`LICENSE`](LICENSE) and
[`docs/contribution/`](docs/contribution/).
