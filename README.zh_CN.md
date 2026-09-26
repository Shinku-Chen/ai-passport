<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# FoloToy AI Passport —— Shinku-Chen 的 fork

本仓库是 [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的个人 fork。
上游仓库是 [FoloToy AI Passport](https://ai-passport.folotoy.cn)（开源可穿戴 AI 设备：
ESP32-C3、240×320 彩屏、三键操作、8 MB Flash、无 PSRAM）的开发基线。

这个 fork 在基线之上承载了**多个独立应用**，每个项目各自位于一个 `feature/*` 分支上，
下面逐个介绍。板卡事实、BSP 与开发流程来自上游仓库 —— 见
[`docs/README.md`](docs/README.md)、[`AGENTS.md`](AGENTS.md) 与
[`docs/contribution/`](docs/contribution/)。每个项目的固件发布挂在本仓库的
[Releases](https://github.com/Shinku-Chen/ai-passport/releases) 上。

## 项目

### 音效钥匙扣（Voice Keychain）

把 AI Passport 变成口袋音频播放器的音效钥匙扣：开机即进入应用，播放来自几十个角色包的
**数百条中文语音片段** —— jojo、meme cat、刘华强、哈吉米、奶龙等等。最新版：**v1.3.0**。

- 分支：[`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain)
- 发布：[v1.1.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.1.0)、[v1.3.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.3.0)
- 经验沉淀：[`docs/reference/shinku-chen/voice-keychain/`](docs/reference/shinku-chen/voice-keychain/)

**操作方式（三键）：** UP / DOWN 在列表中移动，**OK** 进入目录、选择片段或播放，
**OK（长按）** 进入设置（音量、电量）或返回。

**v1.3.0 亮点：**

- **自包含固件** —— `FoloToy-AI-Passport-full.bin` 把 `voicefs` 数据分区（位于
  `0x210000`）打进同一个 8 MB 镜像，从 `0x0` 整体烧录即可，无需再单独刷数据分区。
- **深睡唤醒已修复** —— GPIO0 唤醒源此前从未启用（把引脚号当位掩码传入），导致"睡了
  按不醒"；现改为 5 分钟无操作入睡、任意按键可唤醒（真机验证）。
- **列表播放更可靠** —— 此前每次播放都临时申请 16 KB Opus 解码栈，堆不足时"停掉当前
  声音却不播选中项"；改为单个常驻播放任务（静态栈 + 二值信号量）。
- 电量每 30 秒刷新；CW2017 上电偶发读到 `0xFF` 时，按开路电压查表兜底估算电量。

### 今天吃啥（What to Eat Today）

按键驱动的食物转盘决策器，终结"今天吃什么"。按住 **UP** 播放"今天午餐要吃什么呢？"
引导动画，按住 **DOWN** 滚动食物选择器，松开停在随机结果上。最新版：**v1.2.0**。

- 分支：[`feature/cheerful-goodall`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cheerful-goodall)
- 发布：[v1.2.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.2.0)
- 经验沉淀：[`docs/reference/shinku-chen/eat-what/`](docs/reference/shinku-chen/eat-what/)

**操作方式：** 按住 UP / DOWN 运行两套动画，松开停在当前帧；**OK** 切换 LVGL 局部刷新
与快速隔行刷新。空闲 2 分钟自动关机（深睡，GPIO0 唤醒）。

### 生字卡片识记（Shengzi Cards）

汉字闪卡识记应用。三种模式 —— **浏览（Browse）**：滚动字卡；**自测（Self-test）**：
标记每个字认识/不认识；**拼读（Spell）**：看拼音猜字。**OK 短按**揭晓答案，已认识
标记持久化到 NVS。最新版：**v1.0.0**。

- 分支：[`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards)
- 发布：[v1.0.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.0)

### 四子棋（Connect Four）

AI Passport 上的横屏四子棋：**棋盘 10 列 × 7 行**，可与电脑对战（低 / 中 / 高三档难度，先手可选玩家或电脑），也可双人同机轮下。最新版本：**v1.6.0-connect-four**。

- 分支：[`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four)
- 发布：[v1.6.0-connect-four](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.6.0-connect-four)

**操作：** 上 / 下移动落子列（横屏持握时「上」在右手边），**确定**落子，**长按确定**回设置屏。设置屏上用上 / 下选行、确定切换取值（模式：`HUMAN vs AI` / `AI vs HUMAN` / `TWO PLAYERS`，难度：`低` / `中` / `高`，预览：`落点` / `顶部行`），选到 `START` 按确定开局；两个电脑模式只差先后手。

**亮点：**

- **横屏 320 × 240、棋盘密排** —— 70 个格子、26px 棋子、相邻仅隔 3px，靠 BSP 层的 MADCTL 硬件旋转铺满屏幕。
- **三档电脑难度** —— 用墙钟搜索预算把每步控制在 1 秒内；低 / 中难度带固定失误率放水，保证打得赢。
- **无素材音效** —— 换列、落子、胜、负、平局音都由正弦表实时合成，不占 flash 资源。
- **空闲自动深睡** —— 设置屏 60 秒、对局中 180 秒后进入深睡，任意键唤醒（GPIO0 低电平唤醒，已修复 ADC 占用该脚导致的立即自唤醒）。
- **串口截图** —— `FAP_SCREENSHOT_V1` 命令回传真实 320 × 240 画面，本版封面就是这么抓的。


### 飞鸟会长不肯认输（Asunabi）

移植自小米手环版本的竖屏视觉小说：**30 章、4649 句对白、约 9.4 万字**，从头读到唯一结局。
状态：**开发中** —— 已能构建并已烧录测试，尚未作为 release 发布。

- 分支：[`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame)
- 素材工具链：[`tools/gal/`](tools/gal/README.md)

**操作方式（三键）：** **UP** 推进一句（打字中则立即全显）；**UP（长按）** 快进，松手即停；
**OK** 呼出菜单（继续阅读、保存、读取、跳过本章、设置、返回标题）；**DOWN** 滚动超出面板的文字；
**DOWN（长按）** 隐藏面板看画面。

**亮点：**

- **第三方美术不进仓库** —— 美术与剧本在构建时从本地未跟踪的源目录打包进独立的 4 MiB
  `assets` 数据分区；没有源素材时打包器产出占位包，全新 clone 仍能配置、构建并启动。
- **无 PSRAM 也能铺满全屏美术** —— 背景是内存映射分区里的 LVGL 索引图，直接从 flash 绘制、
  按行解码（约 960 字节），而不是 150 KB 的帧缓冲；占用芯片 128 个 flash-MMU 页中的 83 个。
- **面板高度由字号推导** —— 四行 × 行高 + 内边距，因此 16px 与 20px 下一页四行都完整显示。
  仍然放不下的长句是**翻页而不是裁掉**：切页在纯 model 层完成（含标点禁则），页号进存档。
- **按住快进不改 BSP** —— BSP 没有松手事件，于是快进轮询公开的 `bsp_button_read_mv()`，
  读数一旦离开该键文档化的电压窗口就停。
- **两次把素材体积压下来** —— 立绘先裁到真能进入屏幕的区域、再取 alpha 包围盒
  （0.82 MiB → 0.24 MiB）；被替换的素材只存一份而非两份（4.41 MiB → 3.59 MiB）。
- **解析与排版有 host 测试** —— 包读取、推进规则与分页不依赖 ESP-IDF/LVGL，
  由 11 项 host 测试覆盖，含格式要求的 4 字节结构对齐守卫。
- **顺带修掉上游 8 处素材缺失** —— 上游剧本引用了 8 张它自己仓库里没有的图（其中 5 处是笔误），
  现在打包时逐条替换并报告，而不是在设备上画出空白帧。

## 说明

- 每个应用都是基于上游基线的一个独立 `feature/*` 分支。不要把 demo 分支整支合入
  `main`；需要复用时应抽取可移植的模式（见上游 `AGENTS.md`）。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` ——
  每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。
  目标板卡：8 MB Flash。
- 这些发布沉淀的可复用工程经验位于 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。
