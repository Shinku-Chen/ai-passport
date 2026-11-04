<p align="right">
  <strong>简体中文</strong> · <a href="galgame-summer.md">English</a>
</p>

# 《Summer Pockets REFLECTION BLUE》阅读器

本分支（`feature/port-summer-landscape`）把 AI Passport 做成 320×240 横屏的
RW5《Summer Pockets REFLECTION BLUE》阅读器。源工程是已转换的小米 Vela 项目，
不是 Windows 原版引擎：本固件准确复现**当前 RW5 源快照的范围**，不声称还原
源工程已经精简掉的 PC 独占内容。

## 1. 源范围

只读快照位于
`assessment-games/repos/summer-rw5`（记录提交
`28f8a41c3e77b57c9e519816b27fbc1f1ed94866`）。打包器与审计直接读取该快照，
不依赖任何旧 build 报告。

| 项目 | 当前 RW5 源 | 打包结果 |
| --- | --- | --- |
| 场景 | 422（`converted-story/*.json`） | 422 |
| 指令 | 150,453 | 150,453 |
| 对白/旁白指令 | 81,040 | 81,040 |
| 选项组 / 选项 | 188 / 495 | 188 / 495 |
| 地图组 / 项目 | 27 / 918（全部在 scene 281） | 27 / 918 |
| 路线 / 章节入口 | 14 / 252 | 14 / 252 |
| CG 组 / 差分 | 136 / 712 | 136 / 712 |
| 唯一图片文件 | 1,324（292 立绘 + 1,032 场景） | 1,324 + logo |
| 结局 | 14（`FLOW_ENDINGS`） | 14 |

源工程本身只有五条女主线 + ALKA、Pocket 与尾声；美希、静久、羽未的个人线
已被源工程移除。源工程也没有 BGM、语音、视频、复杂动画或可玩小游戏；本固件
不虚构这些内容。`title-progress.js` 是通关位、ReSTART 阶段与标题动作的权威；
`FLOW_ENDINGS`、`HEROINE_ENDINGS`、`CLEAR_FLAGS` 原样进入包元数据，并由审计
逐项核对。

打包时执行 `names.js` 的可见文本规范化：`䌷→紬`、`紬·文德斯→紬文德斯`、
`Tsumugi→紬文德斯`、`؟→？`。全部 2,842 条源说话人标签都使用 `fullName`，
所以 `紬` 在这里也展开为 `紬文德斯`；场景/路线/章节标签同样展开。对白正文
本身不变。因此字库不需要 U+061F，也不会出现缺字方块。

## 2. 版面与按键

| 区域 | 版面 |
| --- | --- |
| 屏幕 | 320×240 横屏（BSP 硬件旋转） |
| 画面 | 320×180 RGB565 画布 |
| 正文带 | y 180..239，60px，16px 字，3 行 |
| 字体 | 16px 中文，行高 19，3,802 码位，0 缺字 |
| 列表 | 整屏页头 + 最多 7 条紧凑行或 3 张卡片 |
| 标题 | 风景画 + 300×86 logo，菜单行位于 x 206..314 |
| CG 鉴赏 | 完整 fit 到 320×180，下方标题带 |

按键：

- 正文：**UP 短按**下一句/下一页（打字中先补全本页）；**长按 UP**快进，
  松手即停；**DOWN 短按**历史；**长按 DOWN**自动阅读；**OK**打开阅读菜单。
- 选项/地图：UP/DOWN 选择，**OK 短按**确认；**OK 长按**打开菜单。自动/快进
  到选项必须停住，不能代选。
- 列表：UP/DOWN 移动，OK 执行，**OK 长按**返回明确记录的来源页（菜单、标题、
  存读档、路线、鉴赏……）。
- 使用真实 BSP 事件序列：短按 PRESS → RELEASE → 延迟 CLICK，长按
  PRESS → LONG → RELEASE。PRESS 已导航时尾部 CLICK 被消费；长按不会吞掉
  下一次独立短按。
- 所有 LVGL 访问都在 `bsp_lvgl_lock()` 内；按键回调只入队。

## 3. 构建、打包与字体

资源包只从源快照生成，不手改：

```bash
python tools/summer_pack.py \
  --source <rw5-snapshot> \
  --out main/summer_data/summer_pack.bin \
  --report build/summer_pack_report.json \
  --scene-w 256 --scene-h 144 --scene-quality 20 \
  --portrait-w 120 --portrait-h 150 --portrait-quality 30
```

