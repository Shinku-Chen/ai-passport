<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 星光咖啡馆与死神之蝶 · AI Passport 离线阅读器

《星光咖啡馆与死神之蝶》（Café Stella and the Reaper's Butterflies）面向 **FoloToy AI Passport**（ESP32-C3、8 MB Flash、无 PSRAM、320×240 横屏）的完整离线移植候选。固件基于 `feature/galgame-framework` 的页面/按键/持久化约定重写内容与阅读层，不是演示或摘要版。

> 当前状态：完整离线阅读版；已在设备上完成限定范围验收（鉴赏室 OP 清晰/无遮挡/返回、普通章节、正文图文、快进松手停止）。不声称全路线、长稳或掉电场景通过；完整音视频与源码已保留。

## 固定输入

| 项 | 值 |
| --- | --- |
| 上游仓库 | `https://github.com/xuanyide01/cafestella-rw5` |
| 固定提交 | `90ddb9e020970907b61ad4181f25c65267e6e98b` |
| 本地只读来源 | `C:/tmp/ai-passport-six-20261123-79f0/sources/cafestella-rw5` |
| 目标固件基线 | `Shinku-Chen/ai-passport` `feature/galgame-framework` `b9909c13605413142fb35f28aabcc494a666fa00` |

源数据以仓库内已加工的 `converted/graph.json`、`converted/n*.json` 与 `src/common/engine.js` 为权威语义；不按线性事件顺序重新拼接分支。

## 收录范围（逐项核对）

| 内容 | 数量 |
| --- | ---: |
| 剧情节点 | 923 |
| 正文帧 | 53,727（另有 1 个 OP 标记） |
| 选项组 / 选项 | 69 / 141 |
| 主线结局 | 7（bad / normal / 栞那 / 夏目 / 希 / 爱衣 / 凉音） |
| 后日谈 | 5（栞那 / 夏目 / 希 / 爱衣 / 凉音） |
| 章节条目 | 189 |
| 鉴赏 CG 组 / 差分 | 121 / 1,631 |
| 源图文件 | 1,850（1,729 背景 + 121 立绘底图；另 140 张表情图集） |

所有选项、条件、路线、结局与通关后门控都由源 `engine.js` 的 `locate/choose/flags/matches/complete` 语义驱动；`when` 条件（含 `checkAnyClear`、`kan_flag` 的源默认 0）与 `flagRules` 派生值逐条保留。源 `kan_flag == 3` 分支在源图里永远不可达，移植保持该不可达性，不擅自“修复”剧情。

## 操作（320×240 横屏，三键）

| 页面 | 短按 OK / UP / DOWN | 长按 |
| --- | --- | --- |
| 启动 | 短按 OK 进入标题 | — |
| 标题 | 上/下移动，OK 进入 | — |
| 阅读 | 三键均为下一句/下一分页；打字/自动中直接推进，不要求先补全 | 长 OK 游戏内菜单；长 UP 快进；长 DOWN 自动阅读 |
| 选项 | 上/下选择，OK 确认 | 长 OK 返回对话（不替玩家选择） |
| 菜单/设置/存读档/鉴赏/章节 | 上/下移动，OK 执行 | 长 OK 返回 |
| 历史/关于 | 上/下滚动 | 长 OK 返回 |
| OP / 过场 / 结局 | OP：短按跳过；过场/结局：短按继续/回标题 | OP 长按不选择 |

- 短按三键在打字/自动中都**立即推进**：长句到下一分页，否则下一句；不会只补全当前页或只退出自动而吞掉这次按键。
- 自动阅读中手动短按会**退出自动并立即推进**（跳过打字/句间等待）。
- 长按 UP 跳过打字和自动等待持续快进；松开立即停止；到选项、过场或结局停住，不替玩家选择。
- 长按只触发对应长按动作，松手不再补一次短按；进入/退出菜单、选项、自动、快进时清理互斥状态。

## 画面与资源

