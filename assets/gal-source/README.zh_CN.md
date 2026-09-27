<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 上游游戏素材

阅读器打包进 `main/limelight_data/` 的美术与剧本源文件。放在仓库里,是为了让打包器能在
一次普通 checkout 之后重建随固件发布的资源包。

## 来源

- **上游**：[`hezdaaa/limelight-lemonade-jam-miband`](https://github.com/hezdaaa/limelight-lemonade-jam-miband)
  —— 商业视觉小说《limelight lemonade jam》的小米手环移植版。
  `skdkzzx/limelight-lemonade-jam-xiaomi-band10` 是它的 fork，其剧本文件与本副本逐字节一致。
- **目录结构**：保持上游 `src/common/` 原样。`script/` 是 137 个 `scriptDataN.txt`
  （共 68,229 条对白），`bcgi/` 是背景，`cimg/` 是人物立绘，`evig/` 是事件 CG，
  标题页用的 `title_bg*.jpg` 也在同一层。另外收了 `gallery_cgs.txt`：上游鉴赏页引用的
  633 张事件 CG 名单，让打包器不必读那个快应用页面就能区分"剧情 CG"与"只在鉴赏里出现的 CG"。
  手环自带的 6 个小图标（`back.png`、`bt.png`、`go.png`、`hd.png`、`logo.png`、`reset.png`）
  没有收进来：本阅读器不绘制它们；**快应用代码一律不入库**。
- **许可**：上游仓库未声明许可。原作是 **SAGA PLANETS** 的《limelight lemonade jam》；
  这份副本只用于让 fork 可重建、可复核，版权与署名归原作者与上述两个移植版。

## 重建资源包

```bash
python tools/limelight_material_pack.py --source assets/gal-source --out main/limelight_data/limelight_pack.bin
python tools/limelight_script_pack.py --source assets/gal-source --out main/limelight_data/limelight_script.bin
python tools/limelight_lvgl_font.py --font <NotoSansSC-Regular.otf> \
    --pack main/limelight_data/limelight_script.bin --out-dir assets/fonts
```

素材包是确定性的：从本目录重建能**逐字节复现**已入库的 `limelight_pack.bin`。
字体需要另外提供 Noto Sans SC（SIL OFL 1.1）源文件，所以仓库里只提交生成结果。
