<p align="right">
  <a href="galgame-anemoi.md">English</a> · <strong>简体中文</strong>
</p>

# anemoi — 横屏部分内容技术原型

本分支把 Redmi Watch 5 版《anemoi》（`xuanyide01/anemoi-rw5`，固定快照
`3e27714c50b860c674a86df90e3df3e4cf6dd254`）的**现有公共篇内容**搬到 AI
Passport 320×240 横屏：开机直接进作品标题，正文、选项、委托板、存档、鉴赏和
章节列表都使用本作品自己的界面。

**状态。** 这是**只覆盖现有公共篇的技术原型**，不是整作移植完成。候选包为
**80 个场景 / 61,257 条指令**：保留此前已交付的 71 场景原型，只补上正常流程
已经能到达、但原包缺失的源范围。女主角路线正篇、After 篇以及 070/071/072/073
书目仍未导入；越界跳转会显示分卷边界页。该原型用于验证解释器、渲染、存档和已
检查的公共路线，不能作为“整作无法移植”的证明。

## 1. 取景 manifest

`main/anemoi_data/anemoi_scope.json` 是经过审阅的、纯数据的源范围 manifest。
打包器会校验固定 source commit、场景存在性和范围边界，并拒绝重叠范围；它不能
凭空加入文本、flag 或另一棵 source 树。

| 范围 | 为什么需要 |
| --- | --- |
| 场景 2、3、……及原 71 场景基座 | 此前已交付的公共篇原型范围，不新增文本 |
| 场景 26 全段 `[0,len)` | scene 2 的 op145/153 调用 entry 0/10；原范围从 57 开始，没有 entry 0 |
| 场景 27 全段 `[0,len)` | scene 2 的 op279/287 调用 entry 0/10；原范围从 44 开始 |
| 场景 28 全段 `[0,len)` | scene 19/20/21 调用 entry 0；原范围从 143（entry 10）开始 |
| 场景 90 全段 `[0,len)` | scene 2 的 op1030 调用 entry 0；原包没有该场景 |
| 场景 167..174 全段 | scene 134/135/136 以 entry 0 调用全部 8 个 play 场景 |
| 场景 125、131 的 `[0,160)` / `[0,149)` | 只保留原有路线章头；不导入整本 040_淡雪 |
| 场景 133..138 的 `[1,len)` | 原有小詠 entry 范围；a13 增量在 134:622、135:442、136:483 仍被打包 |
| 场景 175 的 `[128,476)` | 原有六花 entry 范围；不扩成未移植路线 |

明确排除：场景 205（070 スピカzero）、206（071 同棲編）、262（073
シルバー編），以及 193 个未移植路线章节。逐场景理由记录在 manifest 的
`reasons` 表中。

## 2. 容量（实际构建，不是估算）

| 量 | 字节 | 说明 |
| --- | ---: | --- |
| `factory` app 分区 | 8,323,072 | `partitions.csv`，0x7f0000 |
| 被审 app 镜像 | 7,396,384 | `build-hotfix/FoloToy-AI-Passport.bin` |
| 合并 full 镜像 | 7,461,920 | `build-hotfix/FoloToy-AI-Passport-full.bin` |
| app 内非包部分 | 1,094,594 | app 镜像减 6,301,790 B 的包 |
| 包的资源上限 | 7,228,478 | 8,323,072 − 1,094,594 |
| 候选包 | 6,301,790 | sha256 `68eec3ca5e332ea734cce94360a9c81609e9cc72a5ba3da57d7d2cfed1ec8160` |
| app/flash 剩余 | 926,688 | app 分区的 11% |

合并 full 镜像为 7,461,920 B，8 MB flash 仍余 926,688 B。full 镜像 = app
镜像 + 65,536 B 的 bootloader/分区表，因此用 app 分区和用整片 flash 算出的
包上限相同。

## 3. 编码候选与视觉对照

