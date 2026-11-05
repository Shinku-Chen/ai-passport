<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 《男娘恋爱物语（NianNiang Love Story）》· AI Passport 240×320 竖屏阅读器

把小米手环上的《男娘恋爱物语》同人移植搬到 AI Passport 的**竖屏**视觉小说阅读器：
**13 个场景、1,248 句对白、两处选项、四个结局、16 张背景、15 张事件 CG、30 张表情立绘**，
全程离线。剧本与素材取自 [`hrk666666/NianNiang-MiBand-10`](https://github.com/hrk666666/NianNiang-MiBand-10)
（其上游为 `hrk666666/Senren-Banka-MiBand-10`）；本分支为独立阅读器，重写了剧本解释器、
图像链路与全部页面。

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

- tag：`v1.0.0-nianniang`（分支 `feature/port-nianniang-landscape`）
- 合并固件：`FoloToy-AI-Passport-full.bin`（1,814,064 B）
- 应用镜像：1,748,528 B，版本 `1.0.0`
- 分区占用：**21.01%**，余量 **6,574,544 B**

## 已知限制

- 分区占用 21.01%，余量 6,574,544 B；改动内容前仍需确认 Flash 预算。
- 内容覆盖范围与取舍见 [docs/reference/shinku-chen/nianniang/](docs/reference/shinku-chen/nianniang/)。
- 本作实际是**竖屏 240×320**；分支名里的 `-landscape` 是历史命名，不代表方向。

## 上游与来源

- 上游移植仓库：[hrk666666/NianNiang-MiBand-10](https://github.com/hrk666666/NianNiang-MiBand-10)（其上游为 hrk666666/Senren-Banka-MiBand-10）
- 应用基线：[`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的 `feature/galgame-framework`
- 原作品：《男娘恋爱物语》；剧本、美术与字库版权归原权利方，上游项目未声明许可，本移植仅供个人、非商业使用

## 移植经验

十二部作品共用的阅读器契约、真机踩坑与验收清单：[`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port)。

## 许可

本仓代码与上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 同为 MIT（见 `LICENSE`）。剧本、美术与字库来自原作及其社区移植，版权归原权利方；本移植仅供个人、非商业使用。
