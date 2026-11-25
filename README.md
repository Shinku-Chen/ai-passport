<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 《9-nine- 天色天歌天籁音》 · AI Passport 离线阅读器

《9-nine- Episode 2》的中文剧情、美术、音乐与完整片头，装进一台可以随身携带的
三键阅读器。启动后无需联网，剧情、存档、鉴赏室和片头全部来自本机资源。

本分支**只包含这一部作品**。发布版本为 `v1.0.0-nine-sorairo`。

## 故事与玩法

故事围绕新海天与伙伴们展开。阅读时用三个实体按键推进对白，遇到分支用上下选择、
OK 确认；长句会先翻分页，不会因为一次按键丢掉整句。随时可以从游戏内菜单保存到
三个存档位，也可以读取旧进度、回溯历史文本、切换字号和分页方式。

鉴赏室把 CG、剧情回想、章节目录、音乐和「OP 鉴赏」集中在一起。OP 鉴赏不需要
通关即可播放完整片头，播放结束后回到同一行；浏览鉴赏室时保持安静，回到标题页会
恢复标题音乐。

## 内容

| 项目 | 数量 |
| --- | ---: |
| 剧情节点 | 56 |
| 正文帧 | 10,385 |
| 章节 | 21 |
| 选项组 / 选项 | 4 / 7 |
| 结局元数据 | 2 |
| 图像 | 1,102 |
| CG 组 / 差分 | 22 / 72 |
| OP | 623 帧 @5 fps，124.66 秒 |
| 音频 | 12 kHz 单声道 IMA ADPCM，标题与 OP 两轨 |

## 操作

| 页面 | 短按 | 长按 |
| --- | --- | --- |
| 阅读 | OK / 上 / 下立即推进一个完整单元：长句先翻下一页，否则下一句 | OK 打开菜单；下 自动阅读；按住上快进，松手立即停止 |
| 选项 | 上/下选择，OK 确认 | OK 打开菜单；返回时保留完整选项与选择位置 |
| 游戏内菜单 | 上/下选择，OK 执行 | OK 返回剧情 |
| 保存 / 读取 | 上/下选三个存档位，OK 保存或读取 | OK 返回 |
| 设置 | 上/下选行，OK 切换 | OK 返回 |
| 鉴赏室 / CG / 章节 / 音乐 / OP / 关于 | 上/下选择或滚动，OK 进入 | OK 返回上一级 |

标题菜单包含「开始新游戏」「继续阅读」「读取存档」「设置」「鉴赏室」「关于」。
OP 鉴赏从鉴赏室的独立行进入；自然播完、短按或长按都会回到鉴赏室。

## 构建与复现

需要 ESP-IDF 5.5.3、Python 3 以及 Pillow、fontTools。所有图像和字体都由固定上游
（`https://github.com/xuanyide01/9-nine-rw5`，提交
`6a8907a5ae576f4377abe4b74f15bd9af13efe07`）生成：

```bash
python tools/nine_pack.py \
  --source /path/to/9-nine-rw5 \
  --out assets/nine \
  --jpeg-quality 42 --op-fps 5 --audio-rate 12000 \
  --expect-commit 6a8907a5ae576f4377abe4b74f15bd9af13efe07

python tools/nine_font.py \
  --font /path/to/9-nine-rw5/nine-sorairo-rw5/src/common/nine-text.otf \
  --out assets/nine --source-root main

./tools/validate.sh --static
./tools/validate.sh --firmware   # 需先激活 ESP-IDF 5.5.3
```

构建会由 `tools/nine_container.py` 生成 `nine_assets.bin`，再合并为
`FoloToy-AI-Passport-full.bin`。不要用只做增量编译的 `idf.py build` 代替完整固件。

## 当前验证范围

- **Build: PASS**（ESP-IDF 5.5.3）。
- **Host tests: PASS**（pack/VM、分支条件、按键时序、存档、音频、图像、字体与源轨迹差分）。
- **Device 已测**: `9a06ff27…` 完整鉴赏 OP 的**画面、声音、播完返回** USER PASS；
  快进松手即停、存读档、字号与分页 USER PASS。
- **未验证**: 所有分支和结局的完整实机遍历、长时间/掉电/休眠稳定性，以及未逐项
  复测的其它页面；不把 Host 或局部实机通过扩大为整机全功能保证。

在更早的 `9f985c49…` 候选上曾观察到 OP 音频卡顿；该旧失败记录保留在开发归档中，
不代表本次 `9a06ff27…` 的已测 OP 结果。

## 资源与产物

| 项目 | 实际 |
| --- | ---: |
| `nine_script.bin` | 737,234 B |
| `nine_images.bin` | 4,366,920 B |
| script + images | 5,104,154 B |
| `nine_assets.bin`（构建生成） | 5,104,264 B |
| assets 分区 | 6,225,920 B（占用 82.0%） |
| factory app | 1,474,928 / 2,097,152 B（占用 70.3%） |
| 合并整机镜像 | 7,266,952 B |
| DRAM（链接时） | 231,106 / 321,296 B（剩余 90,190 B） |
| 画面画布 | 240×135 RGB565，5 条 240×27 带，单块 12,960 B；显示为 320×180 |

发布产物 SHA-256：

```text
full merged .bin   9a06ff27a9798137afa1684b89ca45305fcfccea9852c9d88c2f8df28cb6d007
app .bin           2386bf86d5bdebd0d6517c30ed6d881ee0526aacc4ec6cc92a6edee93310ab1b
ELF                2bc20795791e233b0cf7e17c6ec31775c0c7a26b994fe066d761f0c21000c66e
MAP                19f00fe400f281a7334c61b57d07a0216660084997843d0b9b1627d386701c6f
nine_script.bin    e25d48f371a43a60bcc4df2db51c9caaf923e99efaf2df8abcb99e65503e1f8c
nine_images.bin    c05fe781a006d8656d4b66c7f854761a3747f293ce519d27fb2456ef795dec28
```

## 来源与归属

- 剧情与素材加工来源：`https://github.com/xuanyide01/9-nine-rw5`，提交
  `6a8907a5ae576f4377abe4b74f15bd9af13efe07`。
- 固件源码发布分支：`https://github.com/Shinku-Chen/ai-passport/tree/release/nine-sorairo-20261125`。
- 上游固件项目：`https://github.com/FoloToy/ai-passport`。
- 源仓库代码为 MIT；游戏文本、美术、音乐版权归 **Palette / 9-nine- Project**。
  资源仅用于个人离线阅读，本仓库不授予再分发权。

## 文档

- [移植与实现说明（中文）](docs/games/nine-sorairo.zh_CN.md)
- [Port and implementation notes (English)](docs/games/nine-sorairo.md)
- [开发与构建约定](AGENTS.md)

Device 结果只适用于上述精确固件及其已测范围；UI、存档、媒体和故障路径的代码契约
仍以仓库内的实现说明与测试为准。
