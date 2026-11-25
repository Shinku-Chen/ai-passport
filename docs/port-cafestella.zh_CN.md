<p align="right">
  <strong>简体中文</strong> · <a href="port-cafestella.md">English</a>
</p>

# 星光咖啡馆移植说明

## 来源与范围

- 源：`xuanyide01/cafestella-rw5`，固定提交 `90ddb9e020970907b61ad4181f25c65267e6e98b`。
- 源仓库是 REDMI Watch 5 的已加工移植：`converted/graph.json`、`converted/n*.json`、`src/common/engine.js`、剧情分片、图像、表情和音频。
- C 移植读取 `tools/cafe_pack.py` 生成的专用包；不重新运行手表打包器，也不改只读源。
- 范围：923 节点、53,727 正文帧 + 1 个 OP 标记、69 组/141 个选项、7 主线结局、5 后日谈、189 章节条目、121 组/1,631 差分鉴赏、1,850 张源图。

## 代码结构

| 路径 | 作用 |
| --- | --- |
| `main/cafe/cafe_pack.[ch]` | 段索引、边界/CRC、zlib 块缓存、帧/选项/next/资产访问 |
| `main/cafe/cafe_vm.[ch]` | 等价 `engine.js` 的游标、flag、派生规则、条件、complete、历史 |
| `main/cafe/cafe_store.[ch]` | NVS 设置、3 手动槽、自动进度、永久解锁/CG 位、坏 blob 拒绝 |
| `main/cafe/cafe_image.[ch]` | JPEG + 2bpp mask + 立绘/表情合成到三条 224×42 条带 |
| `main/cafe/cafe_input.[ch]` | 纯逻辑三键短/长按状态机 |
| `main/cafe/cafe_ui.[ch]` | 320×240 LVGL 页面、列表、打字机、鉴赏、章节、弹窗 |
| `main/cafe/cafe_app.[ch]` | 页面/选择状态、推进、自动/快进策略、存读档 |
| `main/main.c` | 启动、横屏、输入队列、空闲/调暗/休眠、电量 |

`assets/cafe/cafe.pak` 通过 `target_add_binary_data` 链接进 factory app，不需要额外 data 分区，`idf.py flash`/`merge-bin` 都会带内容。包 6,424,952 B；app 8,246,672 B（factory 8,323,072 B，余 76,400 B）。

## 包格式

- 128 B 头：`CSPK1` 魔数、版本、头大小、文件大小、段数、源 manifest SHA-256。
- 段表：类型/偏移/大小/数量/CRC32。小表启动时校验；正文块和图片在读取/解码时校验。
- 正文帧序列化：`kind, speaker_len, text_len, bg_asset, sprite_count, sprites[8], speaker+NUL, text+NUL`，按 ≤8,988 B 原始字节切块（754 块）；12 KB 缓存是唯一解压工作区。
- 精灵坐标由打包器按 224×126 舞台预排；资产映射到一张底图 + 可选表情图集 + patch 矩形。
- 图像：剧情 1,997 条（JPEG + 可选 2bpp mask，3,464,731 B）+ OP 287 条（224x126 q15，620,030 B），合计 2,284 条 / 4,084,761 原始 B（去重后 3,360,348 B）。剧情背景 q10，标题背景 q18，立绘/图集 q18 4:4:4。

## VM 语义

`cafe_vm_locate` 严格照源实现：

1. `@end` 返回 END。
2. `index < frames` 返回 TEXT/OP。
3. 节点尾：除非 `progression == chapter`，把该节点所有 `complete` 位或进永久进度。
4. 有选项返回 CHOICE；否则按顺序取第一个 `when` 匹配的 `next`。
5. 目标/条件缺失是错误，绝不静默退回线性顺序。

`flags()` 从本周目 flag 字节开始，加 `flagRules` 派生值，再由五条女主线位算 `checkAnyClear`。`choose` 写选项 effect 并跳转。历史 24 个快照；`skip_chapter` 照源的“下一个 OP / 章节变化”规则。`kan_flag == 3` 因为源里没有写入者而保持不可达。

## 持久化

- 设置：速度、16/20 px 档、自动默认、对话框透明度、标题背景模式。
- 3 个手动槽和自动进度：node、帧下标、本周目 flag、永久进度、progression 模式、最后完成位、章节标签。每个 blob 有 magic/version/size/CRC；坏 blob 拒绝且不碰 live 状态。
- 永久解锁：主线/后日谈进度位、121 个 CG seen 位、最后通关路线。New Game 只清本周目 flag；读档把永久进度取并集，不回退。
- 章节试读（`progression=chapter`）不写完成位和永久进度。

## 图像与内存

