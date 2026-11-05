<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# DLNA Audio Receiver

A **landscape** DLNA/UPnP audio casting receiver for the AI Passport: cast music from a DLNA phone app or Xiaomi MiPlay, and the device advertises itself on your Wi-Fi, receives the stream, decodes on board and plays through the I2S speaker. The firmware boots straight into the receiver with no main menu.

## Controls

- **UP / DOWN** — move between the status and provisioning pages.
- **OK** — enter / confirm.
- **Long press on OK within the first 10 seconds after boot** — clear the stored Wi-Fi configuration and return to provisioning.

## Features

- **DLNA media renderer** — a `custom_dlna` stack (SSDP discovery + SOAP control + GENA events) sized for a single-core, PSRAM-less ESP32-C3.
- **Streaming audio pipeline** — HTTP fills a ring buffer, `minimp3` / `esp_aac` decode it and `bsp_audio` pushes it to I2S.
- **Multiple sources** — the music source profile is chosen from the casting app's `User-Agent`; built-in **Xiaomi MiPlay** support (mDNS + TCP 8899) pauses DLNA playback when a MiPlay client connects, so both share the I2S bus without fighting.
- **Wi-Fi provisioning** — connects to the SSID stored in NVS at boot; without credentials, or when the STA connection fails, it opens a softAP provisioning hotspot.

## Dependencies

**Needs a LAN source**: phone and device on the same Wi-Fi, casting from a DLNA-capable app (or MiPlay). The device provides no library of its own and does not fetch content by itself.

## Build and flash

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## Known limitations

- It plays what the app casts; it cannot browse a network library itself.
- MiPlay and DLNA share I2S: a MiPlay connection pauses DLNA playback, and you need to cast again after it disconnects.
- SSDP/mDNS discovery can fail on 5 GHz-only networks or with client isolation enabled.

## License

MIT, see [`LICENSE`](LICENSE).
