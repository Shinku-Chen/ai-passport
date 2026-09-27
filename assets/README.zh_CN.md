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
| [`images/senren-banka-title.png`](images/senren-banka-title.png) | 402 × 560, PNG | 《千恋＊万花》(SAGA PLANETS)官方主视觉,2026-09-27 由开发者提供。它是本移植标题画面的构建源:`tools/senren_pack.py --title-art` 把它 cover 成 240 × 320,作为背景名字空间里的一个条目打进包,名字见 `main/senren_pack.h` 的 `SENREN_TITLE_ART_NAME`。美术版权归 SAGA PLANETS;本移植非商业、仅供学习交流(见打包器 docstring)。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 移植源素材（移植分支的 assets/*-source）

各移植分支的上游源素材按打包器直接读取的目录结构归档在这里，因此固件里的每一个打包数据块都可以离线重建，不需要联网。

`senren-source/` 是《千恋＊万花》移植的源素材：

| 路径 | 内容 |
| --- | --- |
| `senren-source/bg/` | 92 张背景 |
| `senren-source/ch/` | 123 个立绘（每个含多个姿势） |
| `senren-source/ev/` | 570 张事件插图与 SD 图 |
| `senren-source/scn/` | 112 个剧本分块 |
| `senren-source/MANIFEST.json` | 上游仓库、引用与逐文件 SHA-256，由取源工具写出 |

来源：手环移植工程 [`hrk666666/Senren-Banka-MiBand-10`](https://github.com/hrk666666/Senren-Banka-MiBand-10)，由 `tools/senren_fetch_source.py` 拉取。用这个目录重新打包，结果与已提交的包**字节级一致**：

```bash
python3 tools/senren_pack.py --source assets/senren-source \
  --out build/repack/senren_pack.bin --json build/repack/senren_pack.json \
  --sprite-max-h 320 --quality-scale 1.15 --sd-min-refs 8
python3 tools/senren_scn_pack.py --source assets/senren-source \
  --out build/repack/senren_scn.bin --json build/repack/senren_scn.json
```

两条命令都能复现已提交的 SHA-256（5,263,756 字节图片包 `a0f89a22…`、1,432,148 字节剧本包 `a0d7d3cf…`）。本移植非商业、仅供个人学习；美术版权归原出版方，上游工程已在分支 README 中注明。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