- 三条 224×42 RGB565 条带（共 56,448 B）是唯一常驻舞台缓冲；LVGL 放大到 320×180。
- 每条带：224×42 RGB565 staging、最大 224×126 的立绘 JPEG（TJpgDec）、2bpp mask、106×44 patch 工作区；单块分配/工作区均 <32 KB。
- 背景在 224×126 居中；立绘用源映射舞台坐标；表情从 133×133 图集采样后 alpha 混合。
- 输入诊断版静态 DRAM：239,250 / 321,296 B，余 82,046 B；输入任务栈 4 KB；`-fstack-usage` 最大单帧 `show_frame_page` 1,120 B，图像链约 500 B。
- 合成前校验图片 CRC；失败时清黑条带并显示可见错误，绝不沿用旧帧。

## 阅读框布局

底部对话框高 60px（y180..239），图片仍为 0..179 全宽；16px 正文 3 完整行、
20px 正文 2 完整行，文本对象按字体行高与框内可用高度定位，不再使用旧的固定
4/3 行常量。app 使用按需 `gal_text_page_bounds`/`gal_text_page_count`，不设
16 页数组上限，长源句、中英混排和手动换行不漏字。自动提示使用图片区右下
固定 47x23 槽 x264..311/y154..177，写入文字后显示，Choice/Menu/Title/OP
不残留；名字在框上方，限宽省略。三段图使用 367/256 最小整数缩放修正，覆盖
x0..319，框从 y180 开始。

### 说话人名字贴合

名字背景按实际选中的 16/20px 字体 glyph 宽度加左右各 8px 计算，总宽上限
230px；短中文/英文名不再保留整条深色底。长名使用 `LONG_DOT`，保持在固定
Auto 槽 x264..311 左侧；每次 `set_text` 和切字号都重算；空名字文本与背景
一起隐藏。名字仍在框上方，不进入正文区域。

## JPEG 分段去重与 OP 8 kHz

全部 2284 张 JPEG（1997 故事图 + 287 OP 帧）保持字节不变，只共享完全相同的
DQT/DHT/APP/SOF/SOS/SOI/EOI marker 段：共享池 + 每图描述符 + 熵流 literal。
TJpgDec 直接分段读取，不新增整张 JPEG 拷贝或大栈缓冲；描述符使用显式
offset/length，绝不使用 padded C-struct 偏移；pool/desc section 在加载时校验
CRC。全部图片与 mask 已逐张字节还原并解码。

OP 音频改为 8 kHz 单声道 IMA（373,760 B 数据，743,959 样本 / 92.995 s），标题
仍 4 kHz 并 2x 到 8 kHz codec；OP 8 kHz 不再二次上采样。92.4 s 源 guard、
95.46 s 视频尾、seek/retry、busy-ACK stop 与深睡行为不变。包 6,424,952 B；
app 7,855,760 B（余 467,312 B）。

## ROM 解压栈安全

ESP32-C3 ROM 的 `tinfl_decompress_mem_to_mem` 会把完整的 `tinfl_decompressor`
（0x2af0 B）分配在调用者栈上，进入低层解码器前即占用约 11 KiB；输入任务 4 KiB
栈在 NewGame 和章节预览时因此溢出。阅读器改为直接调用 ROM `tinfl_decompress`，
每次调用用堆上独立持有的 `tinfl_decompressor`，ROM 低层帧仅 0x100 B，不保留共享
解压状态。分配失败返回 `false`，调用方报正文错误且不推进剧情游标；正文
zlib/Adler、长度、CRC 与图片 mask 格式均不变。`tests/test_cafe_inflate.c` 解压
全部 754 个正文块并校验 CRC，损坏/截断流负控拒绝；target 反汇编证据见交付报告。

## 画面缓存

只有 `last_art_valid` 为 true 时才信任 `last_art_hash`；hash 为 0 不是永远有效的哨兵。
hash 覆盖背景 asset 与精灵语义字段（asset/left/top/opacity/reserved），不比较 struct
padding。正文推进和文本 `enter_location` 走缓存，真正不同画面才重绘；标题/CG/OP/
资源故障/新游戏/preview/存读档 rebind/app 初始化都会失效缓存。首次或失效后的重绘
仍做 CRC/JPEG 校验；同视觉正文跳过 scene decode 与 verify。FF 每段只输出一条有界
总结（步数、wall、最大单步、scene/verify 调用），不逐帧打印。

## 回想与快进契约

包内鉴赏记录布局是 `u16 first_variant, u16 count, u8 section, u8 adult,
u32 label_off, u16 first_node, u16 reserved, u16 reserved`；阅读器从 `+6`
读未对齐 label、从 `+10` 读 first_node。旧的 `+8/+12` 结构体填充偏移导致
回想/CG 名称全空白。

