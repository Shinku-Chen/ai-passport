<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Connect Four

A **landscape** Connect Four for the AI Passport: a 10 × 7 board you can play against the computer (easy / medium / hard), against a second player on the same device, or against a second unit over Bluetooth LE.

## Controls

Up / down moves the target column (when held in landscape, "up" is on the right edge), **OK** drops a piece, and a **long press on OK** returns to the settings screen. In settings, up / down picks a row and OK cycles the value (mode: `HUMAN vs AI` / `AI vs HUMAN` / `TWO PLAYERS` / `LINK PLAY`, difficulty: `EASY` / `MEDIUM` / `HARD`, preview: `LANDING` / `TOP ROW`); selecting `START` begins a game.

Link play needs no host/join choice: set both units to `LINK PLAY` and press `START`. Each unit advertises and scans, the one with the larger BLE address connects, and the screen walks through `SEARCHING...` / `CONNECTING...` / `HANDSHAKE...` before the game starts automatically. Only the side to move can drop a piece (the header shows `YOUR TURN` / `PEER TURN`); after a game, **OK** requests a rematch and a **long press on OK** leaves link play and powers the radio down.

## Features

- **Three computer levels** — a wall-clock search budget keeps each move under one second; the lower levels add a fixed blunder rate.
- **Direct unit-to-unit play** — peer discovery plus a stop-and-wait reliability layer (per-packet sequence, piggybacked ACK, timeout retransmit) and a per-move disc count check, so a dropped notification cannot silently desync the boards.
- **Sample-free audio** — column move, drop, win, loss and draw tones are synthesised from a sine table at runtime; no flash assets.
- **Idle deep sleep** — 60 s on the settings screen and 180 s in a game, any key wakes it; link play relaxes this to 10 minutes and stops BLE before sleeping.
- **Optional serial screenshot** — `FAP_SCREENSHOT_V1` returns the real 320 × 240 frame for cover art and checks.

## Dependencies

**Standalone and offline** for normal play; link play uses only the unit's own Bluetooth LE, with no phone, network or account. On a PC, `tools/c4_peer.py` (requires `pip install bleak`) can act as the peer to exercise the protocol when you only have one board.

## Build and flash

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin            # produces merged-binary.bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

The default firmware includes `LINK PLAY` but not the serial screenshot: the screenshot reserves a full 320 × 240 frame in `.bss`, which does not fit alongside the BLE heap on a board without PSRAM. Build with `idf.py -DC4_ENABLE_SCREENSHOT=ON build` when you need it; BLE is unavailable in that configuration and the link screen shows `BLE UNAVAILABLE`.

## Release

Tag `v1.7.0-connect-four`; the release ships the merged image `FoloToy-AI-Passport-full.bin`.

## Known limitations

- Link play needs two units on the same version; BLE and the serial screenshot cannot coexist (see above).
- A dropped link only shows `PEER LEFT` and keeps searching; the previous board state is not restored.
- The computer opponent searches on a wall-clock budget, so the first move after a slow boot can be conservative.

## Reference

- [Two-device BLE link on a PSRAM-less AI Passport](docs/reference/shinku-chen/two-device-ble-link.md) — measured heap figures and two NimBLE traps that only show up on hardware.

## License

MIT, see [`LICENSE`](LICENSE).
