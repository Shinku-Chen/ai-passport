<p align="right">
  <strong>简体中文</strong> · <a href="galgod.md">English</a>
</p>

# 《想成为Galgame领域大神！！！》AI Passport 移植版

把「诺提拉观察所」的《想成为Galgame领域大神！！！》完整离线移植到 FoloToy AI
Passport，**横屏 320×240**，基于 `feature/galgame-framework` 基线（`3c2e5013`）。

- 分支：`feature/port-galgod-landscape`
- 剧本与命名来源：`hrk666666/GalGod-MiBand-10` @ `ebe085d2`
- 美术来源：`mcpotato1123/galgod-band` v2.0.0（672×480 背景、854×480 CG、
  143×380 立绘）。手环仓自己的 336×480 素材是裁过的，**不用**它做画面。
- 授权：代码 GPL-3.0；**剧本与全部美术不在该协议范围内**，版权归原作者，
  本移植仅作非商业个人转换。

## 已实现

| 部分 | 状态 |
| --- | --- |
| 剧情 | 29 个源 chunk 的全部 5,457 句 / 119,410 字 |
| 章节 | 18 个章节卡、章节解锁、章节一览 |
| 分支 | 15 组选项 / 53 项，含唯一的条件选项 |
| 答题与计数 | 13 道题，使用真正的整数计数（`bad_end_flag >= 7`） |
| 结局 | 记录 4 个结局（Bad End、二人的世界、回归日常、—— 全剧终 ——），后日谈由 `true_end_finish` 解锁 |
| 跳转 | 71 个跳转全部是运行时 goto，含 **3 处回跳**；不展开、不跳过 |
| 立绘 | **55 张全部保留**；左/中/右三个槽位，源脚本里双人同屏的行按双槽位渲染 |
| 背景 | 38 个文件，按内容去重后 36 张 |
| CG / SD | 34 个引用（15 事件 CG + 19 SD），去重后 33 张；整幅留边不裁两侧；鉴赏室 5 页 × 8 格 |
| 阅读 | 打字机、分页（16px 每行 19 字 × 4 行；20px 每行 15 字 × 3 行）、自动、按住 UP 快进、历史 64 句、存档预览 |
| 存档 | 3 个手动槽 + 1 个自动槽：位置、章节、变量、解锁位图、预览；带版本与 CRC；覆盖前二次确认 |
| 全局解锁 | 独立的版本化 NVS 记录（CRC），读旧档只合并不回退；首次解锁即节流落盘，失败退避重试 |
| 失败处理 | CG 必须先解码成功才解锁；解码失败保留上一帧、提示可见并在下次翻页重试 |
| 设置 | 文字速度、字号（16/20px）、自动间隔、对话框透明度 |

没有音频：手环源本身就没有 BGM/语音/音效节点，资源包里也没有音频段，本移植不臆造。

## 按键（三键）

| 页面 | UP | DOWN | OK |
| --- | --- | --- | --- |
| 正文 | 短按下一句（打字中先整页显示）；**长按快进**（松开停） | 历史文本 | 菜单 |
| 选项 | 上一项 | 下一项 | 确认 |
| 列表（菜单/存读档/设置/鉴赏入口/结局） | 上一行 | 下一行 | 执行；**长按返回** |
| 历史 / 章节 | 上滚 | 下滚 | 返回 |
| 鉴赏 | 上一格（可跨页） | 下一格 | 全屏查看；**长按返回** |
| CG 浏览 | 上一张已解锁 | 下一张已解锁 | 返回 |
| 结局 | — | — | 回标题 |

## 内容管线

```
python tools/galgod_pack.py --script <band-src/common/scn> \
    --images <upstream-src/common/img> --out main/gal_data/gal_pack.bin \
    --manifest build/pack-manifest.json
python tools/galgod_font.py --pack main/gal_data/gal_pack.bin \
    --font <NotoSansSC-Regular.ttf> --out-dir assets/fonts
python tools/galgod_verify.py main/gal_data/gal_pack.bin
python tools/galgod_audit.py --script <band-src/common/scn> --pack main/gal_data/gal_pack.bin
```

`galgod_verify.py` 校验包内全部交叉引用；`galgod_audit.py` **不 import 转换器**，
自己重新解析 29 个源 chunk，并分别实现"源解释器"和"包解释器"对照：台词顺序与
说话人、每个选项的落点 witness、71 个跳转（含跨块清立绘标记）、立绘槽位与图片
别名、章节顺序。

### 资源包格式（`GALPK001`，小端）

| 段 | 内容 |
| --- | --- |
| TEXT | UTF-8 文本块，每个字符串 NUL 结尾并去重 |
| NAME / LINE / CHOICE / OPTION / ACTION / COND | 定长记录 |
| EVENT | `{kind, a, b, c, d}`：LINE、BG、CG、SPRITE、CHOICE、GOTO、VAR、CHAPTER、ENDING、END、UNLOCK_END |
| IMAGE | 36 字节记录：上下两半 JPEG + 可选 1bpp 遮罩 |
| CHAPTER / ENDING / GALLERY / TITLE / META | 表与来源信息 |

图片：背景等比裁到 320×240，拆成 320×150 上半 + **160×45** 条带（设备端放大 2 倍
画在文本框背后）；CG 按宽度缩放成 320×180 并上下留黑，**完整画面不裁两侧**；立绘缩到
240 高（宽 90），同样拆两半并带 1bpp 透明遮罩。全部是 baseline 4:2:0 JPEG，由
`esp_jpeg`（TJpgDec）解到调用方缓冲 —— 不做整帧 RGBA 解码，不依赖 PSRAM。

