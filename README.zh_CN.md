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


### ATRI 阅读器（ATRI Reader）

把小米手环上的《ATRI -My Dear Moments-》同人移植搬到 AI Passport 上的
**竖屏视觉小说阅读器**：**34 章、1,069 幕、12,188 句对白、5 张全身立绘、三个结局**，完全离线。
剧本与素材取自 [`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI)
（它在 [`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband) 基础上重构，
并补上了原版没有的 `src/common/character/*` 全身立绘）。原版是 Vela OS 的触屏应用，
本分支用 C + LVGL 重写了阅读引擎，改成三键操作。

- 分支：本分支，尚未发版。

```text
┌──────────────────────────────────┐  240 x 320,原生竖屏;画布铺满整屏
│  画面区 240 x 320                │  背景 JPEG + 可选叠加
│  [第N章]                  [电量] │
│                                  │
│ ┌────────┐                       │  说话人名牌
│ │ 亚托莉 │                       │  全身立绘:只在有说话人名字的那句显示
│ └────────┴───────────────────────┤  源工程的半透明蓝底文本带
│  正文,5 行 × 13 个全角字/行      │  直接画进画布,立绘压在带子之上
│                                  │  16px 字体,20px 行距
└──────────────────────────────────┘
```

**操作：** 列表页上 / 下移动光标，**确定**进入，**长按确定**返回上一层。正文里
**上 / 下短按**是下一句，**长按上 / 下**是快进、松手即停（快进时整句直接显示），
**确定**打开菜单：继续阅读、保存进度、读取存档、跳过章节、返回标题（跳过章节会一直推进到下一章，
路上遇到选项或结局就停下）。选项页上 / 下选择、
确定确认。文字速度可选 瞬间 / 慢 / 中 / 快，关于页用上 / 下滚动。画面区左上角显示当前章节，
一句装不下时右下角显示页码。

**存档：** 五个手动槽 + 一个自动槽（每次换幕自动写入，标题页的「继续阅读」读它）。
存档页上短按确定存 / 读，长按确定删除手动槽。空闲 45 秒调暗背光、2.5 分钟熄屏、
7 分钟进入深睡，任意键唤醒。

**三个结局：** 剧本里的三次选择决定走向；圆满结局与悲剧结局都达成后，标题页解锁
「真正的结局」章节 —— 与原版手环应用的门槛一致。

**离线数据管线。** 运行时不下载任何东西；两个脚本生成固件需要的全部数据，产物提交
进仓库，普通 checkout 直接能编：

| 步骤 | 工具 | 产物 |
| --- | --- | --- |
| 剧本 + 图像 | `tools/atri_pack.py` | `main/atri_data/atri_pack.bin`（3.83 MB）：34 章、1,069 幕、12,188 句、73 张整屏背景、14 张特效叠加、5 张全身立绘，外加来源元数据 |
| 字体子集 | `tools/atri_font.py` | `assets/fonts/atri_cjk_16.c` + `assets/fonts/atri_cjk_symbols.txt`（2,771 个码位） |

**亮点：**

- **与原版同样的竖屏** —— 手环画面（336 x 480）按 240/336 等比缩放到 240 x 343，取顶部
  240 x 320 整屏，背景、立绘与文本带都落在原版的位置上。
- **运行时不解析 JSON** —— 资源包是一段小端二进制，直接从 Flash 读；文本按
  `(offset, length)` 寻址，阅读器沿章节 / 场景 / 对白三张表推进。
- **背景约 19 KB 一张** —— 构建期就完成缩放、裁剪与 JPEG(q88) 重编码：72 张场景背景加
  标题画共 1.36 MB，源素材是 4.7 MB 的 PNG。
- **全身立绘,按说话人切换** —— 5 张主角立绘（750×920~1150 的 PNG）按新版引擎
  `width/height:100%` 的方式压成 240×320，再按 alpha 包围盒裁剪，无损存成 RGB565 + 4bpp 遮罩。
  **只有"这句有说话人名字"时才显示**；男主夏生是视角、不出镜；事件 CG / 黑屏整幕不叠立绘
  （CG 里已经画了角色）。旁白、配角、换幕一律收起立绘。
- **不占 RAM 的叠加层** —— 14 张特效叠加按 alpha 包围盒裁剪，无损存成
  RGB565 + 4bpp 遮罩，合成时从 Flash 逐行直接写进画布。本板没有 PSRAM，画布与 LVGL
  建好后堆只剩几十 KB，240 x 320 叠加的 JPEG 解码缓冲根本拿不出来；这条路径全程不分配内存。
- **文本带画进画布** —— 源工程用一张半透明的蓝色 `text_bg` 盖在整屏画面上；这里把同一条带
  子（同样的颜色、同样的自上而下渐深）在像素层混进画布，但把**立绘合成在带子之上**，只在文字
  那几行盖一层浅暗帘：人物不会被"对话框"洗掉，白字也依然清晰。
- **自研字体生成器** —— `lv_font_conv` 在当前 Node.js 下写出的字形位图是坏的（真机表现
  为整屏噪点）；仓库内生成器用 FreeType 栅格化、直接输出 LVGL 9 plain 4bpp 格式，
  并在写完后回读自检、逐像素比对。
- **剧情逻辑可在宿主机上测** —— `tests/test_atri_model.c` 用真实资源包跑：包结构、
  圆满 / 悲剧 / 真正的结局、选择分流、分页规则、存档往返，都在
  `tools/validate.sh --static` 里执行。

**调试通道：** 串口（USB-Serial-JTAG）接受 `ATRISHOT <章下标> <幕下标>`：固件用正常渲染路径把该幕
画进画面区，再把 240 x 320 的 RGB565 原始帧回传（先 `ATRISHOT <w> <h> <字节数>`，随后像素，
最后 `ATRISHOT-END`）。本项目的画面验证就是靠它（不用拍照）；平时它只挂在一个空闲任务上等输入。

**版权：** 剧本、图像与译文来自同人移植工程 `fywmjj/better-mb9p-ATRI`、
`liuyuze61/ATRI-miband` 与原作
《ATRI -My Dear Moments-》（ANIPLEX.EXE / Frontwing / 枕）。本固件是个人非商业移植，
请支持正版。


## 说明

- 每个应用都是基于上游基线的一个独立 `feature/*` 分支。不要把 demo 分支整支合入
  `main`；需要复用时应抽取可移植的模式（见上游 `AGENTS.md`）。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` ——
  每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。
  目标板卡：8 MB Flash。
- 这些发布沉淀的可复用工程经验位于 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。
