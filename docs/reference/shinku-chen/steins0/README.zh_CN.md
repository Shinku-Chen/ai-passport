<p align="right"><a href="README.md">English</a> · <strong>简体中文</strong></p>

# 《命运石之门 0》阅读器（RW5 源，本地技术验证）

这是运行在 AI Passport 上的横屏视觉小说阅读器，取自已转换的小米 Watch 5
源快照 `steins0-rw5`（提交 `dd19f6ce40b348642b17126e25a327846d47367c`）。
它是**本地技术验证版本，不是社区发布**：剧情、图像、标题美术与 PUA 字形
字库归权利人所有，未获再分发授权。

## 交付身份

- 工作目录：`D:/Git-Workspace/Shinku-Chen/ai-passport-steins0`
- 分支：`feature/port-steins0-landscape`
- 框架基线：`3c2e5013a9831c57124ef8f774193f0e06f1f14a`
- 应用镜像：**7,942,464 字节**，SHA256
  `8e0e3e24c7eef711a09751e2bc239a78d02f6b44dfb902a283bc8f223859012c`
  （8,323,072 字节 factory 分区，**余 380,608 字节**）。
- 完整合并镜像：**8,008,000 字节**，SHA256
  `38098013f7745401ae991955f7d54c205edc8af1fc59e062facedc868e62b335`。
- 同次归档：
  `build/final-candidate-20261102T090728Z-43806/archive/38098013f7745401ae991955f7d54c205edc8af1fc59e062facedc868e62b335`
  （ELF、MAP、应用/完整 bin、bootloader、分区表、flash_args、`manifest.json`）；
  archive create+verify 通过。
- **软件审核：PASS；待设备验收。** `38098013…` 归档已由 final-bind 独立
  绑定（9/9 复制、7/7 manifest、实际分区/full/app、源码 compile-command/
  object/MAP、pack/font 与独立 archive verify 全 PASS）。它是当前冻结软件
  身份，源码为 app `604291b0…`、app.h `1142546f…`、engine `8808f552…`、
  text `3e32fd6f…`、image `a48078b9…`。旧 `aabf3025…`/1951 仅保留为历史，
  不是当前身份。**Device review NOT RUN**，因此这只是软件验收，不是设备或
  发布验收。
- ELF：**17,330,216 字节**，SHA256
  `0702f16a2e8d97cf63a464797fa7d211b42c95ddecd1be92a4d5f054c05501d5`；
  MAP：**4,155,368 字节**，SHA256
  `cb98f76db9eb4b4bb88a1cff3e10598479743b17988f777eaf50db59a6ccff04`。
  两者都在上述同次归档目录内。
- 资源包：`assets/steins0/steins0_pack.bin`，**6,815,468 字节**，SHA256
  `42ec84783cafbdbd52c3ce187cb9dca0cc8e0f36723488ea9f3e189bb223d742`
  （派生自原作素材，不提交）。
- 生成字库源码：`assets/fonts/steins0_font16.c`，**2,955,758 字节**，SHA256
  `cd29856e69cd72ed6a6b9b16677d98e4ef0c83c57fa3a92f0f31d5c0dc4bc2b7`；
  编译后的 font 对象为 **444,920 字节**。
- 打包报告：`build/steins0_pack_report.json`；独立源审计：
  `build/validate-host-*/source_audit.json`。

## 保留了什么

独立审计工具 `tools/steins0_audit.py` 不导入打包器，自己解析源快照，再把
每个对话 op、人名、view/layer、选项文案/落点/action、条件、结局、章节入口、
鉴赏项与 RINE 事件同包内字节逐项对照：

| 源内容 | 数量 |
| --- | --- |
| 剧情节点 / 唯一 chunk 文件 / chunk 引用 | 41 / 853 / 1,567 |
| 对话帧（唯一 / 按节点引用） | 33,549 / 61,810 |
| 文本单元 / 码点（人名 + 正文） | 52,279 / 681,673 |
| 不同 PUA 字形 | 3,137 |
| 选项 / 条件 / 结局节点 / 不同结局 | 28 / 1 / 12 / 6 |
| 章节入口 / 章节组 | 104 / 14 |
| 鉴赏组 / 差分 | 106 / 198 |
| view / 唯一图层 / 图层引用 | 3,979 / 2,558 / 9,128 |
| 资源 id / 唯一图片文件 | 1,769 / 1,558 |
| RINE 目录消息 / 回复窗口 / 选项 | 1,385 / 16 / 48 |
| RINE 回复序列 / 联系人 / 基数 / 章节卡 | 504 / 10 / 16 / 14 |
| 剧情图中的源事件 | 1,165 |

