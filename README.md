<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 随身 AI 对讲机（Pocket Intercom）

AI Passport 上的**横屏**随身 AI 对讲机：按住 OK 说话，手机端 App 作为中介把语音交给后端 AI，回答同时回到设备小屏和手机。

## 操作

**按住 OK** 说话 —— 屏幕立刻变红表示准备，变绿就**可以说**，松手即发送；**短按 OK** 只点亮屏幕；**UP / DOWN** 翻看历史消息；**长按 UP** 进设置（亮度 / 音量 / 麦克风增益 / 设备信息 / 重新配对）。

## 功能

- **设备端 Opus 上行** —— 16 kHz、60 ms 帧、DTX，约 3 KB/s；无 PSRAM 的 ESP32-C3 上用静态栈编码。
- **手机端当网关** —— App 负责识别与后端调用，设备不解析 HTTP、不存密钥。
- **按下即红、就绪变绿** —— 按键即开始听，网关就绪后转绿，实测约 280–290 ms。

## 依赖

**需要配套依赖**：Android 8+ 手机应用（另一个仓库 [`Shinku-Chen/ai-passport-openclaw-android`](https://github.com/Shinku-Chen/ai-passport-openclaw-android)），以及可用的后端 —— OpenClaw，或 OpenAI 兼容接口（如 Hermes）。设备不联网，语音只在「设备 ↔ 手机」之间走蓝牙。

## 构建与刷写

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## 发布

最新 tag `v1.14-intercom`。

## 已知限制

- 没有手机 App 时设备只能亮屏，不能完成一轮对话。
- 语音识别质量取决于手机端与后端；设备只负责采集与播放。
- 历史消息不落盘，重启后清空。

## 经验文档

- [对讲机线协议](docs/development/engineering/intercom-wire-protocol.zh_CN.md) —— 设备 ↔ 手机报文与帧格式。


## 许可

MIT，见 [`LICENSE`](LICENSE)。
