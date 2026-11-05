<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 《魔法少女的魔女审判》· AI Passport 320×240 横屏阅读器

把《魔法少女的魔女审判》做成 AI Passport 上的**320×240 横屏**视觉小说阅读器：剧本、背景、立绘与事件 CG 打包进应用分区，离线可读。

- **全部 458 个源剧本文件 / 424 个场景 / 90,243 条指令**打包成一份只读 Flash 包，
  由 C 虚拟机按源 `engine.js` / `scene.js` 语义执行：背景层、立绘实例、焦点、
  条件、`call`/`return`、`offer`/`choice`、地图调查、证据提交、反驳词、审判伤害、
  手册版本、自动存档点、章节入口与 `file:scene/chapter` 两种模式。
- **全量画面**：62 张背景、172 张 CG（61 组）、151 张立绘、19 张机关图、151 张
  手册图、2 张纯色占位图和 2 张标题图，共 559 张；320×240 等比 fit，不裁切；立绘 JPEG + 1bpp 遮罩。
- 作品专属 320×240 横屏 UI：标题、正文、选项、菜单、历史、存读档、设置、
  鉴赏室/CG 全屏、章节选择、魔女手册、结局、关于、错误页。
- 30 个手动存档 + 1 个独立自动档；版本/长度/CRC 校验，写入失败不改旧档；
  全局鉴赏/章节解锁独立版本化存储，只增不减，失败节流重试。
- 三键语义：正文 UP 短按/长按快进，DOWN 历史/长按自动，OK 菜单/反驳词；
  列表 UP/DOWN + OK，长 OK 返回；选项必须 OK 显式确认。

## 玩法与三键

| 操作 | 阅读中 | 列表 / 选项 |
| --- | --- | --- |
| OK 短按 | 推进一页；本页还在打字时立刻显示整页 | 确认 |
| OK 长按 | 打开菜单 | 返回上一级 |
| UP 短按 | 推进一页 | 光标上移 |
| UP 长按 | 快进，松手即停 | — |
| DOWN 短按 | 历史回看 | 光标下移 |
| DOWN 长按 | 开关自动阅读 | — |

## 功能

- 打字机正文，速度可调（可设立即显示）
- 自动阅读：整页打完后自动翻页，期间屏幕不休眠
- 快进瞬显：整页立刻出现，不逐字追赶
- 存档 / 读档：多槽手动档 + 自动档（每次转场写入）
- 历史回看、章节列表、CG 鉴赏
- 空闲：60 s 调暗、300 s 熄屏、1800 s 深睡，任意键唤醒并回到原进度

## 构建与刷写

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o "$PWD/build-release/FoloToy-AI-Passport-full.bin"
python -m esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 \
  build-release/FoloToy-AI-Passport-full.bin
```

板卡：ESP32-C3、8 MB Flash、无 PSRAM；刷机与数据保留规则见 [`docs/development/engineering/firmware-layout.md`](docs/development/engineering/firmware-layout.md)。

## 发布

- tag：`v1.0.0-manosaba`（分支 `feature/port-manosaba-landscape`）
- 合并固件：`FoloToy-AI-Passport-full.bin`（7,959,888 B）
- 应用镜像：7,763,280 B，版本 `1.0.0`
- 分区占用：**94.77%**，余量 **428,720 B**

## 已知限制

- 分区占用 94.77%，余量 428,720 B；改动内容前仍需确认 Flash 预算。
- 内容覆盖范围与取舍见 [`docs/games/`](docs/games/)。

## 上游与来源

- 上游移植仓库：[manosaba-rw5](https://github.com/xuanyide01/manosaba-rw5)（快照 `a309a3614837`）
- 应用基线：[`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的 `feature/galgame-framework`
- 原作品：《魔法少女的魔女审判》；剧本、美术与字库版权归原权利方，上游项目未声明许可，本移植仅供个人、非商业使用

## 移植经验

十二部作品共用的阅读器契约、真机踩坑与验收清单：[`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port)。

## 许可

本仓代码与上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 同为 MIT（见 `LICENSE`）。剧本、美术与字库来自原作及其社区移植，版权归原权利方；本移植仅供个人、非商业使用。
