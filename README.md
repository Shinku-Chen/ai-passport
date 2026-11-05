<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# FoloToy AI Passport —— Shinku-Chen 的 fork

本仓库是 [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的个人 fork。
上游是 [FoloToy AI Passport](https://ai-passport.folotoy.cn) 的开发基线：**ESP32-C3**、
**240×320** 彩屏、**三个按键**、**8 MB Flash、无 PSRAM**、开源可穿戴。

fork 在此之上承载了**多个独立应用**：每个应用一个 `feature/*` 分支，固件发布挂在
本仓库的 [Releases](https://github.com/Shinku-Chen/ai-passport/releases)；每个分支的
根 README 只介绍它自己那个项目，本文件是总览。板卡事实、BSP 与开发流程来自上游——
见 [`docs/README.md`](docs/README.md)、[`AGENTS.md`](AGENTS.md)、
[`docs/contribution/`](docs/contribution/)。

---

## 一、视觉小说阅读器（v1.0.0 批次，12 部）

把 12 部视觉小说做成"三键阅读器"：剧本、立绘、背景、CG、字体子集全部打进应用分区，
克隆仓库即可编译出完整可玩的固件，**不依赖 SD 卡或网络**。

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

**共同能力**：逐字打字机（可调速度/字号）· 自动阅读 · 快进（文字瞬显，句间不停顿）·
存档/读档（含自动档）· 历史回看 · 章节跳转 · CG 鉴赏 · 分支选择 ·
空闲策略（调暗 60–90 s、熄屏 300 s、深睡 1800 s，自动与快进期间不休眠）。

**方向说明**：12 部里只有 `nianniang` 是竖屏 240×320，其余 11 部都是横屏 320×240；
分支名统一带 `-landscape` 只是历史命名，不代表实际方向。

**Flash 预算**：`anemoi`（余 82,224 B）与 `stella`（余 66,992 B）已贴近分区上限，
`clannad` 余 173,056 B；改动这三个分支前请先算预算。

---

## 二、更早的视觉小说移植（9 部）

- **Asunabi（飞鸟会长不肯认输）** —— [`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame)，`v0.1.0-asunabi`
  竖屏；30 章、4,649 句对白、约 9.4 万字，单结局。6 个手动存档位、章节跳转、
  文字速度与字号设置；来源为小米手环快应用移植版，素材随分支提交并标注来源。
- **ATRI -My Dear Moments-（亚托莉）** —— [`feature/atri-reader`](https://github.com/Shinku-Chen/ai-passport/tree/feature/atri-reader)，`v1.0.2-atri-reader`
  竖屏；34 章、1,069 幕、12,188 句、3 个结局。剧本/背景/全身立绘/中文字体子集
  编进同一份合并镜像，全程离线。后续多部阅读器的交互范式都源于这一部。
- **DRACU-RIOT!** —— [`feature/dracu-riot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dracu-riot)，`v0.1.0-dracu-riot`
  竖屏；五条女主线含后日谈与结局，62 个章节节点、244 张事件 CG、85 张背景；
  来源为小米手环同人移植工程，素材与版权归属上游。
- **limelight lemonade jam（聚光灯柠檬果酱）** —— [`cindy/curious-babbage`](https://github.com/Shinku-Chen/ai-passport/tree/cindy/curious-babbage)，`v0.1.0-limelight`
  竖屏；230 个章节点、68,229 句、991 张图像、8 个选项分支；4.49 + 1.81 MiB 两段
  内容包，3,449 个字形码位。上游未声明许可，仅个人非商业。
- **Sanoba Witch（魔女的夜宴）** —— [`feature/sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch)，`v0.1.0-sanoba-witch`
  竖屏；101 章、53,190 句、约 110 万字、5 条女主线 5 个结局。
- **Saya no Uta（沙耶之歌）** —— [`feature/saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/tree/feature/saya-no-uta)，`v0.1.1-saya-no-uta`
  横屏；44 章、3,828 句、3 个结局；社区移植版，含中文文本。
- **Senren \* Banka（千恋＊万花）** —— [`feature/senren-banka`](https://github.com/Shinku-Chen/ai-passport/tree/feature/senren-banka)，`v0.1.0-senren-banka`
  竖屏；92 张背景、123 张立绘、570 张事件 CG，全篇剧情驻留固件。
- **Hoshizora Tetsudou to Shiro no Tabi（星空列车与白的旅行）** ——
  [`feature/starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/tree/feature/starry-sky-railroad)，`v0.1.0-starry-sky-railroad`
  竖屏；39 章、1,260 个场景、13,787 句、一处选项、单结局；连同中文译文一并移植。
- **TSXX RE-BOOT（天使☆騒々 RE-BOOT!）** —— [`feature/tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tsxx-reboot)，`v0.1.0-tsxx-reboot`
  竖屏；45 章、61,436 页、61,289 句、约 116 万字、13 处选择、15 个结局；6.26 MiB
  内容包、3,417 码位；社区封面为 1152×1536（由 420×582 中心裁切放大）。

---

## 三、声音与互联

- **音效钥匙扣（Voice Keychain）** —— [`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain)，`v1.15-voice-keychain`
  把设备变成随身音效播放器：开机进角色目录，几十个角色包、上百段中文语音
  （jojo、meme猫、刘华强、哈基米、奶龙等）。**UP/DOWN** 在目录里移动、
  **OK** 播放、**长按 OK** 进设置（音量、电量）。固件自包含 `voicefs` 数据分区
  （`0x210000`），从 `0x0` 一次烧录即可。单机离线。
- **随身 AI 对讲机（Pocket Intercom）** —— [`feature/openclaw-intercom`](https://github.com/Shinku-Chen/ai-passport/tree/feature/openclaw-intercom)，`v1.14-intercom`
  按住 **OK** 说话（指示灯红→绿），配套 Android 应用把语音交给局域网里的 AI 助理，
  回答回显到设备小屏与手机。设备本身不联网、不解析 HTTP、不存密钥；音频以 Opus
  经 BLE 上行，约 3 KB/s。后端可以是 OpenClaw，也可以是兼容 OpenAI 的接口。
  **依赖：配套 Android 应用 + 可达的局域网后端。**
- **DLNA 音频接收器** —— [`feature/dlna-receiver`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dlna-receiver)
  把设备变成局域网内的 DLNA 渲染端，由手机/电脑推流播放。**依赖：局域网 + 推流源。**
- **DLNA 投屏播放器** —— [`feature/netease-dlna-player`](https://github.com/Shinku-Chen/ai-passport/tree/feature/netease-dlna-player)
  DLNA 投屏播放：开机直达播放器，板载 MP3 解码。配合网易云等支持投屏的音源使用。

---

## 四、小游戏与工具

- **四子棋（Connect Four）** —— [`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four)，`v1.7.0-connect-four`
  横屏 10 列 × 7 行。**UP/DOWN** 左右选列、**OK** 落子、**长按 OK** 回设置；
  三档 AI 难度可对战，也可两人轮流；支持双机 BLE 对下。单机离线。
- **今天吃啥（What to Eat Today）** —— [`feature/cheerful-goodall`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cheerful-goodall)
  按键驱动的食物轮盘：按住一个键让推荐轮转，松手停在想吃的那一项上；
  开机直达应用，无主菜单。**长按 OK** 可切换渲染路径。单机离线。
- **调音器（Tuner）** —— [`feature/tuner`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tuner)
  十二平均律调音器，用板载麦克风做音高检测；AUTO / MANUAL 两种模式，
  **长按 OK** 进入调试并可调增益。单机离线。
- **生字卡片识记（Shengzi Cards）** —— [`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards)，`v1.0.0`
  生字卡片背诵工具：BROWSE / TEST / SPELL 三种模式，进度用 NVS 持久化。单机离线。

---

## 五、框架与经验文档

- **`feature/galgame-framework`** —— 12 部移植共用的能力与踩坑记录：
  [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port)
  （中英成对，含需求、能力、12 条"现象→根因→修法"、验收清单与 12 部分支表）。
- **`docs/*` 分支** —— 按主题沉淀经验（深度睡眠与唤醒、显示刷新、CJK 字体子集、
  打包预算、三键事件语义、线性页表、合成与内存权衡等），正文在
  `docs/reference/shinku-chen/`（每个 `.md` 都有 `.zh_CN.md` 成对文件）。

---

## 三键操作（阅读器类通用）

| 按键 | 正文 / 主界面 | 列表 / 选项页 |
|---|---|---|
| **OK 短按** | 推进下一句或确认 | 确认 |
| **OK 长按** | 打开菜单 | 返回上一页 |
| **UP 短按** | 推进 / 上移 | 上移 |
| **UP 长按** | 快进（松手停） | — |
| **DOWN 短按** | 历史文本 / 下移 | 下移 |
| **DOWN 长按** | 开关自动阅读 | — |

> 各应用的具体键位以该分支 README 为准（例如四子棋的 UP/DOWN 是左右选列、
> 音效钥匙扣的 OK 是播放）。

---

## 构建与刷写

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o <绝对路径>/FoloToy-AI-Passport-full.bin
esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

- 合并镜像从 `0x0` 一次烧录，无需单独刷分区；版本写在 `esp_app_desc`，
  开机日志可见 `App version: 1.0.0`。
- 刷写偶发 `Packet content transfer stopped` 时，改用 `--no-stub` 与更低波特率。
- 各分支的固件都归档在对应 `build/firmware/<full sha256>/`（含 elf、map、
  `flash_args`、`manifest.json`）。

## 版本与发布约定

- 发布 tag 采用 `v<版本>-<项目>`（如 `v1.0.0-stella`、`v1.15-voice-keychain`），
  `v1.0.0` 是更早的"生字卡 demo"发布，与本次 12 部批次无关。
- 每个 release 资产是自包含的 `FoloToy-AI-Passport-full.bin`。
- 分支名不代表方向（`-landscape` 后缀是历史约定），方向以各分支 README 为准。

## 许可

沿用上游仓库的许可与贡献约定，见 [`LICENSE`](LICENSE) 与
[`docs/contribution/`](docs/contribution/)。各移植作品的剧本与美术版权归原权利方，
仅作个人、非商业用途；上游未声明许可的作品已在对应分支 README 中注明。