三个候选都使用同一份 80 场景 manifest 和同一份 16px 字库。

| 候选 | 版面 / 调色板 / JPEG | 包字节 | 结果 |
| --- | --- | ---: | --- |
| A-q30 | 0.52 / 32 / q30 | 7,527,695 | 装不下：超过 7,228,478 B 上限 299,217 B |
| **A-q24（选用）** | **0.52 / 32 / q24** | **6,301,790** | 可装，余 926,688 B；保持原版面和调色板 |
| B | 0.46 / 16 / q25 | 6,470,206 | 可装，但只作为画质对照，不选用 |

本次修补包保持已审阅的 80 场景 manifest、版面和 JPEG 质量，实现源 UI 的
object-fit:contain + #fcfaf0 纸色底（RGB565 0xFFDE）：场景/CG 按可见尺寸
记录并居中，不再复制边缘列，也不补 8 像素黑边。标题选择按
title-buttons.js：fresh/normal 用 title-bg.jpg，level 1/2 用
title-eternicle.jpg，level 3/4 用 title-anemoi.jpg。立绘量化给 alpha<8 的
像素保留独立 clear 项，并把可见 RGB 项拆成不透明/低半透明/高半透明三个
alpha 桶（32 个 RGB 色，最多 97 项，实测最大 75），在不增加整帧 RGBA 缓冲
的前提下按 3 个 alpha 桶近似保留半透明边缘。标题 source blob 不可用时构建硬失败；读取使用
固定 source_commit。不删除台词、选项、图片或表情，也不改变立绘版面。资源
口径以二进制 SEC_IMAGE 行数为准：release 2,000 -> candidate 2,068
（+68）；kind 0 scene 行 +16（322 -> 338），kind 1 body/face 行 +52
（1,676 -> 1,728），kind 2 solid 行不变（2）。65 个新正文字形是独立的
字库口径。标题图来自固定 source 的 title-bg.jpg（sha256
44ba039e08ab7cd9b42cebf46350a8a48dc180a7153059d2c0be9ec082ed3241）、
title-anemoi.jpg（sha256
2546fd25849e26d6e067dedb861616e50f768bb3e7452055d06a4814e085ad48）和
title-eternicle.jpg，均打包为 151x180，不是伪造图。END 时 progress flush 取脚本计算 level 与已加载全局 level 的较大值；END→flush→reload 选择已获得标题，NewGame/Continue 的旧 slot 变量不会降低已有解锁。source image key 与
二进制行不是 1:1，因此这里不用 source-key 计数代替行数。

作为对照，B 候选也已实际构建：app 7,564,512 B / full 7,630,048 B，包 sha256
`5320c42c2b91dbbc376189b0fb1eddf3022599f5afbe25e997af5e93d2206767`。
它可以装下，但版面/调色板/JPEG 的画质回退更大，因此只保留为对照候选。

同一背景、立绘、脸部裁剪和对话页的 A/B 审计渲染在
`audit/repairs/anemoi-screens/`。相对 A-q30 的 PSNR：A-q24 45.31 dB，
B 37.67 dB。渲染器模拟当前游戏画布；生产 C 路径只绘制身体层，因此脸部裁剪
是单独的视觉检查项。

## 4. 字体

本作只编译 `gal_cjk_16.c`，本卷不提供 20px。生成的 16px 子集共
**3,079** 个字形，覆盖包内正文和界面字面量，包括新增的 65 个正文字符。
字体来源：

- `assets/fonts/gal_cjk_16.c`：2,739,863 B，sha256
  `f8553da2f50e00ed653d7184bd8b0f33b056c3ae251fc632bae6344c998b0e5b`
- `assets/fonts/gal_cjk_symbols.txt`：9,200 B，sha256
  `7699760f7cba84074d8c58205aca76327b6389ccba78e68c3a801fd2ebef0e18`
- 源字体：Noto Sans CJK SC Regular OTF，sha256
  `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`
  （不提交）

检查命令：

