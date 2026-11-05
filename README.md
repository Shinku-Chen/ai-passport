<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 《Summer Pockets REFLECTION BLUE》· AI Passport 320×240 横屏阅读器

把 AI Passport 变成 320×240 横屏阅读器，移植 RW5《Summer Pockets REFLECTION
BLUE》源快照：422 个场景、81,040 句正文、188 组选项、27 个地图阶段、14 条路线/
252 个章节入口、136 组 CG（712 张差分）与 292 张立绘/表情全部打包；标题、正文、
选项、菜单、历史、30 个手动档 + 独立自动档、章节/路线、CG 鉴赏与 14 结局进度均可用。

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
- 空闲：90 s 调暗、300 s 熄屏、1800 s 深睡，任意键唤醒并回到原进度

## 构建与刷写

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o "$PWD/build-release/FoloToy-AI-Passport-full.bin"
python -m esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 \
  build-release/FoloToy-AI-Passport-full.bin
```

板卡：ESP32-C3、8 MB Flash、无 PSRAM；刷机与数据保留规则见 [`docs/development/engineering/firmware-layout.md`](docs/development/engineering/firmware-layout.md)。

## 发布

- tag：`v1.0.0-summer`（分支 `feature/port-summer-landscape`）
- 合并固件：`FoloToy-AI-Passport-full.bin`（7,716,528 B）
- 应用镜像：7,585,456 B，版本 `1.0.0`
- 分区占用：**91.86%**，余量 **672,080 B**

## 已知限制

- 分区占用 91.86%，余量 672,080 B；改动内容前仍需确认 Flash 预算。
- 内容覆盖范围与取舍见 [docs/development/engineering/galgame-summer.zh_CN.md](docs/development/engineering/galgame-summer.zh_CN.md)。

## 上游与来源

- 上游移植仓库：[summer-rw5](https://github.com/xuanyide01/summer-rw5)（快照 `28f8a41c3e77`；由小米 Vela 工程转换而来）
- 应用基线：[`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的 `feature/galgame-framework`
- 原作品：《Summer Pockets REFLECTION BLUE》；剧本、美术与字库版权归原权利方，上游项目未声明许可，本移植仅供个人、非商业使用

## 移植经验

十二部作品共用的阅读器契约、真机踩坑与验收清单：[`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port)。

## 许可

本仓代码与上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 同为 MIT（见 `LICENSE`）。剧本、美术与字库来自原作及其社区移植，版权归原权利方；本移植仅供个人、非商业使用。
