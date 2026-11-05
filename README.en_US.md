<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Pocket Intercom

A **landscape** pocket AI intercom for the AI Passport: hold OK to talk, a companion app relays your speech to a backend AI, and the answer returns to both the device screen and the phone.

## Controls

**Hold OK** to talk — the screen turns red while preparing and green when the gateway is ready (you can speak); releasing sends. **A short OK press** only wakes the screen, **UP / DOWN** scroll message history, and a **long press on UP** opens settings (brightness, volume, mic gain, device info, re-pair).

## Features

- **Device-side Opus uplink** — 16 kHz, 60 ms frames, DTX, about 3 KB/s, encoded on a static stack on a PSRAM-less ESP32-C3.
- **The phone is the gateway** — the app does recognition and backend calls; the device never parses HTTP and stores no keys.
- **Red on press, green when ready** — listening starts immediately and turns green once the gateway is ready, about 280–290 ms in testing.

## Dependencies

**Requires companion pieces**: an Android 8+ app (separate repository [`Shinku-Chen/ai-passport-openclaw-android`](https://github.com/Shinku-Chen/ai-passport-openclaw-android)) and a backend — either OpenClaw or an OpenAI-compatible endpoint such as Hermes. The device itself stays offline: audio travels only over Bluetooth between unit and phone.

## Build and flash

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## Release

Latest tag `v1.14-intercom`.

## Known limitations

- Without the phone app the unit can only wake its screen; a conversation cannot complete.
- Recognition quality depends on the phone and backend; the device only captures and plays audio.
- Message history is not persisted and is cleared on reboot.

## Reference

- [Intercom wire protocol](docs/development/engineering/intercom-wire-protocol.md) — device ↔ phone messages and framing.


## License

MIT, see [`LICENSE`](LICENSE).
