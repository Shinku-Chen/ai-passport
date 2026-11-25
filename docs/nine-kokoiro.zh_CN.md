<p align="right">
  <a href="nine-kokoiro.md">English</a> · <strong>简体中文</strong>
</p>

# 9-nine- 九次九日九重色：工程说明

本文说明 Episode 1 离线阅读器的实现、包格式、预算与验证，供复核与复现。用户操作与
内容概览见根 README。

## 1. 固定输入

| 项 | 值 |
| --- | --- |
| 仓库 | <https://github.com/xuanyide01/9-nine-rw5> |
| 提交 | `6a8907a5ae576f4377abe4b74f15bd9af13efe07` |
| 本作目录 | `nine-kokoiro-rw5` |
| Graph | `src/common/graph.js`（74 节点、chunks、条件、效果、选项） |
| 正文 | `src/common/story/**/*.txt`（91 个分片） |
| 运行语义 | `src/common/engine.js`、`src/common/chunk-codec.js` |
| 图像/音频 | `src/common/images`、`faces`、`ui`、`opening`、`converted/gallery.json`、`*-bgm.opus` |

转换器只读这些文件，不运行上游预处理脚本，也不重建未提交的 PC 原作脚本。分支以
graph 为准，正文/画面以 story 分片为准。

## 2. 源清单

| 指标 | 值 |
| --- | ---: |
| 节点 | 74 |
| 记录 | 8050（8049 句正文 + 1 条 OP 标记） |
| 正文 UTF-8 字节 | 395,919 |
| 选项组 / 选项 | 5 / 9 |
| next 边（含选项边） | 94 |
| 状态变量 | 15（5 个永久键） |
| 章节 | 17 |
| 结局 | 2（`loop1`、`true`） |
| CG 组 / 差分 | 22 / 73 |
| 源 OP | 599 帧、6 fps、384×216、99.9 s |
| 源 BGM | 2 轨 Opus |

审计逐节点核对数量、逐句核对正文/说话人、逐边核对目标/条件/效果/标签、逐记录核对
章节/结局，并逐帧核对视觉层。

## 3. 阅读器语义

`main/nine/nine_vm.c` 是已加工 graph 的解释器，不是 PC 原作脚本 VM。复刻自
`engine.js` 的契约：

- **首次匹配 next**：按源顺序遍历 `nexts`，取第一个条件成立者。选项边排在其后；
  next 边若携带效果，视为不支持的包扩展并报错。
- **条件**：`==`、`!=`、`<`、`<=`、`>`、`>=`；嵌套 `all` 编译成后缀 token 与
  一个 value=操作数个数的 AND。布尔栈上限 64。
- **效果**：int32 的 `=` 与 `+`，溢出拒绝。永久键集合记录在包的变量表里；正常
  游玩写入 progress，章节试读跳过永久写入。
- **永久键合入**：源 `flags()` 只把 `loop_my` 暴露给条件（不是所有永久键）。C 完全
  照做。`loop1`、`true`、`clear_true`、`titleBg` 保留在永久记录中，用于结局、标题
  主视觉和解锁。
- **complete**：packer 按源顺序把 `complete[]` 折进节点效果，含派生的
  `loop_my=1`/`clear_true=1`。试读时永久写入被跳过，不会记录结局。
- **筛选后无选项回退**：对存在 `visibleChoices` 的篇章，packer 置位；若所有选项都
  被筛掉，阅读器重新给出全部选项，避免死锁。Ep1 本身没有条件选项；该路径由合成
  测试包覆盖。
- **试读隔离**：试读保留自己的周目标记，但不写永久键、不写自动进度、不解锁
  CG/章节/结局。
- **历史**：24 个检查点，按“已置位变量”稀疏打包。`previous()` 恢复检查点，但叠加
  最新永久进度，回退不会撤销已达成结局。
- **跳转上限**：每次 locate 最多 500 次非正文跳转；自环报 `graph hop limit`，不会
  死循环。
