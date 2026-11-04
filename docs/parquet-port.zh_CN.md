<p align="right">
  <strong>简体中文</strong> · <a href="parquet-port.md">English</a>
</p>

# AI Passport 上的《PARQUET》横屏阅读器（320×240）

把 RW5（REDMI Watch 5）视觉小说阅读器 `parquet-rw5` 完整移植到 FoloToy AI
Passport：ESP32-C3、8 MB Flash、无 PSRAM、320×240 横屏、三键操作。固件开机直接进
本作标题页，不进硬件自检 demo 菜单。

状态：**软件审核通过；生产代码冻结，等待设备验收。Device tests = NOT RUN。**
本作 gate 通过；整仓 `tools/validate.sh --static` 在本机 Windows 上仍有基线失败
（见下）。Lead 审核证据：`build/lead-review-v4/probe-ui.log`、
`probe-badpack.log`、`negative-maintained.log`、`pack-stack.log`、
`archive-verify.log`（本机 gitignored 证据）。

## 源与覆盖

- 只读源快照：`parquet-rw5` @ `8ab38afe77c6a4d282447c7e13135b577a13303e`
  （KiriKiri Z 剧本 + 已转换的 RW5 素材）。打包与审计用的元数据入库在
  `assets/parquet-source/`；图片与源字体不入库。
- 当前源内容全部保留：**107 场景 / 434 剧本块 / 11,205 条 op / 11,098 条正文 /
  2 处选项节点（4 个选项）/ 4 个 END 场景**（`SIDE STORY`、`PARQUET` 与后日谈两个结局）
  / 54 个说话人 / 3 条路线共 37 章 / 37 组 367 张 CG。
- 入口按源实现：`sideInitial = 1`、`flowScene = initial = 5`（首次 START 进前传，
  之后进正篇）、`afterInitial = 93`（`sf.parquet_main_clear` 后标题才出现 AFTER STORY）。
  章节回顾从源章节场景进入，不写周目标记。
- 源事实：**99 号场景**是第二个选项节点（`dummyselect2`），没有任何入口、章节或跳转
  能到达；本作保留在包里，用"直接入口"回归覆盖，而不是删掉。
- 正文由 `tools/parquet_audit.py` 逐 op 对照源快照（整段正文 SHA-256 一致），
  "内容完整"是可审计结论，不是自述。

## 版面与按键

- 上方 320×150 RGB565 画面区（背景 + 最多 3 个立绘 + cut-in），下方 90 px 不透明对白带：
  说话人、章节标签、3 行正文。标题/菜单/列表/鉴赏/CG/结局都是独立 LVGL 版面，不是 demo 外壳。
- `another` 视角（695 条正文）采用横屏专属重设计：画面区固定 320×150，用左上角
  "ANOTHER VIEW" 角标标记；源 RW5 的做法是把美术区整体下移 24px 并缩短到 262 高。
  本作有意不复制该纵向构图，因此两者不是逐像素等价（内容与背景未变）。
- 按键：**UP** 推进（长按快进，松手停）、**DOWN** 历史（长按自动播放）、**OK** 菜单
  （长按 OK 也能开菜单）。列表：**UP/DOWN** 移动、**OK** 确认、**长按 OK** 返回。
  长按后驱动补发的 CLICK 会被吞掉，下一次独立短按不受影响。
- 待选必须显式 OK 确认；自动/快进遇到选项停住。从待选打开菜单、存读档、设置再回来，
  待选项与非零光标都保留。
- 逐字打字按累计时间推进：输入任务 50ms 一拍、最慢 55ms/字，剩余时间会保留，
  不规则短 tick 也不会停住或丢字。
- 自动档在仍有有效阅读点的任意子页（菜单/存读档/历史/设置）都会保存，菜单里睡着
  也保留同一句、同一页与同一待选；写失败按 2 秒退避重试，且不会覆盖手动槽。
