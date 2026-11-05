<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a> · <a href="README.zh_CN.md">简体中文（独立文件）</a>
</p>

# FoloToy AI Passport —— Shinku-Chen 的 fork

本仓库是 [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的个人 fork。
上游是 [FoloToy AI Passport](https://ai-passport.folotoy.cn) 的开发基线：一颗 **ESP32-C3**、
**240×320** 彩屏、**三个按键**、**8 MB Flash、无 PSRAM** 的开源可穿戴设备。

这个 fork 在基线之上承载了**多个独立应用**，每个应用各自位于一个 `feature/*` 分支。
板卡事实、BSP 与开发流程来自上游——见 [`docs/README.md`](docs/README.md)、
[`AGENTS.md`](AGENTS.md) 与 [`docs/contribution/`](docs/contribution/)。

## Galgame 移植（12 部，v1.0.0）

把 12 部视觉小说移植成"三键阅读器"：剧本、立绘、背景、CG 全部打包进应用分区，
克隆仓库即可编译出完整可玩的固件，不需要额外 SD 卡或在线资源。

| 作品 | 分支 | 版本 | 方向 | Flash 占用 |
|---|---|---|---|---|
| anemoi | [`feature/port-anemoi-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-anemoi-landscape) | `v1.0.0-anemoi` | 横屏 | 99.01% |
| 命运石之门 | [`feature/port-steins-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins-landscape) | `v1.0.0-steins` | 横屏 | 97.35% |
| Steins;Gate 0 | [`feature/port-steins0-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins0-landscape) | `v1.0.0-steins0` | 横屏 | 95.47% |
| CLANNAD | [`feature/port-clannad-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-clannad-landscape) | `v1.0.0-clannad` | 横屏 | 97.92% |
| Summer Pockets RB | [`feature/port-summer-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-summer-landscape) | `v1.0.0-summer` | 横屏 | 91.86% |
| 魔法少女的魔女审判 | [`feature/port-manosaba-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-manosaba-landscape) | `v1.0.0-manosaba` | 横屏 | 94.77% |
| PARQUET | [`feature/port-parquet-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-parquet-landscape) | `v1.0.0-parquet` | 横屏 | 86.29% |
| 男娘恋爱物语 | [`feature/port-nianniang-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-nianniang-landscape) | `v1.0.0-nianniang` | **竖屏** | 21.01% |
| 想成为Galgame领域大神 | [`feature/port-galgod-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-galgod-landscape) | `v1.0.0-galgod` | 横屏 | 39.48% |
| eden* | [`feature/port-eden-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-eden-landscape) | `v1.0.0-eden` | 横屏 | 93.54% |
| Stella of the End | [`feature/port-stella-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-stella-landscape) | `v1.0.0-stella` | 横屏 | 99.20% |
| Paper no Mahou Tsukai | [`feature/port-paper-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-paper-landscape) | `v1.0.0-paper` | **竖屏** | 75.56% |

> 注：分支名统一沿用 `-landscape`（含三部实际为竖屏的作品），不改名以免破坏已有引用；
> 实际方向以本表和固件为准。

### 三键操作

| 按键 | 正文页 | 列表 / 选项页 |
|---|---|---|
| **OK 短按** | 推进下一句（打字中先整页显示） | 确认 |
| **OK 长按** | 打开游戏菜单 | 返回上一页 |
| **UP 短按** | 同上（推进） | 上移 |
| **UP 长按** | 快进（松手停） | — |
| **DOWN 短按** | 历史文本 | 下移 |
| **DOWN 长按** | 开关自动阅读 | — |

自动阅读与快进期间**不会熄屏或休眠**；空闲策略为调暗 60 s（summer、galgod 为 90 s）、
熄屏 300 s、深睡 1800 s，任意键唤醒。

### 构建与刷写

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o <绝对路径>/FoloToy-AI-Passport-full.bin
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash @flash_args
```

固件版本写入 `esp_app_desc`，开机日志可见 `App version: 1.0.0`。
刷写偶发 `Packet content transfer stopped` 时，加 `--no-stub` 以更低波特率重试。

### Flash 预算提醒

`anemoi`（余 82,224 B）、`stella`（余 66,992 B）、`clannad`（余 173,056 B）
三个分支已接近分区上限，改动前请先确认预算。

## 经验沉淀

12 部移植过程中踩过的坑（LVGL 9 标签换行语义、tjpgd 的 BGR 输出、
`lv_image_set_src` 会重置尺寸、分带渲染与堆碎片、长按补发 CLICK 的吞掉时机等）
整理在 `feature/galgame-framework` 分支的 `docs/reference/shinku-chen/galgame-port/`。

## 其他分支

本 fork 还包含音效钥匙扣、小游戏、阅读器等其他实验性项目，各自位于独立的
`feature/*` 分支，可按需切换查看。

## 许可

沿用上游仓库的许可与贡献约定，见 [`LICENSE`](LICENSE) 与
[`docs/contribution/`](docs/contribution/)。
