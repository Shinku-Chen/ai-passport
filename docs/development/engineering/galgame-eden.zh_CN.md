<p align="right">
  <strong>简体中文</strong> · <a href="galgame-eden.md">English</a>
</p>

# eden* 横屏移植 —— 实现说明

本分支（`feature/port-eden-landscape`）把 galgame 框架做成一台完整的、离线的《eden*》阅读器，
跑在 320×240 横屏面板上。它沿用框架的排版逻辑（`main/gal/gal_text.c`）与显示设置
（`main/gal/gal_settings.c`），其余与作品相关的部分全部重写：内容、剧本执行、画面解码、持久化与界面。

## 1. 范围

| 项 | 值 |
| --- | --- |
| 来源 | `xuanyide01/eden-rw5`（`src/common/{assets.js,story,reports}`、`src/common/images/*.jpg`） |
| 章节 / 场景 | 21 / 21 |
| 源 chunk | 21 个场景共 28 个文件;场景 4/5/6/7/11/13/14 各多一个 `1.txt` 续块 |
| 剧本指令 | 8,931 条（8,860 `text`、25 `if`、5 `set`、13 `jumpScene`、2 `goto`、5 `end`） |
| 合成画面 | 2,178 个画面，来自 2,667 张 JPEG（1,115 底图 + 1,594 条补丁片段） |
| 画面几何 | 264×165（16:10）1:1 显示；源移植版把 1024×640 原图等比缩到 0.258× |
| CG 鉴赏 | 295 组，每页 6 条分页，按组记解锁位（预留 320 位） |
| 正文 | 包内 490,042 字节 UTF-8；对白里 2,579 个不同码位 |
| 玩家选项 | 无 —— 原作是纯剧情推进；选项页保留但没有指令会触发 |

## 2. 分层

```
main/eden_app.c    状态机:页面、按键语义、历史、存档、解锁、自动/快进
main/eden_ui.c     横屏界面:15 个页面、行列表、打字机、提示、弹窗
main/eden_image.c  画面合成:底图 JPEG 直接解进画布,补丁 JPEG 解进暂存条再逐行贴
main/eden_script.c 纯逻辑指令引擎(text/set/if/goto/jumpScene/end),可宿主机测试
main/eden_pack.c   只读资源包解析,可宿主机测试
main/eden_store.c  NVS:3 个存档位、阅读进度、鉴赏解锁(带版本号的 blob)
main/gal/gal_text.c      复用:分页
main/gal/gal_settings.c  复用:文字速度 / 字号 / 自动阅读 / 对话框透明度
```

`main/gal/gal_app.c`、`gal_ui.c`、`gal_content.c`、`gal_save.c`、`gal_progress.c`、`gal_unlock.c`
仍留在仓库里(它们是框架骨架),但**不参与**本应用的编译;构建清单在 `main/CMakeLists.txt`。

## 3. 资源包格式（`EDENPK01`）

单个小端二进制,直接从 Flash 映射读取,固件不把它载入 RAM。

| 段 | 内容 |
| --- | --- |
| `TEXT` / `TXIDX` | NUL 结尾的 UTF-8 字符串 + `u32` 偏移表 |
| `OPS` | 12 字节记录 `{kind u8, a u8, b u16, c u32, d u32}` |
| `SCENE` | 每个场景号(1 起)的指令区间 `u32[start, end]` |
| `IMAGE` | 16 字节记录 `{off u32, len u32, w u16, h u16, content_w u16, content_h u16}` |
| `FRAME` | 6 字节记录 `{base u16, patch_first u16, patch_count u8, reserved u8}` |
| `PATCH` | 6 字节记录 `{image u16, x u16, y u16}` |
| `CHAP` | 8 字节记录 `{scene u16, name u16, op u16, reserved u16}` |
| `GAL` | 6 字节记录 `{frame u16, group u16, label u16}` |
| `BLOB` | JPEG 载荷 |

入库的 JPEG 在宽高两个方向都补齐到 8 的整数倍(TJpgDec 按整 MCU 出块),解析层同时给出
"存储尺寸"(算缓冲)与"内容尺寸"(贴图用)。宽于 104px 的补丁由打包器按列切开,保证解码暂存条很窄。

## 4. 内存预算

| 用途 | 字节 |
| --- | --- |
| 画面画布(264×168 RGB565) | 88,704 |
| 补丁暂存条(104×168 RGB565) | 34,944 |
| LVGL 堆(`CONFIG_LV_MEM_SIZE_KILOBYTES=64`) | 65,536 |
| LVGL 绘制缓冲(320×40 RGB565) | 25,600 |
| JPEG 工作缓冲 | 4,096 |

底图由 `esp_jpeg` 直接解进画布;补丁解进暂存条后逐行贴到画布。没有整帧 RGBA 缓冲、没有逐图缓存、
没有预合成的表情组合矩阵。

## 5. Flash 预算（本次构建）