- **存读档**：恢复节点/下标/正文页/周目标记，并合入最新永久进度。解码是事务性的：
  CRC、身份、长度、键或游标任一不符都不改动 live 状态；坏档只提示，不静默擦除。

## 4. 包格式

两个不可变的小端文件嵌入 app 镜像；没有 native 指针，也不依赖加载地址。

`nine_script.bin`（魔数 `NINESC1\0`、版本 2、256 B 头、整文件 CRC32）：

| 表 | 记录 | 内容 |
| --- | --- | --- |
| nodes | 48 B | frame/effect/next/choice 范围、标题、章节、稳定 id、源标签 |
| frames | 24 B | kind、visual、正文块/偏移/长度、说话人 |
| text blocks | 16 B | zlib 流、原始大小 ≤4096、CRC32 |
| visuals | 8 B | 首层、层数 |
| layers | 24 B | 图、x、y、宽、高、不透明度 |
| variables | 8 B | 名字、永久标记 |
| conditions | 12 B | 操作码、变量、int32 |
| effects | 12 B | set/add、变量、int32 |
| edges | 24 B | 目标、条件/效果范围、标签 |
| chapters | 8 B | 标题、节点 |
| endings | 12 B | 名字、变量、描述 |
| gallery | 16 B | 名字、图、组、缩略图 |

`nine_images.bin`（魔数 `NINEIM1\0`、版本 2、128 B 头）：

- 32 B 图像记录：kind（JPEG / indexed / raw RGB565）、宽高、绝对负载偏移、负载大小、
  CRC32。
- indexed：调色板数、块行数（8）、块数、RGBA 调色板、zlib 索引块；块偏移为绝对
  文件偏移。
- OP 时间轴：12 B 记录（图、时间戳、持续）。
- 音频：32 B 记录（名字、偏移、大小、时长、采样率、声道、CRC32）；负载为 IMA
  ADPCM 块（132 B / 256 样本）。
- 头部还带 3 张标题主视觉 id。

资源包由 `main/nine/nine_assets.S.in` 经 `gen_assets.cmake` 在构建时生成汇编嵌入，
段基址 16 字节对齐；每个图像负载按 4 字节补齐，保证 RISC-V 上 RGB565 的 16 位读取
对齐。重新生成 pack 会触发构建期汇编再生成，不会残留旧包。

## 5. 显示管线

- 320×240 横屏；舞台 320×180 在 y=0，对话框 60 px 在 y=180。
- 说话人名牌在 60 px 面板上方/外侧（底边 ≤ y179），直接压在舞台画面上；
  背景为贴合名字宽度 + 左右各 8 px 的半透明胶囊，最长 160 px，超长名字省略号；
  名字为空时名牌和背景一起隐藏。正文切到 20 px 时名牌仍用 16 px 档。
  阅读页不显示数字页码。
- 源 432×243 美术两轴统一用 16/27（x 256/432、y 144/243）：背景 256×144，立绘/表情
  的锚点、offset 与裁片都用同一比例；没有纵向单独压缩，不裁切、不加黑边。
- 4 条独立 256×36 RGB565 条带，每条 18,432 B（合计 73,728 B）。JPEG MCU 与
  indexed 块直接解码进条带。每条保持 256×36 原生对象尺寸，放在 y=0/45/90/135，
  LVGL 以 scale=320（1.25x）、pivot=(0,0) 绘制成 320×45；四条覆盖 320×180，
  不会因默认中心 pivot 被裁边，也没有拼带缝隙。应用把 `lv_screen_active()` 作为
  父对象；生产版 `nine_image_init(NULL)` 会被拒绝；真实 LVGL 回归填满四条带、
  检查 0..179 覆盖，并解码全部 gallery/OP 资产。
- JPEG 由 TJpgDec 解码。本树修正了它的 RGB565 打包：解码 MCU 缓冲是 B,G,R，旧代码
  当 R 打包会得到 BGR565，导致红蓝互换。图像参考审计在交付前抓到了该问题。
- indexed 立绘应用“调色板 alpha × 层不透明度”；表情按源锚点从 256×256 图集裁切，
  叠在身体立绘上。
