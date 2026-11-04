<p align="right">
  <strong>简体中文</strong> · <a href="manosaba.md">English</a>
</p>

# 《魔法少女的魔女审判》AI Passport 横屏移植

## 状态

| 项 | 结果 |
| --- | --- |
| 软件审核 | **通过；待设备验收**（生产冻结于 full `7f90c4e1…`） |
| 构建/身份 | PASS：app `7,442,288 B`，full `7,638,896 B`；实际 factory@`0x30000`，余 `749,712 B` |
| Host tests | **PARTIAL**：Gate `PASS=42 FAIL=0 SKIP=1`，仅 Windows `install_passport_skills` 符号链接 SKIP |
| Device tests | **NOT RUN / 待设备验收**：未刷机、未做真机验收 |
| 源审计 | PASS：657,333 项检查（软件审核沿用，本轮不重跑） |
| NVS | `nvs@0x9000/0x26000`（152 KiB/38 页）；离线容量 PASS，物理事务未测 |
- 冻结归档：`build/firmware/7f90c4e15f1bc4f8ff179759123b50b9c63fcdf17c1629200c791e7df538e976/`；
  full `7,638,896 B`，app `7,442,288 B`，factory@`0x30000`，余 `749,712 B`。

- 工作目录：`D:/Git-Workspace/Shinku-Chen/ai-passport-manosaba`
- 分支：`feature/port-manosaba-landscape`
- 基线：`3c2e5013a9831c57124ef8f774193f0e06f1f14a`
- 源快照：`manosaba-rw5 @ a309a3614837b555741e5d8fd7cfe2b9428e2e49`（只读）
- 项目内审核记录：`build/review-app-final-7f90/REPORT_FINAL.md`、
  `build/review-app-round2/REPORT_ROUND2.md`、
  `build/review-model/second-round-20261102-141137/SECOND_REVIEW.md`

## 源内容覆盖

独立审计工具 `tools/manosaba_audit.py` 不导入打包器，直接读取源 JSON 和
MNPK 包并逐项对照：

- 458 个数字 chunk（424 个场景目录，含 `0.txt`/`1.txt`/`2.txt`；每块 ≤512 指令）
- 90,243 条指令；684,461 个正文字符（2,001,703 UTF-8 字节）；
  去重后 32,291 条文本、671,676 个 UTF-16 单元
- 691 个 `choice`、1,522 个 `offer`、596 个反驳 `link`、13 个 `map`、
  87 个 `evidence`、899 个 `bookUpdate`、373 个 `resetBook`
- 371 个章节入口、11 条路线、23 个区域、26 个人名、168 个手册键/307 个版本
- 559 张图：62 背景、172 CG（61 组）、151 立绘、19 机关图、151 手册图、
  2 张纯色占位图、2 张标题图；源 `images/` 的 557 个文件全部出现且无多余文件
- 审计覆盖每个场景的指令类型序列、对白文本/说话人/actor、选项文案与落点、
  反驳文本/落点、跳转/调用/章节/手册/地图/证据/背景/立绘引用，以及 5,586 次
  `if`/`set` 与立绘位置/缩放表达式求值对照（默认值 + 随机变量，含除零/Inf/NaN）

审计报告：`build/recheck-audit3.json`（本轮新包）；旧报告 `build/manosaba-audit.json` 保留。

## 操作

| 页面 | UP | DOWN | OK | 长按 OK |
| --- | --- | --- | --- | --- |
| 标题 | 上移 | 下移 | 进入 | 关于 |
| 正文 | 下一句/下一页（打字中先全显）；长按快进，松手停 | 历史；长按自动播放开关 | 菜单；有反驳词时打开反驳列表 | 菜单 |
| 选项 | 上移 | 下移 | **显式确认** | 菜单（返回保留待选与光标） |
| 列表 | 上移 | 下移 | 执行 | 返回来源 |
| 鉴赏 CG | 上一组 | 下一组 | 下一差分 | 返回列表 |
| 存/读档 | 上移 | 下移 | 保存/读取 | 返回 |
| 设置 | 上移 | 下移 | 切换 | 返回 |

- 按键状态严格按 `PRESS → RELEASE → CLICK` / `PRESS → LONG → RELEASE` 处理；
  正文 UP 在 `PRESS` 推进，尾随 `CLICK` 不会二次推进；长按后下一次独立短按
  不会被吞。