场景回想使用明确的 6 行可见页；选择是全部已解锁组的全局索引，每页只重建
当前可见标签，OK 用同一全局索引解析到组的精确 `first_node`。0 个解锁组显示
`尚未解锁任何 CG` 和解锁说明，既不空白也不伪造解锁；16/20 组受控测试覆盖
首末行、翻页、标签与高亮。

长 UP 达到 500ms 阈值立即进入下一句：跳过 typing 和当前句剩余分页，再只
推进 VM 一次。每个输入控制 tick 最多推进一句；没有 60/90/120ms 句间累积或
catch-up 循环。松手立即停且不补短按；选项/结局/OP/资源错误页停住。正常短按
仍逐完整分页。

## 鉴赏室 OP 播放

鉴赏室新增独立的 `OP 播放` 第五行，无通关前置；使用与剧情 OP 相同的 287 帧
时间轴和 8 kHz OP 音频。短 OK 跳过、长 OK 返回同一行，自然结束也返回该行；
不修改 VM/flag/进度/preview/历史/auto/手动存档/NVS。音频/JPEG 故障重试保留
鉴赏室上下文，返回时停止 OP 音频。剧情 OP 仍只推进 VM 一次。此轮仅 UI/app
接线，pack `b397...` 未变。

## OP 画面呈现与源细节

剧情与鉴赏室 OP 播放时都实际隐藏过场 eyebrow/title 对象，画面区域不再有应用
标题/提示胶囊；仅在非视频的底部条带保留短控制提示。普通章节过场仍显示标题。
OP 源帧原生为 224x126（不再降采样到旧的 128x72），以 q15 编码并映射到既有
224x126 条带；287 帧、3 fps、95.460 s、92.4 s guard 与 8 kHz IMA 字节均不变。
相对源帧测得的新路径 PSNR/SSIM 优于旧 128x72 路径，但源本身只有 224x126，
不伪造更高分辨率细节。

## 验证与证据

- `tests/test_cafe_trace.c` 重放 160 条源 JS 轨迹：141 个包内选项、7 结局（新周目 + 通关后两种起点）、5 后日谈、skip/previous；436,182 条命令逐步比对 node/index/进度/flag/全部 when。合法可达是 79 个选项（正常 67 + 后日谈试读 12）；另 62 个是源 A 类被前置 choices-before-next 遮蔽的重复项。
- `tests/test_cafe_input.c` 可控时钟验证短/长/松手/不双触发。
- `tests/test_cafe_app.c` 驱动横屏状态机：短按推进、长 OK 菜单、长 DOWN 自动、长 UP 不依赖打字/自动等待的持续快进、松手停、选择点停、自动中手动推进、切页清理；并断言 CHOICE 页一次长 OK 直接进菜单、关闭后恢复原选择页，标题/菜单长方向移动一行且 UI 选中同步。
- `tests/test_cafe_input_timing.c` 用事件时间穿过延迟释放，并保留按处理时间判定的旧逻辑负控。
- `tests/test_cafe_input_overload.c` 验证不完整按键/快进周期被取消、下一次完整按键只移动一次，不产生幽灵 SHORT/LONG。
- `tests/test_cafe_nav_pages.c` 用真实按键覆盖音乐 2 轨（短/长、选中行 UI、OK 播放）、剧情鉴赏 7 路线/章节分页/后日谈最后页按实际数量，以及 preview 不写永久进度。
- `tests/test_cafe_recall.c` 覆盖受控 0/1/16/20 组的真实标签、空状态、6 行分页与末项高亮；`tests/test_cafe_ff.c` 覆盖阈值快进、多页首/中页、typing/auto、每 tick 最多一句、松手与无 catch-up。
- `tests/test_cafe_ff_cache.c` 用真实 pack/JPEG 和 decoder 计数：同视觉推进不重解码，标题/rebind/新画面会失效；旧 force-true 变体在同一断言失败。
- `tests/test_cafe_jpeg_dedup.c` 逐张重建并解码 2284 张 JPEG；`tests/test_cafe_jpeg_guard.c` 拒绝 pool/descriptor CRC 与 literal 损坏。`tests/test_cafe_audio*.c` 覆盖 OP 8k/标题 4k 与 guard/seek 输出。
- `tests/test_cafe_inflate.c` 用低层解压路径解压全部 754 个包内正文块并校验 CRC，拒绝损坏/截断 zlib 流。
- `tests/test_cafe_menu_flow.c` 用生产 app 按键走 CHOICE 菜单直达、存/取消/覆盖、读档/空槽、设置/历史、Continue/重启与后日谈 preview 往返；不直接写 `app.page`/`app.vm`/flag 伪造到达。
- `tests/test_cafe_ending.c` 校验 12 条 ending 记录按 pack 布局读回非空；`tests/test_cafe_ending_app.c` 走完 7 主线（卡片 + 永久位）与 5 后日谈 preview。
- `tests/test_cafe_store.c` 设置/槽/解锁往返 + 坏 CRC/长度拒绝。
- `tests/test_cafe_lifecycle_contract.py` 检查源启动顺序（横屏结果、UI/app 初始化在 BSP 锁内、首帧 refresh 后才开背光、button/task 回滚）；UI smoke 还渲染 CJK 错误屏。
- `tests/test_cafe_image.c` 用独立 Pillow 参考解码/合成 7 帧（0–5 立绘、表情差分）比对；2,284 张图片 CRC 全过（1,997 剧情 + 287 OP）。
- `tools/cafe_audit.py` 把 pack 的每一帧/选项/next/complete/章节/后日谈/鉴赏记录与源图对账，并复核源 SHA。

