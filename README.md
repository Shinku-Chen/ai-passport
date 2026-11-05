<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 天使☆騒々 RE-BOOT! —— AI Passport 竖屏阅读器

把小米手环快应用版的《天使☆騒々 RE-BOOT!》同人移植（含中文译文）搬到
[FoloToy AI Passport](https://ai-passport.folotoy.cn) 的**竖屏视觉小说阅读器**：
**61,436 页剧本、61,289 句对白、1,165,911 字正文、45 个章节点、13 个选择点、
15 个结局点**（6 条女主线 Noa / Amane / Kurumi / Kaguya / Orie / Fumika，
2 个 BAD END、1 个 END、6 个回主页），完全离线。

剧本、素材与译文取自
[`hezdaaa/tsxxreboot-miband`](https://github.com/hezdaaa/tsxxreboot-miband)
（小米手环快应用）；本分支在 ATRI / 星空列车阅读器的 LVGL 页面系统上重写阅读引擎，
补上原作的分支系统（15 结局 / 5 个 flag 闸门 / 18 个禁进 / 38 个禁退），
并改成三键操作。状态：**已发布** —— tag `v0.1.0-tsxx-reboot`，并已投稿到
AI Passport 社区市场（审核中）。

- 分支：[`feature/tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tsxx-reboot)
- 发布：[`v0.1.0-tsxx-reboot`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-tsxx-reboot)
  —— 合并镜像 `FoloToy-AI-Passport-full.bin`，**8,135,520 字节**
- 上游作品：[`hezdaaa/tsxxreboot-miband`](https://github.com/hezdaaa/tsxxreboot-miband)
  —— 本次移植所依据的小米手环快应用版本，未声明许可；剧本、立绘与背景的著作权归
  柚子社（Yuzusoft）及原发行方。本移植只做转换与重制：随固件发布的是转换后的资源包，
  原始素材不随固件分发，仅作为来源标注与复现打包的输入提交在 `assets/tsxx-source/`。
  本项目是非商业同人技术研究，请支持正版。
- 素材工具链：
  [`tools/tsxx_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/tsxx-reboot/tools/tsxx_pack.py)
  （剧本 + 图像 + 分支表打包成 `main/tsxx_data/tsxx_pack.bin`，6.26 MiB）与
  [`tools/tsxx_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/tsxx-reboot/tools/tsxx_font.py)
  （16px 中文字体子集，3,417 个码位）。

## 三键操作

标题页上 / 下选择、**确定**进入、**长按确定**关机。正文里 **上 / 下 / 确定短按**
都是下一句（打字中按一下先显示全文），**长按上**快进（松手即停），**长按下**
开关自动阅读（每页显示完整后 0.7 秒翻页，其它键停止，期间不熄屏），
**长按确定**打开菜单（继续阅读、保存进度、读取存档、跳过章节、章节跳转、返回标题）。

选项页上 / 下选择、确定确认；列表页长按确定返回上一层；存档页短按确定存 / 读，
长按确定删除手动槽。设置里可切文字速度（慢 / 中 / 快，50 / 28 / 12 毫秒每字）
与自动阅读开关。空闲 45 秒调暗、2.5 分钟熄屏、7 分钟深睡，任意键唤醒；
自动阅读期间不计入空闲。

## 功能

- **分支与存档** —— 5 个手动槽 + 1 个自动槽；每次换场景自动落盘（4 秒节流，
  选项点与休眠前强制写入）。标题页的「继续阅读」读取自动槽，选择历史一起进存档，
  闸门按累积选择判定，所以读档不会走错分支。
- **数据驱动的原作分支** —— 15 个结局点、5 个 flag 闸门（11 条规则 / 54 个条件）、
  18 个禁进与 38 个禁退跳转全部来自
  [`assets/tsxx-source/branch.json`](assets/tsxx-source/branch.json)；打包时逐条校验
  闸门条件指向真实选项，配置与包不一致时构建直接失败。
- **美术层与屏幕 1:1** —— 画布就是 240 × 320，立绘按画布坐标系存放，合成时不再重采样；
  背景与事件图仍按 180 × 240 存在包里，合成时按最近邻放大 4/3，文字层画在原生分辨率上。
- **无 PSRAM 也扛得住整屏合成** —— 150 KB 的 RGB565 画布不配任何解码暂存区：
  背景、事件图、补丁与立绘全部走 tjpgd 的回调接口逐块解码进画布，立绘另配 1bpp
  遮罩逐行从 Flash blit。
- **资源包按两层映射读取** —— 包尾的脚本区间（约 1.9 MiB：页表、正文码流、选项表、
  分支表）在运行期常驻只读映射；图片段用 256 KB 滑动窗口 `esp_partition_mmap`，
  取图即解码。
- **解析、推进与闸门判定有 host 测试** —— 包读取、推进顺序（结局点 → 禁进 /
  闸门 → 顺序下一页）与分页排版不依赖 ESP-IDF/LVGL；host 测试用真实资源包逐页走完
  整张页表，并单独走完 6 条女主线。

## 构建与刷写

```bash
./tools/validate.sh --firmware   # 构建、合并并校验 0x0 镜像
idf.py build                     # 可选的增量应用构建
# 用网页刷机工具或 esptool 从 0x0 刷入 build/FoloToy-AI-Passport-full.bin
```

打包脚本需要 Pillow 与 FreeType（ESP-IDF 使用的 Python 环境）。只跑 `idf.py build`
不会生成或校验合并镜像。使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)
或 `esptool`；目标板卡为 8 MB Flash、无 PSRAM 的 AI Passport。

## 社区封面与发布信息

- **社区投稿**：已投稿 AI Passport 社区市场（审核中）。
- **社区 / release 封面**：`assets/images/tsxx-reboot-market-cover.png`，
  **1152 × 1536 PNG，3:4**。由 `assets/images/tsxx-reboot-cover.png`
  （420 × 582）以中心为准裁到 420 × 560（上下各去 11 行，精确 3:4），再用 Lanczos
  放大到 1152 × 1536；用于社区 listing 与 release 说明。封面图不在归档中重复提交。
- **发布 tag**：`v0.1.0-tsxx-reboot`；合并镜像 `FoloToy-AI-Passport-full.bin`，
  **8,135,520 字节**，由 tag 触发的 CI 构建。
- **资源包**：`main/tsxx_data/tsxx_pack.bin`，6.26 MiB；16px 中文字体子集
  3,417 个码位。

## 已知限制

- 本作是 **240 × 320 竖屏**阅读器；没有横屏模式。
- 正文字体固定为 16px 子集（3,417 个码位）；没有 20px 正文字号选项。
- 目标板卡没有 PSRAM；150 KB 画布与 6.26 MiB 资源包按 8 MB 单应用镜像预算，
  增加媒体需要重新核算 Flash 与内存。
- 不包含语音、音乐或网络路径；剧情完全从 Flash 里的转换资源包读取。
- 剧本、美术与角色素材的权利归柚子社及原发行方；本移植为非商业同人技术研究，
  固件发布的是转换后的资源包，不包含原始素材。

## 经验文档

这些发布沉淀的可复用工程经验位于
[`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。构建与贡献流程见
[`docs/README.md`](docs/README.md)、[`AGENTS.md`](AGENTS.md) 与
[`docs/contribution/`](docs/contribution/)。

## 许可

阅读器源码使用仓库的 [MIT 许可](LICENSE)。剧本、立绘、背景与角色素材的权利归
柚子社（Yuzusoft）及原发行方所有，本项目不会对其重新授权。
