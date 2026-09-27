<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 魔女的夜宴（Sanoba Witch）

把《魔女的夜宴》手环移植版搬到 AI Passport 的竖屏视觉小说阅读器:**101 章、53,190 句对白、约 110 万字、五条角色线五个结局**,全程离线。剧本与素材来自
[`hrk666666/Sanoba-Witch-MiBand-10`](https://github.com/hrk666666/Sanoba-Witch-MiBand-10)(小米手环 9/10 快应用,其上游是已存档的
[`futrw4v/Sanoba-Witch-MiBand-9Pro`](https://github.com/futrw4v/Sanoba-Witch-MiBand-9Pro));本分支在 ATRI 阅读器的 LVGL 页面体系上重写了阅读器,用三键操作。状态:**已发布** —— tag `v0.1.0-sanoba-witch`,并已投稿 AI Passport 社区市场 `community-8e228d9f`(审核中)。

- 分支:[`feature/sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch)
- 发布:[`v0.1.0-sanoba-witch`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v0.1.0-sanoba-witch) —— 合并镜像 `FoloToy-AI-Passport-full.bin`,5,929,664 字节
- 上游工程:[`hrk666666/Sanoba-Witch-MiBand-10`](https://github.com/hrk666666/Sanoba-Witch-MiBand-10) —— 本分支的移植来源(其上游为已存档的 `futrw4v/Sanoba-Witch-MiBand-9Pro`)。其代码为 GPL-3.0;美术版权归 Yuzusoft,简体中文文本归暗鸽汉化组。打包用的素材随分支提交在 [`assets/sanoba-source/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/sanoba-witch/assets/sanoba-source),并注明上游出处。
- 素材工具链:[`tools/sanoba_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/sanoba-witch/tools/sanoba_pack.py) 与 [`tools/sanoba_scn_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/sanoba-witch/tools/sanoba_scn_pack.py)(图片包 2.97 MB、剧本包 1.45 MB,产出到 `main/sanoba_data/`),外加 [`tools/sanoba_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/sanoba-witch/tools/sanoba_font.py)(16px 中文字库子集,3,458 字形,按剧本码表顺序生成,零缺字)

**操作(三键):** 列表页上/下移动光标、**确定**进入。阅读时 **上/下** 推进一句(打字机没打完按一下就显示完整,长句自动分页用下键翻页),**上 长按** 快进(松手即停),**下 长按** 切换自动阅读,**确定** 打开菜单(存档、读档、跳到下一个选项、自动播放、返回主页);选项处用上/下 + 确定。**停在选项上不会关掉自动阅读**,答完会自己接着读。短按判定 180 ms、长按 500 ms,由 BSP 显式下发给按键组件。

**存档、结局与省电:** 5 个手动槽位 + 1 个自动槽位(每次换场景写入),标题页的"继续阅读"直接接上。五条线对应五个结局。不操作时:60 秒调暗、180 秒熄屏、420 秒休眠,任意键唤醒;自动阅读与快进不计入空闲,屏幕保持常亮。

**亮点:**

- **紧凑剧本包,不是把剧本原样塞进去** —— 5.06 MB 的节点 JSON 变成 1.45 MB 的包,一次只解一块(单块解压 3 KB、缓冲 4 KB),因为开机后最大连续空闲块不到 8 KB;跳转目标走标签表直接定位,不扫块。
- **三个只在真机上才暴露的缺陷** —— 压缩流封装、块缓冲分配、主任务栈三者都表现为"一进阅读就全剧终";启动自检现在会打印首个块解压后的字节数与首句,这类问题在日志里就直接现形,不会再看起来像剧情结束。
- **版面按面板来** —— 240 × 320 竖屏:整宽背景、上三分之一处的 240 × 144 SD 窗、五行正文带。背景直接解进画布、SD 图解码进它占用的那段行,两者都不需要额外解码缓冲。
- **显示剧本自己的章节号** —— 角落浮层与换章卡显示源数据的编号(如 `4-7`),而存档标签与列表用场景名。
- **离线且可复现** —— 两个打包器与字库生成器都从 `assets/sanoba-source/` 的入库素材运行;重建任一包都与固件里的逐字节一致。
- **串口调试通道** —— `SANOBAPAGE [title | <场景> <句>]` 在 LVGL 刷屏路径上抓整帧并以原始 RGB565 回传;`SANOBASHOT`、`SANOBAJUMP`、`SANOBAAUTO` 分别覆盖场景渲染、场景跳转与自动阅读检查。
- **没有人物立绘,也没有事件 CG** —— 上游移植版就没有这两类图,所以人物说话时只显示名字牌,场面由背景 + Q 版演出小图承担;剧情文本、分支与五个结局都是完整的。

## 说明

- 本分支只承载一个应用：`feature/*` 分支的根 README 只介绍本分支自己的应用，全部项目的目录在 fork `main` 的根 README。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` —— 每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。目标板卡：8 MB Flash。
- 这些发布沉淀的可复用工程经验位于 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。
