<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DLNA Audio Receiver

A DLNA/UPnP audio casting receiver that turns the FoloToy AI Passport into a
networked speaker. Cast music from a DLNA phone app or Xiaomi's MiPlay; the
device advertises itself on your Wi-Fi, receives the stream, decodes it on
board, and plays through the I2S speaker. The firmware boots straight into the
receiver (no demo menu).

This is the application built on the `feature/dlna-receiver` branch.

## What it does

- **DLNA media renderer** — a `custom_dlna` stack (SSDP discovery + SOAP control
  + GENA events) adapted to the ESP32-C3's single core with no PSRAM.
- **Streaming audio pipeline** — HTTP pull into a ring buffer, then `minimp3` /
  `esp_aac` decode → `bsp_audio` I2S output.
- **Multi-source** — switches the music source configuration by the pushing
  app's `User-Agent`; includes **MiPlay** (Xiaomi, mDNS + TCP 8899),
  which pauses DLNA playback while a MiPlay client is connected so both share
  the I2S bus without fighting.
- **Wi-Fi provisioning** — on first boot (or when no credentials are saved) it
  opens a soft-AP hotspot `AI-Passport-Prov` (password `00114514`) so you can
  configure the network; credentials persist in NVS.
- **Minimal status UI** — title bar, track name, progress bar, play state, and
  battery on the 240×320 screen.

## Interaction

Three keys navigate the app:

- **UP / DOWN** — navigate the status / provisioning screens.
- **OK** — enter / confirm.
- **OK (hold, within 10 s of boot)** — clear the saved Wi-Fi configuration and
  return to provisioning.

## Firmware / build

Standard ESP-IDF project (target `esp32c3`):

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

## Source

- Branch: [`feature/dlna-receiver`](https://github.com/Shinku-Chen/ai-passport/tree/feature/dlna-receiver)
- Key files: `main/dlna_app.c` / `dlna_app.h` (app state machine + key routing),
  `main/net_prov.c` (soft-AP provisioning), `components/custom_dlna/` (SSDP + SOAP
  + GENA), `components/dlna_audio/` (HTTP → decode → I2S), `components/miplay/`
  (Xiaomi MiPlay), `components/esp_audio_codec/` + `components/dns_server/`.
