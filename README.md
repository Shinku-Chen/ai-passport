<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 生字卡片识记（Shengzi Cards）

汉字闪卡识记应用。三种模式 —— **浏览（Browse）**：滚动字卡；**自测（Self-test）**：
标记每个字认识/不认识；**拼读（Spell）**：看拼音猜字。**OK 短按**揭晓答案，已认识
标记持久化到 NVS。最新版：**v1.0.0**。

- 分支：[`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards)
- 发布：[v1.0.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.0)

## 说明

- 本分支只承载一个应用：`feature/*` 分支的根 README 只介绍本分支自己的应用，全部项目的目录在 fork `main` 的根 README。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` —— 每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。目标板卡：8 MB Flash。
- 这些发布沉淀的可复用工程经验位于 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。