`tools/summer_pack.py` 解析 `catalog.js`、`assets.js`、`gallery.txt` 与
`converted-story/*.json`；不 import 审计工具。资源包通过 `EMBED_FILES` 嵌入。

字体：`assets/fonts/gal_cjk_16.c`、`gal_cjk_20.c` 由 `tools/gal_font.py` 从
Noto Sans SC 与字符清单（`assets/fonts/gal_cjk_symbols.txt`，由包内 chartab
加 `main/` 下全部非 ASCII 字符串字面量生成）栅格化。两档都生成供
`--check` 使用；固件**只链接 16px**，因为 20px 约多 600 KiB，Flash 预算优先
留给剧情、CG 与表情。因此设置页不提供字号选项。

```bash
python tools/gal_font.py --font <NotoSansSC.ttf> --out-dir assets/fonts
python tools/gal_font.py --check --out-dir assets/fonts
```

## 4. 包格式

单个小端包 `SUMMPK01`，25 个段；运行时没有解压器，也没有 JSON 解析。

| 段 | 内容 |
| --- | --- |
| TEXT / CHARTAB | 12 位码位下标（0 结尾）；974,200 个码位，1,461,300 B |
| OPS / OPIDX | varint 指令流；每 32 条一条 u32 字节偏移，跳转最多扫描 31 条 |
| SCENE / SCENE_INDEX | 40 字节场景记录（名称、地图短名、原始日期、日期标签）、id→下标表 |
| CHAPTER / ROUTE | 252 章节、14 路线 |
| GALLERY / GALLERY_ITEM | 136 组、712 个差分图片下标 |
| IMAGE / IMAP / IMGDATA | 1,326 条图片记录、源 id→图片映射、JPEG + RLE 遮罩 |
| EXPR / EXPRIDX | 后缀表达式字节码 |
| VARS | 640 个源变量 id |
| CHOICE / CHOICE_ITEM | 变量、值、条件 |
| MAP / MAP_ITEM / MAPLABEL | 27 组、918 个值、946 条目的地提示 |
| FLOWLABEL | 137 条 flow 标签 + 223 条 flow 日期 |
| PROGRESS | 通关位、女主/flow 结局、globals、ReSTART 变量 |
| META | 入口场景、计数、标题文本、logo/标题图 |

表达式支持 `v`（含计算键与源特例 `24006` 通关计数）、`s`（日期）、
`string`、`native`（0）、一元/二元算术、位运算、比较与 `&&`/`||`。审计会用
六组确定性变量赋值逐条求值，并与 `engine.js` 的直接重实现对照。

### 与源一致的 VM 语义

资源包按源的用途保存 **四种** 文本：`strings[106]` 原始日期（全角数字，如
`７月２９日`）、目录里的 ASCII 日期标签、地图短名，以及 `fullName` 说话人/
场景标签。VM 把原始日期当作不透明值；`sceneLabel` 显示时转成 ASCII，只有
原始日期为空时才回退到目录/flow/父场景日期。进入没有日期的场景不能把之前
的日期清空。显示转换只接受完整的“月/日”后缀（“日”是 3 个 UTF-8 字节），
把全角数字转 ASCII，拒绝过短/不完整/过长后缀；输出 cap 不足时返回 false，
不截断冒充合法日期。它不改变 `vm->date`，也不影响 `EX_DATE` 分支。

顶层 pack 段表在读完 20B header 后、读取任何段记录前，先拒绝
`header_size > size`；再拒绝重复 type、落在 header 内的起点、非零长度段互相
重叠，以及 `count * width` 不严格等于段长的定长段（必须做乘法，不能只做除法）。
零长度段合法，且不参与重叠判断。guard-page probe 覆盖 20/24/28/32 字节且
`total == size` 的 buffer；维护测试覆盖 0..19 以及 20/24/28/32/64/419/420。

地图预览必须是**单步**：复制 VM，执行到第一次场景变化就停。目的地和它的
入口 cursor 用于 route/RB90 过滤与 `mapLabels` 提示。不能继续跑到下一条
TEXT，穿过 `return → flow → next callScene`；那些源本来隐藏的目的地正是 F1
缺陷。同状态源/C 轨迹是回归证据：`281:793` 是 4 个目的地
`[1,13,14,15]`、第一个目的地 102，不是 33 项或 scene 29。

