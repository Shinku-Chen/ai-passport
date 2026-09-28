<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

| 文件 | 格式与大小 | 用途与来源 |
| --- | --- | --- |
| [`../main/fonts/gb2312_14.c`](../main/fonts/gb2312_14.c) 与 [`gb2312_14.h`](../main/fonts/gb2312_14.h) | LVGL 9 C 字库源码，14 px、2 bpp、PLAIN（非压缩）位图，7540 个字形；3073296 字节（2.93 MiB）与 659 字节 | 设备屏中文显示所需的完整 GB2312 覆盖。由 `lv_font_conv` 1.5.3 从 Noto Sans SC（SIL OFL 1.1）生成；用 `tools/gen_chinese_font.py` 重新生成。 |

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### GB2312 中文字库（`lv_font_gb2312_14`）

- 生成命令：在仓库根运行 `python3 tools/gen_chinese_font.py`（加 `--verify-only` 可只自检已有产物而不重新转换）。脚本以固定参数调用转换器：

```bash
lv_font_conv --font C:/Windows/Fonts/NotoSansSC-VF.ttf --size 14 --bpp 2 \
  --format lvgl --no-compress --no-prefilter --lv-include lvgl.h \
  --symbols <脚本列出的 7540 个码位> \
  --lv-font-name lv_font_gb2312_14 --output main/fonts/gb2312_14.c
```

- 工具版本：`lv_font_conv` 1.5.3（全局 npm 安装，`node` 24.18.0）；完整参数同时记录在生成 `.c` 文件头部的 `Opts:` 注释里。
- 字符清单：请求 7540 个码位，全部 7540 个字形均生成。覆盖完整 GB2312 字符集，即 6763 个汉字（一级与二级，0xB0–0xF7 行）加 682 个符号（0xA1–0xA9 行：中文标点、全角 ASCII U+FF01–U+FF5E、单位与货币符号、箭头、制表符、圈数字、希腊与西里尔字母），另加 95 个 ASCII 可打印码位 U+0020–U+007E，用于状态行与提示。
- 本仓库固定参数：`--no-compress --no-prefilter` 保持 `bitmap_format = 0`（PLAIN），因为 lv_font_conv 1.5.3 默认输出的 RLE 压缩流不会被本仓库的 LVGL 9.6 配置解码。`--lv-include lvgl.h` 让两条 include 分支都落到 `#include "lvgl.h"`，与 `main` 引用 LVGL 的方式一致：组件目录名为 `lvgl__lvgl`，`lvgl/lvgl.h` 找不到对应 include 目录，且 `main` 未定义 `LV_LVGL_H_INCLUDE_SIMPLE`。
- 目标路径与集成：生成产物放在 `main/fonts/`，与编译它的应用同处。`main/fonts/gb2312_14.c` 需加入 `main` 组件的 `SRCS`，使用方用 `LV_FONT_DECLARE(lv_font_gb2312_14)` 声明符号。换字号重新生成会得到 `gb2312_<size>.c` 与符号 `lv_font_gb2312_<size>`。
- 来源与许可：字形由 `C:/Windows/Fonts/NotoSansSC-VF.ttf`（Noto Sans SC）渲染，该字体采用 SIL Open Font License 1.1，允许再分发与嵌入派生；仓库不提交该 TTF（同名文件由 Windows 随附，也可从 Noto 项目下载）。不要换成 SimHei、微软雅黑这类随 Windows 授权的字体：由它们派生的位图表不能随固件公开发布。换用其它可再分发字体时加 `--font` 重新生成即可。
- Flash 与 RAM：生成源码 2.93 MiB，链接进固件的字形数据是 Flash 中的只读常量。启用后的实测成本：app 镜像从 1.24 MiB 增至 1.59 MiB，8 MB 的 app 分区仍余 80%。LVGL 按需解码单个字形位图，不会整表载入内部 RAM。ESP32-C3 无 PSRAM，改字号或位深后请用 `idf.py size-components` 重新复核。
- 验证方式：`python3 tools/gen_chinese_font.py --verify-only` 复核字形数、导出符号与 PLAIN 位图格式；生成源码另用 `build/compile_commands.json` 中 `main` 的真实编译参数做过语法检查。

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

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