- 自动播放/快进在选项、反驳词、`pause` 边界的交互点停住，不代选。
- 选项/反驳词只能 OK 确认；菜单/存读档/设置往返保留同一待选和光标。

## 架构

- `tools/manosaba_pack.py`：把源 `catalog.js`、458 个 chunk、`book.txt`、
  `gallery.txt`、`assets.js` 和全部图片编译为单个只读 `MNPK0001` 包。
  指令定长 12 字节；文本 UTF-16LE；表达式预编译为 RPN 字节码；字符串表去重；
  所有段 4 字节对齐（避免未对齐 u32 访问）。
- `main/gal_pack.c`：包校验与只读访问；启动时检查 magic/版本/段边界/记录数/
  字符串/文本/图片/场景/跳转目标/表达式/手册/鉴赏等，坏包直接拒绝。
- `main/gal_vm.c`：源 `engine.js`/`scene.js` 的 C 解释器：背景层、立绘实例
  （actor/sub/appearance/pose）、焦点、条件、call/return、offer/choice、
  map/evidence/execution、clue 图、damage、book、autosave、resetBook、
  dynamic jump、章节模式。新游戏 `story=true` 与章节直达 `story=false`
  分别对应源 `isDebugDirectJump` 语义。
- `main/gal_save.c`：版本/长度/CRC 存档；先完整校验再替换状态；坏档不改当前
  有效状态；手动 30 槽 + 独立自动槽（`auto`）。
- `main/gal_unlock.c`：独立版本化全局解锁位图，只 OR 合并、不回退；失败
  dirty 保留并节流重试。
- `main/gal_image.c`：真实 `esp_jpeg`（TJpgDec）解码到 320×150 RGB565 画布和
  160×45 条带（LVGL 2× 放大）；立绘 JPEG + 1bpp 遮罩逐像素合成。
- `main/gal_ui.c` / `main/gal_app.c`：作品专属暗玫瑰色横屏 UI 与三键状态机；
  不使用 baseline demo/`ui_pixel` 视觉壳。
- `main/main.c`：开机直接进标题页；NVS 失败只降级不擦除；空闲调暗/熄屏/
  deep sleep，睡前强制写自动档、解锁和进度。

## 图像策略

- 所有背景/CG/机关/标题图在打包时按 **320×240 等比 fit**（黑边补齐，不裁切），
  拆成 320×150 上段 + 160×45 下段；阅读时下段被不透明对白带覆盖，鉴赏/标题
  页两段拼成完整 320×240。
- 标题图是 432×514 手表原画，打包时**只裁掉原画自带的触控按钮装饰区**
  （不是剧情 CG），再 fit 成横屏；不声称从原画恢复出原生横图。
- 立绘按原图等比 fit 到 150×150 内，底部与对白带相接；保留全部 151 张表情
  差分，不为省空间预合成“身体×表情”或放大单张表情。
- JPEG 质量：背景/CG/机关 `q=70`，手册 `q=70`，立绘 `q=74`；4:2:0。全部
  图片（557 源文件 + 2 标题）都在包内，不依赖运行时缩放缓存。
- **未使用跨表情/跨图差分**：实测逐表情 JPEG + 遮罩的全量包已在预算内，
  差分收益不足以抵消接缝/负收益风险；没有把“代表姿势”当差分基底。
- 真实 TJpgDec 回归逐张解码 559 张图并核对声明尺寸；151 张立绘按遮罩与
  独立解码缓冲逐像素对照；截断 JPEG 失败后恢复同一资源可重试成功。

## 存档与解锁

- 手动 30 槽，自动档常量独立；自动档在换章、返回标题、睡前强制写入，
  `autosave` 指令只标记“有保存点”，不逐条刷 NVS。
- 存档 v2 保存：剧情位置、页/待选光标、真实 `isDebugDirectJump`（true/false
  都显式写入）、当前 bodies（最多 3 个，image/x/y/scale/order）、手册版本与顺序、
  立绘、背景层、返回栈、offer 分支、选项缓存、解锁快照；v1 旧档继续只读兼容。
- 读取先解码到临时 VM，全部字段校验通过才整体替换；失败提示且当前状态不变。
- 保存必须 `nvs_set_blob` 与 `nvs_commit` 都成功；失败旧档/预览不变，提示不被
  列表刷新覆盖。
- NVS 不可用时只提示“本次不会保存”，绝不 `nvs_flash_erase`；分区扩大到
  152 KB（38 页，`nvs@0x9000`）以容纳 31 个槽位、解锁/进度/设置和至少
  1 个 GC 页；`phy@0x2f000`、`factory@0x30000/0x7d0000`。
