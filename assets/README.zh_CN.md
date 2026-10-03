<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 第三方源素材(`sanoba-source/`)

《魔女的夜宴》移植分支(`feature/sanoba-witch`)把“打包用的上游素材”与其余游戏素材一并入库,
这样 clone 下来既能离线重建资源包,也能看到这些包的取材来源:

| 路径 | 内容 | 来源与版权 |
| --- | --- | --- |
| `sanoba-source/bg/` | 107 张背景,336 × 480 JPEG | Yuzusoft,取自手环移植版 [`hrk666666/Sanoba-Witch-MiBand-10`](https://github.com/hrk666666/Sanoba-Witch-MiBand-10)(其上游是已存档的 `futrw4v/Sanoba-Witch-MiBand-9Pro`) |
| `sanoba-source/sd/` | 292 张 SD(Q 版)演出图,240 × 144 JPEG | 同上 |
| `sanoba-source/scn/` | 101 章剧本,由原版 KiriKiri `.ks` 转成的节点数组 JSON | 同上;简体中文文本为暗鸽汉化组成果 |
| `sanoba-source/images/` | 29 张源工程图标与装饰 PNG —— 游戏素材,阅读器不使用 | 同上 |
| `sanoba-source/title_bg.jpg` | 标题主视觉,336 × 480 JPEG | 同上 |
| `sanoba-source/logo.png` | 源工程 logo —— 游戏素材,阅读器不使用 | 同上 |
| `sanoba-source/game.txt` | 内容包清单:场景顺序与 019 选线规则 | 同上 |
| `sanoba-source/upstream-screenshots/` | 上游 README 用的 4 张截图,供对照参考 | 同上 |
| `sanoba-source/script-format-spec-v1.1.txt` | 上游的剧本格式规范(上游原文件名为 `剧本格式规范v1.1.md`),本仓库打包器实现的就是这个格式 | 同上 |
| `sanoba-source/LICENSE` | 上游 GPL-3.0 许可证正本,覆盖其素材与文档 | 上游仓库 |
| `sanoba-source/MANIFEST.json` | 每个文件的上游路径、字节数与 sha256(记录取源版本) | 由 [`tools/sanoba_fetch_source.py`](../tools/sanoba_fetch_source.py) 生成 |

- **快应用代码不入库**:`src/**/*.ux`、`src/**/*.js`、`src/engine/`、`tests/`、`tools/`
  等上游代码不在这里,只镜像游戏自身的素材。
- **上游状态(2026-10-03 核对)**:上游仓库在 2026-09-26 被**重写了历史** —— 现在
  它的 `main` 是另一个移植(《千恋＊万花》,带自己的 `ch/` 立绘与 `ev/` 事件图),
  魔女的夜宴的素材在上面**已经不存在**;上游只剩 `band9-10-sync` 分支(v1.2.1,
  2026-09-18),它的背景/SD/剧本与本目录**逐字节相同**,但没有 `game.txt`、没有
  `images/`、也没有 `logo.png`。所以**本目录就是权威副本**:clone 下来即可离线重建两个包;
  `tools/sanoba_fetch_source.py` 的默认 ref 已改指那条分支,这三样输入以归档为准。
  现在 `main` 上那 141 张立绘与 570 张事件图是那位作品的素材,不是魔女的夜宴。
- 来源、哈希与上游 commit 见 `MANIFEST.json`;要刷新这份拷贝用
  `python tools/sanoba_fetch_source.py --dest assets/sanoba-source --chunks`。
- 直接从这里重建资源包:
  `python tools/sanoba_scn_pack.py --source assets/sanoba-source --out main/sanoba_data/sanoba_scn.bin`
  与 `python tools/sanoba_pack.py --source assets/sanoba-source --out main/sanoba_data/sanoba_pack.bin`。
- 版权归 Yuzusoft(美术)与暗鸽汉化组(文本)所有,仅用于个人学习与技术展示,
  请勿商用,请支持正版。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
