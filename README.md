<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 爱上火车 · 二六线 —— FoloToy AI Passport 离线阅读版

把《爱上火车》二六线的 RW5 手环移植（[`xuanyide01/maitetsu-rw5`](https://github.com/xuanyide01/maitetsu-rw5)，
固定提交 `9c008406853affe973ceb80f62128696054365e7`）搬到 FoloToy AI Passport 的
**横屏 320 × 240** 三键阅读器上：四章、1,537 段、675 个场景、6 组 35 张 CG，
全程离线，另带完整 283.88 秒标题曲。

本仓库是 [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的个人 fork；
本分支（`release/maitetsu-20261125`）只承载这一部作品，板卡事实、BSP 与开发流程仍来自上游，
见 [`AGENTS.md`](AGENTS.md) 与 [`docs/README.md`](docs/README.md)。

## 范围

- 只包含 RW5 移植里的**二六线四章**：正和四十一年·小仓、御一夜旅行、小仓风暴、尾声。
  这不是原作的全部路线，也不代表原作其他角色线。
- 原作的选项机制在本线没有出现，所以本作是**线性剧情**：1,537 段对白连成一个结局。
- 文本、图像、标志与音乐的权利归 Lose / HIKARI FIELD 及各权利人所有；本仓库的 MIT
  只覆盖自编代码。详见 [`docs/development/engineering/maitetsu-port.md`](docs/development/engineering/maitetsu-port.md)。

## 操作（三键，横屏）

| 页面 | 短按 OK / 上 / 下 | 长按 OK | 长按上 | 长按下 |
| --- | --- | --- | --- | --- |
| 阅读 | 推进正文（逐字显示中同一次按键补全当前页并进入下一句；长句按分页单元推进） | 游戏内菜单 | 立即跳到下一句，按住持续（每拍一句），松手停 | 自动阅读开 / 关 |
| 列表 / 菜单 / 设置 | 上下移动，OK 执行 | 返回 | — | — |

- 快进跳到下一句（跳过当前句剩余分页），不等打字与自动阅读；到选项或结局页面自动停住。
- 自动阅读开启时，短按仍然推进（不会把这次按键吞掉）。
- 章节鉴赏与 CG 回想都是**试读**：入口即标记 preview，即使从第一章试读读到结尾、
  中途存读档或重启，也不会授永久结局；只有「开始游戏」的正式周目读完全部四章才记录通关。

## 功能

- 19 个逻辑屏的 GalGame 框架：标题、对话、菜单、3 个手动存档、自动进度、历史、
  设置、鉴赏室（CG / 回想 / 章节 / 音乐）、过场、结局、关于。
- 场景按源图层合成：背景 + 已与背景合成的立绘补丁（透明与表情在打包时处理），
  320 × 180 精确 16:9，不裁剪构图。
- CG 鉴赏 35 张分页显示（8 张一页），超过旧框架 32 位 CG 上限的 33..35 也能看到。
- 标题曲为原作完整 283.88 秒、4 kHz 单声道源的 4 位 IMA ADPCM（586 KB），
  播放时线性插值到 16 kHz；不截短、不用方波替代。

## 构建

需要 ESP-IDF **5.5.3**、Python 3（生成资源时另需 Pillow）：

```bash
source <ESP-IDF-v5.5.3>/export.sh
idf.py build
```

资源包（6.3 MB → 当前 5.8 MB 条带 JPEG）与标题曲已经提交在
`assets/maitetsu/`；只改代码时不需要重新生成。重新生成需要固定的 RW5 源快照：

```bash
python tools/maitetsu_assets.py --source <maitetsu-rw5>      # 生成资源与内容表
python tools/maitetsu_assets.py --source <maitetsu-rw5> --check
```

## 验证状态

- 构建：ESP-IDF 5.5.3 `idf.py build` 通过；app 7,759,632 字节 / 8,323,072 字节分区。
- 主机测试：内容对账（1,537 段）、条带解码（3,994 条带）、真实 LVGL 解码器集成、
  场景合成采样、三键 / 快进 / 自动 / 试读状态机、存读档与解锁格式。
- 设备测试：**本轮未跑**（未获得刷写授权）。横屏、真机按键与音频仍需实机确认。

完整证据、预算与未验证项见
[`docs/development/engineering/maitetsu-port.md`](docs/development/engineering/maitetsu-port.md)。