- 设置里字号可在 **16 px / 20 px** 之间切换；两档都覆盖全剧本 + 界面（2,589 码位、
  0 缺字）。没有提供的设置不会显示成可选。

## 剧本包（`parquet_scn.bin`，535,634 B）

- 场景表（12 B）+ 16 B 定长 op + 12 B 视觉组合（2,623 种背景/立绘/cut-in 组合）+
  章节/路线/结局/鉴赏表 + 一个字符串段。正文按 32 op 一块压缩（361 块，原始
  552,959 B → zlib 315,531 B），跳到任意章节只需解一块（最大 2,370 B）。
- `pq_pack_init` 把每张表当作有序区间校验：起点必须在 header 之后、整体在包内、
  长度乘法先防溢出、非零长段互不重叠（零长表只要求起点合法）。正文块用减法式边界
  （`comp_off <= size && comp_len <= size - comp_off`），32 位回绕在 init 就被拒绝，
  装载时再查一次，任何解引用之前都先完成校验。
- op 语义与源 `src/common/engine.js` 一致：`text` 停在这一句，并自带视觉快照
  （背景、最多 3 个立绘 id、cut-in、`another` 视角、cut-in 不透明度）；`jumpScene`
  跳到 scene+op；`choice` 写源 `effect` 变量并跳转；`end` 在 story 模式写当前场景的
  `clearFlag`。
- 每条正文运行时都会校验声明长度与 NUL 位置；坏块显式报错，不静默跳过。

## 图片包（`parquet_img.bin`，5,187,277 B）

- 1,510 个条目：402 张 JPEG（背景/CG/标题）、138 张调色板 BLOCK（36 张共用身体 +
  101 张 cut-in + Logo）、970 张表情 PATCH。
- 背景与 CG 按 320×150 内接（fit）重编码，不 cover 裁切；367 张鉴赏图共用同一批
  载荷，完整显示并留黑边。JPEG 质量 62，399 张唯一背景共 2,835,630 B。
- 立绘保留源的"身体 + 表情"分层：979 个使用中的立绘 id → **36 张共用身体**
  （每张只存一份 BLOCK，不按 alias 复制）+ **970 张表情层**。表情按 alpha 包围盒
  裁剪，存成 RGB565 + 8bit alpha 的 PATCH 块（1,409,240 B），而不是重复的 97×148 PNG。
  锚点来自源 `faceAnchor`，所有立绘用同一 150/262 缩放；1/2/3 人同屏比例一致。
- cut-in 为 101 张 108 px 高的 BLOCK，按源的 200/255 不透明度叠在压暗的背景上。
  背景或任一立绘失败会画显式错误色、不解锁该 CG，下一次渲染自动重试。

## 存档与解锁语义

- NVS 里 30 个手动槽 + 1 个独立自动槽（`pqsave`）。存档包含场景、正文 op、页、模式
  （正文/待选）、待选光标、当前视觉组合下标（背景/立绘/cut-in/视角/不透明度）、变量表、
  预览和章节标签；用版本、总长、CRC32、场景/op 边界、视觉下标、变量 id、NUL 终止逐项校验。
  写入失败时旧档与当前可见状态都不变，并提示"保存失败"。
- 自动槽用于换场、睡前、回标题。NVS 从不擦除；打不开时降级为本次运行不落盘并提示。
- 全局周目记录（`pqmeta`）保存 4 个 `sf.*` 标记（side/main/rin/tsu）、37 个 CG 位、
  3 个结局位。启动即加载；所有页面按 2 秒退避重试写盘；读旧档只合并新增位
  （不会把全局已有的标记回退掉）。
- 读档会重放保存时的那条正文 op，因此同一句、同一页、背景、立绘与 cut-in 都恢复；
  待选存档恢复同一组选项与待选光标。

## 内存账（与归档同一次 ELF/MAP）

来自 `build/fw-parquet-fresh/FoloToy-AI-Passport.map`：