存档格式为版本 2。`map_bits` 为 256 B，长度字段改为 `u16`；版本 1 用 `u8`，
256 会回绕成 0，所有已访问位都会静默丢失。版本 1 存档被显式拒绝，不做错误
迁移。存档预览取 20 个 UTF-8 **字符** 再加省略号，不是 20 字节。

## 5. 图像策略

- 源场景图是 320×180 RGB（16:9），不是 4:3。打包缩到 **256×144**、JPEG
  q20，显示时完整放大到 320×180（1.25×），不 cover 裁切。
- 立绘共 292 张 RGBA PNG（252 张去重姿势底图 + 40 张新增表情）。每张缩进
  120×150 画框，编码为 RGB JPEG q30，并带逐行 RLE 1bpp alpha 遮罩（运行时
  不使用 PNG）。全身按底部居中显示到最大 144×180；透明像素不覆盖背景。
- 712 张 CG 差分（136 组）全部打包；标题 logo 以 300×86 的遮罩图打包。
- 有 57 个源图片 id 在 `assets.js` 中没有文件，源运行时回退到 `bg_black`；
  包同样把它们映射到黑屏条目，不拿别的场景顶替。
- 解码走 TJpgDec 回调直接写入 320×180 画布。立绘 JPEG 中途失败时，先把
  画面回滚到完整背景（没有背景时用明确的黑屏占位）再提示，不能留下半截
  立绘。提示、图片错误状态、缓存失效与下次重试逻辑不变，失败也绝不解锁 CG。
  读档前先做 JPEG 头/遮罩预检，失败不能改当前 VM 状态。

## 6. 持久化

NVS 为 88 KiB（`0x9000..0x1F000`），app 分区在 `0x20000`（ESP-IDF 要求
64 KiB 对齐），大小 8,257,536 B。

- 设置：文字速度（即时/逐字）、自动速度。
- 30 个手动栏位，每页 3 个；覆盖前确认。
- 一个**独立自动档**（`auto`）供“继续阅读”，手动栏位不放自动档。
- 周目/解锁：14 个通关位、ReSTART 阶段、6 个 globals、136 个 CG 已看位。
  启动实际加载；变化置 dirty，在标题/鉴赏/结局/睡前节流重试。
- 版本 2 存档写：场景/cursor/页/根章节/模式、非零变量稀疏表、调用栈、
  图片 id、日期、全部 256 个地图访问位（长度 `u16`）、通关/已看快照与
  CRC32。恢复时先校验版本、长度、CRC、场景、cursor、页、图片、调用栈，
  全部通过才替换当前 VM；失败保持原状态。版本 1 被拒绝，不迁移。
- 存档列表区分“空栏位”和“有档但 NVS 读失败”：空栏位提示
  “这个栏位还没有存档”，有档读失败提示“存档读取失败，请重试”（列表行
  “存档无法读取”），并且不改当前 VM。
- `nvs_set_blob` 与 `nvs_commit` 都成功才报成功并更新 RAM 槽位；失败保留旧档
  和旧预览。NVS 初始化失败只降级，**从不调用 nvs_flash_erase**。
- 读取旧档只把新增 CG/通关位并进全局记录（OR，不回退）。

## 7. Flash 与 RAM

2026-11-02 干净构建实测：

| 项目 | 字节 |
| --- | --- |
| 资源包 | 6,425,838（`3a7e95…`） |
| app | 7,585,296（上限 8,257,536；余量 672,240） |
| 合并 full | 7,716,368（上限 8,388,608） |
| 16px 字形位图 | 435,960（对象 `.rodata` 约 473.8 KiB） |
| 静态 DRAM | 269,222 / 321,296（83.79%），链接区余量 52,074 |
| `.bss` / `.text` / `.data` | 193,224 / 68,698 / 7,300 |

`idf.py size` 的 52,074 B 是 `dram0_0_seg` 321,296 B 的**链接区余量**，不是
分配器堆。干净构建里 `_bss_end = 0x3fcc1d58`、`_heap_start = 0x3fcc1d60`、
`APP_USABLE_DRAM_END = 0x3fcdc710`（IDF C3 `SOC_ROM_STACK_START 0x3fcde710`
减 0x2000），所以可分配堆范围为 108,976 B。运行期扣减：BSP LVGL 刷屏缓冲
19,200 B、ESP LVGL port 任务栈 7,168 B、main 任务 3,584 B、esp_timer
3,584 B、event 任务 2,304 B、输入任务 **8,192 B**，合计 44,032 B；在 LVGL
对象（CLIB malloc）、NVS 页缓存与碎片之前还剩约 64,944 B。输入栈从 4 KB
提高到 8 KB，是因为目标端 `-fstack-usage` 显示 app/model/TJpgDec 调用链最坏
约 2.9 KB；这是静态上界估计，不是运行期高水位测量。