```text
python3 tools/gal_font.py --check --sizes 16 --out-dir assets/fonts
```

## 5. 正常路线证据

以下都是正常 NewGame/选择运行，不是章节直达或 flag 注入；只有明确注明时才
使用合法 progress 存档。

- **Spica 入口：** scene37 trace 已到达 `SCENE 28 cursor=21950`（entry 0），
  之后返回 21；原来的 scene28 entry0 阻断已消失。该运行随后仍停在未移植的
  爱乃边界，因此这是入口覆盖，不是完整 Spica 结局。
- **小詠 8 个 play 选项：** `anemoi-134-natural-walk0..7.log` 走完 scene134
  cursor 43186 的 8 个选项；item 7 还经过 scene174 cursor 50927。路径继续
  穿过已打包的 167..174 范围，最后停在小詠路线边界。a13 增量 op
  （134:622、135:442、136:483）都在打包范围内。
- **六花边界：** `anemoi-normal-rikka-A-q24.log` 通过正常板选择到达六花
  资格边界。
- **82 个 normal menu item：** 把 release 包的已审 witness plan 按源 op
  偏移翻译到 80 场景包（新增 entry 0 会移动绝对 cursor），再用生产 C
  解释器重放。选项 label 与原 witness 日志一致，82 个目标 item 全部被消费，
  每个 plan 都到达 `PLAN_CONSUMED target_hit=1`。部分旧 plan 原本停在边界，
  新包会正确继续前进，因此这里是 item 覆盖，不是“终点不变”的声明。

## 6. 阳彩任务门禁：SOURCE_DEADLOCK

阳彩任务 3 的板项在 scene393 op26 由 `$$check_character_bbs_disp(3)` 门控。
当 `a12==2` 时，helper op128 要求 `a2<=15 && a3==1 && a17>=2`。源中：

- `a17` 只有一个正 writer：scene93 op904 的 `a17 += 1`（任务 0003 完成）；
- `a300` 只有一个 writer：scene93 op87 的 `a300 += 1`；
- `a12` 由 scene91..98 增加；scene93 op903 是任务 3 完成时的增量；
- 任务 0003 之前的正常状态是 `a12=2, a17=0, a300=0`。

scene393 的 `a12==2` 分支会设置请求 `040_淡雪0003`，但 helper 无法返回
true，因为 `a17>=2` 只能由同一个任务完成时写入。`a17>=1` overlay 也不是
修复：任务前 `a17=0` 仍然为 false，且该 overlay 已被
`audit/repairs/anemoi-overlay-rejection.json` 否决。当前没有应用任何兼容
overlay；包 manifest 的 `source_overlay` 为 null。因此阳彩任务 3+ 标记为
**SOURCE_DEADLOCK**，不是 PASS。这是固定源的限制，不是 pack 范围缺失。

## 7. 可复现性与门禁

打包器使用显式 manifest，而不是旧的启发式 common 闭包：

```text
python3 tools/anemoi_pack.py --source <固定 anemoi-rw5> \
  --scope-manifest main/anemoi_data/anemoi_scope.json \
  --layout-scale 0.52 --palette-colors 32 --jpeg-quality 24
```

受版本控制的 `main/anemoi_data/anemoi_pack.json` 记录包 hash/大小、source
commit、scope manifest hash、打包参数、字库 hash、容量、分段表、
`image_row_kinds`，以及 `scope_notes`（说明 `included_scenes`/
`scope_manifest` 是权威范围、source image key 与二进制行不是 1:1）。
`main/anemoi_data/anemoi_pack.bin` 仍是 release 产物（gitignore）。可选的
`--source-overlay` 是显式、记录 hash 的兼容机制；本次**未使用**。source 快照可能是 sparse
checkout，缺少 src/common/ui；打包器会用 git show HEAD:src/common/ui/title-anemoi.jpg
读取固定 blob，并在 manifest 的 title_sources 字段记录各图 hash；任一张
不可用时构建硬失败。标题按 source_commit 读取 title-bg.jpg、title-anemoi.jpg
和 title-eternicle.jpg。本作 gate 运行图像编码/包元数据测试、生产 C 的
1729/1739 留边、标题 gutter 与立绘 alpha 像素测试，以及独立 12 行 source
contain 参考（band RMS <= 12）。不使用边缘复制拉伸、cover、模糊或整帧 RGBA
缓冲。

