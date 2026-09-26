<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 飞鸟会长不肯认输（Asunabi）

从小米手环版本移植到 AI Passport 的竖屏视觉小说：**30 章、4649 句对白、约 9.4 万字**，
从头读到唯一结局。

这是建立在 `feature/asunabi-galgame` 分支上的应用。固件开机直接进入标题画面（没有 demo 菜单）。

## 功能

- **阅读** —— 确定键推进一句；若该句正在逐字显示，则先立即全显。超出面板的长句会**翻页**而不是被裁掉。
- **标题画面** —— 开始新游戏、从存档位置继续、跳到任意章节，或进入设置。
- **阅读菜单** —— 继续阅读、保存、读取、跳过本章、设置、返回标题。
- **存档** —— 6 个手动存档槽，位置包含「分页中的第几页」；在存档槽上长按确定键删除。
  最后一次位置也会自动记住，「继续阅读」即从这里接着读。
- **设置** —— 文字速度（慢 / 中 / 快 / 瞬间）、文字大小（16px 或 20px，带实时打字预览）、自动阅读。
- **结局画面** —— 全部只有一个结局；读完后会清掉继续点，并提示返回标题。

## 交互

三个按键驱动全部操作。右上角显示电量百分比；读不到电量计时降级显示 `--%`。

| 按键 | 阅读中 | 菜单中 |
| --- | --- | --- |
| UP 短按 | 下一句，或立即全显当前句 | 选中项上移 |
| UP 长按 | 按住快进，松手即停 | — |
| OK 短按 | 呼出菜单 | 执行选中项 |
| OK 长按 | — | 退出菜单 |
| DOWN 短按 | 滚动超出面板的文字 | 选中项下移 |
| DOWN 长按 | 隐藏面板看画面 | — |

## 素材

美术与章节剧本属于**第三方内容，本仓库不跟踪**。它们放在本地、已 git 忽略的
`assets/gal-source/` 目录里，构建时通过 [`tools/gal/`](tools/gal/README.md) 打包进一个
独立的 4 MiB `assets` 数据分区。那个工具链才是本分支可复用的部分：一套有文档的包格式、
带可视预览的打包器、包检查器，以及 CJK 字体子集生成器。

构建或发版前有两点需要知道：

- 没有 `assets/gal-source/` 的 clone 依然能配置、构建并启动 —— 打包器会改为生成占位包。
  该固件显示的是占位剧本、没有美术。
- **因此 CI 构建出的 release 不含本作品。** 请用本地构建的合并镜像发版，
  不要用 tag 触发的 CI 产物。

## 固件 / 构建

本分支用 galgame 替代了 demo 菜单：`main/gal/`（包读取与排版 model、内存映射素材层、
NVS 存档、按键驱动的界面）、改写过的 `main/main.c`、一个独立的 `assets` 数据分区
（4 MiB，data 子类型 `0x40`），以及提交在 `assets/fonts/` 下的两个 CJK 字体子集。

打包真实美术需要 ESP-IDF 构建所用的解释器里装有 Pillow：

```bash
"$IDF_PYTHON_ENV_PATH/Scripts/python.exe" -m pip install Pillow   # Windows
python -m pip install Pillow                                       # Linux/macOS
```

然后校验合并镜像（发版应交付这个）：

```bash
./tools/validate.sh --firmware      # -> build/FoloToy-AI-Passport-full.bin
```

`./tools/validate.sh --static` 会跑 host 测试，其中覆盖包读取、推进规则与分页。

## 来源

- **分支**：[`feature/asunabi-galgame`](https://github.com/Shinku-Chen/ai-passport/tree/feature/asunabi-galgame)
- 素材工具链：[`tools/gal/README.md`](tools/gal/README.md)