- 标题主视觉 3 张、CG 缩略图 22 张以原始 RGB565 存在 Flash，直接交给 LVGL 图像
  描述符，无需解码或内存拷贝。标题主视觉把源 Logo 烘焙到横屏位置。CG 全图与每一帧
  OP 都按同一 256×144 16:9 打包并适配到 320×180 舞台；CG 信息条从 y=180 开始，
  OP 页复用同一舞台，不再有第二块画布。
- 预解码失败时先把画布清黑再返回失败，绝不把上一帧当作当前句的画面。
- LVGL 刷新缓冲为 320×16（10,240 B，单缓冲）。启动时在 BSP 锁内设置横屏并核验
  LVGL 分辨率必须为 320×240，否则拒绝继续。背光门在锁内 arm 并请求真实界面刷新，
  只有在 LVGL 完成一次完整刷新周期（先 FLUSH_START、再 REFR_READY；非物理传输完成）后才开背光；最多重试 3 次，
  锁失败或超时保持暗屏并拒绝 ready。所有 UI 初始化与 setter 都由输入任务在
  `bsp_lvgl_lock()` 内调用，不只依赖 LVGL 任务。框架 UI 重排为横屏：标题菜单右置、
  6 行菜单、5 行存读档（边缘翻页）、5 行章节/历史窗口、3×3 CG 格、60 px 对话框。
- GAME 或 CHOICE 上**一次长 OK**直接进入菜单；菜单、存读档、设置、历史返回时恢复
  原正文分页，或恢复完整选项列表与 `choice_sel`，不会留下 GAME 页面 + CHOICE VM。
  从 START 进入 TITLE 不会用初始 n0 覆盖 Continue 已加载的自动档；从活动阅读/菜单
  离开到标题仍会保存当前点。

## 6. 正文与字库

- 正文共 100 个 zlib 块，单块原始 ≤4096 B；一帧 = 块号 + 字节偏移 + 长度，块内以
  NUL 结尾。阅读器只把当前块解压进 4096 B 缓存，并在下次调用前复制该句。
- 分页用 `gal_text_pages`：16 px 档每行 36 个半角单位、3 行；20 px 档 28 单位、
  2 行。行首禁则标点留在上一行；全文完整分页。16 px 标签高 57 px，落在 60 px 面板
  内；20 px 标签高 46 px。源最长句 80 字（16 px 最多两页，20 px 最多三页）。
- 打字机跑在 LVGL 定时器。短按就是一次真实推进：先显示当前分页单元，再在同一次
  按键里进入下一个分页单元/下一句；绝不只用来结束动画。快进按句推进：每个 50 ms 控制 tick 最多一句，跳过打字、自动间隔
  和当前句剩余分页；到选项/结局/OP 停止，松手不补短按。普通短键仍是
  一次分页推进。
- 字库由 `tools/nine_font.py` 生成（复用仓库自研 FreeType 栅格化 + LVGL 字模发射）。
  16/20 px 两档、2182 字形、4bpp。主字体为 `nine-text.otf`，其中缺的 19 个码位由
  Noto Sans SC 补齐。每个字形按 LVGL cmap 语义回读并与栅格逐像素比对（0 不符、
  0 缺字）。`tests/test_nine_assets.py` 再从 pack 可达的全部字符与 ASCII 反查
  symbols 覆盖。

## 7. 音频

- 两首源 Opus 由 ffmpeg 离线解码为 8 kHz 单声道 PCM，再编码为 IMA ADPCM（132 B 块 /
  256 样本）。
- 播放跑在独立 3 KB 栈任务里；解码出 512 B PCM 块后经 `bsp_audio_write()` 输出。
  音频任务不访问 LVGL，也不访问图像解码器。
- OP 页复用同一任务；音频位置选择 3 fps 的 OP 帧，时间戳表驱动幻灯片。跳过或播完
  都停止任务并回到正文/鉴赏。
- 启动时校验音轨 CRC 与块头；坏轨让 media init 失败，UI 提示音频不可用，而不是
  播放噪声。

