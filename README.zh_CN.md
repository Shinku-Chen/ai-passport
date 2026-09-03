<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# DLNA 音频接收器（DLNA Audio Receiver）

一个 DLNA/UPnP 音频投屏接收器，把 FoloToy AI Passport 变成联网音箱。用支持 DLNA 的
手机 App 或小米妙播投音乐：设备在你的 Wi-Fi 上广播自己、接收流、板载解码，再从 I2S
喇叭播出。固件开机直接进入接收器（无主菜单）。

本应用构建在 `feature/dlna-receiver` 分支上。

## 功能

- **DLNA 媒体渲染器** —— `custom_dlna` 协议栈（SSDP 发现 + SOAP 控制 + GENA 事件），
  适配 ESP32-C3 单核、无 PSRAM。
- **流式音频管线** —— HTTP 拉流进环形缓冲，`minimp3` / `esp_aac` 解码 →
  `bsp_audio` I2S 输出。
- **多音源** —— 按投送 App 的 `User-Agent` 切换音乐源配置；内置**小米妙播（MiPlay）**
  支持（mDNS + TCP 8899），MiPlay 客户端连接时暂停 DLNA 播放，两者共享 I2S 总线不冲突。
- **Wi-Fi 配网** —— 首次开机（或无凭证）时开启 softAP 热点 `AI-Passport-Prov`
  （密码 `00114514`）供配置网络；凭证保存在 NVS。
- **极简状态 UI** —— 240×320 屏显示标题栏、曲名、进度条、播放状态与电量。

## 操作方式

三键导航：

- **UP / DOWN** —— 在状态 / 配网页之间切换。
- **OK** —— 进入 / 确认。
- **OK（长按，开机 10 秒内）** —— 清除已保存的 Wi-Fi 配置并回到配网。

## 构建 / 固件

标准 ESP-IDF 工程（target `esp32c3`）：

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

## 来源

- 分支：[`feature/dlna-receiver`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dlna-receiver)
- 关键文件：`main/dlna_app.c` / `dlna_app.h`（应用状态机 + 按键路由）、
  `main/net_prov.c`（softAP 配网）、`components/custom_dlna/`（SSDP + SOAP + GENA）、
  `components/dlna_audio/`（HTTP → 解码 → I2S）、`components/miplay/`（小米妙播）、
  `components/esp_audio_codec/` + `components/dns_server/`。
