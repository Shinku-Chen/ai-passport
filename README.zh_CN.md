<p align="right">
  <a href="README.md">简体中文（默认）</a> · <a href="README.en_US.md">English</a>
</p>

# FoloToy AI Passport —— Shinku-Chen 的 fork

本仓库是 [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的个人 fork。
上游是 [FoloToy AI Passport](https://ai-passport.folotoy.cn) 的开发基线：一颗 **ESP32-C3**、
**240×320** 彩屏、**三个按键**、**8 MB Flash、无 PSRAM** 的开源可穿戴设备。

这个 fork 在基线之上承载了**多个独立应用**，每个应用各自位于一个 `feature/*` 分支，
固件发布挂在本仓库的 [Releases](https://github.com/Shinku-Chen/ai-passport/releases)。
板卡事实、BSP 与开发流程来自上游——见 [`docs/README.md`](docs/README.md)、
[`AGENTS.md`](AGENTS.md) 与 [`docs/contribution/`](docs/contribution/)。

## 项目一览

### 一、视觉小说阅读器（v1.0.0 批次，12 部）

把 12 部视觉小说移植成"三键阅读器"：剧本、立绘、背景、CG 全部打包进应用分区，
克隆仓库即可编译出完整可玩的固件，不需要 SD 卡或在线资源。

| 作品 | 分支 | 版本 | 方向 | Flash 占用 |
|---|---|---|---|---|
| anemoi | [`feature/port-anemoi-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-anemoi-landscape) | `v1.0.0-anemoi` | 横屏 | 99.01% |
| 命运石之门 | [`feature/port-steins-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins-landscape) | `v1.0.0-steins` | 横屏 | 97.35% |
| 命运石之门0（Steins;Gate 0） | [`feature/port-steins0-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins0-landscape) | `v1.0.0-steins0` | 横屏 | 95.47% |
| CLANNAD | [`feature/port-clannad-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-clannad-landscape) | `v1.0.0-clannad` | 横屏 | 97.92% |
| Summer Pockets RB | [`feature/port-summer-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-summer-landscape) | `v1.0.0-summer` | 横屏 | 91.86% |
| 魔法少女的魔女审判 | [`feature/port-manosaba-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-manosaba-landscape) | `v1.0.0-manosaba` | 横屏 | 94.77% |
| PARQUET | [`feature/port-parquet-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-parquet-landscape) | `v1.0.0-parquet` | 横屏 | 86.29% |
| 男娘恋爱物语 | [`feature/port-nianniang-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-nianniang-landscape) | `v1.0.0-nianniang` | **竖屏** | 21.01% |
| 想成为Galgame领域大神 | [`feature/port-galgod-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-galgod-landscape) | `v1.0.0-galgod` | 横屏 | 39.48% |
| eden* | [`feature/port-eden-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-eden-landscape) | `v1.0.0-eden` | 横屏 | 93.54% |
| 星之终途（Stella of the End） | [`feature/port-stella-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-stella-landscape) | `v1.0.0-stella` | 横屏 | 99.20% |
| 纸上的魔法使（Paper no Mahou Tsukai） | [`feature/port-paper-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-paper-landscape) | `v1.0.0-paper` | **竖屏** | 75.56% |

> 分支名统一带 `-landscape`（含两部实际为竖屏的作品），保留此命名只为不破坏既有引用；
> 方向以本表为准。

### 二、视觉小说阅读器（更早的移植）

| 作品 | 分支 | 版本 | 方向 | 规模 |
|---|---|---|---|---|
| Asunabi | [`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame) | `v0.1.0-asunabi` | 竖屏 | 30 章 / 4,649 句 / 约 9.4 万字 |
| ATRI -My Dear Moments- | [`feature/atri-reader`](https://github.com/Shinku-Chen/ai-passport/tree/feature/atri-reader) | `v1.0.2-atri-reader` | 竖屏 | 34 章 / 1,069 场景 / 12,188 句 / 3 结局 |
| DRACU-RIOT! | [`feature/dracu-riot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dracu-riot) | `v0.1.0-dracu-riot` | 竖屏 | 五条女主线（含后日谈与结局） |
| limelight lemonade jam | [`feature/limelight`](https://github.com/Shinku-Chen/ai-passport/tree/feature/limelight) | `v0.1.0-limelight` | 竖屏 | 230 章节点 / 68,229 句 |
| Sanoba Witch | [`feature/sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch) | `v0.1.0-sanoba-witch` | 竖屏 | 101 章 / 53,190 句 / 约 110 万字 / 5 线 5 结局 |
| Saya no Uta | [`feature/saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/tree/feature/saya-no-uta) | `v0.1.1-saya-no-uta` | 横屏 | 44 章 / 3,828 句 / 3 结局 |
| Senren * Banka | [`feature/senren-banka`](https://github.com/Shinku-Chen/ai-passport/tree/feature/senren-banka) | `v0.1.0-senren-banka` | 竖屏 | 全篇剧情 + 背景 / 立绘 / 事件 CG |
| Hoshizora Tetsudou to Shiro no Tabi | [`feature/starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/tree/feature/starry-sky-railroad) | `v0.1.0-starry-sky-railroad` | 竖屏 | 含中文民间汉化 |
| TSXX RE-BOOT | [`feature/tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tsxx-reboot) | `v0.1.0-tsxx-reboot` | — | 离线三键阅读 |

### 三、声音与互联

| 项目 | 分支 | 版本 | 说明 |
|---|---|---|---|
| 音效钥匙扣（Voice Keychain） | [`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain) | `v1.15-voice-keychain` | 口袋音频播放器：几十个角色包、数百条中文语音片段（jojo、meme cat、刘华强等） |
| 口袋对讲机（Pocket Intercom） | [`feature/openclaw-intercom`](https://github.com/Shinku-Chen/ai-passport/tree/feature/openclaw-intercom) | `v1.14-intercom` | 按住 OK 说话，配套 Android 应用把语音送到局域网 AI，回答回显到设备与手机 |
| DLNA 音频接收器 | [`feature/dlna-receiver`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dlna-receiver) | — | 把设备变成局域网 DLNA 播放端 |
| DLNA 投屏播放器 | [`feature/netease-dlna-player`](https://github.com/Shinku-Chen/ai-passport/tree/feature/netease-dlna-player) | — | DLNA 投屏播放（配合网易云等） |

### 四、小游戏与工具

| 项目 | 分支 | 版本 | 说明 |
|---|---|---|---|
| 四子棋（Connect Four） | [`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four) | `v1.7.0-connect-four` | 横持设备，10×7 棋盘，三档 AI 或双人对下 |
| 今天吃啥（What to Eat Today） | [`feature/cheerful-goodall`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cheerful-goodall) | — | 按住键滚动推荐，松手停在候选项上 |
| 调音器（Tuner） | [`feature/tuner`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tuner) | — | 用板载麦克风做音高检测与调音指示 |
| 生字卡片识记（Shengzi Cards） | [`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards) | — | 生字卡片背诵小工具 |

## 三键操作

| 按键 | 正文 / 主界面 | 列表 / 选项页 |
|---|---|---|
| **OK 短按** | 推进下一句或确认 | 确认 |
| **OK 长按** | 打开菜单 | 返回上一页 |
| **UP 短按** | 推进 / 上移 | 上移 |
| **UP 长按** | 快进（松手停） | — |
| **DOWN 短按** | 历史文本 / 下移 | 下移 |
| **DOWN 长按** | 开关自动阅读 | — |

自动阅读与快进期间不会熄屏或休眠。空闲策略：调暗 60 s（summer、galgod 为 90 s）、
熄屏 300 s、深睡 1800 s，任意键唤醒。

## 构建与刷写

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o <绝对路径>/FoloToy-AI-Passport-full.bin
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash @flash_args
```

版本写在 `esp_app_desc`，开机日志可见 `App version: 1.0.0`。
刷写遇到 `Packet content transfer stopped` 时，用 `--no-stub` 以更低波特率重试。

## Flash 预算提醒

`anemoi`（余 82,224 B）、`stella`（余 66,992 B）、`clannad`（余 173,056 B）
三个分支已贴近分区上限，改动前先确认预算。

## 经验沉淀

- 12 部移植踩过的坑（LVGL 9 标签换行语义、tjpgd 的 BGR 输出、
  `lv_image_set_src` 会重置尺寸、分带渲染与堆碎片、长按补发 CLICK 的吞掉时机等）：
  `feature/galgame-framework` 分支的
  `docs/reference/shinku-chen/galgame-port/`（中英成对）。
- 各项目的实现与踩坑记录：`docs/reference/shinku-chen/`。

## 许可

沿用上游仓库的许可与贡献约定，见 [`LICENSE`](LICENSE) 与
[`docs/contribution/`](docs/contribution/)。