| 项 | 字节 |
| --- | --- |
| 资源包(JPEG q27) | 6,189,252 |
| 标题徽标(RGB565A8) | 32,670 |
| 16px + 20px 字体(4bpp,2,681 码位) | 871,958(322,521+5,172+21,456 / 493,767+5,172+21,456) |
| 应用镜像 | **7,783,760**(app 分区 8,323,072,余 539,312) |
| 合并镜像 | **7,849,296**(上限 8,388,608) |

JPEG 用 q27 而不是 q30：完整剧本下包里有 2,667 张图，实测总量 q25 5.71 MB / q27 6.19 MB /
q28 6.59 MB / q30 7.80 MB —— q30 单独就要多占 1.55 MB，分区放不下。对源 tile 实测二次编码质量：
PSNR 均值 30.69 dB(最差 26.91 dB)、MAD 5.37，74 帧抽样(源本身已是 q56、0.258× 二次压缩)。

## 5b. 内部 RAM 预算(无 PSRAM)

下面的数字来自本次构建的 MAP(`_bss_start=0x3fc92ad0`、`_bss_end=0x3fcc5718`、
`_heap_start=0x3fcc5720`)与 `sdkconfig`。这是**静态账**，不是实测水位:真机 high-water 仍未验证。

| 占用方 | 字节 | 位置 |
| --- | --- | --- |
| 画面缓冲 264×168 RGB565 | 88,704 | `.bss`(`s_canvas_pixels`) |
| 补丁暂存条 104×168 RGB565 | 34,944 | `.bss`(`s_patch_pixels`) |
| JPEG 工作缓冲 | 4,096 | `.bss`(`s_work_buf`) |
| LVGL 堆(`CONFIG_LV_MEM_SIZE=65536`) | 65,536 | `.bss` |
| 其余静态 BSS(IDF、LVGL 库、驱动、应用状态) | 14,664 | `.bss` |
| **`.bss` 合计** | **207,944** | MAP |
| LVGL 刷屏缓冲 240×40 RGB565(端口在 init 时分配) | 19,200 | 堆 |
| 输入任务栈 | 4,096 | 堆 |
| LVGL 任务栈(`ESP_LVGL_PORT_INIT_CONFIG`) | 7,168 | 堆 |
| main / timer / idle / event 任务栈合计 | 约 10,200 | 堆 |
| JPEG 解码瞬时 | 约 0(复用 4 KB 工作缓冲) | — |

`.bss` 之后内部 SRAM 尾部:`0x3FCE0000 - 0x3FCC5720 = 108,768 B`(乐观上界:假定 `.bss_end`
到 DRAM 顶端全部可用)。扣掉上面的堆分配后约剩 **68 KB** 给 LVGL 对象、无网络的协议栈缓冲与栈保护余量
—— 够当前界面(约 120 个对象),但**不够再放一张整帧画面缓冲**。不要把"400 KB SRAM"当成预算,
`.bss` 加这些堆项才是真实约束。

## 6. 持久化

| 命名空间 | 键 | 内容 |
| --- | --- | --- |
| `edensave` | `slots` | 3 个槽位:场景、指令下标、页、章节、剧情变量、预览文字 |
| `edenprog` | `pos` | 自动阅读位置(节流为每 2 秒最多写一次) |
| `edenulck` | `bits` | 鉴赏组(320 位)、章节(21 位)、结局 |
| `galcfg` | `cfg` | 框架显示设置(文字速度、字号、自动阅读、透明度) |

每个 blob 都以 `u16` 格式版本开头;版本不认识时按"没有记录"处理,不做半截解析。

## 7. 操作

| 页面 | 短按 | 长按 |
| --- | --- | --- |
| 启动页 | 确定进入标题 | — |
| 标题 / 任意列表 | 上 / 下移动,确定确认 | 长按确定返回 |
| 正文 | 上推进(打字中先补全,再翻页),下开历史,确定开菜单 | 长按上快进,松手停 |
| 设置 | 上 / 下移动,确定切换取值 | 长按确定返回 |
| 存档位 | 确定保存(槽位已占用时需再按一次) | 长按确定返回 |
| 读档位 | 确定读取 | 长按确定返回 |

## 8. 验证

```bash
python tools/eden_pack.py --source <eden-rw5> --out main/eden_data/eden_pack.bin   # 重新打包
python3 tests/test_eden_artifacts.py                                               # 包/剧本/解码 gate
./tools/validate.sh --static      # 仓库检查 + 宿主机测试
./tools/validate.sh --firmware    # ESP-IDF 构建 + 合并镜像验证
```

`tests/test_eden_artifacts.py` 用独立实现重新读取生成的包,检查剧本(每条指令、每个跳转目标、
8,860 句的完整跑通)、画面几何、18 个抽样画面的解码对照(对源仓库 tile 逐像素比对)、
每个码位的字体覆盖、分页容量与 Flash 预算。`tests/test_eden_pack.c` 是同一套契约的 C 版本,
给有 C 编译器的宿主机用。

`tests/test_eden_source_audit.py` 是源⇄等价 gate:它用自己的一套读取逻辑(不复用打包器的
`Source` 类)重新遍历源仓库的**每个 chunk**,把 8,931 条指令逐条比对 —— 种类、正文、说话人、
画面 id,以及从"场景内相对"换算成"绝对"的跳转目标;并单独钉住 7 个双块场景:接缝指令必须
读得到,落在第二块里的每个跳转目标必须解析到同一个绝对指令。

