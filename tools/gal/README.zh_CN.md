<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Galgame 素材流水线

这套工具把上游视觉小说转换成 `main/gal/*` 读取的只读 `assets` flash 分区。

## 素材放在哪里

美术与章节剧本属于第三方内容，**本仓库不跟踪**。把本地副本放到:

```text
assets/gal-source/common/
├── bg/       背景与演出效果图(PNG)
├── fg/       角色立绘与表情补丁(PNG)
├── *.txt     章节剧本(JSON,每章一个文件,命名为 1..N)
└── text_bg.png, logo.png
```

该路径已加入 git 忽略。目录结构与上游项目的 `src/common` 一致,可以直接原样拷进来。

目录缺失时打包器会改为生成一个小型占位包,因此全新 clone 依然能配置、构建并启动。
占位路径不需要图像库。

## 工具

| 工具 | 用途 |
| --- | --- |
| `gal_format.py` | 磁盘格式的唯一权威定义。同时生成 `main/gal/gal_format.h`,后者带 `_Static_assert` 守卫,使 C 与 Python 两端无法悄悄漂移。 |
| `pack_assets.py` | 把美术与剧本打包成 `gal_assets.bin`。 |
| `gen_font.py` | 在 `assets/fonts/` 下生成 CJK 字体子集。 |
| `inspect_pack.py` | 反向读取包:布局、对齐、逐图统计、章节导出、单图导出。 |

### 打包

```bash
python tools/gal/pack_assets.py --source assets/gal-source/common --out build/gal
python tools/gal/inspect_pack.py build/gal/gal_assets.bin
python tools/gal/inspect_pack.py build/gal/gal_assets.bin --chapter 3 --lines 4
python tools/gal/pack_assets.py --preview 8 --preview-out scene8.png
```

打包真实美术需要 Pillow。请装进 ESP-IDF 构建所用的解释器,否则固件构建会在打包器这一步停下:

```bash
"$IDF_PYTHON_ENV_PATH/Scripts/python.exe" -m pip install Pillow   # Windows
python -m pip install Pillow                                       # Linux/macOS
```

`--preview` 会按屏幕分辨率合成某一章首个带立绘的场景。对白面板几何与立绘原点就是这样定下来的;
改动任何版式常量之前先用它核对。

### 字体

```bash
python tools/gal/gen_font.py
```

子集 = 已打包剧本中的全部码位 ∪ `main/gal/gal_strings.h` 中的全部非 ASCII 字符,
因此界面文案不可能落在子集之外。**在固件的其它位置新增中文字面量会渲染成占位方框** ——
请写进那个头文件再重新生成。

生成的 `.c` 文件需要提交:CI 既没有转换器也没有源字体,构建不能依赖这两者。
源字体与许可见 [`assets/README.md`](../../assets/README.zh_CN.md)。

转换器通过 `node` 直接调用已安装的脚本,而不是 `lv_font_conv` 的 shim:
shim 会把参数经 `cmd.exe` 传递,而 cmd.exe 在约 8 KiB 处截断命令行,无法容纳 CJK 子集的字符表。

## 包格式

```text
gal_pack_header_t        magic 'GALA'、版本、图片与章节数量、各项偏移
gal_image_entry_t[]      每张图一项,含其在屏幕上的偏移
名字块                    以 NUL 分隔的图片名
gal_chapter_entry_t[]    每章的偏移与长度
图片负载                  [256 x lv_color32_t 调色板][每像素一字节索引]
章节负载                  头 + 场景记录 + 对白记录 + 文本池
```

改动这里时需要记住的几点:

- 图片使用 LVGL 的索引 `I8` 布局。`LV_BIN_DECODER_RAM_LOAD` 处于关闭状态,
  LVGL 每次只解码一行扫描线,因此一张全屏背景只占约一行扫描线的 RAM,而不是 150 KB 帧缓冲。
  本板没有 PSRAM。
- 固件会强制转换为结构体的每个偏移都按 4 字节对齐,读取器会拒绝不满足这一点的包。
  未对齐的结构体访问是未定义行为,在本目标上编译器有权假设类型所要求的对齐。
