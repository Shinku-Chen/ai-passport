<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a> · <a href="README.zh_CN.md">简体中文（独立文件）</a>
</p>

# FoloToy AI Passport —— Shinku-Chen 的 fork

本仓库是 [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的个人 fork。
上游是 [FoloToy AI Passport](https://ai-passport.folotoy.cn) 的开发基线：一颗 **ESP32-C3**、
**240×320** 彩屏、**三个按键**、**8 MB Flash、无 PSRAM** 的开源可穿戴设备。

这个 fork 在基线之上承载了**多个独立应用**，每个应用各自位于一个 `feature/*` 分支，
固件发布挂在本仓库的 [Releases](https://github.com/Shinku-Chen/ai-passport/releases)。
板卡事实、BSP 与开发流程来自上游——见 [`docs/README.md`](docs/README.md)、
[`AGENTS.md`](AGENTS.md) 与 [`docs/contribution/`](docs/contribution/)。

## 一、视觉小说阅读器（v1.0.0 批次，12 部）

把 12 部视觉小说移植成"三键阅读器"：剧本、立绘、背景、CG 全部打包进应用分区，
克隆仓库即可编译出完整可玩的固件，不需要 SD 卡或在线资源。

| 作品 | 分支 | 版本 | 方向 | Flash 占用 |
|---|---|---|---|---|
| anemoi | [`feature/port-anemoi-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-anemoi-landscape) | `v1.0.0-anemoi` | 横屏 | 99.01% |
| 命运石之门（Steins;Gate） | [`feature/port-steins-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins-landscape) | `v1.0.0-steins` | 横屏 | 97.35% |
| 命运石之门0（Steins;Gate 0） | [`feature/port-steins0-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins0-landscape) | `v1.0.0-steins0` | 横屏 | 95.47% |
| CLANNAD | [`feature/port-clannad-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-clannad-landscape) | `v1.0.0-clannad` | 横屏 | 97.92% |
| Summer Pockets RB | [`feature/port-summer-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-summer-landscape) | `v1.0.0-summer` | 横屏 | 91.86% |
| 魔法少女的魔女审判 | [`feature/port-manosaba-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-manosaba-landscape) | `v1.0.0-manosaba` | 横屏 | 94.77% |
| PARQUET | [`feature/port-parquet-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-parquet-landscape) | `v1.0.0-parquet` | 横屏 | 86.29% |
| 男娘恋爱物语 | [`feature/port-nianniang-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-nianniang-landscape) | `v1.0.0-nianniang` | **竖屏** | 21.01% |
| 想成为Galgame领域大神 | [`feature/port-galgod-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-galgod-landscape) | `v1.0.0-galgod` | 横屏 | 39.48% |
| eden* | [`feature/port-eden-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-eden-landscape) | `v1.0.0-eden` | 横屏 | 93.54% |
| 星之终途（Stella of the End） | [`feature/port-stella-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-stella-landscape) | `v1.0.0-stella` | 横屏 | 99.20% |
| 纸上的魔法使（Paper no Mahou Tsukai） | [`feature/port-paper-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-paper-landscape) | `v1.0.0-paper` | 横屏 | 75.56% |

12 部的功能一致：三键阅读（短 OK 推进、长 OK 菜单、长 UP 快进、长 DOWN 自动）、
逐字打字机、存档读档、历史回看、章节跳转、CG 鉴赏、自动阅读、快进瞬显，
以及"自动/快进期间不熄屏不休眠"。

> 分支名统一带 `-landscape`（含两部实际为竖屏的作品），保留此命名只为不破坏既有引用；
> 方向以本表为准。

## 二、视觉小说阅读器（更早的移植）

- **Asunabi** —— `feature/asunabi-galgame`（`v0.1.0-asunabi`）：竖屏，30 章、4,649 句、
  约 9.4 万字，单结局；含 6 个手动存档位、章节跳转与文字速度/字号设置。
- **ATRI -My Dear Moments-** —— `feature/atri-reader`（`v1.0.2-atri-reader`）：竖屏，
  34 章、1,069 场景、12,188 句、3 结局，全离线；本项目同时沉淀了 ATRI 阅读器的交互范式。
- **DRACU-RIOT!** —— `feature/dracu-riot`（`v0.1.0-dracu-riot`）：竖屏，五条女主线
  含各自后日谈与结局，是米哈游手环移植版的第四部。
- **limelight lemonade jam** —— `feature/limelight`（`v0.1.0-limelight`）：竖屏，
  230 个章节点、68,229 句，商业作品的完整移植。
- **Sanoba Witch** —— `feature/sanoba-witch`（`v0.1.0-sanoba-witch`）：竖屏，
  101 章、53,190 句、约 110 万字、5 条女主线 5 个结局。
- **Saya no Uta** —— `feature/saya-no-uta`（`v0.1.1-saya-no-uta`）：横屏，
  44 章、3,828 句、3 结局；社区移植版。
- **Senren \* Banka** —— `feature/senren-banka`（`v0.1.0-senren-banka`）：竖屏，
  全篇剧情与背景/立绘/事件 CG 全部驻留固件。
- **Hoshizora Tetsudou to Shiro no Tabi（星空列车与白的旅行）** ——
  `feature/starry-sky-railroad`（`v0.1.0-starry-sky-railroad`）：竖屏，含中文民间汉化。
- **TSXX RE-BOOT** —— `feature/tsxx-reboot`（`v0.1.0-tsxx-reboot`）：离线三键阅读。

## 三、声音与互联

- **音效钥匙扣（Voice Keychain）** —— `feature/voice-keychain`（`v1.15-voice-keychain`）：
  把设备变成口袋音频播放器，几十个角色包、数百条中文语音片段；UP/DOWN 在目录中移动、
  OK 播放、长按 OK 进设置（音量、电量）。
- **口袋对讲机（Pocket Intercom）** —— `feature/openclaw-intercom`（`v1.14-intercom`）：
  按住 OK 说话，配套 Android 应用把语音送到局域网内的 AI，回答回显到设备屏幕与手机；
  涉及双设备 BLE 链路与语音上行。
- **DLNA 音频接收器** —— `feature/dlna-receiver`：把设备变成局域网内的 DLNA 渲染端，
  由手机/电脑推流播放。
- **DLNA 投屏播放器** —— `feature/netease-dlna-player`：DLNA 投屏播放，配合网易云等
  支持投屏的音源使用。

## 四、小游戏与工具

- **四子棋（Connect Four）** —— `feature/connect-four`（`v1.7.0-connect-four`）：
  横持设备在 10×7 棋盘上对局，三档 AI 难度，也可两人轮流。
- **今天吃啥（What to Eat Today）** —— `feature/cheerful-goodall`：按住键滚动推荐，
  松手停在候选项上，把"吃什么"变成一个按键游戏。
- **调音器（Tuner）** —— `feature/tuner`：用板载麦克风做音高检测并给出调音指示。
- **生字卡片识记（Shengzi Cards）** —— `feature/shengzi-cards`：生字卡片背诵小工具。

## 五、文档与经验分支

- `feature/galgame-framework`：12 部移植的共性能力与踩坑记录，见
  `docs/reference/shinku-chen/galgame-port/`（中英成对）。
- `docs/*` 分支按主题沉淀经验（深度睡眠与唤醒、显示刷新、字体子集、打包预算、
  三键事件语义等），对应正文在 `docs/reference/shinku-chen/`。

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

## 许可

沿用上游仓库的许可与贡献约定，见 [`LICENSE`](LICENSE) 与
[`docs/contribution/`](docs/contribution/)。