`tests/test_eden_app.c` 用 `tests/app_stubs`(LVGL 对象模型、NVS、JPEG、BSP 的替身)编译**生产代码**
`eden_app.c` / `eden_ui.c` / `eden_store_nvs.c`,并真的灌按键事件。它钉住只有应用层才看得见的路径:

- 页面来源:标题→设置、标题→读档都回到标题,正文→菜单→设置回到菜单(栈语义,不是写死目标);
- 覆盖写失败:注入 `nvs_commit` 失败后,RAM 槽位保留旧档预览,旧档仍能立即读出;
- 解码失败:坏补丁既不解锁 CG,同一帧重渲染也不解锁;故障解除、成功解码后才解锁;
- 快进:短按只推进一句;长按后按其它键 / 换页 / 松手都会取消;
- LVGL 锁:未持 `bsp_lvgl_lock()` 时不会发生任何 UI 写操作;
- 解锁落盘:写失败后退避重试而不是每 tick 刷 flash;标题 / 鉴赏 / CG 列表 / 结局页(没有正文)同样会重试,
  恢复后一次写入成功,并能从记录重新读回;
- CG 导航:列表内移动不压栈、关闭大图回到列表、列表再一次长按回到鉴赏入口。

NVS 替身现在把 `nvs_set_blob` 写进待提交区、只有 `nvs_commit` 成功才落到已提交区,于是"提交失败"在测试里
真的等于"flash 里还是旧记录"。但它仍然只是**替身**:证明的是应用/存储层的契约(RAM 保留旧槽、不报成功、
重新加载读到旧记录),不是真机断电后的 flash 持久性。

`tests/test_eden_image.c` 用 JPEG 替身跑真实 `eden_image.c`,验证失败语义:补丁失败回退到完整底图、
底图失败清黑、越界补丁与头尺寸不符在碰画布之前被拒。

宿主机 C 测试在没有系统编译器时走 `tools/cc-zig.sh`(`pip install ziglang`);
gate 会输出 passed/failed/skipped 计数,只要有 SKIP 就打印 `Host tests: PARTIAL`,
绝不会一边跳过一边报 PASS。

## 9. 已知限制

- 原作没有玩家选项:选项页存在但不可达。
- 没有音乐:源仓库只有曲目名,没有音频数据。
- CG 鉴赏改为"分页列表 + 全屏查看",不做缩略图墙(6 张解码缩略图约 64 KB,本板没有 PSRAM)。
- 镜头平移(`reports/camera-commands.json`,24 条)在源移植版里已经落成离散的机位帧,阅读器显示
  这些帧,不重放平移动画。
- 真机渲染、按键时序与 NVS 行为**尚未验证**;Build / Host tests / Device tests / Unverified
  四项见交付报告。

## 10. 交付状态 —— 软件审核通过，待设备验收

本版本为**冻结版**,等待设备验收。本次说明没有改生产代码、没有重打包、没有重建固件;
下面的哈希就是审核过的那次构建。

| 项 | 值 |
| --- | --- |
| 应用镜像 | 7,783,760 B,SHA256 `5b39da679672375120e5c2f4351f12144989820b903ac795afc72e2e2947ff9b` |
| 合并镜像 | 7,849,296 B,SHA256 `a8922b1b8a105bafd8425b608028544cbaf0607820e0fc0180ab25f1004d179d` |
| 同次归档 | `build/firmware/a8922b1b8a105bafd8425b608028544cbaf0607820e0fc0180ab25f1004d179d/` |

Lead 侧证据(在本项目内,Worker 未改动):

- `build/lead-review-v4/static.log` —— Lead 独立跑 `tools/validate.sh --static`:42 passed /
  0 failed / 2 skipped,Host tests PARTIAL。
- `build/lead-review-v4/archive-verify.log` —— Lead 独立复核归档,PASS。
- `build/lead-review-v4/probe_app.c` + `probe_app.log` —— 断言校正后的 C 探针(真实
  app/UI/image/store/adapter 路径):维护 105 项 + 新增 11 项 = 116 checks,0 failures。

`build/lead-review-v3/` 里两条旧断言作废(用 ASCII `????` 匹配全角 `？？？` 标签;另有一条断言与
它自己"返回鉴赏入口"的文案矛盾)。原始证据保留,勘误记在 Lead 的审核记录里。

**本轮没有重复干净目录重建**:被接受的哈希来自审核过的 `idf.py -B build` 构建加
`merge-bin` / `verify_firmware.py` / `archive_firmware.py`;可复现性证据是第二轮那次干净目录
重建与交付版尺寸一致。

Device tests:**NOT RUN**。横屏方向、两档字号的实际分页与缺字、BSP 按键时序、真机 RAM 水位、
JPEG 解码耗时、断电后的 NVS 持久性、深睡唤醒、电量显示都还待设备验收;§5b 的静态 RAM 账
不作为通过结论。