- 章节正文与说话人名按章分池存放,并以 NUL 结尾。

## 屏幕几何

源素材是 336x480,屏幕是 240x320,因此打包器按 `240/336` 缩放并丢弃多出的行。
立绘在取 alpha 包围盒之前会先裁到真正能进入屏幕的区域;不做这一步会让立绘体积翻倍以上。

屏幕版式的权威源是 [`main/gal/gal_layout.h`](../../main/gal/gal_layout.h)。
打包器在 `--preview` 时会反读该文件,因此它合成的画面不可能与固件实际绘制的不一致。
它唯一自带副本的几何量是立绘原点——那个值已经烧进了存储的立绘偏移里。

对白面板与 Saya 参照版一致:扁平、完全不透明、满宽、顶部一条 2 px accent 线、无圆角。
上游那张面板图干脆不打包——它是带重复装饰纹样的近白底板,在任何可用的不透明度下都
会与画面争夺注意力。去掉它同时省下 37 KiB。

面板高度是量出来的,而且**由字号推导**,不是固定值:面板 = `4 × 行高 + 6`,
因此两种字号下一页四行都能完整显示。

| 字号 | 行高 | 面板高度 | 面板顶边 | 画面可见区 |
| --- | --- | --- | --- | --- |
| 16 px(小) | 19 px | 82 px | 238 | 214 px(67%) |
| 20 px(大) | 23 px | 98 px | 222 | 198 px(62%) |

16 px 下 220 px 文字区每行放 13 字,因此一页可容纳:

| 文本区 | 面板高度 | 容纳 | 需要翻页的对白句数 |
| --- | --- | --- | --- |
| 3 行 | ~64 px | 39 字 | 175 (3.76%) |
| **4 行** | **82 px** | **52 字** | **8 (0.17%)** |
| 5 行 | 100 px | 65 字 | 2 (0.04%) |
| 6 行 | ~118 px | 78 字 | 0 |

面板直抵屏幕底边,底部的圆角由显示驱动的圆角遮罩裁出。默认 16 px 下高 82 px,
约为原版 152 px 的一半。注意这里与 Saya 参照版不同:它是固定框高、大字号时每页降到三行;
本移植版始终显示四行——这正是「面板高度随行高推导」换来的。
放不下的那八句是**翻页而不是裁掉**:`gal_text_pages()` 在纯 model 层切页,阅读器一次显示一页,
页号也是存档位置的一部分。这与 Saya 版的做法一致,包括它
「闭合标点不得落在行首」的禁则规则。
`units_per_line` 特意取保守值(27.5 单位的文字区只用 26 单位),
因此 model 放在一行里的字永远不会超过 LVGL 实际能排下的量。

说话人名放在面板之外,位于画面左下角。

## 上游缺陷

上游剧本引用了 8 张它自己的仓库里并不存在的图,其中 5 处是明显的笔误。
原版渲染器在这种情况下会画出空白帧,因此打包器会替换为最接近的现有素材,并逐条报告:

```text
line2            -> line21           文件名被截断
zev_ask_c03_01   -> ev_ask_c03_01    多了一个前导 "z"
zev_ask_c03_11   -> ev_ask_c03_11    多了一个前导 "z"
ev_ask_c03_03    -> ev_ask_c03_01    该素材不存在
ev_ask_c01_06    -> ev_ask_c01_03    该素材不存在
a0014h           -> a0015h           缺一个表情
ask_z2a0100      -> ask_z1a0100      缺一个立绘变体
ask_z2b0100      -> ask_z1b0100      缺一个立绘变体
```

被替换的引用在剧本中保留自己的名字,但解析到借用的素材,因此该素材只存一份而非两份。
如果不认同某个替换,修改 `pack_assets.py` 顶部的映射表即可。

## Flash 预算

`assets` 分区为 4 MiB(`partitions.csv`)。`main/CMakeLists.txt` 会给打包器传
`--max-bytes`,超出时构建失败而不是产出被截断的分区镜像,因此分区尺寸与包体量不会悄悄失配。
当前包体量约 3.6 MiB。
