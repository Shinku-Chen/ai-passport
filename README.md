<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# FoloToy AI Passport —— Shinku-Chen 的 fork

本仓库是 [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的个人 fork。
上游是 [FoloToy AI Passport](https://ai-passport.folotoy.cn) 的开发基线：**ESP32-C3**、
**240×320** 彩屏、**三个按键**、**8 MB Flash、无 PSRAM**、开源可穿戴。

fork 在此之上承载多个独立应用：已发布到社区的应用在 `feature/*`，尚未发布的在
`pending/*`；固件发布挂在本仓库的 [Releases](https://github.com/Shinku-Chen/ai-passport/releases)。
每个应用分支的根 README 只介绍它自己那个项目，本文件是总览。板卡事实、BSP 与开发流程
来自上游——见 [`docs/README.md`](docs/README.md)、[`AGENTS.md`](AGENTS.md)、
[`docs/contribution/`](docs/contribution/)。

---

## 一、视觉小说阅读器（28 部）

### 1.1 v1.0.0 批次（13 部）

| 作品 | 分支 | 最新版本 | 方向 | Flash | 说明 |
|---|---|---|---|---|---|
| anemoi | [`feature/port-anemoi-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-anemoi-landscape) | `v1.0.2-anemoi` | 横屏 | 99.01% | 共通篇 71/264 场景，身体与表情两层立绘 |
| 命运石之门 | [`feature/port-steins-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins-landscape) | `v1.0.1-steins` | 横屏 | 97.35% | 完整剧本、背景、立绘与事件 CG |
| 命运石之门0 | [`feature/port-steins0-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-steins0-landscape) | `v1.0.1-steins0` | 横屏 | 95.47% | 104 个章节入口、6 个结局 |
| CLANNAD | [`feature/port-clannad-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-clannad-landscape) | `v1.0.1-clannad` | 横屏 | 97.92% | 212 场景 / 195 章，立绘跟随说话人 |
| Summer Pockets RB | [`feature/port-summer-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-summer-landscape) | `v1.0.0-summer` | 横屏 | 91.86% | 422 场景、81,040 句、14 条路线 |
| 魔法少女的魔女审判 | [`feature/port-manosaba-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-manosaba-landscape) | `v1.0.0-manosaba` | 横屏 | 94.77% | 包内为源引擎指令，C 虚拟机执行 |
| PARQUET | [`feature/port-parquet-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-parquet-landscape) | `v1.0.0-parquet` | 横屏 | 86.29% | 双带画布、章节自动续播、按内容去重立绘 |
| 男娘恋爱物语 | [`feature/port-nianniang-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-nianniang-landscape) | `v1.0.0-nianniang` | **竖屏** | 21.01% | 13 场景、4 结局、四条渲染分带 |
| 想成为Galgame领域大神 | [`feature/port-galgod-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-galgod-landscape) | `v1.0.0-galgod` | 横屏 | 39.48% | 条带画布 pivot 修正、按内容去重立绘 |
| eden* | [`feature/port-eden-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-eden-landscape) | `v1.0.0-eden` | 横屏 | 93.54% | 真机无大块连续堆，用单图缩放铺满 |
| 星之终途 | [`feature/port-stella-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-stella-landscape) | `v1.0.0-stella` | 横屏 | 99.20% | 21 章，本批分区最紧 |
| 纸上的魔法使 | [`feature/port-paper-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-paper-landscape) | `v1.0.1-paper` | 横屏 | 75.56% | 修正 BGR→RGB565 换序，CG 铺满 |
| 死馆 | [`feature/port-shinuyakata`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-shinuyakata) | `v0.1.3-shinuyakata` | **竖屏** | 98.29% | 198 页、17,409 条文本、56 组 CG，无声阅读器 |

**方向说明**：本批只有 `nianniang` 与 `shinuyakata` 是竖屏 240×320，其余为横屏 320×240；
分支名里的 `-landscape` 是历史命名，不代表实际方向。

### 1.2 2026-11-25 批次（6 部）

| 作品 | 分支 | 版本 | 方向 | Flash | 说明 |
|---|---|---|---|---|---|
| 星光咖啡馆与死神之蝶 | [`feature/cafestella-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cafestella-20261125) | `v1.0.0-cafestella` | 横屏 | 99.87% | 923 节点、7 结局、5 后日谈，完整音频与 OP |
| 爱上火车·二六线 | [`feature/maitetsu-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/maitetsu-20261125) | `v1.0.0-maitetsu` | 横屏 | 94.02% | 二六线 4 章、35 张 CG、完整 283.88s 标题曲 |
| 9-nine- 九次九日九重色 | [`feature/nine-kokoiro-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/nine-kokoiro-20261125) | `v1.0.0-nine-kokoiro` | 横屏 | 64.94% | 17 章、2 结局、30 个手动存档位 |
| 9-nine- 天色天歌天籁音 | [`feature/nine-sorairo-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/nine-sorairo-20261125) | `v1.0.0-nine-sorairo` | 横屏 | 87.31% | 21 章、22 组 CG、OP 鉴赏 |
| 9-nine- 春色春恋春熙风 | [`feature/nine-haruiro-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/nine-haruiro-20261125) | `v1.0.0-nine-haruiro` | 横屏 | 97.66% | 15 章、13 个选项、2 个结局 |
| 9-nine- 雪色雪花雪之痕 | [`feature/nine-yukiiro-20261125`](https://github.com/Shinku-Chen/ai-passport/tree/feature/nine-yukiiro-20261125) | `v1.0.0-nine-yukiiro` | 横屏 | 99.23% | 23 章、47 个选项节点、2 个结局 |

### 1.3 更早的移植（9 部）

| 作品 | 分支 | 最新版本 | 说明 |
|---|---|---|---|
| 飞鸟会长不肯认输 | [`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame) | `v0.1.0-asunabi` | 竖屏；30 章、4,649 句、单结局，6 个手动存档位 |
| 亚托莉 -My Dear Moments- | [`feature/atri-reader`](https://github.com/Shinku-Chen/ai-passport/tree/feature/atri-reader) | `v1.0.2-atri-reader` | 竖屏；34 章、1,069 幕、12,188 句、3 结局 |
| DRACU-RIOT! | [`feature/dracu-riot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dracu-riot) | `v0.1.0-dracu-riot` | 竖屏；五条女主线、62 章节点、244 张事件 CG |
| 聚光灯柠檬果酱 | [`feature/curious-babbage`](https://github.com/Shinku-Chen/ai-passport/tree/feature/curious-babbage) | `v0.1.0-limelight` | 竖屏；230 章节点、68,229 句、8 个选项分支 |
| 魔女的夜宴 | [`feature/sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch) | `v0.1.0-sanoba-witch` | 竖屏；101 章、53,190 句、5 条线 5 结局 |
| 沙耶之歌 | [`feature/saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/tree/feature/saya-no-uta) | `v0.1.1-saya-no-uta` | 横屏；44 章、4,078 句、3 结局 |
| 千恋＊万花 | [`feature/senren-banka`](https://github.com/Shinku-Chen/ai-passport/tree/feature/senren-banka) | `v0.1.2-senren-banka` | 竖屏；92 张背景、123 张立绘、570 张事件 CG |
| 星空列车与白的旅行 | [`feature/starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/tree/feature/starry-sky-railroad) | `v0.1.0-starry-sky-railroad` | 竖屏；39 章、1,260 场景、13,787 句、单结局 |
| 天使☆騒々 RE-BOOT! | [`feature/tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tsxx-reboot) | `v0.1.1-tsxx-reboot` | 竖屏；45 章、61,436 页、61,289 句、15 结局 |

---

## 二、声音与互联

| 应用 | 分支 | 最新版本 | 说明 |
|---|---|---|---|
| 音效钥匙扣 | [`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain) | `v1.15.1-voice-keychain` | 角色音效目录、上百段中文语音；固件自带 `voicefs` 数据分区 |
| 随身 AI 对讲机 | [`feature/openclaw-intercom`](https://github.com/Shinku-Chen/ai-passport/tree/feature/openclaw-intercom) | `v1.14-intercom` | 按住 OK 说话，配套 Android 应用转发到 OpenClaw / OpenAI 兼容后端 |

---

## 三、小游戏与工具

| 应用 | 分支 | 最新版本 | 说明 |
|---|---|---|---|
| 四子棋 | [`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four) | `v1.7.0-connect-four` | 横屏 10×7，三档 AI、双人模式、双机 BLE |
| 今天吃啥 | [`feature/cheerful-goodall`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cheerful-goodall) | `v1.2.0` | 按键驱动的食物轮盘，松开停在推荐项 |

---

## 四、未发布（pending）

| 应用 / 能力 | 分支 | 说明 |
|---|---|---|
| DLNA 音频接收器 | [`pending/dlna-receiver`](https://github.com/Shinku-Chen/ai-passport/tree/pending/dlna-receiver) | 把设备变成局域网 DLNA 渲染端 |
| DLNA 投屏播放器 | [`pending/netease-dlna-player`](https://github.com/Shinku-Chen/ai-passport/tree/pending/netease-dlna-player) | 开机直达的 DLNA 投屏播放器，板载 MP3 解码 |
| 调音器 | [`pending/tuner`](https://github.com/Shinku-Chen/ai-passport/tree/pending/tuner) | 十二平均律调音器，板载麦克风音高检测 |
| 生字卡片识记 | [`pending/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/pending/shengzi-cards) | BROWSE / TEST / SPELL 三种模式，NVS 保存进度 |
| Galgame 框架 | [`pending/galgame-framework`](https://github.com/Shinku-Chen/ai-passport/tree/pending/galgame-framework) | 多部阅读器共用的页面、按键、分页与持久化框架 |
| 双机 BLE 链路 | [`pending/ble-link-transport`](https://github.com/Shinku-Chen/ai-passport/tree/pending/ble-link-transport) | 可复用的双设备 BLE 发现、连接与停等传输组件 |

---

## 五、框架与经验文档

- **Galgame 框架**：[`pending/galgame-framework/README.md`](https://github.com/Shinku-Chen/ai-passport/blob/pending/galgame-framework/README.md)
  —— 阅读器页面、三键交互、分页、存档与资源打包的共同约定；分支内另有
  [`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/pending/galgame-framework/docs/reference/shinku-chen/galgame-port)。
- **经验档案**：上游 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)
  按主题收录显示刷新、深睡、CJK 字体、打包预算、三键语义、合成与内存权衡等中英成对文档。
- **`docs/*` 分支**：承载尚未合入上游的文档、归档与经验增量；不提供应用固件。

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
- 各分支的固件归档在对应 `build/firmware/<full sha256>/`（含 elf、map、
  `flash_args`、`manifest.json`）。

## 版本与发布约定

- 发布 tag 采用 `v<版本>-<项目>`（如 `v1.0.0-stella`、`v1.15.1-voice-keychain`）；
  `v1.0.0` 是更早的"生字卡 demo"发布，与 13 部视觉小说批次无关。
- 每个 release 资产是自包含的 `FoloToy-AI-Passport-full.bin`。
- 分支名不代表方向（`-landscape` 后缀是历史约定），方向以各分支 README 为准。

## 许可

沿用上游仓库的许可与贡献约定，见 [`LICENSE`](LICENSE) 与
[`docs/contribution/`](docs/contribution/)。各移植作品的剧本与美术版权归原权利方，
仅作个人、非商业用途；上游未声明许可的作品已在对应分支 README 中注明。