运行期 free/largest、帧时与物理交互 **未在真机测量**，因此列为待真机项。

## 8. 验证与构建身份

命令（日志都在 `build/logs/`）：

```bash
CC="$PWD/tools/cc-host.sh" SUMMER_SOURCE=<snapshot> ./tools/validate.sh --static
./tools/validate.sh --static                 # 无 SUMMER_SOURCE -> PARTIAL
CC="$PWD/tools/cc-host.sh" SUMMER_SOURCE=<snapshot> bash tools/summer_gate.sh
bash tools/summer_gate_selfcheck.sh          # 独立坏副本负例
powershell -File tools/summer_firmware.ps1 -IdfPath <esp-idf-5.5.3>
```

每次 Host 运行都在 `build/host-tests/` 与 `build/logs/host-tests-*` 下创建
唯一目录并保留；Summer gate 使用 `build/summer-gate/` 与
`build/logs/summer-gate-*`；测试目录和产物不删除。

- `summer_gate.sh`：分页、NVS/存档故障注入（含“有档但读失败”）、真实
  TJpgDec + 遮罩、立绘失败回滚、生产 VM 遍历真实包、版本 2 地图访问位存档
  往返、`strings[106]` 日期/UTF-8 回退、20 字符预览、顶层 pack 段表负例、
  生产 app/UI 真实按键序列、独立源审计、字体覆盖，以及两个负例（图片测试
  编译失败必须非零退出；缺字必须被拒绝）。pack-format case 打印正常 open
  和 27/27 个短/截断 buffer 被拒（含 `total == bad size`）；复制的 Windows
  guard-page probe 对 20/24/28/32/64 返回 `ok=0`，不触碰 guard page。地图访问位回归走真实 `281:1468`
  序列：首次预览 5 项，选择并重复后 4 项，存读档恢复后仍是 4 项（版本 2
  的访问位保留）。正常有源运行 **15/15 PASS**；缺少 `SUMMER_SOURCE` 时为
  **PARTIAL**：13 PASS、2 SKIP；只缺 node 时审计仍可通过、只跳过 trace。
  任一跳过路径都退出 3，不会报 PASS。独立自检把副本测试分别弄成编译失败
  和运行失败，两份副本都使 gate 以 1 退出
  （**12 PASS / 1 FAIL / 2 SKIP**，2/2 被拒绝，不是 PASS）。
- `tests/summer_trace_compare.py` 让未修改的源 `engine`/`title-progress` 与
  生产 C VM 从同一状态运行，逐行比较文本/说话人/标签/地图/日期/结局。十个
  纳入维护的用例完全一致：原有九个，加 chapter 137（本作者 harness 5,946
  行）。独立复制的 probe 对 chapter 137 的严格比较是 **5,906 行 / 0 差异**
  （5,905 个 stop + 头部）；scene 177 继承原始全角日期并显示 `8月30日`，
  不再是旧的硬编码 `7月29日`；转换器拒绝短/不完整/非法 UTF-8 尾和不足的
  输出缓冲。新的全章节 datewalk 为 79 个有日期场景 / 863 次访问、4 个继承
  日期场景 / 59 次访问，错误 fallback 为 0；独立复核报告的 10 场景/117 次
  fallback 集合已修复。walker 还覆盖 15 个章节入口与 2,842 条说话人标签
  （同一资源包）。**独立源语义复核 r2 的 F1/F3/F4/F5/F6 目标项已闭环，
  F2 显示层在本轮修复**；F6 的段重叠/重复/目录起点/严格宽度现在会被拒绝。
  原有九个作者轨迹是回归证据，不是 14 个结局终点的验收：实际只观察到
  4/13/8/9 与特定策略下的 `281:1404` MEXHAUST，其余结局终点**未逐条实测**；
  14 结局进度元数据存在，不等于每条路线都已走到结局。
- `summer_audit.py`：独立解析源与包；逐条核对 150,453 条指令、正文/人名、
  跳转目标、选项/地图组、946 条地图提示、360 条 flow 标签、136 组鉴赏、
  图片/JPEG/遮罩几何与进度元数据。
