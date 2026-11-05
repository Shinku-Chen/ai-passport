<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# DLNA 投屏播放器（DLNA Player）

AI Passport 上的**横屏** DLNA 投屏播放器：用手机上的网易云音乐等 App 把音乐投到设备，设备拉流、板载解码，再从 I2S 喇叭播出。

## 操作

**UP / DOWN 短按** 调音量，**OK 短按** 播放 / 暂停，**UP / DOWN 长按** 切换本地曲目（对手机 App 的上一首 / 下一首无效），**长按 OK** 返回演示菜单。

## 功能

- **DLNA/UPnP 渲染器** —— SSDP 发现 + HTTP/SOAP 控制端点，手机 App 能发现并推送。
- **HTTP 流式播放** —— 压缩流进环形缓冲，音频任务边解码边喂 I2S，网络抖动不卡顿。
- **板载 MP3 解码** —— 解码在设备端完成，不占手机。
- **Wi-Fi 配网** —— 无凭证或 STA 连不上时自动开 softAP 热点配网，密码断电不丢。

## 依赖

**需要局域网推流源**：手机与设备在同一 Wi-Fi，用支持 DLNA 的 App 投屏；设备本身不提供曲库。

## 构建与刷写

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## 已知限制

- 手机 App 的上一首 / 下一首不会转发到设备，只能长按 UP / DOWN 切本地曲目。
- 只能播 App 推过来的流，不能自己浏览网络曲库。
- 5 GHz-only 或开启客户端隔离的网络下发现可能失败。

## 许可

MIT，见 [`LICENSE`](LICENSE)。