| 项目 | 字节 |
| --- | ---: |
| DRAM 段（`dram0_0_seg`） | 321,296 |
| DRAM 内的 IRAM 镜像复制区 | 69,120 |
| `.dram0.data` | 7,316 |
| `.dram0.bss`（含 24,576 LVGL 池、10,992 共享 tinfl、8,192 正文块、4,096 JPEG 池、4,096+2,048 补丁缓冲、1,688 BLOCK 行缓冲） | 65,344 |
| `_heap_start` → `0x3fcce710` | **179,504** |

动态分配（内部 8bit 堆）：画布 96,000（在 `app_main` 最早申请）、LVGL 刷屏缓冲
19,200（BSP 240×40×2，面板原生宽度）、输入任务栈 8,192、LVGL 任务栈 7,168、main/esp_timer/event
栈 3,584+3,584+2,304、NVS 页缓存与句柄约 8,400、堆元数据约 2,000 → **150,432 B**，
余约 **28.4 KiB**。idle 任务栈 1,536 是 FreeRTOS 静态栈、已在 MAP 里，不重复计入。
NVS 与元数据是估算值；真实 free/largest、帧时仍需真机测量。

## 验证

Tracked gate：`tools/parquet_gate.sh`（也被 `tools/validate.sh` 调用）。

1. `tools/parquet_verify.py --fixtures tests/parquet/fixtures` —— 独立解析两个包，逐 op/
   逐正文/逐鉴赏对照源快照（11,098 条正文 SHA-256 一致），并用自带的 BLOCK/PATCH
   解码器与 Pillow JPEG 解码生成逐图期望值。
2. `tools/tests/parquet_host_build.sh` —— 在宿主上构建并运行生产 C 代码：**109,324 项** + 坏包负例 **18 项**
   检查（包结构、全部 op、4 条路线/结局、孤儿场景 99、章节入口、存读档/损坏/失败、
   全局周目、1,510 张图全解码并与参考解码对照 —— 无损格式逐字节一致、JPEG 按
   均值/最大误差判定）；**168 项** UI 检查用真实 PRESS/RELEASE/CLICK/LONG 序列驱动
   生产 UI（标题、正文、历史、菜单、覆盖存档、待选读档、按键守卫、鉴赏、设置、
   字号切换、AFTER STORY 解锁），均 0 失败。
   坏包原生回归（`tests/host/test_pack_negative.c`，18 项）只改内存副本：正文块回绕
   在 init 被拒；两个包的段重叠/侵入 header 被拒；零长段保持合法边界；立绘 alias
   共享同一身体图仍然通过；正常包与正文数据段末尾的合法边界仍被接受。
   回归明确覆盖：50ms 节拍 + 55ms/字慢速打字与不规则短 tick；在菜单/历史/设置页
   flush 后仍恢复同一句/同一页/同一待选，以及自动档写失败后的退避重试；以及在别的
   场景停留后读取待选存档，背景/立绘/cut-in 与待选一起恢复。
3. 负例门禁：故意缺头文件的图片测试必须编译失败且 gate 非零退出
   （`tools/tests/parquet_negative_gate.sh`）。
4. `tools/parquet_audit.py` —— 独立源审计（不 import 打包器）：22 项检查覆盖全部 op、
   章节、路线、结局、鉴赏、立绘 alias 去重与可达性；0 失败。
5. 字库覆盖（16 px + 20 px，2,589 码位，0 缺字）与 `tools/check_repo.py`。

固件：ESP-IDF v5.5.3，用 tracked `sdkconfig.defaults` 在干净目录构建，`merge-bin`，
`verify_firmware.py`，`archive_firmware.py create` + `verify`。

整仓门禁（逐 case 统计、每次唯一 `build/validate-*` 输出目录、不删除）：

- `tools/validate.sh --firmware` 对冻结构建通过（干净目录构建 → merge → 布局校验 →
  归档校验）。本轮仅文档收尾、不重建固件，因此不重跑。
