<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 生字卡片识记（Shengzi Cards）

汉字闪卡识记应用：滚动字卡、自测认识 / 不认识、看拼音猜字三种模式，已认识的标记持久化到 NVS。
最新版：**v1.0.0**。

- 分支：[`feature/shengzi-cards`](https://github.com/Shinku-Chen/ai-passport/tree/feature/shengzi-cards)
- 发布：[v1.0.0](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.0)

## 操作

三个按键驱动全部操作；底部信息行显示当前模式、字卡序号和已认识数量。

- **长按上 / 下**：切换模式 `BROWSE` → `TEST` → `SPELL`。
- **BROWSE（浏览）**：字卡同时显示汉字与拼音，**上**回到上一张字卡、**下**随机跳到另一张。
- **TEST（自测）**：拼音先隐藏，**OK** 揭晓；揭晓后**上**标记为认识、**下**标记为不认识，
  两个选择都会随机推进到下一张字卡。
- **SPELL（拼读）**：拼音同样先隐藏，**OK** 揭晓；再按一次 **OK** 随机推进到下一张字卡。

## 说明

- 本分支只承载一个应用：`feature/*` 分支的根 README 只介绍本分支自己的应用，全部项目的目录在 fork `main` 的根 README。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` —— 每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。目标板卡：8 MB Flash。
