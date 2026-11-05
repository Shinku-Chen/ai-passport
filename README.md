<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 《anemoi》· AI Passport 320×240 横屏阅读器

把 Redmi Watch 5 版《anemoi》的**一部分**搬到 AI Passport 320×240 横屏：身体 + 原生表情
两层立绘、320×180 背景/CG、委托板、30 存档 + 自动档、CG 鉴赏与章节列表。这是全批里最重的
作品，**本版是只覆盖源作品 71/264 场景的技术原型**（共通篇 23 章 + 委托信/任务，正文
764,847 / 3,088,939 字节）；路线正篇与 After 未移植，走到那里会显示分卷边界页，不静默丢内容。

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

- tag：`v1.0.0-anemoi`（分支 `feature/port-anemoi-landscape`）
- 合并固件：`FoloToy-AI-Passport-full.bin`（8,306,384 B）
- 应用镜像：8,240,848 B，版本 `1.0.0`
- 分区占用：**99.01%**，余量 **82,224 B**

## 已知限制

### 余量告警

- 分区余量仅 **82,224 B**（占用 99.01%），任何改动前先做 Flash 预算。
- 内容覆盖范围与取舍见 [docs/development/engineering/galgame-anemoi.zh_CN.md](docs/development/engineering/galgame-anemoi.zh_CN.md)。

## 移植经验

十二部作品共用的阅读器契约、真机踩坑与验收清单：[`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port)。

## 许可

本仓代码与上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 同为 MIT（见 `LICENSE`）。剧本、美术与字库来自原作及其社区移植，版权归原权利方；本移植仅供个人、非商业使用。
