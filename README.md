<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 9-nine- 九次九日九重色 · AI Passport 离线阅读器

把《9-nine- 九次九日九重色》（Episode 1）完整移植到 FoloToy AI Passport：
ESP32-C3、8 MB Flash、无 PSRAM、320×240 横屏、三键操作。固件自带剧情、
立绘、表情差分、CG、标题主视觉与两首 BGM，不联网、不依赖外部文件。

本 worktree 只负责这一部作品。系列其余三部（天色、春色、雪色）不在本固件内。

## 固定输入

| 项 | 值 |
| --- | --- |
| 上游工程 | <https://github.com/xuanyide01/9-nine-rw5> |
| 固定提交 | `6a8907a5ae576f4377abe4b74f15bd9af13efe07` |
| 本作目录 | `nine-kokoiro-rw5` |
| 权威数据 | `src/common/graph.js`（已加工 graph）+ `src/common/story/**` |
| 语义来源 | `src/common/engine.js`、`src/common/chunk-codec.js` |

不重建未提交的 PC 原作脚本；以该提交里的已加工 graph、正文分片和转码素材为准。

## 内容范围

- **17 章**、**74 个剧情节点**、**8049 句正文**（另 1 条 OP 标记，源记录共 8050 条）。
- **5 组选项 / 9 个选项**：一周目跳过选项节点；二周目按 `loop_my` 出现新选项。
- **2 个结局**：一周目「轮回终点」(`loop1`) 与二周目九條都真结局 (`true`)。
- 全部正文、说话人、选项、条件、效果、有序跳转、CG 差分引用均来自源 graph；
  没有截断剧情、删路线或用占位图凑容量。
- 15 个状态变量（含 5 个永久键），不是固定 8 变量/32 位 flags 的裁剪版。

## 操作

| 场景 | 短按 | 长按 |
| --- | --- | --- |
| 阅读页 | OK / 上 / 下 **都立即推进**（跳过逐字动画；长句按分页单元前进） | OK = 游戏内菜单；上 = 按住快进；下 = 自动阅读开关 |
| 选项 | 上/下移动，OK 确认 | OK = 游戏内菜单（一次长按进入；返回恢复完整选项与选择位置） |
| 菜单 / 存读档 / 设置 / 鉴赏 / 章节 | 上/下移动，OK 执行 | OK 返回 |
| 历史 / 关于 | 上/下滚动 | OK 返回 |
| 标题 | 上/下移动，OK 进入 | — |
| 章节过场 / 结局 | 任意短按继续 / 回标题 | — |

补充约定：

- 长按只执行对应的长按动作；松手不会再触发一次短按推进。
- 阅读页或选项页一次长 OK 进入菜单；菜单/存读档/设置/历史返回时恢复原正文分页或完整选项与选择位置，不会落到空对话页。
- 自动阅读中按任意阅读键会**先停止自动、同时推进**，不会只退出自动而吞掉这次按键。
- 按住上快进时立即跳到下一句，跳过逐字显示、自动间隔和当前句剩余分页；
  每个 50 ms 控制 tick 有界推进一句（不是 busy-loop），到选项/结局/OP 停住，
  不自动替用户选择；松开立即停止，不补短按。
- 16px 档 3 行/页、20px 档 2 行/页；长句按标点禁则分页，正文不丢。

## 存档与永久进度

- **30 个手动存档栏位**（5 栏/页，可翻页），另有自动阅读进度用于「继续游戏」。
- 存档记录当前节点、句内下标、正文页、当周目变量；稀疏编码 + CRC32 + 资源包身份校验。
- **永久进度独立**：`loop_my`、`loop1`、`true`、`clear_true`、`titleBg` 存于单独记录；
  读档、回退、章节试读都不会把已通关记录回滚。
- **章节试读不污染进度**：从鉴赏室章节列表进入时使用隔离会话，不写永久键、
  不写自动进度、不解锁 CG。
- 永久解锁：22 组 CG、17 章阅读标记、2 个结局；标题主视觉按一周目/真结局解锁，
  可在设置里切换已解锁的主视觉。
- 存档/进度损坏会在屏幕上提示，并保持当前 live 状态，不会静默套用坏记录。

## 鉴赏

- **CG 鉴赏**：22 组 / 73 张差分；缩略图 8 格/页，共 3 页；全屏查看可上下切换差分。
- **章节回顾**：17 章直达试读（不写通关记录）。
- **OP 动画**：源 599 帧 / 6 fps / 99.9 s，固件以 300 帧 / 3 fps 图片序列 + 原曲播放；
  任意键跳过。