- `tools/validate.sh --static` 在本机 Windows：**PASS 18、FAIL 2、SKIP 4**。
  4 个 SKIP 是已知 baseline demo 链接差异（`undefined symbol: lv_* / ui_pixel_*`）；
  2 个 FAIL 是**未修改的基线自检**，与本作无关：`tests/test_check_repo.py` 断言诊断
  路径用正斜杠（Windows 输出反斜杠）、`tests/test_install_passport_skills.py` 需要
  目录符号链接权限（WinError 1314）。它们按 FAIL 如实报告，没有被跳过。
- 该次运行中本作 gate 为 PASS（109,324 项 C + 18 项坏包 + 168 项 UI + 22 项审计）；
  tracked 负例 `tools/tests/validate_negative.sh` 证明本作 C 编译或运行失败会让
  汇总 FAIL 且退出非零。
- 编译器发现已内置于 tracked 脚本（ZIG_CC/zig/`python -m ziglang cc`/CC），
  不再依赖 `build/bin/` 下被忽略的个人 wrapper。

## 构建身份

- `FoloToy-AI-Passport.bin` **7,179,360 B**
  SHA-256 `2b634e0f3c499f2dd61acd3ed6203ce8ab66b7ba9516c3b6a3c7861dc28a7724`
- `FoloToy-AI-Passport-full.bin` **7,244,896 B**
  SHA-256 `c5167b5dc92e77aef4f2b53816e060821ec18f8ef2c801dfbbb72c24c1fdea98`
- ELF SHA-256 `6701796901372aa691e403e77d21df6d4671a124dc5048c18c84cb8646acdb20`，
  MAP SHA-256 `e82b458bf605b01c4cb91354755965a5cf3f6114d1f0cb4d887d7070b62c0115`
- 归档：`build/firmware/c5167b5dc92e77aef4f2b53816e060821ec18f8ef2c801dfbbb72c24c1fdea98/`（full/app/ELF/MAP/bootloader/分区表/flash_args/
  manifest），已用 `archive_firmware.py verify` 校验。
- 资源包：`parquet_img.bin` 5,187,277 B
  SHA-256 `b6eef1efb14a65c56c026c08768c4fea581bea96d36751586d92217747114d89`；
  `parquet_scn.bin` 535,634 B
  SHA-256 `e092ae53db6d181df8235e6c16ca1fe591a9ab6c902368007a99c641b5fc9ad5`。

## 已知限制 / 未验证

- **Device tests = NOT RUN**：屏幕方向、三键手感、真实 free/largest、帧时、
  调色板/JPEG 观感与深睡唤醒都未上机验证。
- 源里的鉴赏室原本不锁；本作改为"CG 第一次完整显示后解锁该组"（有意的 UI 策略，
  内容未删减）。
- 章节回顾有意不写周目标记与结局。
- 99 号场景在源流程里没有入口，只能直接入口进入（测试已覆盖），标题与章节列表都到不了。
- 不含音乐/语音/动画：RW5 快照本身没有这些素材。
- 标题菜单没有 EXIT，因为固件没有应用退出接口；熄屏/深睡由空闲策略负责。
- 不显示电量；它是可选项，且需要版面空间。

## 复现

```bash
# 资源包（需要只读 RW5 源目录提供图片/字体；元数据快照已入库）
python tools/parquet_pack.py --source assets/parquet-source \
    --images <parquet-rw5>/src --out main/parquet_data

# 字体（Noto Sans SC / 思源黑体，SIL OFL 1.1；字体文件本身不入库）
python tools/gal_font.py --font <NotoSansSC-VF.ttf> --out-dir assets/fonts --sizes 16,20

# 全量 gate
bash tools/parquet_gate.sh

# 固件（已激活 ESP-IDF 5.5.3 的 PowerShell）
idf.py -B build/fw-parquet-fresh build
idf.py -B build/fw-parquet-fresh merge-bin -o build/fw-parquet-fresh/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build/fw-parquet-fresh
python tools/archive_firmware.py create build/fw-parquet-fresh --archive-root build/firmware
```