- 全局鉴赏/章节解锁在启动时实际加载；读档只合并新增位并置 dirty；标题、
  鉴赏、结局等所有页面都可由 tick 节流重试落盘。

## 内存与 Flash

同一次 ELF/MAP（`build/recheck-build-nvs/FoloToy-AI-Passport.map`）：

- app `7,442,288 B` / app 分区 `8,192,000 B`（app 余 749,712 B）；full `7,638,896 B` / 8 MB
- 包 `6,318,856 B`；16px 字库 2,999 字形（源码 2,666,593 B）
- `.dram0.data` 7,300 B，`.dram0.bss` 244,856 B；`s_app` 34,728 B（含可重用
  `load_vm` 8,472 B 和 1,232 B 手册工作区（union）；NVS 扩容前的静态值）
- 静态画面缓冲：art 320×150 RGB565 96,000 B + strip 160×45 14,400 B +
  立绘 scratch 150×150 45,000 B = 155,400 B；**不分配整屏 153,600 B 连续块**
- LVGL 内置池 44 KB；TJpgDec 工作区 4 KB；立绘/背景解码不启用整帧 RGBA/PNG 缓存
- 动态项（未在 MAP 静态区重复计算）：draw 11,520 + input 7,168 + LVGL port
  7,168 + main 6,144 + esp_timer 3,584 + event 2,304 + FreeRTOS timer 2,048 +
  controls 1,824 = **41,760 B**。按 5.5.3 注册区：A=
  `0x3fcdc710-0x3fcce710`=57,344 B，B=14,576 B；地址相邻但注册/阶段不同，
  **A+B 不能推出最大连续块**。A-41,760=15,584 B。
- NVS 38 页新增动态缓存：target `sizeof(Page)=84`，Page 数组 38×84=3,192 B
  （旧 16 页 1,344 B，22 页增量 1,848 B）；target `HashListBlock=128 B`：
  官方生成镜像实际 31 个有项页/35 个逻辑 key，31×128=3,968 B；reviewer
  保守 32 used/6 free 模型 32×128=4,096 B；全 38 页最坏 4,864 B。
  NVS 总缓存：官方镜像 7,160 B、保守 32 页模型 7,288 B、全 38 页 8,056 B；
  这些是“每页 1 个 HashListBlock”的场景；HashListBlock ENTRY_COUNT=29，
  单页最多 ceil(126/29)=5 个 block，故不是生命周期最坏上界。旧 16 页基线
  3,392 B，全 38 页增量 4,664 B。A-41,760 后为 15,584 B；
  扣 NVS 后约 8,424 / 8,296 / 7,528 B（依次对应 31/32/38 有项页），
  尚未扣 SPI/面板/I2C/队列/TCB/堆元数据/碎片；不是 free/largest，也未上机。

## 构建与验证

```bash
python tools/manosaba_pack.py --source <源快照> --out main/gal_data/manosaba.pack
python tools/manosaba_font.py --font <源快照>/src/common/manosaba.otf \
    --fallback-font "$WINDIR/Fonts/NotoSansSC-VF.ttf" \
    --pack main/gal_data/manosaba.pack --source-root main --out-dir assets/fonts
idf.py build
idf.py merge-bin -o <绝对路径>/build/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build
python tools/archive_firmware.py create build --archive-root build/firmware
python tools/manosaba_audit.py --source <源快照> --pack main/gal_data/manosaba.pack \
    --report build/recheck-audit3.json
```

- 独立源审计：`AUDIT PASS: 657333 checks`（`build/recheck-audit3.log`；
  `#000000FF` 白、逗号 actor 默认立绘两项已按源修正）
- 路线审计：371 个章节入口全部可启动；新游戏 DFS + 每个选项逐行探测 +
  3,000 次随机试玩；**本轮路径观察到** 398/424 场景；可达
  `bad`、`clear`、`trialFail` 等结局。精灵池 96（源路径实测最高 33）；超过 96 的异常状态会明确报
  “立绘状态超过容量”，不静默丢角色。
- 其余 26 个场景本轮未被观察到：11 个 `_TrialInit`（系统过场，单次
  `read` 内穿过）、13 个 `CommonBad01..13`、`System_BadEnd`、
  `System_Title`。这**不证明**它们不可达；`CommonBad` 的
  `Common/CommonBad{commonBadEndId}` 零填充（ID 1 vs `CommonBad01`）是
  **当前源行为**，需独立验证/复核；数据均仍在包内。
