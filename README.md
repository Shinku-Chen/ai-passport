<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 千恋＊万花（Senren \* Banka）

移植自小米手环同人移植版《千恋＊万花》的竖屏视觉小说阅读器：源移植版的完整剧情，外加 **92 张背景、123 张立绘（含多套姿势）、570 张事件立绘** 及其附带内容，全程离线。剧本与美术来自 [`hrk666666/Senren-Banka-MiBand-10`](https://github.com/hrk666666/Senren-Banka-MiBand-10)（小米手环 9/10 快应用）；本分支在 ATRI 阅读器的 LVGL 页面系统上重写阅读引擎，改由三个按键操作。状态：**已发布** —— tag `v0.1.0-senren-banka`，并以 `community-07c775dc` 提交到 AI Passport 社区市场（审核中）。

- 分支：[`feature/senren-banka`](https://github.com/Shinku-Chen/ai-passport/tree/feature/senren-banka)
- 发布：[`v0.1.0-senren-banka`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-senren-banka) —— 合并固件 `FoloToy-AI-Passport-full.bin`，7,997,648 字节（该镜像已在真机验证；CI 用同一份源码重建）
- 上游来源：[`hrk666666/Senren-Banka-MiBand-10`](https://github.com/hrk666666/Senren-Banka-MiBand-10) —— 本分支移植的来源项目，未声明许可证；其剧本与美术由 [`tools/senren_fetch_source.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_fetch_source.py) 拉取而不入库，打包进固件并署名来源。按应用首次启动的说明，本移植仅供个人学习交流。
- 素材工具链：[`tools/senren_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_pack.py)（图片包 5,263,756 字节，输出到 `main/senren_data/`）、[`tools/senren_scn_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_scn_pack.py)（剧本包 1,432,148 字节）与 [`tools/senren_lvgl_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/senren-banka/tools/senren_lvgl_font.py)（16px 中文子集，3,492 个字形，按码点顺序供稀疏字体二分查找）

**操作（三键）：** 列表页上 / 下移动光标、**确定**进入。阅读时 **上 / 下** 推进一行（打字过程中按一下先补全，一行放不下会分页），**长按上**快进、松手即停，**长按下**切换自动阅读，**确定**（短按或长按）打开菜单 —— 继续、保存、读取、跳过章节、返回标题。选项用上 / 下 + 确定，选中即存档。短按阈值 180 ms、长按 500 ms，由 BSP 显式传给按键组件。

**存档、章节与空闲：** 1 个自动存档位 + 5 个手动位，自动位在每次换章、每次选项和入睡前写入，标题页以「继续」入口呈现。每章进入时显示 `Chapter X-X` 卡片（章 + 节）。空闲策略：60 秒调暗背光、180 秒熄屏、420 秒进入 deep sleep，任意键唤醒；自动阅读与快进不计入空闲时间，因此长时间挂着不会自动变暗或休眠。

**亮点：**

- **自写 inflate 替代 ROM 解压器** —— 整块解压到一个缓冲区，不需要 32 KB 滑动字典，在没有 PSRAM 的板子上把工作缓冲压到 4 KB。
- **分块调色板图片格式** —— 背景与事件立绘存为 240 × 320 JPEG，立绘按姿势分块、底部对齐，事件立绘的变体以基础图 + 稀疏遮罩补丁表示，哪种更小就用哪种。
- **剧本按 3 KB 分块打包** —— 用约 200 KB 压缩率换来同样的 4 KB 缓冲；所有跳转目标经分块表解析，不做全文扫描。
- **「菜单自己弹出来」背后的真机发现** —— 三个按键共用一个 ADC 引脚、按电压区间区分；长按过程中触点短暂断开会让电压扫过相邻区间而被读成另一个键。BSP 现在在电压回到释放区间前锁定按键身份，并在确认前持续 40 ms 才接受按键。这属于板级逻辑，应回到上游。
- **串口调试通道** —— `SENRENPAGE [title | <章节> <步>]` 从 LVGL flush 路径抓帧并以原始 RGB565 回传；`SENRENJUMP`、`SENRENSKIP`、`SENRENSAVE`、`SENRENLOAD`、`SENRENINFO` 与 `SENRENKEYS`（最近 32 个按键事件及其被识别时的 ADC 电压，正是上面按键区间缺陷的证据）覆盖跳章、跳过、存读档、状态读取与按键追踪。

## 说明

- 本分支只承载一个应用：`feature/*` 分支的根 README 只介绍本分支自己的应用，全部项目的目录在 fork `main` 的根 README。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` —— 每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。目标板卡：8 MB Flash。
- 这些发布沉淀的可复用工程经验位于 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。
