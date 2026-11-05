<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 《想成为Galgame领域大神！！！》· AI Passport 320×240 横屏阅读器

把「诺提拉观察所」的 galgame **《想成为Galgame领域大神！！！》** 完整搬到 AI Passport：
横屏 320×240、全部 18 章 / 5,457 句 / 119,410 字、15 组选项（含答题计分与
`bad_end_flag >= 7` 分支）、4 个结局与后日谈、55 张立绘全表情、38 张背景、
34 个 CG/SD 引用（去重后 33 张，鉴赏室分页）、三槽存档 + 自动存档、历史与设置。

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

- tag：`v1.0.0-galgod`（分支 `feature/port-galgod-landscape`）
- 合并固件：`FoloToy-AI-Passport-full.bin`（3,351,360 B）
- 应用镜像：3,285,824 B，版本 `1.0.0`
- 分区占用：**39.48%**，余量 **5,037,248 B**

## 已知限制

- 分区占用 39.48%，余量 5,037,248 B；改动内容前仍需确认 Flash 预算。
- 内容覆盖范围与取舍见 [`docs/games/`](docs/games/)。

## 移植经验

十二部作品共用的阅读器契约、真机踩坑与验收清单：[`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port)。

## 许可

本仓代码与上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 同为 MIT（见 `LICENSE`）。剧本、美术与字库来自原作及其社区移植，版权归原权利方；本移植仅供个人、非商业使用。
