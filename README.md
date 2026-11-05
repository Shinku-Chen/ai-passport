<p align="right"><strong>简体中文</strong> · <a href="README.en_US.md">English</a></p>

# 死馆 GalGame

把《死に逝く君、館に芽吹く憎悪》的 RW5 移植内容带到 AI Passport，离线横向阅读故事、选择分支，随时保存进度并回看已解锁的 CG。

![官方包装封面](assets/images/shinuyakata-cover.png)

当前版本：**0.1.3-shinuyakata**。本分支仅维护《死馆》阅读器；[其他应用目录](https://github.com/Shinku-Chen/ai-passport/tree/main)。

## 下载与安装

- [Release：v0.1.3-shinuyakata](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.3-shinuyakata)
- [完整固件 FoloToy-AI-Passport-full.bin](https://github.com/Shinku-Chen/ai-passport/releases/download/v0.1.3-shinuyakata/FoloToy-AI-Passport-full.bin)
- [官方刷机页](https://ai-passport.folotoy.cn/tools/web-flasher/)

首次安装使用完整固件，从 `0x0` 烧录。完整烧录可能清除原有数据；开发版升级需保留存档时，应按[固件布局与数据策略](docs/development/engineering/firmware-layout.zh_CN.md)使用兼容的分段应用镜像。旧竖屏或较高对白框的存档会保留事件和路线变量，从当前句开头重新分页。

## 阅读与操作

- 标题页用上/下选择，确定进入“开始游戏”或“继续游戏”。无需联网配置。
- 正文短按确定/上/下：打字中先补全，再按翻页。
- 长按确定打开菜单；长按上立即快进，持续跨章，松手或遇到选项、结局停止。
- 长按下切换自动阅读；自动模式保持常亮，等待选项时也不休眠。动作键先退出自动阅读。
- 菜单中可保存到三个手动栏位、读档、查看历史、调整字号、文字速度及对白框透明度；自动进度用于继续游戏。
- 鉴赏室提供已解锁 CG、回想和章节入口。CG 查看时上/下切换差分，确定返回。

姓名底板随文字宽度伸缩，80 像素对白框支持两档字体，每页三行。包含源数据的 198 个运行页面、17,409 条正文记录、11 组选项，以及 56 组 / 311 张 CG 差分。

## 内容与验证范围

本作含成人、暴力与惊悚内容。移植源没有提供语音、BGM 或 OP 视频；本版本为静音阅读。保留源数据中未解析/不可达的脚本节点，未独立承诺原商业游戏完整性，详见[阅读器说明](docs/assets/shinuyakata.zh_CN.md)。

Build、Host tests：PASS。0.1.3 已实机确认阅读/鉴赏、姓名底板、较矮对白框和即时快进可用。长时间运行、穷举全部路线与休眠唤醒仍未完成完整回归。发布固件使用已验证本地镜像，SHA-256：

`4f3b0d6777eb464c3114078f68b024d45ff19a60ceaf4ae06b9695f936f47318`

## 构建与来源

使用 ESP-IDF 5.5.3，在仓库根目录执行：

```bash
./tools/validate.sh
```

生成并校验 `build/FoloToy-AI-Passport-full.bin`。已保留生成数据、字体子集和固定版本素材；普通构建无需重新导入原始素材。重新转换时参阅[离线重建说明](docs/assets/shinuyakata.zh_CN.md#离线重建)。

- 基座：[GalGame 框架](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework)，`7f04b554`。
- 内容来源：[xuanyide01/shinuyakata-rw5](https://github.com/xuanyide01/shinuyakata-rw5)，固定提交 `76bba0bb0bfcc066ce6a35883d76c5b19981e0cb`。
- 原作与美术：BUG SYSTEM / CLEARRAVE。封面使用发行渠道的官方包装图；原作素材权利归各权利人。
- 源移植自编代码：MIT；Noto Sans SC 字体：SIL Open Font License。许可证不转授原作素材权利。

[发布与移植说明](docs/assets/shinuyakata.zh_CN.md) · [贡献规范](docs/contribution/commit-and-pr.zh_CN.md)
