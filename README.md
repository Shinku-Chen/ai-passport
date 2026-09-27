<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# ATRI 阅读器（ATRI Reader）

把小米手环上的《ATRI -My Dear Moments-》同人移植搬到 AI Passport 上的
**竖屏视觉小说阅读器**：**34 章、1,069 幕、12,188 句对白、5 张全身立绘、三个结局**，完全离线。
剧本与素材取自 [`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI)
（它在 [`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband) 基础上重构，
并补上了原版没有的 `src/common/character/*` 全身立绘）。原版是 Vela OS 的触屏应用，
本分支用 C + LVGL 重写了阅读引擎，改成三键操作。
状态：**已发布** —— tag `v1.0.2-atri-reader`，并已投稿到 AI Passport 社区市场（作品 `my-dear-moments-2`，审核中）。

- 分支：[`feature/atri-reader`](https://github.com/Shinku-Chen/ai-passport/tree/feature/atri-reader)
- 发布：[`v1.0.2-atri-reader`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.2-atri-reader) —— 合并镜像 `FoloToy-AI-Passport-full.bin`，5,134,544 字节
- 上游作品：[`fywmjj/better-mb9p-ATRI`](https://github.com/fywmjj/better-mb9p-ATRI) 与 [`liuyuze61/ATRI-miband`](https://github.com/liuyuze61/ATRI-miband) —— 本次移植所依据的同人版本，两个仓库都未声明许可；其剧本、背景与立绘随分支提交并打包进固件，来源以上游项目标注。
- 素材工具链：[`tools/atri_pack.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/atri-reader/tools/atri_pack.py)（剧本 + 图像打包成 `main/atri_data/atri_pack.bin`，3.83 MB）与 [`tools/atri_font.py`](https://github.com/Shinku-Chen/ai-passport/blob/feature/atri-reader/tools/atri_font.py)（16px 中文字体子集，2,771 个码位）

```text
┌──────────────────────────────────┐  240 x 320,原生竖屏;画布铺满整屏
│  画面区 240 x 320                │  背景 JPEG + 可选叠加
│  [第N章]                  [电量] │
│                                  │
│ ┌────────┐                       │  说话人名牌
│ │ 亚托莉 │                       │  全身立绘:只在有说话人名字的那句显示
│ └────────┴───────────────────────┤  源工程的半透明蓝底文本带
│  正文,5 行 × 13 个全角字/行      │  直接画进画布,立绘压在带子之上
│                                  │  16px 字体,20px 行距
└──────────────────────────────────┘
```

**操作：** 列表页上 / 下移动光标，**确定**进入，**长按确定**返回上一层。正文里
**上 / 下短按**是下一句，**长按上**是快进、松手即停（快进时整句直接显示），
**长按下**开关自动阅读模式（打字完停 0.7 秒自动翻页，右下角显示「自动」，任意键停止；自动阅读期间不会熄屏），
**确定**打开菜单：继续阅读、保存进度、读取存档、跳过章节、返回标题（跳过章节会一直推进到下一章，
路上遇到选项或结局就停下）。选项页上 / 下选择、
确定确认。文字速度可选 瞬间 / 慢 / 中 / 快，关于页用上 / 下滚动。画面区左上角显示当前章节，
一句装不下时右下角显示页码。

**存档：** 五个手动槽 + 一个自动槽（每次换幕自动写入，标题页的「继续阅读」读它）。
存档页上短按确定存 / 读，长按确定删除手动槽。空闲 45 秒调暗背光、2.5 分钟熄屏、
7 分钟进入深睡，任意键唤醒；自动阅读期间这三种空闲行为都会暂停，停止后重新计时，
停在选项或结局上时照常熄灭。

**三个结局：** 剧本里的三次选择决定走向；圆满结局与悲剧结局都达成后，标题页解锁
「真正的结局」章节 —— 与原版手环应用的门槛一致。

**离线数据管线。** 运行时不下载任何东西；两个脚本生成固件需要的全部数据，产物提交
进仓库，普通 checkout 直接能编：

| 步骤 | 工具 | 产物 |
| --- | --- | --- |
| 剧本 + 图像 | `tools/atri_pack.py` | `main/atri_data/atri_pack.bin`（3.83 MB）：34 章、1,069 幕、12,188 句、73 张整屏背景、14 张特效叠加、5 张全身立绘，外加来源元数据 |
| 字体子集 | `tools/atri_font.py` | `assets/fonts/atri_cjk_16.c` + `assets/fonts/atri_cjk_symbols.txt`（2,771 个码位） |

**亮点：**

- **与原版同样的竖屏** —— 手环画面（336 x 480）按 240/336 等比缩放到 240 x 343，取顶部
  240 x 320 整屏，背景、立绘与文本带都落在原版的位置上。
- **运行时不解析 JSON** —— 资源包是一段小端二进制，直接从 Flash 读；文本按
  `(offset, length)` 寻址，阅读器沿章节 / 场景 / 对白三张表推进。
- **背景约 19 KB 一张** —— 构建期就完成缩放、裁剪与 JPEG(q88) 重编码：72 张场景背景加
  标题画共 1.36 MB，源素材是 4.7 MB 的 PNG。
- **全身立绘,按说话人切换** —— 5 张主角立绘（750×920~1150 的 PNG）按新版引擎
  `width/height:100%` 的方式压成 240×320，再按 alpha 包围盒裁剪，无损存成 RGB565 + 4bpp 遮罩。
  **只有"这句有说话人名字"时才显示**；男主夏生是视角、不出镜；事件 CG / 黑屏整幕不叠立绘
  （CG 里已经画了角色）。旁白、配角、换幕一律收起立绘。
- **不占 RAM 的叠加层** —— 14 张特效叠加按 alpha 包围盒裁剪，无损存成
  RGB565 + 4bpp 遮罩，合成时从 Flash 逐行直接写进画布。本板没有 PSRAM，画布与 LVGL
  建好后堆只剩几十 KB，240 x 320 叠加的 JPEG 解码缓冲根本拿不出来；这条路径全程不分配内存。
- **文本带画进画布** —— 源工程用一张半透明的蓝色 `text_bg` 盖在整屏画面上；这里把同一条带
  子（同样的颜色、同样的自上而下渐深）在像素层混进画布，但把**立绘合成在带子之上**，只在文字
  那几行盖一层浅暗帘：人物不会被"对话框"洗掉，白字也依然清晰。
- **自研字体生成器** —— `lv_font_conv` 在当前 Node.js 下写出的字形位图是坏的（真机表现
  为整屏噪点）；仓库内生成器用 FreeType 栅格化、直接输出 LVGL 9 plain 4bpp 格式，
  并在写完后回读自检、逐像素比对。
- **剧情逻辑可在宿主机上测** —— `tests/test_atri_model.c` 用真实资源包跑：包结构、
  圆满 / 悲剧 / 真正的结局、选择分流、分页规则、存档往返，都在
  `tools/validate.sh --static` 里执行。

**真机验收开关：** `idf.py -DATRI_BOOT_CHAPTER=<章下标> build` 可以让固件开机直接进某一章
（下标见 `tools/atri_pack.py` 的 `CHAPTER_ORDER`；31 = 圆满结局 b501，32 = 悲剧结局 b601，
33 = 真正的结局 b701，也就是最后一章）。默认 `-1` 走正常标题页流程。

**调试通道：** 串口（USB-Serial-JTAG）接受 `ATRIJUMP <章下标> [幕下标]`（直接把进度拨过去）
与 `ATRISHOT <章下标> <幕下标>`：固件用正常渲染路径把该幕
画进画面区，再把 240 x 320 的 RGB565 原始帧回传（先 `ATRISHOT <w> <h> <字节数>`，随后像素，
最后 `ATRISHOT-END`）。本项目的画面验证就是靠它（不用拍照）；平时它只挂在一个空闲任务上等输入。

**版权：** 剧本、图像与译文来自同人移植工程 `fywmjj/better-mb9p-ATRI`、
`liuyuze61/ATRI-miband` 与原作
《ATRI -My Dear Moments-》（ANIPLEX.EXE / Frontwing / 枕）。本固件是个人非商业移植，
请支持正版。

## 说明

- 本分支只承载一个应用：`feature/*` 分支的根 README 只介绍本分支自己的应用，全部项目的目录在 fork `main` 的根 README。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` —— 每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。目标板卡：8 MB Flash。
- 这些发布沉淀的可复用工程经验位于 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。