`--static` 在流程修复后 PASS；`./tools/validate.sh --firmware` 在 Git Bash 仍为 **FAIL (environment)**（`idf.py` 不可用；ESP-IDF 5.5.3 不支持 MSys/Mingw），native PowerShell 的 ESP-IDF 5.5.3 build/merge/verify/archive 等价链 PASS；新流程修复 archive 位于 `build/firmware/` 的内容寻址目录，旧 `caa05...` bundle 单独保留。设备显示方向、真实按键时序、长时间阅读 heap 与音频仍为 NOT RUN/未验证。

## 流程修复差异

- ending 记录按 pack 的 `+2/+6` 偏移读取；12 条记录均可读回非空标题/正文，旧 `caa05` bundle 与证据保留。
- CHOICE 页一次长 OK 直接打开菜单；菜单记录来源页和选择位置，关闭时恢复同一 `choice_sel` 的选择页，GAME 页则恢复原正文分页。Save/Load/Settings/History 都沿同一来源页返回。
- 严格 host 回归为 `test_cafe_menu_flow.c`、`test_cafe_ending.c`、`test_cafe_ending_app.c` 和更新的 `test_cafe_app.c`；修复前 38-check/5-FAIL 与 7/7 空白卡日志仍保留在旧 supplemental/audit 目录。
## 第三方代码

`main/cafe/tjpgd.c` / `tjpgd.h` 为 TJpgDec R0.03（(C)ChaN, 2021），保留原始许可声明；配置头为本移植本地设置（RGB565、512 B 输入缓冲、支持缩放、table clipping）。

## UI smoke 与媒体预算

- `tests/ui_smoke/run.sh` 用 LVGL 9.5 软件显示 + 64 KB 池编译生产 `cafe_ui.c`。当前 30 屏包括标题、16/20px 长句、菜单、存读档、鉴赏、CG 列表、章节路线/章节、历史/回想、CG 全屏、OP 首/中帧、音乐、音频错误、设置和 CJK 错误屏。64-bit 宿主：总 58,120 B、结束余 11,376 B、峰值 48,832 B、碎片 23。32-bit WoW64（生产 UI/字体/图像路径，音频 HAL 桩）：56 KB 配置 30 屏 PASS，总 53,484 B、结束余 23,096 B、最大连续块 21,576 B、峰值 32,156 B；64 KB 配置 30 屏 PASS，总 61,676 B、结束余 31,288 B、最大连续块 29,768 B、峰值 32,156 B。均为 x86 宿主，不是 ESP32-C3 设备证据。
- 当前生产媒体：标题 4 kHz IMA 516,096 B（1,027,198 samples/256.7995 s）；OP 8 kHz IMA 373,760 B（743,959 samples/92.995 s）；OP 287 帧 224x126 q15/3fps（620,030 B，95.460 s）。92.4 s OP guard 是源 pre-EOF 停止，不声称全采样均播放；源文件未裁剪/拉伸。OP 显示复用 224x42 条带和 `s_stage`，无整帧分配。
- 全量画质探索数据保留在 `scratch/cafestella-review/quality/`；生产选择为 BG10/224 + q18 立绘/图集/标题，不是 q16/BG12/256 备选。

## 已知缺口与风险

- 音频与 OP 视频已内置（标题 4 kHz / OP 8 kHz IMA；224x126 3fps），但未做设备扬声器/面板时序、heap/CPU 验收。
- 未做设备测试：横屏 MADCTL 方向、真实 ST7789 上的 LVGL 图片缩放、首帧/背光顺序、长时间阅读 heap 碎片均待真机确认。
- 源字体缺 U+FF1B、U+76CF、U+00E9；UI 文案已避开（全角分号改逗号，UI 的 `Café` 写 `Cafe`）。正文 U+3000 因源字体无字形，在 pack 内统一成半角空格。
- 224×126 舞台是刻意降采样，背景 q10；在 320×240 屏上可见，是容量决策而非保留源分辨率。