- **音乐鉴赏**：标题曲与 OP 主题曲两轨，8 kHz 单声道 IMA ADPCM，可播放/停止。

## 画面与音频实现

- 舞台 **320×180**（源 432×243 等比 16:9；内部 256×144 解码，LVGL 1.25x 显示），下方 60 px 对话框；
  舞台上方是浮动说话人名牌（半透明、贴合名字、最长 160 px、超长省略；空名隐藏），
  不压正文、不显示数字页码；
  整屏 320×240 横屏，由 BSP 的 `bsp_lvgl_set_landscape(true)` 设置。
- 背景/OP/CG：JPEG q38（441 张，1,827,874 B）。
- 立绘身体与表情裁片：8 位调色板 + alpha（225 张，446,231 B）；保留多角色合成、
  不透明度、源 `zpos/order` 层级与 256×256 表情图集裁切锚点。
- 标题主视觉 3 张 320×240 原始 RGB565（含烘焙 Logo）、CG 缩略图 22 张 96×51
  RGB565（25 张 raw，676,224 B）。
- 合成器按 4 条 256×36 条带解码（单块 **18,432 B**，合计 73,728 B），四条以 1.25x 拼成 320×180；失败时清空画布，
  不显示上一帧的陈旧画面。
- 音频 2 轨，IMA ADPCM 8 kHz 单声道：标题 85.4 s / 352,440 B，OP 99.9 s / 412,236 B。
- 字库：16 px / 20 px 两档，**2182 字形**，覆盖全部 8049 句正文、说话人、选项、
  章节/结局/鉴赏名与 UI；19 个源字体缺字用 Noto Sans SC 补齐，0 缺字。
  源字体文件不随固件提交。

## 资源与预算

| 项 | 字节 | 说明 |
| --- | ---: | --- |
| 剧情包 `nine_script.bin` | 555,087 | 74 节点、8050 帧、100 个 ≤4096 B 正文块 |
| 图像/音频包 `nine_images.bin` | 3,350,479 | 691 张图 + 2 轨 + 300 OP 帧 |
| 16px 字库（编译估算） | 297,240 | 4bpp，2182 字形 |
| 20px 字库（编译估算） | 419,393 | 4bpp，2182 字形 |
| 应用镜像 `FoloToy-AI-Passport.bin` | 约 5.34 MB | 含两包与两档字库 |
| 合并镜像 `FoloToy-AI-Passport-full.bin` | 约 5.40 MB | 从 0x0 整片烧录 |
| factory 分区 | 8,323,072 | 剩余约 2.98 MB |

- 静态 DRAM：171,660 B 已用 / 321,296 B；启动后余 149,636 B。
- 运行期固定分配：4 条舞台带 73,728 B + 16 行刷新缓冲 10,240 B +
  输入任务栈 4,608 B + 音频任务栈 3,072 B + 24 段历史 4,608 B + I2S DMA 约 2,880 B。
- 预估剩余堆约 **46 KB**（未在真机 `heap_caps` 实测；见「验证边界」）。
- 目标 48 KB 池用 32 位 zig 真 LVGL 验证：初始化后 used 26,540 / free 18,396 /
  largest 17,984；全页面切换后 used 28,700 / free 15,968 / largest 12,004
  （host 32-bit proxy，非真机）。
- 64-bit host 需要 72 KB 池才能装下同一 UI（used 45,824 / free 18,848），
  这是 8 字节指针的悲观代理，不能当作目标结论。

## 构建

需要 ESP-IDF **5.5.3**。资源包不提交二进制，先在本机生成：

```bash
# 1) 固定源提交的剧情 + 图像/音频包
python3 tools/nine_pack.py \
  --source /path/to/9-nine-rw5 --episode kokoiro \
  --out assets/nine/kokoiro

# 2) 16/20px 字库（必须在 pack 之后运行；会补 UI 字符与 ASCII）
cd tools && python3 nine_font.py \
  --font /path/to/nine-text.otf \
  --fallback /path/to/NotoSansSC-VF.ttf \
  --out ../assets/nine/kokoiro --source-root ../main

# 3) 静态门禁（含本作的 pack/VM/存档/字库/图像/音频/输入回归）
cd .. && ./tools/validate.sh --static

# 4) 固件 + 合并镜像 + 独立验包 + 调试归档
./tools/validate.sh --firmware
```

