<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 沙耶之歌 —— AI Passport 视觉小说阅读器

本项目把小米手环 10 上的《沙耶之歌》同人移植
（[`liuyuze61/Saya-miband10`](https://github.com/liuyuze61/Saya-miband10)）搬到 AI Passport：
**44 章、3,828 段对白、3 个结局**，全程离线。手环版是竖屏触控应用，本分支用 C + LVGL
重写阅读引擎，改由三个按键操作。

- 分支：[`feature/saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/tree/feature/saya-no-uta)

## 版面

```text
┌──────────────────────────────────┐  横屏 320 x 240
│  画面区 320 x 150                │  背景 JPEG + 立绘
│                          [电量]  │  电量贴在右上角
│  说话人名字(左下角)              │  半透明标签,文本框正上方
├──────────────────────────────────┤
│  正文最多 4 行                   │  文本框 82 px,底部留 8 px 边距
└──────────────────────────────────┘  （16px 字号：一行 19 个全角字）
                                        （20px 字号：15 字、3 行）
```

## 操作

- **开机警告页**：上、下滚动正文（短按滚一行，长按整屏），**读到最后**提示才会变成
  「按确定继续阅读」；没读完按确定只会往下翻一屏，不会直接进游戏。
- **标题 / 列表页**：上、下移动光标（到顶 / 到底就停住，不绕圈），**确定**进入，
  **长按确定**返回。
- **正文页**：**确定**打开菜单；**上**短按 = 下一段（打字过程中按一下先补全），
  **上**长按 1 秒以上 = 快进（每 180 ms 一段），松手即停；**下**短按 = 回看上一页。
- **选项页**：上、下选择，**确定**确认。
- **菜单**：保存、读取、跳过章节、返回标题、关闭菜单。跳过章节时，若本章后面还有
  未遇到的选项，会停在那个选项上让你选，不会替你决定。
- **设置**：文字速度（慢 / 中 / 快 / 瞬间）、字号（小 16px / 大 20px）、关于、返回。
- **关于页**：上、下滚动正文（短按滚一行，长按滚四行），**确定**返回。
- **存档**：5 个手动存档位 + 1 个自动位（每次换场景写入），标题页的“继续”从自动位接上；
  保存模式下在槽位上**长按确定**可删除该存档。

空闲策略：60 秒调暗背光，3 分钟熄屏，7 分钟进入 deep sleep；按任意键唤醒并从自动存档继续。

## 离线数据管线

固件运行时不联网、不下载任何东西。两个工具负责生成全部素材，产物已提交进仓库，
因此普通 checkout 即可直接编译：

| 步骤 | 工具 | 产物 |
| --- | --- | --- |
| 剧本 + 图片 | `tools/saya_pack.py` | `main/saya_data/saya_pack.bin`（约 2.8 MB）：44 章、473 场景、3,828 段对白、193 张背景、75 张立绘，外加来源元数据 |
| 字体子集 | `tools/saya_font.py` | `assets/fonts/saya_cjk_16.c`、`saya_cjk_20.c`，以及字符清单 `assets/fonts/saya_cjk_symbols.txt` |

资源包直接从 Flash 读取：运行时不解析 JSON、不做解压。背景在打包时已裁成 320 × 150 的
JPEG；立绘按屏幕高度缩放、裁到画面区可见部分，存成 JPEG + 1bpp 遮罩。固件只在
（背景, 立绘）真正变化时才解码一次画面，并把立绘按遮罩合进 320 × 150 的 RGB565 画布。

重新生成需要源移植仓库的 checkout 与一个许可允许分发的 CJK 字体，具体命令见两个工具的
文件头注释。字体由仓库内的小生成器产出，不用 `lv_font_conv` —— 该工具最后一版在当前 Node.js
下写出的字形位图是坏的（同样输入、加不加 `--no-prefilter` 产出字节完全相同，按 LVGL 的
PLAIN 4bpp 读法解不出字形）；生成器会把写出的 C 文件回读、逐像素与栅格化结果比对。
字体来源与许可记录在 [`assets/README.md`](assets/README.md)。

Flash 预算（ESP-IDF 5.5，应用分区 8,323,072 字节）：community 变体应用约 4.3 MB，分区剩余 46%；
含补丁的 release 变体约 4.6 MB，剩余 43%。

## 变体与发布规则

仓库里提交的 `main/saya_data/saya_pack.bin` 与 `assets/fonts/saya_cjk_*.c` 是
**community 变体**：不含源移植仓库 `补丁/` 里的任何内容（7 个加长章节 + 22 张 R18 CG）。
发布到 AI Passport 社区市场的固件只能用它。

自用的 **release 变体**才带补丁，只在本机刷机使用：

```bash
python tools/saya_pack.py --source <Saya-miband10 checkout> \
    --patch <checkout>/补丁 --commit <commit> --out build/release/saya_pack.bin
SDKCONFIG_DEFAULTS=sdkconfig.defaults idf.py -B build/release/idf \
    -D SAYA_PACK_FILE=build/release/saya_pack.bin build
```

字体不分变体：`assets/fonts` 里的子集是两个变体字符的并集（含补丁正文用到的字形），
两个固件共用它。补丁换新后如果引入了新字，需要用两个 pack 一起重生成字体：

```bash
python tools/saya_font.py --font <NotoSansSC-Regular.otf> \
    --pack main/saya_data/saya_pack.bin --pack build/release/saya_pack.bin \
    --out-dir assets/fonts
```

`build/` 已被 `.gitignore` 忽略：补丁版 pack 以及用它们构建的固件都
**不得提交、不得发布到社区**。字体是两变体的并集（只含字形，不含剧情内容），
已随仓库提交；它的字符清单 `assets/fonts/saya_cjk_symbols.txt` 会记住补丁用过的字形，
所以之后只用社区 pack 重生成也不会丢字。

## 发布

- **GitHub Release**：[`v0.1.0-saya-no-uta`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-saya-no-uta)
  （显示名「沙耶之歌 (Saya no Uta) v0.1.0」），挂两份附件：`FoloToy-AI-Passport-full.bin`
  （tag 触发的 CI 构建，community 变体）与 `FoloToy-AI-Passport-full-patched-r18.bin`
  （同一 commit 的本地构建，含补丁，仅自用）。
- **社区市场**：以「沙耶之歌(社区版) / Saya no Uta (Community)」提交，上传的是不含补丁的
  community 合并镜像；发布信息与简介原文记录在
  [`docs/reference/shinku-chen/saya-no-uta/`](docs/reference/shinku-chen/saya-no-uta/README.zh_CN.md)。
- 发布流程与检查项见 [`docs/development/release/publish-to-community.md`](docs/development/release/publish-to-community.md)。

## 说明

- **内容**：剧情含大量血腥描写，应用保留了源移植的首次启动内容警告页。
- **版权**：《沙耶之歌》是 Nitroplus 的商业作品。本固件是个人同人移植；美术素材与中文译文
  来自公开的小米手环移植项目，项目与应用内的免责声明都请读者支持正版。请勿把生成的资源包
  当作自己的素材再分发。
- **无音频**：源移植没有音频素材，本移植也不添加。
- **基线 demo**：仓库的硬件自检菜单与 `demo_*.c` 仍保留在 `main/`，但不参与本应用的构建；
  相关约定见仓库的 [AI 指南](docs/development/ai-guide.md) 与 [fork 指南](docs/fork-guide.md)。

## 构建与验证

```bash
./tools/validate.sh --static     # 仓库检查、宿主机测试、字体覆盖度
./tools/validate.sh --firmware    # ESP-IDF 构建 + 合并镜像校验
```

宿主机测试 `tests/test_saya_model.c` 用真实资源包跑阅读逻辑（章节图、三个结局可达性、
分页往返、存档编解码）；`tools/saya_font.py --check` 会在界面文案或剧本出现字体子集没覆盖的
字时让门禁失败。
