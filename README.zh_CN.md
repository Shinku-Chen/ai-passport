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

## 说明

- 每个应用都是基于上游基线的一个独立 `feature/*` 分支。不要把 demo 分支整支合入
  `main`；需要复用时应抽取可移植的模式（见上游 `AGENTS.md`）。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` ——
  每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。
  目标板卡：8 MB Flash。
- 这些发布沉淀的可复用工程经验位于 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。