## 8. 持久化

NVS 命名空间 `nine`：

| Key | 内容 |
| --- | --- |
| `cfg` | 文本速度、字号档、默认自动、透明度、音乐、标题主视觉 |
| `unl` | 16 B 永久位图：CG 组 0–31、章节 32–63、结局 64–65 |
| `prg` | 永久进度映射（稀疏 key/value + 置位），CRC32 + 包身份 |
| `auto` | 自动阅读进度（同一稀疏存档记录）；到结局时写入 1 B 无效记录 |
| `s00`..`s29` | 30 个手动存档（稀疏存档记录） |
| `snd` | 音乐开关 |

存档记录 = 32 B 头 + 每个已置位变量 6 B + 计数 + CRC32，并绑定脚本 CRC（包身份）。
单槽最大 220 B；30 槽 + auto/progress/unlock/config 约占 24 KB NVS 分区的 9 KB。
启动时扫描一次槽位用于列表标签；头部损坏的槽显示为“已占用/损坏”，读取时提示错误，
不改动 live 会话。

## 9. 内存与 Flash 预算

| 区域 | 字节 |
| --- | ---: |
| Flash：剧情包 | 555,087 |
| Flash：图像 + 音频 | 3,350,479 |
| Flash：16 px 字库（编译估算） | 297,240 |
| Flash：20 px 字库（编译估算） | 419,393 |
| Flash：app 镜像 | 5,339,504（0x517970） |
| Flash：合并镜像 | 5,405,040 |
| factory 分区 | 8,323,072（余约 2.98 MB） |
| DRAM 静态已用 / 总量 | 171,660 / 321,296 |
| 启动后 DRAM 余量 | 149,636 |

运行期固定分配：条带 73,728 + 刷新 10,240 + 输入栈 4,608 + 音频栈 3,072 +
历史 4,608 + I2S DMA 约 2,880 = 99,136 B。NVS/音频初始化后剩余堆估计约 46 KB。
这是静态估算，不是真机 `heap_caps` 读数。

目标 LVGL 池为 48 KB。32 位 zig + 真实 LVGL/真实 UI 的结果：初始化后 used 26,540 /
free 18,396 / largest 17,984；全部页面切换后 used 28,700 / free 15,968 / largest 12,004。
40 KB 也能跑，但全页面后 largest 仅 2,932 B，因此保留 48 KB 给目标配置差异留余量。
64 位 host 同一 UI 需要 72 KB（used 45,824 / free 18,848 / largest 12,704），这是
8 字节指针代理，不能当作目标结论。

真实目标构建的 `-fstack-usage` 关键链：

- 输入任务：48 B → `nine_app_key` 16 B → `handle_click` 32 B → `load_slot` 1,360 B
  → `nine_vm_save_decode` 1,136 B，约 2.6 KB；任务栈 4,608 B。
- 最大页面构建帧：`fill_slots` 640 B；`fill_chapter_rows` 512 B。
- 阅读推进：`nine_app_tick` 16 B → `handle_view` 48 B → `load_text_frame` 80 B →
  `nine_image_show_visual` 800 B → `jpeg_decode` 224 B → `jd_decomp` 48 B。
- 音频任务：544 B，任务栈 3,072 B。

24 条历史在 init 时按变量数从堆分配（本作 24×192 B），只在 deinit 释放。

## 10. 验证

本 worktree（Windows 宿主；绝对路径见交付报告）的本地证据：

