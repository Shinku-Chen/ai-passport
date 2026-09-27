<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Connect Four

A landscape Connect Four for the AI Passport: a **10 × 7 board**, human versus
computer with three difficulty levels (either side can move first), two players
sharing one device, or **two devices playing each other over Bluetooth LE**.

Latest release: **v1.7.0-connect-four**, which adds two-device play over Bluetooth LE.

- Branch: [`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four)
- Release: [v1.7.0-connect-four](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.7.0-connect-four)

## Controls

UP / DOWN move the column cursor (held in landscape, UP is the right-hand key),
**OK** drops a disc, and **OK (hold)** returns to the settings screen. On the
settings screen UP / DOWN picks a row and OK changes its value (mode: `HUMAN vs AI`
/ `AI vs HUMAN` / `TWO PLAYERS` / `LINK PLAY`, level: `EASY` / `MEDIUM` / `HARD`,
preview: `LANDING` / `TOP ROW`); selecting `START` begins a match, or enters the
link screen in `LINK PLAY`. The two computer modes differ only in who moves first.

## Two-device link play

1. Set `MODE = LINK PLAY` on both devices and press `START`. Each device
   broadcasts its own service and scans for the other at the same time, so there
   is no host/join choice: whichever BLE address is larger connects out, and the
   two devices independently reach opposite roles.
2. The link screen reports `SEARCHING...`, `CONNECTING...` or `HANDSHAKE...`,
   then the match starts by itself once both sides have exchanged a `HELLO`.
3. The device that initiated the connection (the one with the larger BLE address) plays
   first; the first player alternates on every rematch. Only the player whose turn it is can drop a disc - the top line
   shows `YOUR TURN` or `PEER TURN`, and the opponent's disc lands with the same
   animation and sound.
4. After a game, `OK` asks for a rematch; the new game starts once both sides have
   asked. `OK (hold)` leaves the link and powers the radio down. If the peer
   leaves or the link drops, the screen shows `PEER LEFT` and keeps searching.
5. The match is kept in step by a stop-and-wait layer (per-packet sequence number,
   piggybacked acknowledgement, retransmission) plus a per-move ply check, so a
   lost Bluetooth notification cannot silently desynchronize the two boards.

[`tools/c4_peer.py`](tools/c4_peer.py) plays this protocol from a PC over BLE
(`python -m pip install bleak`, then `python tools/c4_peer.py --list`), so the
protocol can be exercised with a single board. The measured heap numbers, the two
NimBLE traps that only appear on hardware, and the reason the serial screenshot
tool is now opt-in are written up in
[Two-Device BLE Link Between AI Passport Boards](docs/reference/shinku-chen/two-device-ble-link.md).

## Highlights

- **Landscape 320 × 240 with a dense board** — 70 positions of 26 px discs spaced
  3 px apart, fitted to the panel by a BSP-level MADCTL rotation.
- **Three AI levels** — a wall-clock search budget keeps every move under about a
  second, while EASY and MEDIUM deliberately blunder at a fixed rate so the game
  stays winnable.
- **Two-device play without a phone** — a symmetric peer discovery over BLE
  (`components/bsp/bsp_ble_link.c`) plus a host-tested message layer
  (`main/c4_link_proto.c`); about 73 KB of heap, measured on the board.
- **Sound without assets** — column, drop, win, loss and draw cues are synthesized
  from a sine table; no audio files are stored in flash.
- **Idle deep sleep** — 60 s on the settings screen or 180 s in a match, then any
  key wakes the device (GPIO0 low-level wake, fixed for the ADC-owned pad). A
  linked match uses a longer 10-minute window, and the radio is stopped before the
  device sleeps.
- **Serial screenshots** — the `FAP_SCREENSHOT_V1` command returns the real
  320 × 240 frame, which is how the release cover was captured. It is now an
  opt-in development build (see below).

## Build variants

The BLE link takes about 73 KB of heap, and the screenshot tool reserves a whole
320 × 240 frame (150 KB) in `.bss`. This board has no PSRAM, so the two cannot
coexist in one image. The default build therefore ships `LINK PLAY` and leaves the
screenshot tool out:

- `idf.py -DC4_ENABLE_SCREENSHOT=ON build` — development/cover build with the
  serial screenshot tool. The BLE link cannot start in this configuration and the
  app reports `BLE UNAVAILABLE` on the link screen.

## Notes

- This branch carries one application. A `feature/*` branch README describes only
  its own application; the fork `main` root README is the catalog of every hosted
  project.
- Firmware is flashed with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/)
  or `esptool` — every release ships a merged `FoloToy-AI-Passport-full.bin`
  written from offset `0x0`. Target board: 8 MB Flash.
- Reusable engineering experience collected from these releases lives under
  [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/).