### 二进制审计

`tools/anemoi_binary_audit.py` 直接读取 source story JSON、scope manifest
和二进制，不 import 打包器的 `Source`/`load_scope_manifest`。它强制 pack
场景集合与 scope 集合相等、每个 scene 的范围长度与 source op 顺序精确一致，
且 `checked == scope total == pack op count`；同时核对每个 static
`if`/`goto`/`call` 重定位和每个 static 跨场景 entry 引用与 entry 表。
仅允许 2 个 chapter-head 越界 return 和 1 个 `input_update` -> `ARRIVE`
poll 的明确语义特例；3 个 dynamic jump entry 只计数，不伪称静态穷尽。

当前 pack 报告 80 场景、61,257/61,257 ops、15,620 个 static branch target、
333 个 static 跨场景 entry 引用。333 是引用检查，不是 641 行 ENTRY 表的全部
条目。`tests/test_anemoi_binary_audit.py` 增加 6 个负例：范围少一 op、
pack scene 缺失/多出、同 kind 跳转目标改值、跨场景 entry 值错误、scope
source commit 错误、scope 范围越界；每个都必须以对应的 coverage/target/scope
错误非零失败。

本候选的门禁结果（完整输出在 `audit/repairs/`）：

| 命令 | 结果 | 日志 |
| --- | --- | --- |
| `ANEMOI_SOURCE=... bash tools/validate.sh --static` | PASS | `audit/repairs/anemoi-device-hotfix/logs/static-final.log` |
| `ANEMOI_SOURCE=... python3 tests/test_anemoi_scope.py` | PASS | `audit/repairs/anemoi-device-hotfix/logs/scope-final.log` |
| `python3 tools/gal_font.py --check --sizes 16` | PASS | `build/anemoi-gate-static/font.log` |
| `ANEMOI_SOURCE=... bash tools/anemoi_gate.sh build/anemoi-gate-final` | PASS (31/0/0) | `audit/repairs/anemoi-device-hotfix/logs/gate-final.log` |
| `python3 tools/anemoi_binary_audit.py --source ... --baseline-pack ...` | PASS (61,257/61,257; 15,620 targets; 333 refs) | `audit/repairs/anemoi-device-hotfix/logs/binary-audit-final.log` |
| `python3 tests/test_anemoi_image_encoder.py` | PASS | `audit/repairs/anemoi-device-hotfix/logs/image-encoder-final.log` |
| production-C pixel ref (1729/1739/title/alpha) | PASS | `audit/repairs/anemoi-device-hotfix/logs/pixel-ref-final.log` |
| `python3 hotfix_scene_reference.py` (12 contain rows, band RMS <= 12) | PASS | `audit/repairs/anemoi-device-hotfix/logs/scene-reference-final.log` |
| tracked app fake-NVS title persistence (END/flush/reload, levels 0/2/4, history, write retry) | PASS | `audit/repairs/anemoi-device-hotfix/logs/title-persist-final.log` |
| `python3 tests/test_anemoi_binary_audit.py` | PASS (positive + 6 negatives) | `audit/repairs/anemoi-device-hotfix/logs/binary-audit-negatives-final.log` |
| `idf.py -B build-hotfix build` | PASS | `audit/repairs/anemoi-device-hotfix/logs/build-title-persist.log` |
| `idf.py -B build-hotfix merge-bin` | PASS | `audit/repairs/anemoi-device-hotfix/logs/merge-title-persist.log` |
| `python3 tools/verify_firmware.py build-hotfix` | PASS | `audit/repairs/anemoi-device-hotfix/logs/verify-title-persist.log` |
| `python3 tools/archive_firmware.py create build-hotfix --archive-root build/firmware` | PASS | `audit/repairs/anemoi-device-hotfix/logs/archive-title-persist.log` |
| `python3 tools/archive_firmware.py verify build/firmware/381704e7...` | PASS | `audit/repairs/anemoi-device-hotfix/logs/archive-title-persist.log` |
| 82-item 翻译后生产 C 重放 | PASS | `audit/repairs/anemoi-normal-items-translated/receipt.json` |

