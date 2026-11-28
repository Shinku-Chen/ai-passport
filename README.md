<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# Galgame 阅读器框架（pending/galgame-framework）

这是 AI Passport 上多部视觉小说阅读器共用的框架分支，不是一部可直接刷写的作品。
它把三键阅读器的页面、交互、分页、存档与资源打包约定收在一处，供各作品分支复用。

## 提供什么

- **统一三键语义**：短按 OK / UP / DOWN 推进或补全文字，长按 OK 打开菜单，长按 UP
  快进，长按 DOWN 开关自动阅读；列表与选项使用短按移动 / 确认。
- **分页与对话框**：固定像素宽度排版，按字号换算全角 / 半角单位；对话框贴底、半透明，
  页码不压正文。
- **存档与进度**：多个手动槽 + 自动槽，运行状态与永久通关记录分开。
- **离线资源包**：剧本、图像与字体打包进应用分区，clone 后即可构建，不依赖网络或 SD 卡。
- **分带渲染**：按屏幕水平分带解码，内存不随整屏尺寸增长；JPEG 走 ROM 解码器，
  立绘与叠加层使用 RGB565 + 1 bpp 遮罩。
- **标准页面**：菜单、存读档、历史、章节、鉴赏、设置与关于页。

## 适用作品

12 部 v1.0.0 批次移植共用本框架；`galgod` 与 `manosaba` 自带阅读 / UI 层。
后续的 2026-11-25 批次也沿用同一套页面、按键与持久化契约。

## 文档

- [Galgame 阅读器移植经验](docs/reference/shinku-chen/galgame-port/README.zh_CN.md)
  —— 共同需求、框架能力、12 条只在真机上暴露的缺陷，以及发版前验收清单。
- [上游 Galgame 经验档案](https://github.com/FoloToy/ai-passport/tree/main/docs/reference/shinku-chen)
  —— 后续八部移植的源固定、试读隔离、分带预算与剧情图对账。

## 状态

本分支为 `pending/*`，尚未作为独立应用发布到社区。它是开发基线，不提供可直接烧录的
成品固件；具体作品请使用对应的 `feature/*` 分支。

## 上游

- 仓库：[`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport)
- 分支基线：`upstream/main`，按 fork 的 `feature/*` / `pending/*` 约定维护。

