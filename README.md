<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 调音器（Tuner）

AI Passport 上的**横屏**十二平均律调音器：板载麦克风实时采集，显示音名、八度、频率以及与基准音的 cents 偏差。

## 操作

**OK 短按** 在 `AUTO`（自动识别当前音名）与 `MANUAL`（用 UP / DOWN 选目标音 C4..B4）之间切换，**UP / DOWN 短按** 在 MANUAL 模式循环选目标音。**长按 OK** 进入调试模式：大字显示原始频率与 RMS / NSDF 中间量，UP / DOWN 调麦克风增益。

## 功能

- **实时音高检测** —— 麦克风在独立 worker 任务采集，整数 NSDF 跟踪输出音名 / 八度 / 频率。
- **AUTO 模式** —— 自动识别当前音名，表头指示离最近标准音的 cents。
- **MANUAL 模式** —— 选定目标音后照着调，表头指示与目标的偏差。
- **调试模式** —— 大字频率 + RMS / NSDF，可调增益。

## 依赖

**单机离线**：只用板载麦克风，不需要手机、网络或账号。

## 构建与刷写

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## 已知限制

- 精度受板载麦克风与扬声器回声影响，低音区（C3 以下）误差更大。
- 只在 C4..B4 之间可选 MANUAL 目标音。
- 没有节拍器或和弦识别。

## 许可

MIT，见 [`LICENSE`](LICENSE)。
