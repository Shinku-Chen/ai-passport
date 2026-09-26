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

### 《沙耶之歌》阅读器 —— `fonts/saya_cjk_16.c`、`fonts/saya_cjk_20.c`

`feature/saya-no-uta` 分支的视觉小说阅读器使用的两个 LVGL 子集（4bpp、未压缩）；
`fonts/saya_cjk_symbols.txt` 是它们必须覆盖的字符清单。

| 项 | 值 |
| --- | --- |
| 来源 | Noto Sans SC Regular（OFL-1.1），2026-09-26 经 jsDelivr 取自 `googlefonts/noto-cjk` 镜像；8.3 MB 源 OTF **不**提交 |
| 字符范围 | 2,839 个码位 = `main/*.c` 的全部界面文案 + 两个变体资源包（社区版与含补丁的 release 版）的全部剧本字符（含剧本用于首行缩进的全角空格 U+3000） |
| 转换工具 | `tools/saya_font.py`（Pillow/FreeType 栅格化，直接输出 LVGL 9 位图字体格式）。生成后会回读自己写出的 C 文件，与栅格化结果逐像素比对，不合格就报错。 |
| 重新生成 | `python tools/saya_font.py --font <NotoSansSC-Regular.otf> --pack main/saya_data/saya_pack.bin --out-dir assets/fonts` |
| 校验 | `python tools/saya_font.py --check --pack … --out-dir …`（由 `tools/validate.sh --static` 执行） |
| 集成方式 | 通过 `main/CMakeLists.txt` 的 `target_sources()` 编进 `main` 组件 |
| 影响 | 约 0.94 MB Flash；不占静态 RAM（字形常驻 Flash） |

**不用 `lv_font_conv` 的原因**：该工具最后一版是 1.5.3（2021），在当前 Node.js 下写出的字形位图是坏的 —— 同一个 OTF、同一套参数，`--no-compress` 与 `--no-compress --no-prefilter` 产出的字节完全相同，按 LVGL 的 PLAIN 4bpp 读法解码全是噪点；而 LVGL 自带的 Montserrat 字体用同一解码器完全正常。设备上的表现就是“文字全部乱码”。改用仓库内生成器后不再依赖 Node，且生成阶段就会失败报错，不会默默产出坏字形。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |
| [`images/saya-no-uta-cover.png`](images/saya-no-uta-cover.png) | 1152 × 1536，PNG | 《沙耶之歌》阅读器（`feature/saya-no-uta`）的竖版 3:4 封面，用于社区市场列表封面与 Release 说明。原 Nitroplus 作品主视觉（第三方素材，作者指定使用）；应用档案见 [`../docs/reference/shinku-chen/saya-no-uta/`](../docs/reference/shinku-chen/saya-no-uta/README.zh_CN.md)。 |

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
