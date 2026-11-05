<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# DLNA Player

A **landscape** DLNA casting player for the AI Passport: cast music from an app such as NetEase Cloud Music, and the device streams, decodes on board and plays through the I2S speaker.

## Controls

**Short UP / DOWN** adjust volume, **short OK** toggles play/pause, **long UP / DOWN** step through local tracks (the phone app's previous/next is not forwarded), and a **long press on OK** returns to the demo menu.

## Features

- **DLNA/UPnP renderer** — SSDP discovery plus HTTP/SOAP control endpoints, so phone apps can find and cast to it.
- **HTTP streaming** — the compressed stream fills a ring buffer and the audio task decodes straight into I2S, so network jitter does not stall playback.
- **On-board MP3 decoding** — decoding happens on the device, not the phone.
- **Wi-Fi provisioning** — without credentials, or when the STA connection fails, it opens a softAP hotspot whose password survives power loss.

## Dependencies

**Needs a LAN source**: phone and device on the same Wi-Fi, casting from a DLNA-capable app. The device provides no library of its own.

## Build and flash

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## Known limitations

- The phone app's previous/next is not forwarded; only long UP / DOWN steps local tracks.
- It plays what the app casts; it cannot browse a network library itself.
- Discovery can fail on 5 GHz-only networks or with client isolation enabled.

## License

MIT, see [`LICENSE`](LICENSE).
