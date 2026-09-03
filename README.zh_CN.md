<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# DLNA 投屏播放器（DLNA Player）

一个 DLNA/UPnP 媒体渲染器，把 FoloToy AI Passport 变成联网音箱。用任意支持
DLNA 的手机 App（网易云音乐、多数播放器等）把音乐投到设备上；设备通过 HTTP
拉流、板载解码，再从 I2S 喇叭播出。固件开机直接进入播放器（无主菜单）。

本应用构建在 `feature/netease-dlna-player` 分支上。

## 功能

- **DLNA/UPnP 媒体渲染器** —— SSDP 发现 + HTTP/SOAP 控制端点，手机 App 能发现
  设备并把音乐推给它。
- **HTTP 流式播放** —— `esp_http_client` 把压缩流拉进环形缓冲，音频任务边解码边
  喂给 I2S 输出，播放不因网络抖动而卡顿。
- **板载 MP3 解码** —— 本分支新增 `music_decoder` 组件（MP3）；解码在设备端完成，
  不占用手机。
- **Wi-Fi 配网** —— 开机连 NVS 里保存的 SSID；无凭证或 STA 连不上时自动开启
  softAP 热点配网，密码断电不丢。
- **像素风 UI** —— 240×320 屏显示状态、曲名、音量与电量。

## 操作方式

- **UP / DOWN（短按）** —— 音量 + / −。
- **OK（短按）** —— 播放 / 暂停。
- **UP / DOWN（长按）** —— 本地切换曲目（对手机 App 的"上一首/下一首"无效）。
- **OK（长按）** —— 返回演示菜单。

## 构建 / 固件

标准 ESP-IDF 工程（target `esp32c3`）。常规流程构建：

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

本分支尚未单独发布固件，请从源码构建。

## 来源

- 分支：[`feature/netease-dlna-player`](https://github.com/Shinku-Chen/ai-passport/tree/feature/netease-dlna-player)
- 关键文件：`main/dlna_service.c`（SSDP + SOAP）、`main/dlna_player.c` +
  `main/dlna_pipeline.c`（HTTP 拉流 → 环形缓冲 → 解码）、`main/dlna_wifi.c`
  （STA + softAP 配网）、`components/music_decoder/`（MP3）。
