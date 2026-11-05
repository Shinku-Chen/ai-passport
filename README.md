<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# DLNA 音频接收器（DLNA Audio Receiver）

AI Passport 上的**横屏** DLNA/UPnP 音频投屏接收器：用支持 DLNA 的手机 App 或小米妙播投音乐，设备在你的 Wi-Fi 上广播自己、接收流、板载解码，再从 I2S 喇叭播出。固件开机直接进入接收器（无主菜单）。

## 操作

- **UP / DOWN** —— 在状态 / 配网页之间切换。
- **OK** —— 进入 / 确认。
- **OK（长按，开机 10 秒内）** —— 清除已保存的 Wi-Fi 配置并回到配网。

## 功能

- **DLNA 媒体渲染器** —— `custom_dlna` 协议栈（SSDP 发现 + SOAP 控制 + GENA 事件），适配 ESP32-C3 单核、无 PSRAM。
- **流式音频管线** —— HTTP 拉流进环形缓冲，`minimp3` / `esp_aac` 解码 → `bsp_audio` I2S 输出。
- **多音源** —— 按投送 App 的 `User-Agent` 切换音乐源配置；内置**小米妙播（MiPlay）**支持（mDNS + TCP 8899），MiPlay 客户端连接时暂停 DLNA 播放，两者共享 I2S 总线不冲突。
- **Wi-Fi 配网** —— 开机连 NVS 里保存的 SSID；无凭证或 STA 连不上时自动开 softAP 热点配网。

## 依赖

**需要局域网推流源**：手机与设备在同一 Wi-Fi，用支持 DLNA 的 App（或小米妙播）投屏；设备本身不提供曲库，也不联网取流。

## 构建与刷写

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## 已知限制

- 只能播 App 推过来的流，不能自己浏览网络曲库。
- MiPlay 与 DLNA 共享 I2S：MiPlay 连接时 DLNA 播放会暂停，断开后需要重新投送。
- 5 GHz-only 或开启客户端隔离的网络下 SSDP/mDNS 发现可能失败。

## 许可

MIT，见 [`LICENSE`](LICENSE)。