当前包格式是 **32 段**（`SG0_SEC_COUNT=32`），包含 RINE 目录、回复、选项、
序列、联系人、基数和章节卡段。没有删掉任何剧情帧、CG 差分、表情图层、RINE
事件或章节卡；所有源图片资源 id 都在包内，包括标题合成图（`@title`）、
遮罩和裁剪资源。打包器只对完全相同的 part 与未引用载荷去重。

## 文本编码、排版与字库

源快照使用私用区编码（`U+E000` + SC3 字形 id），`sg-original.ttf` 没有完整
Unicode 映射。本移植保留 PUA 文本与源字形，不臆造 OCR/Unicode 转写。
`tools/steins0_font.py` 把 `sg-original.ttf` 的 PUA 字形、ASCII 与 Noto Sans SC
的常用中文 UI 字形合并为一个 16px/4bpp LVGL 字体：**3,733 码位，0 缺字，
0 空字形，行高 19，基线 2**。这比早先的 3,700 码位子集新增 **33 glyph**；
编译后的 font 对象由 438,156 字节增至 444,920 字节（**+6,764 字节**）。
PUA 拉丁与标点的推进宽度取自源 `glyph-widths.js`（`main/steins0_widths.h`），
保证 C 分页与字体 advance 一致。

排版实现 UTF-8 按码点分页、打字机与闭标点行首禁则。禁则会跨连续闭标点/开
括号做**有界回退**，直到找到合法断点或 `line_start`，避免第二个 `。` 再次
落到下一行行首。宿主测试把全量语料排版一遍（61,810 texts / 62,100 pages）：
**0 行超宽、0 个 U+3002 行首、0 个等价 PUA 句号行首**，不拆 UTF8、不静默截断。

## 画面、屏幕与操作

- 逻辑屏幕 320×240。场景沿用源 432×243 坐标空间，等比缩放到 320×180；
  对话框为底部 320×60。立绘/表情 part 按源坐标定位并裁剪到画面区。
- 背景/CG/标题是 JPEG（运行时不解析 PNG）；角色身体与表情补丁是
  JPEG + 1bpp 遮罩，用真实 TJpgDec 回调逐 MCU 解码合成。换场前先完整预检
  view 与遮罩；任一图层失败时缓存保持失效，下一帧重试。
- 质量档：照片 320×180 q38，身体 q48，表情补丁 q58，标题 q70；JPEG 4:2:0，
  非渐进。当前图片资产合计 **5,325,033 字节**：照片 483 文件/2,732,592 B，
  身体 347/1,580,016 B + 184,435 B 遮罩，表情 728/572,894 B + 76,519 B
  遮罩，标题 14,161 B，14 张章节卡 98,506 B，21 个 RINE 贴纸
  59,958 B JPEG + 5,952 B 遮罩 = 65,910 B。编码 image blob 为 5,329,007 B。
  抽样 PSNR：照片 36.71 dB，身体 28.07 dB，表情 24.73 dB；保留实测档以
  维持 Flash 余量。解码目标 surface 的 width/height/stride 显式贯穿：
  场景/CG/标题/章节卡用 320×240，RINE 贴纸用真实 96×96。guard/canary
  测试覆盖全部 21 个贴纸、目标边界 RGB/透明像素、clear/failure 路径和坏
  贴纸重试；已移除固定 320 stride 写入。
- 操作：UP 短按推进（打字中先全显），UP 长按快进、松手停；DOWN 短按历史，
  DOWN 长按自动；OK 短按确认/推进，OK 长按菜单。列表 UP/DOWN、OK、长 OK 返回。
  长按后的尾随 CLICK 被吞掉，下一次独立短按仍生效。
- 自动阅读会跨句、跨页、跨章节卡持续。手动短按/长按 UP/长按 OK 会停止；
  菜单/标题也会停止。自动不会代选选项或来电，遇到待选/来电会停住。
  快进松手停止；快进与自动都在选择处停止。