- 离线容量（仅离线模型）：31 个 save + 3 个 metadata（unlock/prog/set）+
  namespace；工具摘要 `slots=34` 表示 31 个存档 BLOB + 3 个 metadata，**不是
  34 个存档槽**。IDF 5.5.3 官方 `nvs_partition_gen.py` 实际镜像：used
  `3,824` / free `964` entries、31 used / 7 free pages；30 个双 BLOB_DATA
  chunk（`s01..s29`+`auto`）使 entries 比原 3,794 模型多 30，并少用 1 页。
  page 30 余 82 entries，新写最大 BLOB 需 122，必须用新页；用后余 6 空页。
  encoder witness：ordinary `257 B`、构造 max `3,278 B`、`GAL_SAVE_MAX=3,824`；
  31×3,824 是 synthetic upper，不是 31 个真实剧情档。源可达最坏未证；
  此镜像不证明物理 commit/GC/掉电。旧 64 KiB 负例被拒。
  日志：`build/nvs-capacity-e_3uh9hp/`、`build/recheck-app-nvs-final3/validate-static.log`。
- NVS RAM：target `sizeof(Page)=84`，38 页 Page 数组 3,192 B；实际 31 个
  有项页 HashListBlock 3,968 B，reviewer 32 used/6 free 模型 4,096 B，全 38
  页上界 4,864 B；详见 `build/nvs-page-size-nvs.log`。
- 严格 trace（完整长度，非共同前缀）：`test` 411/411、`trial04` 179/179、
  `trial14` 249/249，`mismatches=0`；`adv25` 仅 28 个地图“已调查”行文案、
  `adv35` 仅 1 个处刑按钮行文案差异，均由 app 层真实 UI 补偿，不伪装成
  VM 文本相等。项目内 x4 严格器已真实导出/比较 row.value（不再硬编码 0）；
  仅验证样本/静态入口，未逐项点击全部 UI、未跑全剧情。日志见项目内
  `build/review-model/second-round-20261102-141137/logs/*.strict.log`。
- 图像：559/559 张真实解码通过，151/151 立绘遮罩逐像素通过
- 存档：v2 完整往返（含真实 `isDebugDirectJump` 与 bodies）、CRC/版本/长度/
  截断拒绝、失败不改旧档、31 槽 NVS、commit 失败旧记录保留（测试替身契约）、
  解锁版本/CRC/合并/重试；旧 v1 档保持只读兼容
- UI：真实 `gal_app.c`/`gal_ui.c` 薄桩驱动，标题→新游戏→正文→选项→菜单→
  保存/读取→历史→鉴赏→章节→手册→设置→关于→结局；LVGL 锁违规 0；
  61 组 172 张 CG 与 33 个跨页章节样本均可直接打开；待选/反驳/正文在
  手册/详情/历史/设置/读档/保存往返后恢复同一光标、页和 VM 位置
- 字体：`--check` 2,999 码位、0 缺字、0 空字形；图片/字体负例门禁确认
  编译失败或缺少字库都会非零退出
- 完整 `tools/validate.sh --static`：本作全部测试 PASS；仅
  `install_passport_skills` 的 Windows 符号链接测试被 SKIP，因此整体
  `Host tests: PARTIAL`（本轮日志见 `build/recheck-app-nvs-final3/validate-static.log`）。
- 门禁不删除证据：宿主临时目录 `build/host-tests.*`、固件
  `build/firmware-build.*`、负例 `build/gate-evidence/` 全部保留并打印绝对
  路径；结尾 `Gate summary: PASS/FAIL/SKIP` 统计本轮全部门禁项。

同次归档：`build/firmware/7f90c4e15f1bc4f8ff179759123b50b9c63fcdf17c1629200c791e7df538e976/`
（app/full/ELF/MAP/bootloader/partition/flash_args + manifest）；旧 `99b…`/`a14d…` 保留。

## 构建身份