真机测试为 **NOT RUN**：未进行 USB/烧录操作；运行时 free/largest block 和
帧时间未在硬件上测量。

共享 static 链现在会直接调用 `tools/anemoi_gate.sh`。未设置
`ANEMOI_SOURCE` 或缺少 pack 都是硬失败，不是 skip。Darwin 上 host-only
demo runtime 测试使用原生 `-dead_strip`，替代 GNU `--gc-sections`。

## 8. RAM/heap 预算（最终 MAP）

flash 余量不是 RAM。最终 ESP32-C3 MAP/ELF 报告：

- `.dram0.data` 7,300 B；`.dram0.bss` 240,976 B。
- 静态 bss 包括 canvas 115,200 B、LVGL pool 65,536 B、应用状态 32,832 B、
  block buffer 8,224 B、tinfl 状态 10,992 B，以及 palette/行缓冲/LVGL global
  2,208 B；其余 bss 约 5,984 B。
- `_data_start = 0x3fc90e00`，`_heap_start = 0x3fccd7e0`；静态数据保留区
  248,288 B。
- Heap 区 A（`_heap_start..APP_USABLE_DRAM_END 0x3fcdc710`）为 61,232 B。
- Heap 区 B（`0x3fcdc710..SOC_DIRAM_DRAM_HIGH 0x3fce0000`）为 14,576 B；
  ROM 保留尾区在调度器启动后才并入。启动时 heap 合计 75,808 B（A+B）。
- 已命名动态预留：LVGL draw buffer 19,200 B（240×40×2）、LVGL task stack
  7,168 B、input task 6,144 B（分配失败时回退 4,096 B）、main task 8,192 B、
timer task 3,584 B、event task 2,304 B = 46,592 B。
- 源码锚点：draw buffer `components/bsp/src/bsp_display_lvgl.c:97`；
  LVGL task `managed_components/espressif__esp_lvgl_port/include/esp_lvgl_port.h:68`；
  input task `main/main.c:283-288`；main stack `sdkconfig.defaults:38`；
  timer/event `build-hotfix/config/sdkconfig.h:488,512`；
  `APP_USABLE_DRAM_END` `components/heap/port/esp32c3/memory_layout.c`；
  ROM stack `components/soc/esp32c3/include/soc/soc.h:205-206`。
- 扣除后到 LVGL 对象/NVS/驱动/碎片之前：A 区余 14,640 B；A+B 余 29,216 B。
  这是静态链接预算，不是运行期实测；真机 free/largest block 与帧时间
  NOT RUN。

## 9. 已知缺口

1. 阳彩任务 3+ 是 source deadlock，不声称自然完成。
2. 场景 205/206/262 和 193 个路线章节不在现有原型内；边界页是预期行为。
3. Spica/小詠/六花的证据覆盖已打包的资格/play 范围和路线边界，不是未导入
   整本的完整结局。
4. 本卷不编译 20px 字体变体。
5. 本修补候选的真机验收 NOT RUN。已发布的 v1.0.1 在真机上 FAILED：标题图
   缺失、部分立绘背景不透明且为黑色、ef_avan_bg01 背景左侧出现拉伸条纹。
   本候选已实现三项修复并通过 host 像素门禁；必须重新烧录并复测后才能
   写 Device PASS。