- 页面：源标题美术 + 六项菜单；正文（人名、3 行、页码、章节/日期、电量）；
  两路选项浮层；菜单；30 个手动存档 + 独立自动档；设置（文字速度、自动间隔、
  通关记录、关于）；章节选择（14 组/104 入口，空进度全开）；鉴赏（106 组/
  198 差分，CG 全屏切换，空进度全开）；历史（8 条）；关于；结局；覆盖/返回
  确认；章节卡；RINE 手机（收件箱、已发送、联系人、来电、回复、贴纸）。

## RINE / 手机子系统

手机 UI 已实现，不再作为缺口。它使用源 RINE 事件流和包内 RINE 段：

- 收件箱/已发送/联系人/来电视图，未读/已读、回复窗口与回复选项、21 个动画
  贴纸、章节卡与日期/联系人标签。
- 非关键来电的 `locate` 迟到时，自动 defer 到 `n2@0` 并保留 `pendingCall`；
  稍后从手机接听跳到 `n1@0`。`pendingCall` 按源时机由 `closeChoice` 清除，
  存档/读档会保留该状态。
- RINE 状态进入存档：目录 refs、已读位、已回复/窗口位、待处理来电和 last
  card 都会在 checkpoint 恢复，不会重放消息。
- 回复/连续消息行继承母邮件 date，使 sent 与 incoming 落在同一
  `(base,date,sender)` 线程；源 S_reply 对照日期为 101123。
  `sg0_engine_rine_date` 返回 DATE 表下标（uint16）；app 再用
  `sg0_pack_date()` 取实际六位日期 VALUE，以 `uint32_t` 存入
  `phone_thread_date`、typed thread key 及所有列表/header 格式化，不再把
  VALUE 强转 uint16；base/消息 id/日期下标保持原宽度。`unread` 只统计
  incoming 未读（源 S_reply = 6），不会把 outgoing 塞进 inbox read 集合或
  算进徽标。
- `expireRine` 只在窗口已读且未回复时补 fallback 消息；未读或已回复分支
  不会补。

源大小与编码大小分开列：

| 项目 | 字节 |
| --- | ---: |
| 源 phone + reply 正文（`phone/*.txt` 49 文件 + `reply/*.txt` 16 文件） | 68,467（33,600 + 34,867） |
| 编码 RINE 定长记录（RMSG/RREPLY/RCHOICE/RSEQ/RCONTACT/RBASE/CARD） | 23,820 |
| 编码 RINE 文本 payload | 48,767 |
| 编码 RINE 贴纸（21 个；JPEG + mask） | 65,910 |

68,467B 源正文、23,820B 定长记录、48,767B 文本 payload 与 65,910B 贴纸是
不同口径，不能相加成一个“117 KB”数字。

## 存档、条件与解锁

- NVS 命名空间 `steins0`；手动槽 `s00`..`s29` 加 `auto`：**共 31 个槽记录**。
  新写入为存档 **version 3，432 字节**。前 64 字节仍为 core；扩展区携带
  RINE active/read 位图（各 1,408 个目录项）、replied/window 位、待处理来电、
  last card、**event_node**、**event_cursor**（已消费节点事件序号，不是 story
  cursor）和扩展 CRC16。保存会记录事件位置；apply 后 `load_op` 不会重放已消费
  的 `rineReset`/`rineReply`。v3 的 event_node 必须等于保存节点，event_cursor
  不得超过该节点 `event_count`，否则拒收。
- 旧 v2（428 字节）保留 folded RINE 状态，读取时按保存 node/story cursor
  扫描事件求已消费序号，但不 apply 事件；旧 v1（64 字节）没有 RINE 历史，
  以空 RINE 和派生事件序号载入，不会在未经确认时改写或擦除 NVS。迁移本身
  绝不擦除 NVS。
- EMPTY、读取失败（IO）与损坏/版本不兼容（CORRUPT）是不同状态。`nvs_open`/
  `nvs_get_blob` 失败不会被当作空槽；对已有、IO 或 CORRUPT 槽保存必须显式
  确认。取消保存后原 blob 逐字节不变。只有 `nvs_set_blob` **与**
  `nvs_commit` 都成功，才更新 RAM 槽位、预览和“保存成功”提示。NVS 打开失败
  降级为只读/不落盘；绝不调用 `nvs_flash_erase`。
- 选项处存档记录节点末尾，读取后回到同一待选；页码与 story/chapter 模式保留。
  `n0@108` page1 回归会核对读取后的 node、cursor、page、view 与正文。