| 产物 | 字节 | SHA-256 |
| --- | --- | --- |
| app | 7,442,288 | `81905d3c6f90dc24322132e41fbe0c4fc747bd3619249af9c3bb1a94fd688f5c` |
| full | 7,638,896 | `7f90c4e15f1bc4f8ff179759123b50b9c63fcdf17c1629200c791e7df538e976` |
| ELF | 17,258,540 | `116b3ac91d219f57f782f010da06f23c2bb327856e44209a0709498742318ff3` |
| MAP | 4,148,310 | `da803cfbfcb8b7d7a175f45cf875d5d0d9b0774bc1dad1a876c31fb19e8c2918` |
| pack | 6,318,856 | `e96268ff7c829507236767afb7ea014059d975b1068ad61ab3c896913ca813ea` |

本轮从 `sdkconfig.defaults` 新建 `build/recheck-build-nvs` 干净构建并 merge/verify；
上一轮 `build/clean-repro` 保留但不对应本轮源码，不能作为本轮回归。

## 本轮软件/源修复（节选）

- 包层：link count 加上固定“返回发言”行不超过 31；顶层非零段两两不重叠、
  段起点不得落入目录、重复 type/超范围拒绝；TEXT 只检查被声明引用的 UTF-16
  范围，拒绝 NUL/非法代理，允许合法空串与 payload 共享。
- 运行层：`build_objections` 对超限/缺失 link fail-closed；`GAL_MAX_SPRITES`
  从 24 提升到 96，`sceneBodies` 稳定排序（按 order 再按 x，等值保持源顺序），
  超限明确错误；`damage` 在 HP<=0 时不前进 cursor，`end_kind=trialFail`。
- 文本/UI：UTF-16→UTF-8 失败路径保持合法 NUL 前缀；手册最长 1,205 B 用
  1,232 B 有界工作区完整分页，307 个版本按源区间拼回一致；存档预览按源
  20 码点加省略号。
- 状态：阅读 checkpoint 与当前展示页解耦；存档 v2 保存真实
  `isDebugDirectJump` 与 bodies；读档失败不改当前 VM；NVS 读区分
  NOT_FOUND/IO_ERROR/CORRUPT/OK，列表已占但读失败显示“读取失败”并要求覆盖确认。
- 内容入口：鉴赏 61 组/172 张与 371 章节入口恢复源全开语义；解锁位仍记录，
  不擦用户数据。
- 资源：打包器仅把精确 `#000000` 当黑（`#000000FF` 为白），actor base 按
  `[-,]` 分割，`Alisa,...` 默认立绘恢复；新增 `trialFail` 结束 kind 字符串。
- 显示/RAM：BSP 刷屏 40→24 行（19,200→11,520 B），单缓冲/旋转/锁不变；
  可重用 `load_vm` 取代每次读档/槽位摘要的临时 malloc；目标 RISC-V
  `-fstack-usage` 最大帧 704 B（render_history），VM 最大 320 B（dynamic_jump）。

## 已知取舍

- 不包含源手表移植本就省略的语音、音乐、视频、3D 审判镜头和实时动画；
  立绘只保留主要表情帧（源 151 张全保留）。
- 标题图裁掉原画自带的触控按钮装饰区；不宣称保留 432×514 原始构图。
- 只装 16px 字库；20px 在 8 MB 预算下未提供，设置页只显示实际可用选项。
- 源 `index.ux` 的鉴赏室/章节列表全开；本实现已恢复源入口语义：61 组 172 张
  CG 和 371 个章节入口从首次启动即可选，不再由全局解锁位阻挡。解锁位仍保留
  作记录，不擦用户数据。
- “下一章”快速跳过未实现；用章节选择/正常阅读替代。
- 复审：源 `CommonBad` 动态跳转零填充缺陷是已确认的当前源行为；本轮未命中
  13 个 CommonBad 场景，待独立验证，不据此宣称不可达；不修改源文本，数据仍
  完整打包。
- 5 个字形（`↑`、`↓`、`栈`、`棚`、`盏`）源 OTF 不含，用系统 Noto Sans SC
  （SIL OFL 1.1）补齐；未使用商业字体。
- 设备端启动、按键时序、真实 RAM/free、帧时、NVS 磨损、屏幕取景均未验证。

## 来源、许可

- 原作与源移植：见源快照 README、`LICENSE`、`VALIDATION.md`；剧本/美术/角色/
  商标权利属于原权利人。
- 本移植代码为独立实现；上游 BSP 与仓库继续遵循其许可证。
- 字体：源快照 `manosaba.otf`（Noto Sans Mono CJK SC，SIL OFL 1.1）；
  5 个缺字由 Noto Sans SC（SIL OFL 1.1）补齐。字库 C 文件由
  `tools/manosaba_font.py` 生成，不改变原字形许可。
