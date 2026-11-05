<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 聚光灯柠檬果酱（limelight lemonade jam）—— AI Passport 阅读器

把小米手环版《limelight lemonade jam》搬到
[FoloToy AI Passport](https://ai-passport.folotoy.cn) 的**竖屏视觉小说阅读器**：
**230 个章节点、68,229 句对白、约 120 万字、991 张打包图像、8 个选项点**，
在 8 MB Flash、无 PSRAM 的 ESP32-C3 上完全离线运行。

> 音乐、伙伴，以及爱情。从遇见她的那天起，世界就变得明亮——

剧本与素材取自
[`skdkzzx/limelight-lemonade-jam-xiaomi-band10`](https://github.com/skdkzzx/limelight-lemonade-jam-xiaomi-band10)
（面向小米手环的 Vela OS 触屏应用）；本分支用 C + LVGL 重写阅读引擎，改成三键操作。
状态：**已发布** —— tag `v0.1.0-limelight`，并已投稿到 AI Passport 社区市场（审核中）。

- 分支：[`cindy/curious-babbage`](https://github.com/Shinku-Chen/ai-passport/tree/cindy/curious-babbage)
- 发布：[`v0.1.0-limelight`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-limelight)
  —— 合并镜像 `FoloToy-AI-Passport-full.bin`，**7,897,824 字节**
- 上游作品：[`skdkzzx/limelight-lemonade-jam-xiaomi-band10`](https://github.com/skdkzzx/limelight-lemonade-jam-xiaomi-band10)
  —— 本次移植所依据的手环版本。该仓库未声明许可；其美术与剧本随本分支提交并打包进固件，
  来源以上游项目标注。
- 素材工具链：
  [`tools/limelight_material_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/cindy/curious-babbage/tools/limelight_material_pack.py)、
  [`tools/limelight_script_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/cindy/curious-babbage/tools/limelight_script_pack.py)
  （打包成 `main/limelight_data/` 下的 4.49 MB + 1.81 MB），以及
  [`tools/limelight_lvgl_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/cindy/curious-babbage/tools/limelight_lvgl_font.py)
  （Noto Sans SC 的 16px 中文字体子集，3,449 个码位）。

## 三键操作

左上角显示当前章节「章-节」（例如 `7-2`），右上角是电量。短按判定 180 ms，
长按判定 500 ms；由 BSP 显式下发，而不是使用按键组件默认的 1,500 ms。

| 按键 | 正文 | 菜单 / 列表 |
| --- | --- | --- |
| 上（短按） | 下一句；打字中则立即显示整句 | 光标上移 |
| 上（长按） | 按住快进，松手即停 | — |
| 确定（短按） | 打开菜单 | 进入所选行 |
| 确定（长按） | — | 返回上一层 |
| 下（短按） | 下一句；打字中则立即显示整句 | 光标下移 |
| 下（长按） | 开关自动阅读（右下角显示「自动」；任意键停止） | — |

选项页用上 / 下选择、确定确认。菜单包含继续阅读、保存、读取、章节跳转、CG 鉴赏、
设置与返回标题。空闲 60 秒调暗、3 分钟熄屏、7 分钟深睡，任意键唤醒；自动阅读与
快进算作活动，期间屏幕保持常亮。

## 功能

- **五个手动存档槽 + 一个自动存档槽。** 自动槽在每次换幕时写入，读取时单独一行提供；
  在手动槽上长按确定可删除。分页位置属于存档状态。
- **230 个章节点。** 章节跳转列表把上游剧本里的 `[CHAPTERx-y]` 标记显示成 `X-Y`，
  正文页左上角使用同样的格式。
- **无 PSRAM 也能铺满竖屏美术。** 背景与 CG 存 240 × 214 JPEG，直接解码进画面区；
  立绘为 alpha 裁剪的 RGB565 + 1bpp 遮罩。对白带半透明，画面保持可见、文字仍可读。
- **两个打包数据共 5.9 MB。** 素材包 4.49 MiB（991 条背景、CG 与立绘），剧本包
  1.81 MiB（68,229 句，分 273 个 deflate 块）；由 ESP32-C3 ROM 的 inflater 解压，
  解压不占固件空间。
- **打包时清洗上游文本。** 104 条带内联排版指令（`%f`、`$名字$`、`#rrggbbaa`）与
  7 条混进正文的译者备注会在打包阶段剔除，对白本身一字未改。
- **串口验收通道。** `LIMEIMAGE <偏移>` 回传原始帧，`LIMEPAGE` 跳对白或按名字切页，
  `LIMEAUTO on|off` 开关自动阅读，`LIMEBTN` 读按键 ADC 电压；每次亮度变化都带空闲
  毫秒数记日志，「自动阅读不调暗」靠它验证。

## 构建与刷写

```bash
./tools/validate.sh --firmware   # 构建、合并并校验 0x0 镜像
idf.py build                     # 可选的增量应用构建
# 用网页刷机工具或 esptool 从 0x0 刷入 build/FoloToy-AI-Passport-full.bin
```

打包脚本需要 Pillow 与 FreeType（ESP-IDF 使用的 Python 环境）。只跑 `idf.py build`
不会生成或校验合并镜像。使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)
或 `esptool`；目标板卡为 8 MB Flash 的 AI Passport。

## 发布

`v0.1.0-limelight` 提供 tag 触发的 CI 合并镜像 `FoloToy-AI-Passport-full.bin`
（7,897,824 字节）。两份打包数据提交在 `main/limelight_data/` 下，普通 checkout
即可构建完整本篇。

## 已知限制

- 本固件是纯文字/美术阅读器：没有语音、音乐或网络路径；剧情完全从 Flash 里的
  打包数据读取。
- 正文字体固定为 16px Noto Sans SC 子集（每行 13 个全角字、每页 5 行），没有
  20px 正文字号选项。
- 目标板卡没有 PSRAM。5.9 MB 打包数据与 240 × 214 画面区按 8 MB 单应用镜像预算；
  增加大体积媒体需要重新核算内存与分区。
- 上游美术与剧本未声明许可，仅作为标注来源的同人转换随分支提供；下方 MIT 许可
  覆盖阅读器代码，不覆盖第三方剧情与美术。Noto Sans SC 子集使用 OFL-1.1。

## 经验文档

这些发布沉淀的可复用工程经验位于
[`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。构建与贡献流程见
[`docs/README.md`](docs/README.md)、[`AGENTS.md`](AGENTS.md) 与
[`docs/contribution/`](docs/contribution/)。

## 许可

阅读器源码使用仓库的 [MIT 许可](LICENSE)。剧情、美术与角色素材的权利归原作者所有，
本项目不会对其重新授权。