- 全局解锁是独立的 34 字节带版本 blob（6 个结局位、104 章节位、106 鉴赏位、
  flags、CRC）。启动加载；读旧档时做并集合并并置 dirty；所有页面按 5 秒节流
  重试，睡前强制再试。存档内也带一份解锁快照。
- **章节/CG 访问不受 unlock 门禁**。空进度从标题 UI 即可进入全部 104 个章节
  入口和全部 106 个鉴赏组（198 差分），对应源 `openRoutes`/`openChapters`
  与 `galleryList`/`viewCg`。unlock/章节/鉴赏位只保留为已阅/到达 metadata，
  不再用来隐藏内容。
- 唯一的源条件 `n8` 按源语义：结局 0（`prComplete`）走 `n13`，否则 `n14`；
  结局 5 同时记录结局 3。chapter 模式到 END 走与源 `finishGame` 等价的路径，
  会写结局 progress。所有分支可达；宿主遍历覆盖两种条件结果、41 个节点、
  6 个结局和 104 个章节入口。

## 容量

Lead 本地离线 NVS 容量证明（不是项目文件）覆盖真实生产 key 集：NVS 24 KiB；
baseline **507 used / 249 free entries**（1 个整页 erased），保守 shadow
**546 used / 210 free entries**（1 个整页 erased）。包含 31 条 ×432B
（`s00..s29` + `auto`）、unlock 34B、cfg 4B。shadow 是离线并发放置上界，
不是物理 GC/掉电事务；A/B 不是 heap 最大连续块。这不构成物理 NVS、heap
或 stack 验收。

## RAM 账

以下来自当前归档的 ELF/MAP，不沿用旧的 1951 构建。DRAM 段与 heap start
分开列出；低地址尾部**不当作实测运行期 heap**。

```text
dram0_0_seg        321,296 B  (0x4e710)
.dram0.data          7,300 B
.dram0.bss         240,352 B
.dram0.dummy        69,120 B  (链接填充/IRAM 重叠，不是 heap)
.iram0.text         68,768 B  (IRAM，仅为完整起见列出)
_heap_start       0x3fccd570
segment end       0x3fcce710
link-time tail       4,512 B  (0x11a0；不是运行期 free-heap 声明)
```

镜像内静态缓冲包括 320×240 RGB565 画布（153,600 B）、96×96 贴纸画布
（18,432 B）、LVGL 池 `work_mem_int`（配置 32,768 B）、正文解压缓冲（4,608 B）、
遮罩解压（7,168 B）、JPEG 工作池（4,096 B）与 `s_app`（191,228 B，上述画布/
状态都在其中）。显示使用单缓冲 240×40×2=19,200 B DMA draw buffer，不是双缓冲。
初始化动态预算包括输入任务栈 4,096 B（+TCB/队列）、LVGL 任务栈 7,168 B
（+TCB）与 main 任务栈 3,584 B；main 任务释放生命周期未实测。未计入
NVS cache/driver、任务 TCB、碎片、LVGL port heap 及运行期最大连续块/剩余
堆；链接期 4,512 B 尾部不是 heap。未使用 PSRAM，也不使用整帧 RGBA/PNG
缓存。

same-MAP 预算视图为 A **61,856 B** / B **14,576 B**；known dynamic
**37,632 B** 后 A 余 **24,224 B**，这是 pre TCB/queue/NVS/driver/fragmentation
预算，不是实测 heap 或最大连续块。上面的静态池已在 BSS；链接期 4,512 B
尾部不是 heap。

实际运行期 heap、free/largest block、high-water mark、NVS 分配行为和帧时
均为 **NOT RUN**，不能从链接期尾部推断。不同动作序列的 unread 与 auto-card
自然复现仍为 **PARTIAL**；同一动作 S_reply 的 incoming-only unread 为 6，
不与包含 outgoing 或已打开 thread read 状态的探针计数混比。

## 验证状态

- **Build/merge/verify/archive：PASS。** 用 tracked `sdkconfig.defaults`
  （LVGL 9.5.0 固定、32 KiB 池）在 ESP-IDF 5.5.3 隔离目录构建、绝对路径
  `idf.py merge-bin`、`verify_firmware.py`、archive create+verify 与独立
  archive verify 全通过。应用 **7,942,464 / 8,323,072 B**；完整镜像
  **8,008,000 B**。第一次 wrapper merge 为 **PARTIAL**，仅因相对
  `-BuildDir` 生成相对输出路径（`FileNotFoundError`）；target compile/link
  已成功，绝对路径 merge 重试 PASS。
