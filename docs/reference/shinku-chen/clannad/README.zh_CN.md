<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI Passport《CLANNAD》阅读器

从 REDMI Watch 5 版本 [`xuanyide01/clannad-rw5`](https://github.com/xuanyide01/clannad-rw5)
快照 `2c67720807db9d9761c22ddbc1f11e6a90cc2adc`（源 README：0.2.0）移植的横屏视觉小说
阅读器。应用基线为 `feature/galgame-framework` 的
`3c2e5013a9831c57124ef8f774193f0e06f1f14a`。

## 交付状态

- **UI 启动候选；软件复核与设备验收均待完成。** 本候选不是此前已接收的
  `21e3fe93…` 构建。生产仅改标题画布清理和下方 LVGL pool 日志；资源包、字体与
  配置不变。**真机测试：未运行（NOT RUN）。**
- app 镜像：**8,149,744 字节**，SHA256
  `3b3a979af17fd849818ec30062bf9264e5da0780aa030f5abba8228b87074525`。
- 合并镜像：**8,215,280 字节**，SHA256
  `263a4846aa2699d6a39d4df51c742c5c9bdda04d98bb56daef44e416c5017767`
  （从 `0x0` 烧录；合并镜像可能重置已存数据的说明见
  [`firmware-layout.md`](../../../development/engineering/firmware-layout.md)）。
- 同一次归档：
  `build/firmware/263a4846aa2699d6a39d4df51c742c5c9bdda04d98bb56daef44e416c5017767/`
  （ELF、MAP、app、full、bootloader、分区表、`flash_args`、manifest）。
- ELF SHA256 `ab8ac5a4888d68933a0a0f5563cb660565ae5efdc7488e4626fe39fd2d053a5d`；
  MAP SHA256 `fd0685194f5b18781ab4aaedb29361991498b980cba6c8bbc969fc0850729428`。
- app 分区余量：**173,328 字节**（`8,323,072 - 8,149,744`）。
- 资源包：**7,168,054 字节**，SHA256
  `594affb1db5da33842556cfaabab0ffe31923f9c1ca9d28ae485e46b2a928dd6`。

固件完全离线：不链接 Wi-Fi、BLE、音频、语音或视频。

## 独立审核依据

- 源/引擎：`build/review-model/final-20261102T045914Z/REPORT.md`
- 存储：`build/review-store/final-20261102T045911Z/REPORT.md`
- 图像：`build/review-app/final-20261102T045921Z/REPORT.md`
- 12 KiB 身份/栈与 MAP 定点：
  `build/review-app/final-12k-20261102T063832Z/REPORT.md`

前三份终报和 12 KiB 报告绑定已接收的 `21e3fe93…` 身份，不覆盖本次 UI 启动候选；
新候选仍需最终身份/栈定点复核。它们均不包含设备验收。

## 源内容覆盖

保留当前手表源快照的逐 op 语义；这不是 PC 原作，也不声称恢复 PC 独有内容。

| 项目 | 数量 |
| --- | --- |
| 场景 / 分块 | 212 / 411 |
| 剧情 op | 150,288 |
| 对白 op | 101,488 |
| 选项点 | 309（302 静态 + 7 动态） |
| 章节入口 / 路线 | 195 / 19 |
| 显式 `end` | 30 |
| 鉴赏组 / 图片 | 68 / 174 |
| 唯一图像资源 | 920（356 剧情背景、174 鉴赏、386 立绘、2 标题、2 UI） |
| 源立绘 ID → 文件 | 1,083 个 ID 映射到 386 个文件（源本身已合并表情差分） |
| 字体 | 16px、3,662 个码位、缺字 0 |

`9035`、`9054`、`9070`–`9083`、`9600`、`9999` 共 17 个场景不在章节目录中，但通过共通篇
的调用保留并可到达。每个场景的入口表保存到脚本实际使用的最大下标 32；212 个场景的
源入口值逐项对照。

独立解释器 [`tools/clannad_audit.py`](../../../../tools/clannad_audit.py) 以两种确定性
选择策略遍历全部 195 个章节入口（390 次），并跑两条新游戏路径；生产 C VM 复现全部
390 份文本/选项计数、结束位置与 FNV-1a 摘要。源状态探索枚举到 105 个不同**运行时
终止点**（显式 `end` 与空栈 `return`；不是 105 个不同的原作结局）。390 次策略遍历
覆盖其中 98 个，另外 7 条显式选择路径覆盖其余 7 个。另有 5 个源生选择自环被有界
截断，它们不是结局、也不是移植挂死，因此不能写成 110 个结局。After Story 与真结局
进度由 app 的全局 `g[]` 回归单独覆盖。

## 画面与图像

逻辑分辨率 320x240。画面区 320x168（4:3 背景完整 fit 为 224x168 居中，左右为主题色
边条）；下方 72px 为对白带（姓名行 + 3 行 16px 正文）。

| 资源 | 编码 |
| --- | --- |
| 剧情背景 | 224x168 JPEG，quality 40（保留 4:3，不裁切） |
| 鉴赏 CG | 320x240 JPEG，quality 30 |
| 角色立绘 | alpha 裁边、高 160，JPEG quality 55 + 1bpp 遮罩 |
| 标题 | 源 432x514 原画的 320x240 裁剪，quality 45 |
| Logo / 副标题 | alpha 裁边 JPEG quality 75 + 1bpp 遮罩 |

386 个立绘文件按内容共享 107 张 1bpp 遮罩（共 286,550 字节）。除源自身的别名合并外
没有再做身体/表情归并。CG 全屏浏览与正文共用 224x168 画面区（完整 4:3 fit），而非
320x240 铺满；这是下面的内存取舍。

JPEG 由组件 TJpgDec 逐 MCU 回调直接写入 320x168 RGB565 画布；没有整帧 JPEG/RGBA
缓冲，也不使用 PSRAM。坏 JPEG、声明尺寸不符、遮罩截断或资源缺失会拒绝绘制、显示
提示、保持缓存失效并允许重试；返回失败前会完整清空并 invalidate 画布，标题/CG
后段组件失败也不会留下半帧。

## 操作

- 正文：UP 短按推进（打字中先补全本页）；长按 UP 快进、松手停。DOWN 短按历史；
  长按 DOWN 自动播放。OK 打开菜单。
- 选项：UP/DOWN 移动，OK 短按确认，长 OK 打开菜单；菜单/存档/读档/设置往返保留
  待选与非零光标。
- 列表：UP/DOWN 移动或滚动，OK 执行，长 OK 返回来源。
- CG：UP/DOWN 切换差分，OK 返回列表。
- 标题：新游戏、继续（自动档）、读取、After Story（按源条件解锁）、CG 鉴赏、
  章节/路线、设置。

## 存储与进度

- NVS 命名空间 `clannad` 下 30 个手动栏位（`s00`–`s29`），以及独立的
  `clannad_auto/auto` 自动档。自动档使用当前 VM 的 TEXT/CHOICE 阅读 checkpoint，
  即使画面在 MENU/HISTORY/SETTINGS/SAVE/LOAD/标题也保存正文位置；在换场、每推进
  18 句、回标题和睡前写入。写失败保留 `auto_dirty` 并按上限节流重试；睡前/回标题
  会强制重试。
- 存档保存精确的 scene/cursor/page/选项光标、图像、变量、计数器、调用栈、预览和
  CRC32；使用固定 Huffman DEFLATE 压缩，解压复用剧情包的 inflate。实测存档压缩后
  152–287 字节（原始最大 661 字节），30 个栏位加自动档/进度/设置可放进 24KB NVS，
  并留出垃圾回收空间。栏位区分 EMPTY/VALID/CORRUPT/IO_ERROR：读取失败不显示为空，
  SAVE 会先重新读取；只有证实的空槽才免确认保存，有效槽必须走默认取消的覆盖确认。
  NVS 写入失败时保留旧栏位和当前状态并提示失败；固件从不自动擦除 NVS。
- 容量有界扩容（2026-11-12）：VM 变量表为 512 槽、3/4 探测上限；49g + 74-choice
  witness 峰值为 195 个活变量。存档原始预算为 3 KiB（最大实测 1,415 字节；真结局
  checkpoint 1,069 字节）。进度 codec 上限 128 条 g[] / 1 KiB（67 条真结局记录
  414 字节；旧 64 条 blob 仍可读）。零值条目是显式覆盖，不会被省略。上面的 30 槽
  实测压缩存档仍是普通存档口径；更大预算不表示 128/512 条能容纳所有理论状态。
- 全局 `g[]` 进度独立存放（`clannad/prog`），读档时覆盖到存档之上，旧档不能回退
  光玉或 After Story 标志。章节入口会应用已存 `g[]`（源 `selectBrowse` 调用
  `withProgress(newState(id), progress)`），但 `progress_sync` 仍只在 story 模式
  导出，章节不回写。真结局与 After Story 分别按源 `g[45]` 与源标志集合判断。
- 本板没有 RTC；栏位显示章节、模式、预览和占用情况，不显示真实时钟时间。

## RAM（同次 MAP 证据）

`tools/clannad_mem_budget.py` 读取本次 ELF 的 MAP：DRAM data+BSS 静态 290,992 字节；
BSS 之后的堆区 91,568 字节。可用末尾取同一 IDF 的
`SOC_ROM_STACK_START - SOC_ROM_STACK_SIZE`，不是乐观的 `0x3fce0000`；运行期
free/largest 与碎片未测量。刷屏缓冲按面板物理宽度计算：240 x 40 行 x 2 字节 =
19,200 字节（单缓冲，不是 LVGL 横屏逻辑宽度 320）。命名的动态项为 19,200 字节刷屏
缓冲、31,488 字节堆上的任务栈和约 1,600 字节队列/TCB 估算，剩余 39,280 字节供
SPI/面板/I2C/NVS 驱动分配与碎片使用。LVGL port 任务栈为 7,168 字节；idle 任务栈已在 MAP 中静态计入，不重复扣减。
静态命名缓冲包括 107,520 字节的 320x168 画布、28,672 字节剧情 scratch、约 14,584
字节 VM、1,040 字节 app 列表行 scratch 和 32,768 字节 LVGL 池。这是链接期证据，
不是真机堆或帧时测量。

### 输入任务栈

输入任务栈为 **12,288 字节**。此前的 2,064/2,096 B 是 inflate 读块/读档验证链，
不是整个 VM 的最坏栈。最终保守表达式审核采用真实根深度 8、全偏移上界 9；父链为

`input_task 80 + app_key 48 + load_slot_blob 240 + run_frame 144 + vm_run 224 = 736 B`。

目标 ELF 的实际 `eval_expr_end` 帧为 768 B，`9 * 768 + 736 = 7,648 B`；独立审核
私有编译的 784 B 代理值为 `7,792 B`。相对 12 KiB 输入栈分别余 4,640/4,496 B，
尚未计入 libc/FreeRTOS/调度峰值。真实根 8 层估算为 6,880 B，但根字段覆盖按保守
边界处理；两者都不是真机证据。主任务仍为 4,096 B。生产侧仍保留 30 符号 DEFLATE
距离树和 `clannad_app_t` 内的 1,040 字节列表行 scratch。目标 `-fstack-usage` 为：

| 函数 | 目标栈帧 |
| --- | --- |
| `inflate_dynamic` | 1,200 B |
| `inflate_fixed` | 1,008 B |
| `refresh_page` | 272 B |
| `run_frame` | 144 B |
| `clannad_vm_run` | 224 B |
| `eval_expr_end` | 768 B |
| `load_slot_blob` | 240 B |
| `input_task` | 80 B |

单独的 2,064 B 读链和 2,096 B 读档验证链仍低于整个表达式预算。以上是编译期栈帧；
运行期 high-water、free/largest 与帧时 **NOT RUN**。

## 构建与验证

```bash
python3 tools/clannad_pack.py --source <clannad-rw5> \
    --out assets/clannad/clannad_pack.bin --report build/clannad_pack_report.json
python3 tools/clannad_font.py --font <NotoSansSC-VF.ttf> \
    --pack assets/clannad/clannad_chars.txt --source-root main --out-dir assets/fonts
./tools/validate.sh --static        # 设置 CLANNAD_SOURCE 可跑源审计
python3 tools/clannad_paths.py --source <clannad-rw5>   # 重新生成 7 条显式路径
./tools/validate.sh --firmware      # ESP-IDF 5.5.3
```

已记录的宿主证据：

- `tests/test_clannad_vm.c`：194 项检查，390 个章节策略 + 新游戏路径、文本/选项
  存读档、preview 截断/UTF-8 拒绝、跨场景与调用栈存档、通关记录、分页、DEFLATE；
  6729 witness 验证带 `g2=1/g32=0/g0=5` 时 cursor 114、无进度时 203，以及章节内
  `g32=1/g0=4` 的局部变化。
- `tests/test_clannad_image.c`：2,825 项检查，920 个资源全部经真实 TJpgDec 解码，
  遮罩透明、坏图/截断/尺寸不符拒绝，以及 scene/CG/标题组合失败瞬间的完整
  clear/invalidate。
- `tests/test_clannad_stack_contract.py` 与 `clannad-frame-budget` 编译 case 防止
  12 KiB 输入栈、app 行 scratch 和 30 符号距离表回退；目标栈帧见
  `build/stack-probe-12k/`。
- `tests/test_clannad_app.c`：138 项检查，用真实 PRESS/RELEASE/CLICK 与长按序列驱动
  生产 app，覆盖文本/选项存读档、auto 恢复、6 条子页 checkpoint flush、存档读失败
  重试/确认、写失败、坏档、进度重载/重试、快进遇选项停止、图片失败和坏包。
- `tools/clannad_audit.py --pack`：逐项对照 150,288 个 op、411 个分块、212 个场景、
  19 条路线、195 个章节、68 组/174 条鉴赏，结果 `passed`。
- 门禁负例：故意破坏图片回归的编译，必须非零退出。

宿主门禁为 **PARTIAL：30 PASS / 0 FAIL / 27 baseline 平台 SKIP**。本作必需的
C 回归全部通过（VM 194 / 图像 2,825 / app 138 项检查）；27 个跳过均为 Windows
symlink 测试（无 Developer Mode）：`test_install_passport_skills` 18 个、
`test_check_repo` 4 个、`test_archive_firmware` 5 个。这些是 baseline/平台限制，
不是真机或运行期结果；CLANNAD 专属 case 无 skip。门禁逐 case
统计 PASS/FAIL/SKIP；任一 C 编译或运行失败都会以非零退出。证据写入唯一的
`build/host-tests/<时间戳>-<pid>/` 目录且不删除；固件校验使用
`build/firmware-validation/` 下的唯一目录，不再用删除 trap。

## UI 启动审核（候选）

- **标题画布清理（已修）**：标题页透明叠在画面画布上。`draw_title_art` 现在先
  `clannad_image_clear` 再合成背景/Logo/副标题，使 48px 侧边使用主题 clear，而不是
  BSS 零值或上一帧残留；app 回归同时检查左右侧边与 clear 基线。
- **所有页面仍 eager 创建**：标题/正文/选项/列表/CG/消息页与 notice 在启动时全部创建；
  切页只改 hidden 标志，不删除对象。真实 LVGL 宿主 64-bit 上界 smoke（`LV_MEM_SIZE=32768`，
  无显示）测得 UI 创建后约 100% pool / 88B free；这不是目标机 32-bit 测量（目标还使用
  裁剪过的 LVGL 配置），不能断言目标 pool 不足，但也没有实测余量。
- `clannad_app_init` 现在在 UI 创建后输出 `lv_mem_monitor` total/used/free/largest，
  供下一次真机启动验证；诊断本身不改变行为，运行期 pool high-water 仍 **NOT RUN**。
- 空 notice 文本会隐藏；notice 初始隐藏，仅非空时置前。未引入非活动页删除或任务生命周期
  变化。
- 本 checkout 的主任务实际为 4,096 B（不是 3,584 B）；`clannad_app_init` 目标帧 96 B，
  `clannad_ui_create` 64 B，输入任务仍 12,288 B；未改 VM 仍沿用 7,648 B 表达式/栈预算。

## 复核修复与剩余证据

- **文本存档 preview 游标（P0）**：`clannad_vm_save_validate()` 在读完 preview 后
  推进 `r.pos` 再读 `name_len`；修复前生成的 73/115 字节文本 blob 在修复后从
  validate=0 变为 validate/apply=1，空 preview 的选项档仍有效。截断字段和坏 UTF-8
  仍拒绝且不改变 VM。
- **章节进度**：真实 6729 入口在 `g2=1/g32=0/g0=5` 下到达源可见的 cursor 114
  （旧无进度路径为 203）；章节内 `g32=1/g0=4` 的变化不改变全局 progress blob。
  源 witness：`src/pages/index/index.ux:183` 与 `story/6729/0.txt:113`。
- **自动档/槽位**：6 条子页/标题 flush 保存当前阅读光标；占用但不可读的槽显示
  IO_ERROR/CORRUPT，不再显示为空，必须重试成功并经过默认取消的确认才可覆盖。
- **图片失败**：坏第 2 立绘和真实截断 JPEG 的失败瞬间画布与 clear 基线
  `diff_vs_clear=0`；A → 坏 B → A 恢复后哈希等于 A。
- **未验证**：真机栈 high-water、运行期 free/largest 与碎片、帧时、物理
  NVS/GC/断电、物理 LCD 与字形、物理按键、深睡及真机 30 槽容量。本语料未找到
  真实多页正文（101,488 对白 op，max_pages=1），多页页码行为标记为未测而非
  已通过。
- 证据目录保留在 `build/recheck-save/`、`build/recheck-image-fail/`、
  `build/recheck-store/`、`build/stack-probe-12k/` 与 `build/firmware/`；旧归档
  `64f553a2…`、`6570063f…`、`73223756…`、`21e3fe93…` 未删除。

## 已知限制

- 源手表移植已合并表情差分：1,083 个立绘 ID 对应 386 张图。本移植保留这 386 张，
  不声称恢复 PC 版缺失表情。
- 源快照不含 PC 独有剧情；本移植不重建此类内容。
- 不含音频：源只提供标题曲；该快照的剧情本身没有 BGM/语音/视频。
- 只提供 16px 字体；20px 设置不显示为可选项，因为完整文本/CG/立绘下放不进 8MB 镜像。
- CG 全屏浏览与正文共用 224x168 画面区（完整 4:3 fit），不是 320x240 铺满；这是把
  静态画布压进 C3 DRAM 的明确取舍。
- 标题图是源 432x514 原画的 320x240 裁剪；Logo/副标题来自源 UI 资源。
- 设备验收仍未运行：栈 high-water、free/largest、物理 NVS/GC/断电、LCD/字形、
  物理按键、JPEG 帧时、深睡和破坏性容量检查均未验证；静态 4,640 B 栈余量不能
  替代这些测量。

## 许可

移植代码为 MIT。CLANNAD 文本、图像、音乐及第三方素材归各自权利人所有，不在 MIT
范围内。源快照的 `LICENSE` 与 `THIRD_PARTY_NOTICES.md` 保留在源目录；字体为
SIL OFL 1.1 的 Noto CJK。
