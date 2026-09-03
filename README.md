<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DLNA Player

A DLNA/UPnP media renderer that turns the FoloToy AI Passport into a networked
speaker. Cast music to it from any DLNA-capable app (NetEase Cloud Music, most
media players, etc.) over your home Wi-Fi; the device pulls the stream over
HTTP, decodes it on board, and plays it through the I2S speaker. The firmware
boots straight into the player (no demo menu).

This is the application built on the `feature/netease-dlna-player` branch.

## What it does

- **DLNA/UPnP media renderer** — SSDP discovery plus an HTTP/SOAP control
  endpoint, so a phone app can find the device and push music to it.
- **HTTP streaming player** — `esp_http_client` pulls the compressed stream into
  a ring buffer while an audio task decodes and feeds the I2S output, so playback
  never blocks on network jitter.
- **On-board MP3 decode** — the branch adds a `music_decoder` component (MP3);
  decode happens on the device, not on the phone.
- **Wi-Fi provisioning** — connects to the saved SSID from NVS on boot; if there
  are no credentials or the STA link fails, it starts a soft-AP hotspot so you
  can configure the network once and the password survives reboots.
- **Pixel UI** — status, track title, volume, and battery on the 240×320 screen.

## Interaction

- **UP / DOWN (short)** — volume + / −.
- **OK (short)** — play / pause.
- **UP / DOWN (hold)** — skip tracks locally (no effect on the phone app's
  previous/next).
- **OK (hold)** — back to the demo menu.

## Firmware / build

Standard ESP-IDF project (target `esp32c3`). Build with the usual flow:

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

No separate release is attached for this branch yet; build from source.

## Source

- Branch: [`feature/netease-dlna-player`](https://github.com/Shinku-Chen/ai-passport/tree/feature/netease-dlna-player)
- Key files: `main/dlna_service.c` (SSDP + SOAP), `main/dlna_player.c` +
  `main/dlna_pipeline.c` (HTTP pull → ring buffer → decode), `main/dlna_wifi.c`
  (STA + soft-AP provisioning), `components/music_decoder/` (MP3).