`nine_pack.py` 会校验源仓库 HEAD 必须等于固定提交。每次重新生成 pack 后都要
重新生成字库（`nine_pack.py` 会重写 `nine_symbols.txt`，字库工具再把它补齐到
包含 UI 与 ASCII 的 2182 字形）。

手工构建：

```bash
source /path/to/esp-idf-v5.5.3/export.sh
idf.py -B build/fw -D SDKCONFIG=build/fw/sdkconfig build
idf.py -B build/fw merge-bin -o "$PWD/build/fw/FoloToy-AI-Passport-full.bin"
python3 tools/verify_firmware.py build/fw
```

从合并镜像整片烧录（会覆盖设备现有固件与数据）：

```bash
esptool.py --chip esp32c3 -p <PORT> -b 460800 \
  --before default_reset --after hard_reset write_flash 0x0 \
  build/fw/FoloToy-AI-Passport-full.bin
```

> 本轮未授权任何设备/COM/刷写操作，因此仓库只交付可复核的构建产物与命令。

## 验证

| 项 | 结果 |
| --- | --- |
| Build (ESP-IDF 5.5.3, C3, 8 MB) | **PASS** |
| Host tests（静态门禁，含本作 6 组 C 回归；Windows symlink 平台跳过 4/5/12 项） | **PASS** |
| 合并镜像 / `verify_firmware.py` / 调试归档 | **PASS** |
| 源 graph ↔ C VM 同轨迹差分（8 个场景，约 2.7 万条视图/状态） | **PASS**（本机证据） |
| 源→包逐句/逐层审计（8049 句、94 条边、20850 层） | **PASS** |
| 图像参考合成（209 帧；JPEG/调色板/raw 真实解码） | **PASS**（容差见文档） |
| 生产路径真实 LVGL（无 `NINE_IMAGE_HOST`；32 位 48 KB 池 / 64 位 72 KB 代理） | **PASS**（本机证据） |
| 宿主真 LVGL 截图（正常/多人/CG/20px/菜单） | **PASS**；`build/nine/shots/index.html`（非设备截图） |
| Device tests | **NOT RUN**（本轮禁止刷写/串口） |

**未验证**：真机横屏方向/圆角、实际按键手感与长按/释放时序、真机剩余堆与最长
连续块、音频输出响度/底噪、NVS 掉电/磨损、自动休眠与唤醒。软件门禁通过不等于
真机通过。

## 已知限制（如实说明）

- **OP 不是视频**：ESP32-C3 无视频解码器，固件以 3 fps 图片序列 + 原曲还原；
  源 6 fps/599 帧降为 3 fps/300 帧，时长 99.9 s 不变。
- **音频为 8 kHz 单声道 IMA ADPCM**：源 Opus 轨离线转码；不是原始 44.1/48 kHz 音质。
- 源 watch 工程本就不含语音与 R18 内容；本固件同样不含。
- 标题主视觉由源 432×514 竖版中心裁切为 320×240 横版；立绘/舞台按源 16:9 等比适配 320×180，无纵向压缩。
- 立绘 8 位调色板（最多 192 色）与 JPEG q38 是有意的容量/内存取舍；不是无损。
- 章节过场页沿用框架页面，但本作 graph 没有独立过场节点，正常阅读直接显示章节标签。
- 运行期剩余堆为静态估算，未在真机 `heap_caps_get_free_size()` 读取。

## 版权与来源

- 原作《9-nine- 九次九日九重色》版权归 **Palette** 所有；本移植仅使用上述公开
  RW5 工程中已经加工的文本与转码素材，不包含原作安装文件。
- 上游 RW5 工程的 `package.json` 标注 MIT；游戏素材权利另计。公开分发前请自行
  确认素材授权。
- 仓库代码沿用 FoloToy AI Passport 的许可证与贡献规范；源字体（`nine-text.otf`、
  Noto Sans SC）不提交，只提交生成的字形子集。

## 文档

- 工程细节（包格式、VM 契约、预算、验证命令）：[`docs/nine-kokoiro.zh_CN.md`](docs/nine-kokoiro.zh_CN.md)
- English engineering notes: [`docs/nine-kokoiro.md`](docs/nine-kokoiro.md)
- 框架与硬件基线：[`docs/README.md`](docs/README.md)、[`AGENTS.md`](AGENTS.md)