| 命令 | 结果 |
| --- | --- |
| `./tools/validate.sh --static`（`CC=gcc`） | PASS |
| `./tools/validate.sh`（静态 + IDF 5.5.3 固件） | PASS |
| `idf.py ... build` / `merge-bin` | PASS；app 0x517970，merged 5,405,040 |
| `python3 tools/verify_firmware.py build/gate-firmware` | PASS |
| `python3 tools/archive_firmware.py create ...` | PASS |
| `python3 tools/nine_audit.py ...` | PASS；8049 句、94 边、20850 层 |
| `nine_source_trace.cjs` vs `nine_trace`（8 场景） | PASS；约 2.7 万行视图/状态 |
| `python3 tools/nine_image_audit.py ...` | PASS；209 帧，24 帧源参考合成 |
| `python3 tests/test_nine_assets.py` | PASS |
| `-fstack-usage` 目标构建 | 见上 |
| `tests/nine_realgl/run_gate.sh`（32 位 48 KB 真 LVGL；64 位 72 KB 兜底） | PASS |
| `python3 tools/nine_shots.py`（宿主真 LVGL PNG/HTML） | PASS；正常/多人/CG/20px/菜单 |

Host 测试跳过逐组保留：`test_check_repo` 跑 28 项、跳过 4 项；`test_archive_firmware`
跑 26 项、跳过 5 项；`test_install_passport_skills` 跑 18 项、跳过 12 项。全部跳过
都是 Windows symlink 平台保护。CMake 的 compiler-check 跳过是构建探测，不是用例，
不计入上述数字。

源/VM 差分场景：`first-run`、`true-run`、`bad-run`、`mixed-run`、`alt-run`、
`previous`、`skip`、`previews`（后三者覆盖回退、跳章、章节试读）。并集覆盖全部
8050 条源记录与两个结局。

图像参考分两级：

- **包参考**：用 Pillow 解码包内真实 JPEG/indexed/raw 资产，按精确层表合成；209 帧
  最坏平均通道误差 7.268、p99 25、max 57（JPEG 解码器差异与 alpha 舍入；容差
  mean ≤ 7.5、p99 ≤ 27）。
- **源参考**：按相同目标几何重新解码源美术并合成；最坏 mean 16.987、p99 58
  （JPEG q38 + 192 色调色板；容差 mean ≤ 18、p99 ≤ 60）。256×144 内部舞台的边缘
  像素占比更高，因此容差比 320 宽代理略宽；这是来源一致性检查，不代表无损。

## 11. 复现

```bash
python3 tools/nine_pack.py --source <固定提交检出> --episode kokoiro \
    --out assets/nine/kokoiro
(cd tools && python3 nine_font.py --font <nine-text.otf> \
    --fallback <NotoSansSC-VF.ttf> --out ../assets/nine/kokoiro --source-root ../main)
python3 tools/nine_audit.py --source <固定提交检出> --episode kokoiro \
    --pack-dir assets/nine/kokoiro --out build/nine/audit-kokoiro.json
python3 tests/nine_host/make_filter_pack.py
# 编译宿主工具后：
./build/nine/nine_trace.exe assets/nine/kokoiro/nine_script.bin \
    tests/nine_host/scenarios/first-run.txt
node tests/nine_host/nine_source_trace.cjs <nine-kokoiro-rw5> \
    tests/nine_host/scenarios/first-run.txt
python3 tools/nine_image_audit.py --source <固定提交检出> \
    --pack-dir assets/nine/kokoiro --render build/nine/nine_render.exe \
    --out build/nine/image-audit.json
./tools/validate.sh
```

更换 pack 后不要复用旧构建目录：构建期汇编生成器会在任一 `.bin` 变化时重跑；
`validate.sh --firmware` 默认使用全新目录。

## 12. 已知缺口

- 无视频解码器：OP 用 3 fps 图片序列 + 原曲（源 599 帧降为 300 帧，时长仍为
  99.9 s）。这是有意的、已记录的能力取舍，不宣称完整视频支持。
- 音频为 8 kHz 单声道 IMA ADPCM；不是源 Opus 的逐位回放。
- 源工程本来就没有语音与 R18 内容，固件同样没有。
- 16:9 舞台内部 256×144、以 1.25x 显示为 320×180；标题主视觉由竖版中心裁切为
  横版，立绘量化为 8 位调色板，背景为 JPEG q38；全部内容保留，没有纵向压缩。
- 本 graph 没有独立章节过场节点；框架过场页保留但正常阅读不使用。
- 真机行为、真实堆、音频输出与 NVS 磨损均为 NOT RUN / 未验证。
