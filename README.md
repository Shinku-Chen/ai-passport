<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 9-nine- 雪色雪花雪之痕 · AI Passport

《9-nine- 雪色雪花雪之痕》（Episode 4）的 AI Passport 离线阅读器：
**320×240 横屏、三键、完全离线**。固件内置第 4 部全部正文、全部分支与结局，
不需要网络、账号或 PC 运行时。

- 上游手表工程：<https://github.com/xuanyide01/9-nine-rw5>
- 固定提交：`6a8907a5ae576f4377abe4b74f15bd9af13efe07`
- 发布源码分支：`release/nine-yukiiro-20261125`（基于 `feature/galgame-framework`
  `b9909c13605413142fb35f28aabcc494a666fa00`）
- 详细说明、预算与已知限制：
  [English](docs/reference/shinku-chen/nine-yukiiro/README.md) ·
  [简体中文](docs/reference/shinku-chen/nine-yukiiro/README.zh_CN.md)

## 内容与范围

| 项目 | 数量 |
| --- | --- |
| graph 节点 / 正文帧 | 212 / 12,665（另有 1 条 OP 标记） |
| 章节 | 23 |
| 选项节点 / 选项记录 | 47 / 70（63 条在源条件门控下可达，7 条结构上不可达） |
| 结局记录 | 2（`loop1` 中盘完成、`true` 终止） |
| CG 组 / 变体 | 19 / 108 |
| 运行期变量 | 78 |
| 正文 | 668,498 字节 UTF-8，84 个 zlib 块 |

素材来自上游手表工程的已加工 graph、正文分片、背景/CG、立绘/表情、标题
主视觉、OP 与两首 BGM；**不重建未提交的 PC 原作脚本**。PC 版的语音与
R18 内容不进入固件（手表源本身已是非 R18、零语音）。

## 操作（三键）

- **短按 OK / UP / DOWN**：推进正文。当前长句还有下一页时推进**下一个完整
  分页**，否则下一句；即使正在打字或自动阅读，也立即推进并整页显示目标，
  不会只“补全字”或只退出自动。
- **长按 OK**：正文页或选项页进入游戏内菜单；二级页面返回。菜单中的存读档、设置、历史返回后恢复原正文分页或完整选项及原选择位置；菜单动作不推进 VM，也不授予永久进度。
- **长按 UP**：达到长按阈值立即进入下一句，跳过当前句剩余分页与打字/自动等待；持续按住时每个输入 tick 最多不过一句，没有额外 90/120ms 累积；松手立即停。到选项/结局/片头停住，不替玩家选择。正常短按仍逐完整分页。
- **长按 DOWN**：自动阅读开关；松手不会多走一句。
- **鉴赏室 → OP 播放**：短按 OK 播放真实 OP 画面与片头音轨；可短按 OK 跳过，播完回到同一 OP 行。鉴赏室播放不推进剧情 VM，不写自动/手动存档或永久进度，也不解锁结局/CG。
- 长按松手不会额外补一次短按。章节试读不写自动进度或永久进度。

## 构建

ESP-IDF 5.5.3、ESP32-C3、8 MB Flash、无 PSRAM：

```bash
source <ESP-IDF-v5.5.3>/export.sh
idf.py build
idf.py merge-bin -o "$PWD/build/FoloToy-AI-Passport-full.bin"
python3 tools/verify_firmware.py build
```

完整宿主检查（源目录只读，可放在仓库外）：

```bash
NINE_SOURCE=<固定-9-nine-rw5-源目录> CC=<宿主C编译器> ./tools/validate.sh --static
```

`NINE_SOURCE` 启用源到包审计和源 JS/C VM 轨迹差分；未设置时这两项为 SKIP，
其余包、应用、图像和字库测试照常执行。

## 验证状态

- Build：PASS（ESP-IDF 5.5.3，merged image 通过 layout 校验）
- Host tests：PASS（源到包审计 0 错误；36 条轨迹 / 457,632 步 / 0 失败；
  生产状态机、严格选项/菜单返回回归、真实图像解码与源美术参考合成、真实 32 位 LVGL 19 页渲染（含鉴赏室 OP 入口）、
  字库覆盖）
- Device tests：**LIMITED USER PASS**（2026-11-24，E 固件：姓名/正文布局；完整鉴赏 OP 播放、声音与返回；快进松手停、存读档、字号分页）
- Unverified：全部路线/结局、长稳、掉电与 NVS 持久性、所有硬件项；不把有限真机验收扩大为全量通过。

软件通过不等于全部硬件通过；上列只是已测范围。

## 权利说明

原作：Palette《9-nine-》第 4 部。手表移植工程及素材来自 `xuanyide01` 的
固定提交仓库；本移植的阅读器、打包器、C 解码器、测试与文档为本 fork 新代码。
上游代码适用其 MIT 许可；游戏美术、剧本与音频权利归各自权利人。
