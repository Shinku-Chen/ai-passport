<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Voice Keychain

A pocket sound keychain for the AI Passport: it boots straight into the player and plays hundreds of Chinese voice clips from dozens of character packs.

## Controls

**UP / DOWN** move through the list, **OK** opens a pack, selects a clip or plays it, and a **long press on OK** opens settings (volume, battery) or goes back one level.

## Features

- **Character catalogue** — a scrollable list of every character pack, with its clip names one level down.
- **One-key playback** — built-in 16 kHz mono Opus decoding, with the decoder reused across clip changes.
- **Smooth browsing** — long names scroll at a steady speed; hold UP/DOWN to browse continuously and release to stop without queued movements.
- **Settings page** — battery percentage and voltage, plus playback volume.
- **Self-contained image** — the `voicefs` data partition is bundled into the same 8 MB image.

## Dependencies

**Standalone and offline**: no phone, network or account; all audio ships inside the firmware.

## Build and flash

```bash
./tools/validate.sh --firmware
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/FoloToy-AI-Passport-full.bin
```

## Release

Tag `v1.15.1-voice-keychain`; the release ships the device-tested, self-contained `FoloToy-AI-Passport-full.bin`.

## Known limitations

- Only the clips bundled in the firmware can be played; you cannot import your own audio.
- Volume and battery are shown on the settings page only.

## Reference

- [Audio compression trade-offs](docs/reference/shinku-chen/audio-compression-trade-offs.md) — measured size/quality comparison for IMA-ADPCM.
- [Peripheral power-off before deep sleep](docs/reference/shinku-chen/deep-sleep-peripheral-power-off.md) — what to switch off to save power.


## License

MIT, see [`LICENSE`](LICENSE).