## 固件体积

| 项 | 字节 |
| --- | ---: |
| app 二进制 | 3,284,048 |
| `factory` 分区 | 8,323,072（占用 39.4%） |
| 合并镜像 `full.bin` | 3,349,584 |
| 资源包 | 1,767,140（124 张图共 1,306,052） |
| 静态画面缓冲 | 96,000 + 14,400 + 28,800 |
| 链接期 DRAM | 227,774 / 321,296（70.9%），约 93KB 留给堆 |

## RAM 预算（来自同一次构建的 MAP，尚无真机实测）

| 项 | 字节 | 依据 |
| --- | ---: | --- |
| DRAM 区 `dram0_0_seg` | 321,296 | MAP：起点 `0x3fc80000`、长度 `0x4e710` |
| 静态 `.data` + `.bss` + IRAM | 227,774 | `idf.py size` |
| 链接期堆跨度（保守区间） | 93,088 | MAP：`_heap_start = 0x3fcb7b70`，段末 `0x3fcce710` |
| BSP 刷屏缓冲 240×24×2 | 11,520 | 堆，`bsp_lvgl_init()` 分配 |
| LVGL 任务栈 | 7,168 | `ESP_LVGL_PORT_INIT_CONFIG()` 默认 |
| 输入任务栈 | 4,096 | `main.c` |
| `esp_timer` 任务栈 | 3,584 | `CONFIG_ESP_TIMER_TASK_STACK_SIZE` |
| 系统事件任务栈 | 2,304 | `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` |
| LVGL 对象 + 标签文本（估） | 15,000–25,000 | 页面/列表/历史行 |
| **启动后估算剩余** | **约 35–45 KB** | 仅为算术估算 |
| 真机水位 | **NOT RUN** | 需上机 `heap_caps_get_free_size` |

93,088 B 是本次链接在 `_heap_start` 与 `dram0_0_seg` 末尾之间留下的**保守预算区间**，
既不是实测堆，也不是分配器唯一的来源区域（驱动分配、可回收缓冲、碎片都由运行时决定）。
SOC 名义上界 `0x3fce0000`（约 165KB）不在本次链接的段内，不要按 400KB、165KB 或
链接器余量直接当成可用堆做预算。

## 构建与校验

```bash
# Windows（维护入口，不依赖 build/ 下的临时脚本）
powershell -File tools/idf.ps1 -B build build
powershell -File tools/gate-firmware.ps1        # 构建 + merge + verify + archive

# 宿主门禁（本机无系统 cc，tools/cc-host.sh 包了 Zig clang）
CC="$(pwd)/tools/cc-host.sh" GALGOD_SOURCE=<band-src/common/scn> ./tools/validate.sh --static
```

Windows 上 Git Bash 里跑不了固件门禁（ESP-IDF 拒绝 MSYS），改用
`tools/gate-firmware.ps1` 原生执行同样的步骤：在全新的 `build/firmware-gate/run-*`
目录里干净构建、merge、`verify_firmware.py`、`archive_firmware.py`；不删除任何目录，
每次运行都留下日志与产物。

宿主测试覆盖：分页（`test_gal_text`）、包结构（`test_gal_pack`）、虚拟机语义
（`test_gal_model`）、真实包全路线且走 UI 同一入口（`test_gal_routes`：405 状态 /
329 个选项停点 / 53 个选项 / 结局掩码 0xF）、画面合成（`test_gal_image`，JPEG/LVGL
薄桩）、存档与设置（`test_gal_store`，内存 NVS + 故障注入）、坏包（`test_gal_badpack`）、
真实 app+ui 按键流程（`test_gal_app`：选择点开菜单、选择点存读档、toast 可见性、
历史行布局、快进取消）。

## 未验证 / 已知限制

- 尚未上真机（本任务不刷机）：显示时序、JPEG 解码耗时、按键手感仍需设备验收。
- 对话框盖住画面底部 90 px；CG 采用留边而非裁切，背景采用等比裁切（保留 95.2%）。
- 支持左/中/右三个立绘槽位，同一行最多两个角色（源脚本也没有三人的行）。
- 脚本从未引用的 2 张立绘与 1 张背景仍然打包（保证素材完整），所以包内有 3 张未引用图。

## 交付状态：冻结（软件审核通过，待设备验收）

**状态：软件审核通过，待设备验收（Device tests: NOT RUN）。**

| 项 | 值 |
| --- | --- |
| app 二进制 | 3,284,048 B — `cc8e350d300109c39551f8b975a14373a828e1cf869d43587d6b47b7a57486d9` |
| 合并镜像 `full.bin` | 3,349,584 B — `f2fe13166b9bc86321bb72036c46b63c46edce776f86a8533294c0532706ef0e` |
| ELF / MAP | `build/firmware-gate/run-20261102-040848/FoloToy-AI-Passport.{elf,map}` |
| 同次归档 | `build/firmware/f2fe13166b9bc86321bb72036c46b63c46edce776f86a8533294c0532706ef0e/` |
| Lead 审核日志 | `build/lead-review-r3/probe_app.log`、`probe_image.log`、`archive-verify.log` |

本版本已冻结：不再改生产代码、配置与资源。“软件审核通过”不等于真机堆、显示时序或
NVS 行为已验证，这些仍是待设备验收项。旧归档（`3a2bc8a9…`、`8fe3b642…`、
`bc92e302…`、`b48f4633…`、`98170bb3…`）与全部 review 探针、日志均保留。
