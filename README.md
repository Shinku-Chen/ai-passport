<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
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

从小米手环版本 [`liuyuze61/Asunabi-miband`](https://github.com/liuyuze61/Asunabi-miband)
移植到 AI Passport 的竖屏视觉小说：**30 章、4649 句对白、约 9.4 万字**，从头读到唯一结局。
状态：**已发布** —— tag `v0.1.0-asunabi`，并已投稿到 AI Passport 社区市场（审核中）。

- 分支：[`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame)
- 发布：[`v0.1.0-asunabi`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-asunabi) —— 合并镜像 `FoloToy-AI-Passport-full.bin`，7,917,142 字节
- 上游作品：[`liuyuze61/Asunabi-miband`](https://github.com/liuyuze61/Asunabi-miband) —— 本次移植所依据的小米手环快应用版本。该仓库未声明许可；其美术与章节剧本随分支提交在 `assets/gal-source/`，并以上游项目作为来源标注。
- 素材工具链：[`tools/gal/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame/tools/gal)

**操作方式（三键）：** **UP** 推进一句（打字中则立即全显）；**UP（长按）** 按住快进、松手即停；
**OK** 呼出菜单（继续阅读、保存、读取、跳过本章、设置、返回标题）；**DOWN** 滚动超出面板的文字；
**DOWN（长按）** 开关自动阅读：每句显示完毕后 0.9 秒自动推进，按任意键解除。
长按判定 300 ms（由 BSP 显式下发，而不是用 button 组件默认的 1500 ms）。

**模式与存档：** 6 个手动存档槽，位置包含「分页中的第几页」（在槽上长按确定键删除）；
最后一次位置自动记住，「继续阅读」即从这里接着读；另有章节跳转列表，以及设置页
（文字速度：慢 / 中 / 快 / 瞬间，文字大小：16px / 20px 带实时打字预览，自动阅读开关）。
全部只有一个结局，读完后会清掉继续点。

**亮点：**

- **第三方美术随分支提交** —— 美术与剧本来自上游项目，而它未声明任何许可；它们提交在 `assets/gal-source/`，
  因此 clone 就能构建出完整作品。打包器把它们打进独立的 4 MiB `assets` 数据分区（实际用 3.55 MiB），
  并在放不下时让构建失败而不是产出被截断的镜像；没有该目录时构建仍能启动，只是改为占位包。
- **串口截图** —— `FAP_SCREENSHOT_V1` 命令会通过控制台回一帧当前画面（RGB565LE），这是社区发布接受投稿前的硬性要求。
- **画面优先的界面** —— 标题背景、选项底板与对白面板都改为半透明叠色（30% / 50% / 70%）而不是不透明，
  画面保持可见、近白色文字也压得住；固件也不会熄灭屏幕。
- **无 PSRAM 也能铺满全屏美术** —— 背景是内存映射分区里的 LVGL 索引图，直接从 flash 绘制、
  按行解码（约 960 字节），而不是 150 KB 的帧缓冲；占用芯片 128 个 flash-MMU 页中的 83 个。
- **面板高度由字号推导** —— 四行 × 行高 + 内边距，因此 16px 与 20px 下一页四行都完整显示。
  仍然放不下的长句是**翻页而不是裁掉**：切页在纯 model 层完成（含标点禁则），页号进存档。
- **按住快进不改 BSP** —— BSP 没有松手事件，于是快进轮询公开的 `bsp_button_read_mv()`，
  读数一旦离开该键文档化的电压窗口就停。
- **两次把素材体积压下来** —— 立绘先裁到真能进入屏幕的区域、再取 alpha 包围盒
  （0.82 MiB → 0.24 MiB）；被替换的素材只存一份而非两份（4.41 MiB → 3.55 MiB）。
- **解析与排版有 host 测试** —— 包读取、推进规则与分页不依赖 ESP-IDF/LVGL，由 host 测试覆盖，
  含格式要求的 4 字节结构对齐守卫，以及一条让打包素材名与固件查询名保持一致的守卫。
- **顺带修掉上游 8 处素材缺失** —— 上游剧本引用了 8 张它自己仓库里没有的图（其中 5 处是笔误），
  现在打包时逐条替换并报告，而不是在设备上画出空白帧。

### ATRI 阅读器（ATRI Reader）

把小米手环上的《ATRI -My Dear Moments-》同人移植搬到 AI Passport 上的**竖屏视觉小说阅读器**：
**34 章、1,069 幕、12,188 句对白、5 张全身立绘、三个结局**，完全离线。剧本与素材取自
[`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI)（它在
[`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband) 基础上重构，补上了原版没有的
`src/common/character/*` 全身立绘）。原版是 Vela OS 的触屏应用，本分支用 C + LVGL 重写阅读引擎，
改成三键操作。状态：**已发布** —— tag `v1.0.2-atri-reader`，并已投稿到 AI Passport 社区市场
（作品 `my-dear-moments-2`，审核中）。

- 分支：[`feature/atri-reader`](https://github.com/Shinku-Chen/ai-passport/tree/feature/atri-reader)
- 发布：[`v1.0.2-atri-reader`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.2-atri-reader) —— 合并镜像 `FoloToy-AI-Passport-full.bin`，5,134,544 字节
- 上游作品：[`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI) 与 [`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband) —— 本次移植所依据的同人版本，两个仓库都未声明许可；其剧本、背景与立绘随分支提交并打包进固件，来源以上游项目标注。
- 素材工具链：[`tools/atri_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/atri-reader/tools/atri_pack.py)（剧本 + 图像打包成 `main/atri_data/atri_pack.bin`，3.83 MB）与 [`tools/atri_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/atri-reader/tools/atri_font.py)（16px 中文字体子集，2,771 个码位）

**操作方式（三键）：** 列表页上 / 下移动光标、**确定**进入、**长按确定**返回上一层；正文里
**上 / 下短按**是下一句、**长按上**是快进（松手即停，快进时整句直接显示）、
**长按下**开关自动阅读（打字完停 0.7 秒自动翻页，右下角显示「自动」，任意键停止，期间屏幕不熄灭）、
**确定**打开菜单（继续阅读、保存进度、读取存档、跳过章节、返回标题）；选项页上 / 下选择、确定确认，
跳过章节会一直推进到下一章，遇到选项或结局停下。长按判定 300 ms、短按 120 ms；文字速度可选
瞬间 / 慢 / 中 / 快，关于页用上 / 下滚动。

**存档、结局与空闲：** 五个手动槽 + 一个自动槽（每次换幕自动写入，标题页的「继续阅读」读它），
存档页短按确定存 / 读、长按确定删除手动槽；剧本里的三次选择通向圆满或悲剧结局，两者都达成后
标题页解锁「真正的结局」，与原版手环应用的门槛一致。空闲 45 秒调暗、2.5 分钟熄屏、7 分钟深睡，
任意键唤醒，自动阅读期间不计入空闲。

**亮点：**

- **立绘跟着说话人** —— 只有带人物名称的台词才出现全身立绘，说完即收；事件 CG、黑屏与男主视角不出镜，立绘按原作站位靠右。
- **无 PSRAM 也扛得住整屏合成** —— 背景用 JPEG 直接解码进画布，立绘与特效叠加用无损 RGB565 + 4bpp alpha 遮罩（按 alpha 包围盒裁剪），空闲堆仍留 40 KB 以上。
- **离线数据管线** —— 运行时不下载任何东西，`tools/atri_pack.py` 与 `tools/atri_font.py` 的产物提交进仓库，普通 checkout 直接能编。
- **串口调试通道** —— `ATRISHOT <章> <幕>` 把任意一幕渲染进画面区并回传原始 240 × 320 帧，本版封面与历次版面验证靠的就是它；`ATRIJUMP` 可直接跳章。

### 星空列车与白的旅行（Starry Sky Railroad and Shiro's Journey）

把小米手环上的《星空鉄道とシロの旅》同人移植（含中文译文）搬到 AI Passport 上的**竖屏视觉小说阅读器**：
**39 章、1,260 幕、13,787 句对白、一处选项、一个结局**，完全离线。剧本、素材与译文取自
[`liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P`](https://github.com/liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P)
（小米手环 9 Pro 快应用）；本分支在 ATRI 阅读器的 LVGL 页面系统上重写阅读引擎，改成三键操作。
状态：**已发布** —— tag `v0.1.0-starry-sky-railroad`，并已投稿到 AI Passport 社区市场
（作品 `community-0d8223f7`，审核中）。

- 分支：[`feature/starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/tree/feature/starry-sky-railroad)
- 发布：[`v0.1.0-starry-sky-railroad`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-starry-sky-railroad) —— 合并镜像 `FoloToy-AI-Passport-full.bin`，4,627,696 字节
- 上游作品：[`liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P`](https://github.com/liuyuze61/Starry_Sky_Railroad_and_Shiro-s_Journey_miband9P) —— 本次移植所依据的同人版本，未声明许可；其剧本、背景与立绘随分支提交并打包进固件，来源以上游项目标注。
- 素材工具链：[`tools/starry_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/starry-sky-railroad/tools/starry_pack.py)（剧本 + 图像打包成 `main/starry_data/starry_pack.bin`，3.44 MB）与 [`tools/starry_lvgl_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/starry-sky-railroad/tools/starry_lvgl_font.py)（16px 中文字体子集，2,708 个码位）

**操作方式（三键）：** 列表页上 / 下移动光标、**确定**进入、**长按确定**返回上一层；正文里
**上 / 下短按**推进一步（打字中按一下先显示全文）、**长按上**是快进（松手即停）、
**长按下**开关自动阅读（每页显示完整后 0.9 秒翻页，按其它键停止）、**确定**打开菜单
（继续阅读、保存进度、读取存档、跳过章节、返回标题）；选项页上 / 下选择、确定确认，
跳过章节会播放章节过场卡并进下一章。短按判定 180 ms、长按判定 500 ms，由 BSP 显式
下发给按键组件。

**存档、结局与空闲：** 五个手动槽 + 一个自动槽（每次换场景自动写入，标题页的「继续阅读」读它），
存档页短按确定存 / 读、长按确定删除手动槽；剧本只有一处选择、一个结局。空闲 60 秒调暗、
3 分钟熄屏、7 分钟深睡，任意键唤醒，自动阅读与快进不计入空闲。

**亮点：**

- **立绘只在该角色本人说话时出现** —— 打包器按「哪个有名字的说话人引用了这张立绘」统计归属，全剧本 11,777 个对白步里有 3,444 步会画出立绘；事件插画与纯色幕额外打标，永远不会被贴上人脸。
- **立绘压在半透明正文带下面** —— 蓝色文本框叠在人物之上，与源移植版一致：下半身落在面板里，带子以上的部分不受影响。
- **无 PSRAM 也扛得住整屏合成** —— 背景直接解码进 240 × 320 画布，立绘用无损 RGB565 + 4bpp 遮罩从 Flash 逐行 blit（完全不需要立绘解码缓冲），面板由 40 行分部缓冲推送。
- **离线数据管线** —— 运行时不下载任何东西，`tools/starry_pack.py` 与 `tools/starry_lvgl_font.py` 的产物提交进仓库，普通 checkout 直接能编。
- **串口调试通道** —— `STARRYPAGE [title | <章> <幕>]` 从 LVGL 刷屏路径取帧并回传原始 RGB565，本项目的历次版面核对靠的就是它；`STARRYJUMP` 可直接跳章。

### 天使☆騒々 RE-BOOT!（Tenshi☆Sousou RE-BOOT!）

把小米手环快应用上的《天使☆騒々 RE-BOOT!》同人移植（含中文译文）搬到 AI Passport 上的
**竖屏视觉小说阅读器**：**61,436 页剧本、61,289 句对白、1,165,911 字正文、45 个章节点、
13 个选择点、15 个结局点**（6 条女主线 Noa / Amane / Kurumi / Kaguya / Orie / Fumika
+ 2 个 BAD END + 1 个 END + 6 个回主页），完全离线。剧本、素材与译文取自
[`hezdaaa/tsxxreboot-miband`](https://github.com/hezdaaa/tsxxreboot-miband)（小米手环快应用）；
本分支在 ATRI / 星空列车阅读器的 LVGL 页面系统上重写阅读引擎，补上原作的分支系统
（15 结局 / 5 个 flag 闸门 / 18 个禁进 / 38 个禁退），并改成三键操作。
状态：**已发布** —— tag `v0.1.0-tsxx-reboot`，并已投稿到 AI Passport 社区市场（审核中）。

- 分支：[`feature/tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tsxx-reboot)
- 发布：[`v0.1.0-tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-tsxx-reboot) —— 合并镜像 `FoloToy-AI-Passport-full.bin`，8,135,520 字节
- 上游作品：[`hezdaaa/tsxxreboot-miband`](https://github.com/hezdaaa/tsxxreboot-miband) —— 本次移植所依据的小米手环快应用版本，未声明许可；剧本、立绘与背景的著作权归柚子社（Yuzusoft）及原发行方。本移植只做转换与重制：随固件发布的是转换后的资源包，原始素材不随固件分发，仅作为来源标注与复现打包的输入提交在 `assets/tsxx-source/`。本项目是非商业同人技术研究，请支持正版。
- 素材工具链：[`tools/tsxx_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/tsxx-reboot/tools/tsxx_pack.py)（剧本 + 图像 + 分支表打包成 `main/tsxx_data/tsxx_pack.bin`，6.26 MiB）与 [`tools/tsxx_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/tsxx-reboot/tools/tsxx_font.py)（16px 中文字体子集，3,417 个码位）

**操作方式（三键）：** 标题页上 / 下选择、**确定**进入、**长按确定**关机；正文里
**上 / 下 / 确定短按**都是下一句（打字中按一下先显示全文）、**长按上**快进（松手即停）、
**长按下**开关自动阅读（每页显示完整后 0.7 秒翻页，按其它键停止，期间不熄屏）、
**长按确定**打开菜单（继续阅读、保存进度、读取存档、跳过章节、章节跳转、返回标题）；
选项页上 / 下选择、确定确认；列表页**长按确定**返回上一层；存档页短按确定存 / 读，
长按确定删除手动槽。设置里可切文字速度（慢 / 中 / 快，50 / 28 / 12 毫秒每字）与自动阅读开关。

**分支与存档：** 5 个手动槽 + 1 个自动槽 —— 每次换场景自动落盘（4 秒节流，选项点与休眠前强制写入），
标题页的「继续阅读」读它；选择历史一起进存档，闸门按累积选择判定，所以读档不会走错分支。
空闲 45 秒调暗、2.5 分钟熄屏、7 分钟深睡，任意键唤醒，自动阅读期间不计入空闲。

**亮点：**

- **美术层与屏幕 1:1** —— 画布就是 240 × 320，立绘按画布坐标系存放，合成时不再重采样；背景与事件图仍按 180 × 240 存在包里，合成时按最近邻放大 4/3，文字层画在原生分辨率上。
- **无 PSRAM 也扛得住整屏合成** —— 150 KB 的 RGB565 画布不配任何解码暂存区：背景、事件图、补丁与立绘全部走 tjpgd 的回调接口逐块解码进画布，立绘另配 1bpp 遮罩逐行从 Flash blit。
- **资源包按两层映射读取** —— 包尾的脚本区间（约 1.9 MiB：页表、正文码流、选项表、分支表）在运行期常驻只读映射；图片段用 256 KB 滑动窗口 `esp_partition_mmap`，取图即解码。
- **分支系统是数据驱动的** —— 15 个结局点、5 个 flag 闸门（11 条规则 / 54 个条件）、18 个禁进与 38 个禁退跳转全部来自 [`assets/tsxx-source/branch.json`](assets/tsxx-source/branch.json)，打包时逐条校验闸门条件指向真实选项，配置与包不一致时构建直接失败。
- **解析、推进与闸门判定有 host 测试** —— 包读取、推进顺序（结局点 → 禁进 / 闸门 → 顺序下一页）与分页排版不依赖 ESP-IDF/LVGL，host 测试用真实资源包逐页走完整张页表，并单独走完 6 条女主线。

## 说明

- 每个应用都是基于上游基线的一个独立 `feature/*` 分支。不要把 demo 分支整支合入
  `main`；需要复用时应抽取可移植的模式（见上游 `AGENTS.md`）。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` ——
  每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。
  目标板卡：8 MB Flash。
- 这些发布沉淀的可复用工程经验位于 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。