- 源舞台 432×243，打包为 **224×126**（比例严格 1:1.777），运行时三条 **224×42 RGB565** 条带由 LVGL 放大到 **320×180**。
- 背景 JPEG q10；标题 7 张背景单独 q18；立绘/表情图集 q18 + **2 bpp alpha mask**（4 级），透明区 RGB 用最近不透明边缘填充、立绘/图集用 4:4:4，保留透明、差分、表情锚点与舞台位置；无整幅 RGB565 画布。
- 鉴赏 CG 直接复用同一批背景图；缩略图 32×18 解码后 2× 显示。
- 中文字库由源 OTF 生成 16 px / 20 px 两个 4 bpp 子集，覆盖**实际 pack 内全部剧情/UI 字符**（3,401 码位；`tools/gal_font.py --check` 0 缺字、0 空字形）；不依赖系统默认字库。
- 实际 UI 软件显示 smoke（真实生产 `cafe_ui.c` + LVGL 9.5，320×240，64 KB 池）覆盖标题、16/20px 长句、菜单、存读档、CG/章节、历史/回想、OP 首/中帧、音乐、音频错误、设置和 CJK 错误屏，共 30 屏；输出 `contact-sheet.png`。64-bit 宿主机：总 58,120 B、结束余 11,376 B、峰值 48,832 B、碎片 23。32-bit WoW64（生产 UI/字体/图像路径，音频 HAL 桩）：56 KB 配置 30 屏全过，总 53,484 B、结束余 23,096 B、最大连续块 21,576 B、峰值 32,156 B；64 KB 配置 30 屏全过，总 61,676 B、结束余 31,288 B、最大连续块 29,768 B、峰值 32,156 B。两者均为 x86 宿主机，不是 ESP32-C3 设备证据。

## 音视频实现（用户选择：完整音画）

用户已选择保留全部源音视频，按当前画面质量实现，不再以 q18/BG12/BG14 取代：

- 标题曲：真实源 `title-bgm.opus` 转 4 kHz 单声道 IMA ADPCM，载荷 516,096 B，1,027,198 samples = 256.7995 s；完整样本，不裁剪。
- OP 曲：真实源 `opening-bgm.opus` 转 8 kHz 单声道 IMA ADPCM，载荷 373,760 B，743,959 samples = 92.995 s；播放在源语义的 92.4 s pre-EOF guarded stop 停止，视频继续到 95.46 s 静音尾；**不是全采样均播放**，也不拉伸/截断文件。
- OP 视频：源 477 帧@5fps、95.460 s 映射为 287 帧 224×126 q15 @3fps，载荷 620,030 B；按 `floor(k*5/3)` 取源帧，最后一帧持有到 95.460 s。
- OP 画面区域 `y=0..179` 不再显示应用标题/提示胶囊；标题/eyebrow 对象实际隐藏，仅保留画面下方 `180..239` 的短控制提示。普通过场仍显示章节标题；源字卡不受影响。
- OP JPEG 通过生产 `cafe_image_render_op` 等比映射到完整 224×126 舞台，直接复用三条 224×42 条带（总 56,448 B）与 `s_stage`；不分配整幅 224×126/320×180 RGB565，不增加 LVGL 变换缓存。
- 音频故障先于按键/长按定时收口；触发故障的同一 press/release 被消费，重试保留原 OP 帧/时间并把音轨 seek 到对应位置；停止用 active-busy 确认，超时可观察。标题 4 kHz PCM 由音频任务有界 2× 重采样；OP 8 kHz 不做倍频；两者均为 codec/I2S 的 8 kHz、16-bit 单声道；写失败停止并进入 resource fault，不忙等。标题曲按标题/菜单/鉴赏/设置页面生命周期播放，OP 标记进入音画播放，结束或短按跳过只推进 VM 一次；OP 后恢复正文/自动状态。
- 音乐鉴赏列出标题曲与 OP 曲并可播放；设置新增声音开关与音量。OP 页面：短按跳过；播放约 95.46 s 后自动回到正文。长按 OK 不替玩家选择。
- 图像：现有剧情 1,997 张（BG q10、portrait/atlas/title q18）逐项保留；OP 额外 287 张，合计 2,284 张 image records。新增媒体不被计作剧情图像改动。

## Flash / DRAM 实际预算

