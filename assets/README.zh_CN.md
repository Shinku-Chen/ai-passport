<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

| 文件 | 格式与大小 | 用途与来源 |
| --- | --- | --- |
| [`fonts/intercom_cjk_16.c`](fonts/intercom_cjk_16.c) | LVGL 9 C 字库源码，16 px、4 bpp、PLAIN（非压缩）位图，7540 个字形；链接进镜像的位图数据 900691 字节，源码 6622442 字节（6.32 MiB） | 对讲机设备屏显示所需的完整 GB2312 覆盖。由 Pillow（FreeType）从 Source Han Sans SC Normal（SIL OFL 1.1）栅格化；用 `tools/intercom_font.py` 重新生成。 |
| [`fonts/intercom_cjk_symbols.txt`](fonts/intercom_cjk_symbols.txt) | 码位清单，每行一个 `U+XXXX`，升序去重；52780 字节 | 覆盖的 7540 个码位，便于消费方不解析 `.c` 就能做字符覆盖检查。与字库一同重新生成。 |

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### GB2312 中文字库（`lv_font_intercom_cjk_16`）

- 生成命令：在仓库根运行 `python3 tools/intercom_font.py`（加 `--check` 只自检已有产物而不重新生成）。脚本用 Pillow/FreeType 自己栅格化，并自行写出 LVGL 9 的 `fmt_txt` 格式：

```bash
python3 tools/intercom_font.py                 # 生成,并逐像素比对
python3 tools/intercom_font.py --check         # 只自检已提交的产物
python3 tools/intercom_font.py --font <静态 CJK 字体>
```

- 为什么不用 `lv_font_conv`：该工具停在 1.5.3（2021 年），在 Node 24 下写出的 4 bpp 位图与 LVGL 9 的 PLAIN 解码路径不匹配，实机表现为乱码。本脚本移植自 `feature/sanoba-witch` 分支的 `tools/sanoba_font.py`（该方案已在该分支真机验证过），只把字符集合与界面常量换成本应用自己的。
- 来源与许可：字形由 `SourceHanSansSC-Normal.otf`（思源黑体 SC Normal）渲染，该字体采用 SIL Open Font License 1.1，允许再分发与嵌入派生。脚本优先使用 LVGL 组件自带的那份：`managed_components/lvgl__lvgl/scripts/generators/built_in_font/SourceHanSansSC-Normal.otf`；仓库不提交字体文件本身。不要换成 SimHei、微软雅黑这类随 Windows 授权的字体：由它们派生的位图表不能随固件公开发布；也不要用可变字体 `NotoSansSC-VF.ttf`——它过细的默认实例正是旧版 14 px / 2 bpp 字库在屏上发糊的原因。
- 字符清单：请求 7540 个码位，全部 7540 个字形均生成（源字体全部命中）。覆盖完整 GB2312 字符集，即 6763 个汉字（一级与二级，0xB0–0xF7 行）加 682 个符号（0xA1–0xA9 行：中文标点、全角 ASCII U+FF01–U+FF5E、单位与货币符号、箭头、制表符、圈数字、希腊与西里尔字母），另加 95 个 ASCII 可打印码位 U+0020–U+007E，用于状态行与提示。同一集合也写在 `fonts/intercom_cjk_symbols.txt`。
- 本仓库固定参数：16 px、4 bpp、PLAIN（非压缩）位图，`line_height = 20`、`base_line = 3`。字形按连续 nibble 打包、字形起点按字节对齐，所以 `stride` 保持 0；ASCII 用紧凑的 `FORMAT0_TINY` cmap，其余码位用 `SPARSE_TINY` cmap。`main/oc_ui.c` 的排版就是按这个 20 px 行高对齐的。
- 目标路径与集成：`main/CMakeLists.txt` 用 `target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/intercom_cjk_16.c")` 编译生成源码，使用方（`main/oc_ui.c`）用 `LV_FONT_DECLARE(lv_font_intercom_cjk_16)` 声明符号。不生成声明头；`.c` 自包含，只 include `"lvgl.h"`。
- Flash 与 RAM：位图数据 900691 字节（880 KiB），以只读常量（DROM）链接进 Flash；6.32 MiB 的源码文本只是构建输入，不进镜像。本次改动的实测成本：app 镜像从 1669472 字节（1.59 MiB）增至 2218384 字节（2.12 MiB），即 +548912 字节，8 MB 的 app 分区仍余 73%。LVGL 按需解码单个字形位图，不会整表载入内部 RAM。ESP32-C3 无 PSRAM，改字号或位深后请用 `idf.py size-components` 重新复核。
- 验证方式：`python3 tools/intercom_font.py --check` 重新栅格化源字体并与已提交的 `.c` 逐像素比对，同时复核导出符号、字形数、PLAIN 位图格式、ASCII `FORMAT0_TINY` 区间、行高与码位覆盖。重新生成的字体另用 ESP-IDF 5.5.3 （`idf.py -B build build`）完成过一次真实构建。

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