- **C/Python 套件：游戏专项 PASS，整体 FAIL。** 当前
  `tools/validate.sh --static` 退出码为 **1**，汇总为
  `Host tests: FAIL (suites PASS=34/36 FAIL=2 SKIP=0; source_audit=PASS)`。
  **23/23 C 回归**通过，其中 **9/9 个 Steins;Gate 0 C 套件**（pack、text、
  story、app、UI、RINE、all-open、IO replay、event save）通过；**Python 4/6
  通过**。
  独立源审计对当前 32 段包（6,815,468 B，SHA `42ec8478…`）PASS。app 级
  event-save 回归覆盖 n0@243/n31@730 的保存/读取/继续/自动、v2 folded 迁移
  与非法 event-node/ordinal/CRC/version 拒收。app UI 对 R1 原线程/回复/已发页
  保持 `11.23`；v3 为 432B、event ordinal 4/6，v1/v2 迁移与 page1 往返均有
  覆盖。不同动作序列的 unread 与 auto-card 自然复现仍为 PARTIAL。脚本已移除
  按历史失败数量降级：任何非零结果都计入 FAIL。
- **基线 Python 门禁：实际 FAIL，不写成 PASS/PARTIAL。**
  `test_check_repo` 在当前 Windows 主机有 **5 个实际失败**；
  `test_install_passport_skills` 有 **3 个实际失败**。它们作为两个失败套件
  计入整体 FAIL；不存在组合运行“30/30 全通过”的说法。日志：
  `build/validate-host-20261102-152002-35404/test_check_repo.log` 与
  `.../test_install_passport_skills.log`；分类维护日志：
  `build/gate-classification-*/`。
- **分类维护测试：PASS。** 静态门禁会运行
  `tests/test_validate_classification.sh`（共用 `tools/validate_summary.sh`），
  验证历史形似 5/3 计数、编译/运行非零、失败+缺源都保持 FAIL；仅 SKIP 为
  PARTIAL，干净运行才是 PASS。
- **独立软件审核：PASS；待设备验收。** 引用：
  `build/review-final-bind-20261102T093438Z-45265-b44fc5d7/REPORT.md`、
  `build/review-phone-date-20261102-154228/REPORT.md`、
  `build/review-app/v14-20261102T080736Z/REPORT-v14.md`、
  `build/review-app/v18-20261102T090420Z/REPORT-v18.md`、
  `build/review-app/v15-20261102T082519Z/REPORT-v15.md` 与
  `build/review-sg0-pack-font-supplement-20261102T064045Z/REPORT.md`。
  项目内 v18 已正确引用 Lead 本地容量结果；本文不把它改写成物理事务 PASS。
- **Device tests：NOT RUN。** 运行期堆/最大连续块/high-water/帧时、真实 NVS
  提交与重试、LCD/按键时序、物理屏 PUA 渲染以及全部交互验收仍未验证。

## 已知缺口 / 明确取舍

- 未包含 OP 视频、标题曲、BGM/音效/语音。
- 只提供 16px 字库；不提供 20px 选项。
- 这不是硬件、审核或发布验收。不要发布资源包或由游戏字库派生的 C 文件；
  请用合法持有的源副本在本地重新生成。
- RINE 手机 UI、迟到来电、章节卡、空进度章节/CG 全开、32 段包与新存档布局
  已在本候选实现，不再列为 deferred 缺口。

## 复现

```bash
# 1. 生成资源包（需 Pillow），并用独立解析器审计。
python tools/steins0_pack.py --source <steins0-rw5 checkout> \
    --out assets/steins0/steins0_pack.bin --report build/steins0_pack_report.json
python tools/steins0_audit.py <steins0-rw5 checkout> \
    --pack assets/steins0/steins0_pack.bin --json build/steins0_source_audit.json

# 2. 重新生成合并 16px 字体与宽度头（需源字体）。
python tools/steins0_font.py --pack assets/steins0/steins0_pack.bin \
    --pua-font <checkout>/src/common/sg-original.ttf \
    --ui-font <有许可的 Noto Sans SC OTF> \
    --widths <checkout>/src/common/glyph-widths.js --out-dir assets/fonts

# 3. 激活 ESP-IDF 5.5.3 后跑完整门禁。
STEINS0_SOURCE=<checkout> ./tools/validate.sh
```
