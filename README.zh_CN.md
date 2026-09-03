<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 调音器（Tuner）

面向 FoloToy AI Passport 的十二平均律调音器。用板载麦克风实时采集声音、检测音高，
显示音名/八度/频率以及与基准音的 cents 偏差。构建在标准 BSP demo 架构（菜单 + 演示页）
之上，以 Tuner 为主打演示页。

本应用构建在 `feature/tuner` 分支上。

## 功能

- **实时音高检测** —— 麦克风在独立 worker 任务中采集；整数 NSDF 音高跟踪把采样转成
  音名、八度与频率。
- **AUTO 模式** —— 自动识别当前音名，表头指示它离最近标准音多远（cents）。
- **MANUAL 模式** —— 用 UP / DOWN 选目标音（C4..B4）并照着调，表头指示与目标音的
  偏差，更符合调弦直觉。
- **调试模式**（长按 OK）—— 大字显示原始频率及 RMS / NSDF 中间量，并可用 UP / DOWN
  调整麦克风增益，便于真机校验。

## 操作方式

- **OK（短按）** —— 在 AUTO / MANUAL 模式间切换。
- **UP / DOWN（短按）** —— MANUAL 模式下循环选择目标音；调试模式下调节麦克风增益。
- **OK（长按）** —— 切换调试模式。
- **OK（长按，在 Tuner 页内）** —— Tuner 页自行消费长按 OK（不返回菜单）。

其余演示页（Display / Button / Audio / Battery / Wi-Fi / BLE / Low Power）仍可从
菜单进入，作为 BSP 参考。

## 构建 / 固件

标准 ESP-IDF 工程（target `esp32c3`）：

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

## 来源

- 分支：[`feature/tuner`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tuner)
- 关键文件：`main/demo_tuner.c`（页面 + 按键处理）、`main/tuner_engine.c` /
  `main/tuner_engine.h`（整数 NSDF 音高检测）。