| 项 | 字节 |
| --- | ---: |
| `cafe.pak`（文本 + 元数据 + 全部图像） | 6,424,952 |
| 其中图像数据（含 OP 620,030） | 4,084,761 原始 B（去重后 3,360,348） |
| 其中音频 IMA / OP JPEG（子集，不另加总） | 1,509,886（音频 889,856 + OP 620,030） |
| 其中文本 zlib 数据 | 1,694,803 |
| app 分区（factory） | 8,323,072 |
| 固件 app bin | 8,246,672 |
| 固件剩余 | 76,400（1%；此前 190 KB 估算的差约 75 KB） |
| 合并镜像（含 bootloader/分区表） | 8,312,208 |

| 交付物 | SHA-256 |
| --- | --- |
| `assets/cafe/cafe.pak` | `39130be057c33ef7252b1fd6fd8526937aa4bf4c225467513890cdd7e6ce7c46` |
| `FoloToy-AI-Passport.bin` | `d05c9a248269570f6cbcdeef409821671368aceb2f5801493ecb4683342915ac` |
| `FoloToy-AI-Passport-full.bin` | `d706c5a67a2feeb0d757d54af4ba72753f6e62e854633977e37d28b143dff7d4` |

- 静态 DRAM 使用 239,250 / 321,296 B，剩余 82,046 B；音频静态 PCM/decode 缓冲约 5.1 KB 已含在静态值内；运行时三条条带 56,448 B + 输入任务栈 4 KB，余约 21.6 KB。
- `-fstack-usage`（真实 RISC-V 目标 flags）关键链：`show_frame_page` 1,120 B，`cafe_app_key` + `handle_short` + `advance_reading` + `show_frame` 约 1.4 KB；图像链 `render_sprite_band` 304 B + `decode_jpeg` 176 B，均在 4 KB 输入栈内。
- 条带/掩码/JPEG 工作区均为有界静态或 ≤32 KB 单块；没有整幅 320×180 RGB565（115,200 B）动态分配。

## 构建

需要 ESP-IDF **5.5.3**（本机 `C:\Espressif\frameworks\esp-idf-v5.5.3`）：

```bash
source <ESP-IDF-5.5.3>/export.sh
idf.py -B build-cafe build
idf.py -B build-cafe merge-bin -o FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build-cafe
python tools/archive_firmware.py create build-cafe --archive-root build/firmware
```

资源与字体可复现：

```bash
python tools/cafe_media.py                # title 4 kHz + OP 8 kHz IMA + OP 287 帧（需 ffmpeg）
python tools/cafe_pack.py                 # 生成 assets/cafe/cafe.pak + reports
python tools/gal_font.py --font <cafestella-text.otf> \
    --out-dir assets/fonts --source-root assets/cafe
```

## 验证

```bash
CC=gcc ./tools/validate.sh --static       # 仓库检查 + 全部宿主机测试 + 字体覆盖
./tools/validate.sh --firmware            # 需已激活 ESP-IDF 5.5.3
```

分类（不是把所有命令混成一个 PASS）：`--static` 已执行 PASS；`./tools/validate.sh --firmware` 在 Git Bash 环境已执行 **FAIL (environment)**（`idf.py` 不可用，ESP-IDF 5.5.3 不支持 MSys/Mingw；日志 `reports/cafestella/logs/firmware-gate-attempt.log`）。native PowerShell 等价链 build/merge/verify/archive 分别 PASS，日志 `reports/cafestella/logs/firmware-{build,merge,verify,archive,archive-verify}.log`。Device tests **NOT RUN**。

宿主机回归包括：启动/锁/背光/输入任务顺序契约（`tests/test_cafe_lifecycle_contract.py`）、源 `engine.js` 与 C VM 的 160 条轨迹（141 选项、7 结局、5 后日谈、skip/previous；436,182 条命令逐步比对 node/index/进度/派生 flag/全部 when）、三键可控时钟状态机、保存/坏 blob、横屏应用状态机、Pillow 参考解码/合成 7 帧（0–5 立绘、表情差分）。这些是**宿主机**证据，不是设备验收。

## 版权

原作《星光咖啡馆与死神之蝶》全部剧情、美术、音乐与角色版权归 **YUZUSOFT** 所有。本仓库仅包含移植代码、转换工具与为离线阅读生成的降采样资源；请支持正版，不要公开分发原作素材。