- `validate.sh --static`：正常有源 **24/24 PASS**（Summer gate 15/15）；
  缺少 `SUMMER_SOURCE` 时 **23 PASS + 1 PARTIAL**（Summer gate 13 PASS /
  2 SKIP），退出码 3，不会报成 PASS。包含两处 Windows 基线修复：COFF
  链接器缺的 demo 空实现、skills 安装器测试的 symlink skip。每个 case 的
  日志和 C 二进制都保留；Host 汇总打印 PASS/FAIL/SKIP/PARTIAL 计数与绝对
  路径。把 `CC` 设为 `/bin/false` 的负例返回 1（9 PASS / 15 FAIL）；独立
  坏副本自检的编译/运行两份都是 gate 12 PASS / 1 FAIL / 2 SKIP（2/2 被
  拒绝，不是 PASS）。`--all` 按 FAIL > PARTIAL > PASS 聚合；本轮验证没有
  重跑固件那一半。
- 干净固件 gate：PASS；app 7,585,296 / 8,257,536（余量 672,240），full
  7,716,368 / 8,388,608；`verify_firmware.py` PASS；archive create + verify
  PASS。这是日期显示与 pack 段表修复后的新构建；旧 `4744417…` 与 `dc98ec…` 归档
保留。
- `./tools/validate.sh --firmware` 在 Git Bash/MSYS 下不可运行（ESP-IDF
  5.5.3 拒绝 MSYS 环境）；原生 `tools/summer_firmware.ps1` 是已核验的等价
  构建，只在 MSYS 命令路径上记为 PARTIAL。
- 真机：**NOT RUN**。没有刷机、没有 `nvs_flash_erase`，也没有 commit、push
  或上架。

构建身份：

```text
pack        3a7e95655a33f1e619af4cc44686a25d569ae99908fa7b082aa755797503702d
app         06bff487d72f2eec2cc2e326c9e949a131d4d38dc1ec85f3dfedf69960b1e72f
full        6147075a06e2c19a73837e98191811260a6eeb18dfb9261036abdbe8dde00bc2
elf         5f1d057bdd7998bf2fec940985f30e8e3006c204a1b383799e646e885134abf8
archive     build/firmware/6147075a06e2c19a73837e98191811260a6eeb18dfb9261036abdbe8dde00bc2
build dir   build/firmware-gate/run-20261102-124856
map         8c8fca74da5493e45a78a37ca0c7dbd0f29c1de5f0172a76ac8efda9f80cb7e5
```

归档含同次 app/full/ELF/MAP/bootloader/partition/flash_args，且
`archive_firmware.py verify` 通过。独立源复核 r2/r3 目标项已闭环。最终
header 复核为 **PASS、无阻断**：
`build/review-final-header-20261102T061006Z/REPORT.md`（guard 矩阵 0..420、
结构负例与交付 app/full/ELF/MAP/pack 身份）。本批软件审核记录在
`implementation-logs/summer-software-acceptance.md`；**设备验收仍待完成**。
运行期 heap/stack、物理 NVS、LCD/按键/睡眠与真机仍为 **NOT RUN**。

14 结局元数据完整，但实际只走通 4/13/8/9；`lf` 路径与源同样在
`281:1404` MEXHAUST。这不等于 14 个结局端点都已实测。16px 单字号、
未收录音频/源小游戏等已披露的源取舍不变。Git Bash 固件包装仍为
**PARTIAL**；原生 PowerShell 构建才是上面的 **PASS**。

## 9. 已知限制

- 20px 字体只为覆盖自检生成，固件不链接；界面只有 16px。
- 标题 BGM 与语音未收录（Flash 预算；RW5 源本身也没有剧情 BGM/语音）。
- 源工程已精简的小游戏、地图玩法、动画与 PC 独占内容不复现。地图耗尽
  是**策略相关**的源行为，不是普遍事实：last-choice/first-map 时源和 C 都在
  `281:1404` 停下；last-choice/last-map 会到达结局 13。地图阶段若所有目的地
  都已访问，会显示与源一致的错误页（“此地图阶段暂无支持的剧情选项”）。
- 章节入口故意不伪造通关位/结局；只有 story 模式回写结尾进度。
- 源素材版权归权利人；本分支只做本地技术验证，不分发资源包，也不做社区发布。
