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

[`fonts/tsxx_symbols.txt`](fonts/tsxx_symbols.txt) 是 `feature/tsxx-reboot` 上《天使☆騒々
RE-BOOT!》阅读器的字符清单：3,417 个码位，顺序就是资源包给 1 字节符号码排的词频
顺序。由 `tools/tsxx_pack.py --symbols-out` 生成，供字体子集工具消费。字体本体是构建
产物，不提交在这里。

## 《天使☆騒々 RE-BOOT!》阅读器 —— `tsxx-source/`

`feature/tsxx-reboot` 分支视觉小说阅读器的基础源素材，原样提交（22.0 MB、1,065 个
文件），因此 clone 之后不需要外部 checkout 就能重建资源包。来源是小米手环快应用
同人移植 [`hezdaaa/tsxxreboot-miband`](https://github.com/hezdaaa/tsxxreboot-miband)：

| 目录 | 文件数 | 内容 |
| --- | ---: | --- |
| `script/` | 123 | `scriptData<编号>.txt`，线性页表（61,436 页） |
| `bcgi/` | 120 | 背景，336 × 480 JPEG |
| `cimg/` | 185 | 全身立绘，带 alpha 的 PNG（178 × 715 至 396 × 649） |
| `evig/` | 637 | 事件 CG 与场景道具，336 × 480 |

上游项目未声明许可。剧本、立绘、事件 CG 与背景的版权归柚子社（Yuzusoft）及原发行方
所有；这里把该目录作为唯一的来源标注，资源包元数据里记录同一来源，并注明手环移植版
是转换来源。本项目是非商业技术研究，使用时需要合法购买的正版原作。

目录结构与上游 `src/common/` 的四个子目录一致，`tools/tsxx_pack.py` 可以直接接受：

```bash
# main/tsxx_data/tsxx_pack.bin 的内容
python tools/tsxx_pack.py --source assets/tsxx-source --out main/tsxx_data/tsxx_pack.bin \
    --bg-quality 65 --sprite-height 320 --sprite-quality 95 --event-quality 53 \
    --max-bytes 6475000 \
    --title-image assets/images/tsxx-reboot-cover.png \
    --symbols-out assets/fonts/tsxx_symbols.txt
```

用同一条命令重建可以逐字节复现仓库里提交的 `main/tsxx_data/tsxx_pack.bin`。资源包
6.17 MiB，放在独立的 6.5 MiB assets 分区里；`--max-bytes 6475000`（该分区的 95%）
让构建在装不下时直接失败，而不是默默溢出，`--event-quality 53` 则是在这个上限内能
保住的最高事件图质量。立绘每个角色只保留一种姿势（源工程同一件衣服有 2-4 张手臂/
表情差分），存满 320 px 美术层高度、JPEG 质量 95，底边贴底、下半身由文本框盖住
（与源工程一致）。

立绘用的是美术层自己的坐标系：画布 240 × 320（`--art-width` 默认值），固件 1:1 铺在
面板上，显示时不再重采样。背景与事件帧仍按 180 × 240 存（`--bg-width` /
`--event-width` 默认值），两个尺寸写进资源包的 META（`art=` / `bg=` / `event=`），
固件合成时按 4/3 最近邻放大。240 × 320 的画布占 240 × 320 × 2 = 150 KB 静态 RAM，
所以立绘与事件图的 JPEG 是走 `tjpgd` 回调接口逐块解进画布的，不再为每张图开一块
整图暂存区。文字层仍在原生 240 × 320 上绘制。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |
| [`images/tsxx-reboot-cover.png`](images/tsxx-reboot-cover.png) | 420 × 582，PNG | `feature/tsxx-reboot` 上《天使☆騒々 RE－BOOT!》阅读器的封面，同时用作标题背景图。第三方主视觉，来自柚子社原发行版，由作者为本移植提供。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
