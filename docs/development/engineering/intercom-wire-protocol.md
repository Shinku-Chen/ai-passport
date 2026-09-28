<p align="right">
  <a href="intercom-wire-protocol.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Intercom Wire Protocol (Device to Phone)

This document is the contract between the AI Passport intercom firmware on
`feature/openclaw-intercom` and the Android companion app in
`ai-passport-openclaw-android` on `feature/gateway-adapters`. The device is a BLE
peripheral that captures speech and renders text; the phone is the BLE central
that owns the network path to an agent gateway. Either side may change this file;
the two repositories must be changed in the same iteration, because the framer is
the only thing that keeps them in sync.

The firmware side of this contract is specified to be transport-independent where
possible: frames, merging, sequencing, and loss accounting are pure logic and are
covered by host tests. The BLE layer only moves bytes.

## Roles and transport

| Item | Value |
| --- | --- |
| Device role | BLE peripheral, connectable advertising, one connection (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`) |
| Phone role | BLE central (GATT client) |
| Advertising name | `Passport-XXXX` (last two bytes of the BT MAC) |
| Service | Nordic UART Service, `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| Host to device | `6e400002-...` (RX), `WRITE` + `WRITE_NO_RSP` + `WRITE_ENC` |
| Device to host | `6e400003-...` (TX), `READ` + `NOTIFY` + `NOTIFY_INDICATE_ENC`, CCCD `00002902-...` |
| Preferred ATT MTU | 247 (notify payload `min(mtu - 3, 244)`) |
| Connection interval | negotiated by the phone; a turn may request a high-priority interval, idle may relax it |
| Encryption | LE Secure Connections, MITM, bonding, 16-byte key; all payload characteristics are encryption-gated |

The NUS layout is used deliberately: it is what generic BLE tools and a phone can
talk to without either side inventing a new profile, and it keeps the device
compatible with the two-device link and the Claude Buddy port.

Audio flows device to host as notifications. Control and text flow host to device
as writes. Both directions use the same framing, so one reassembler implementation
serves both sides.

## Frame format

```text
[0] MAGIC0   0xA5
[1] MAGIC1   0x5A
[2] TYPE     see the type table
[3] FLAGS    see the flag table
[4..5] LEN   payload length, big endian
[6..] payload
```

Total frame length is `6 + LEN`. The magic is not decoration: an early version
used a single type byte, and because PCM samples frequently equal `0x01`-`0x04`, a
single lost notification locked the receiver into treating audio bytes as headers
forever. A two-byte magic makes false alignment roughly a 1-in-65536 event per
byte offset, and it lets the receiver resynchronize after any loss.

### Types

| Type | Name | Direction | Payload |
| --- | --- | --- | --- |
| `0x01` | `AUDIO_PCM` | device to host | `[SEQ:1]` + `int16` little-endian mono PCM, 1024 bytes of PCM per frame |
| `0x02` | `TEXT` | host to device | `[role:1]` + UTF-8 text, at most 2048 payload bytes per frame |
| `0x03` | `CONTROL` | both | UTF-8 JSON object, command or acknowledgement |
| `0x04` | `EVENT` | device to host | UTF-8 JSON object |
| `0x05` | `AUDIO_OPUS` | device to host | `[SEQ:1]` + one Opus packet, at most 512 payload bytes |

`AUDIO_OPUS` is the default uplink. `AUDIO_PCM` remains implemented as the
fallback path so a device that cannot meet the encoder budget can still talk, and
so the app can be tested against a build without the encoder.

### Flags

| Bit | Name | Meaning |
| --- | --- | --- |
| `0x01` | `MORE` | another frame of the same logical message follows |
| `0x02` | `FIRST` | first frame of a multi-frame message |
| `0x04` | `LAST` | last frame of a multi-frame message |

A frame with all flag bits clear is a complete single-frame message. Text uses
`FIRST` / `MORE` / `LAST`; audio frames are independently decodable and their
boundaries are expressed by `turn_start` / `turn_end` events, not by flags.

## Session handshake

After the phone has discovered the service, enabled encryption, and subscribed to
the TX characteristic, each side announces itself once:

```json
{"ev":"hello","proto":1,"caps":["opus","pcm","text","time"],"model":"Passport-3A2B","fw":"0.1.0"}
```

The device sends this as an `EVENT` frame as soon as notification subscription is
confirmed; the phone sends the same shape as a `CONTROL` frame once its write
queue is usable. `proto` is the integer protocol version of this document. The
phone must not send audio-dependent commands before it has seen the device hello,
and the device must show a connected state only after both halves are done:
encrypted, subscribed, and hello exchanged.

Capability negotiation is deliberately one-way and advisory: the phone picks the
uplink codec from `caps` (`opus` when present, otherwise `pcm`) and reports its
choice through the `turn_start` control frame, so a mismatch is visible in the log
instead of being silent.

## Audio uplink

Default codec is Opus, matching what the reference voice assistant firmware on
this chip family ships and what the ASR endpoint accepts directly:

| Parameter | Value |
| --- | --- |
| Sample rate | 16000 Hz |
| Channels | 1 |
| Frame duration | 60 ms (960 samples, 1920 PCM bytes) |
| Application | `OPUS_APPLICATION_VOIP` |
| Complexity | 0 (higher settings starve the ESP32-C3 and are not needed for speech) |
| DTX | enabled; silence may produce no packet at all |
| Expected packet size | roughly 40 to 200 bytes, worst case 512 payload bytes |

Consequences that shape the rest of the design:

- The uplink moves about 3 KB/s instead of the 32 KB/s of raw 16-bit PCM, so one
  Opus frame fits in a single notification at MTU 247 and no fragmentation or
  reassembly is needed on the hot path.
- Because a 60 ms frame is small, the BLE link no longer has to be tuned for
  bandwidth; it can be tuned for latency during a turn and for power when idle.
- Losing a packet costs 60 ms of audio, which the recognizer tolerates. Audio is
  never retransmitted.

The capture pipeline is: I2S read at 16 kHz, a 60 ms accumulation buffer, one
encode, one frame, one notify. The encoder runs in the audio task; the frame is
handed to a bounded transmit ring that a separate sender drains, so a busy radio
can never block capture.

`SEQ` is a one-byte counter incremented per audio frame and wraps at 256. It is
used only for loss accounting: the phone counts gaps and reports a loss ratio, the
device counts failed notifications. A loss ratio above about one percent over a
turn means the chosen codec or connection parameters are wrong, and the log must
say so instead of silently degrading.

## Text downlink

`TEXT` payload starts with a role byte:

| Role | Meaning |
| --- | --- |
| `U` | recognized user speech (what the user said) |
| `A` | assistant reply (what the gateway answered) |
| `R` | reserved for system notices rendered as a message |

Rules:

- Long text is split at 2048 payload bytes per frame, never in the middle of a
  UTF-8 sequence; the device merges `FIRST` / `MORE` / `LAST` frames into one
  message and only renders after `LAST`.
- The device merges into a bounded buffer (2048 bytes); an overflow must be
  reported and truncated, not silently dropped or truncated mid-glyph.
- A text frame may arrive while audio is being sent. It must not be interleaved
  into an audio payload: the sender must finish or drop the current audio frame
  before writing text, and the reassembler on the phone must still handle the case
  where an inserted frame header appears inside a partially received audio frame.
- Text is the reliable direction: it is written with response, so each frame is
  acknowledged at the ATT layer. The phone must treat a failed write as a lost
  message and surface it, because the device screen is the only place the user can
  read the answer.

## Control channel

`CONTROL` payload is a JSON object. Defined commands:

| Command | Direction | Meaning |
| --- | --- | --- |
| `{"cmd":"hello","proto":1,...}` | host to device | phone half of the handshake |
| `{"cmd":"turn_start","codec":"opus"}` | host to device | the phone accepted the turn and started recognition |
| `{"cmd":"time","epoch":<seconds>}` | host to device | set the device clock shown in the status bar |
| `{"cmd":"clear_display"}` | host to device | clear the conversation area |
| `{"cmd":"status"}` | host to device | request device status (battery, volume, microphone gain, link) for the app console |
| `{"cmd":"audio","volume":<percent>,"mic_gain_db":<number>}` | host to device | set output volume and microphone gain; the app owns both settings |

Unknown commands must be ignored, not treated as an error, so a newer phone can
talk to an older device.

## Events

`EVENT` payload is a JSON object. Defined events:

| Event | Meaning |
| --- | --- |
| `{"ev":"hello",...}` | device half of the handshake, sent once per subscription |
| `{"ev":"turn_start"}` | the user started holding the talk button; audio frames follow |
| `{"ev":"turn_end","frames":<n>,"codec":"opus","dropped":<n>}` | the user released the button; the phone may now finalize recognition |
| `{"ev":"status","battery":<percent>,"volume":<percent>,"mic_gain_db":<number>,"link":"ready"}` | device status snapshot, also sent on request and after any change |
| `{"ev":"error","code":"<code>","detail":"<text>"}` | device-side failure worth showing in the app console |

`turn_end` is the only signal that ends a turn. The phone must never infer the end
of speech from a gap in audio frames, because DTX produces legitimate silence with
no packets at all.

## Reliability, backpressure, and loss accounting

There is no application-level acknowledgement for audio, and none is wanted. The
rules that keep the pipeline honest are:

- Device: the transmit ring is bounded. When the radio is unavailable, is
  backlogged, or returns no buffer, the frame is dropped, a counter is incremented,
  and the counter is reported in the next `turn_end`. Capture never blocks on the
  radio.
- Device: a text frame is written by the phone with response, so a failure means
  the phone must retry once after a short delay and then report the message as
  undelivered in its own UI.
- Phone: incoming notification bytes are copied immediately into a queue and
  processed on one worker, never in the BLE callback.
- Phone: starting a new turn clears any queued but unsent text frames, so a stale
  answer cannot be rendered after the user has started speaking again.

## Resynchronization

The receiver treats its input as a byte stream and must recover from any of:

1. A truncated frame at the end of a notification, which is normal.
2. A lost notification, which leaves a hole in the middle of a frame.
3. A frame header inserted in the middle of another frame's payload.

Recovery rules:

- While waiting for a header, scan forward for `0xA5 0x5A`, then require a known
  type and a length that fits the reassembly buffer. Anything else advances one
  byte and keeps scanning.
- While collecting a payload, also scan for an inserted header. The check is
  deliberately narrow: a candidate must have a known type, a plausible length, and
  must not be an audio frame nested inside an audio frame. On a hit, discard the
  partial frame and restart at the candidate.
- The reassembly buffer must be large enough for the largest legal frame
  (`6 + 2048`), and a declared length larger than that is treated as corruption.

## Pairing and security

- Pairing is initiated by the phone. The device displays a random six-digit
  passkey; the phone prompts for it and calls `setPin` plus
  `setPairingConfirmation`.
- The displayed passkey must be at least 100000. A value with leading zeros fails
  on the Android side during the confirm/random exchange while still working with
  a Linux central, which makes it a hardware-only bug worth stating explicitly.
- Bonds are stored in NVS, so a reconnect encrypts without a prompt. NVS must be
  initialized before the radio starts, otherwise the PHY reloads calibration data
  on every boot.
- Every characteristic access callback must exist, including for notify-only
  characteristics; a missing callback fails service registration with an error that
  reads like an allocation failure.
- Because all payload characteristics are encryption-gated, a write that arrives
  unencrypted must be rejected rather than processed.

## Versioning

`proto` in the hello payload is the version of this document. A change that is not
backward compatible bumps it and must be implemented on both sides in the same
release. Additive changes (a new event, a new control command) do not bump the
version; the receiver ignores what it does not understand.

## Deliberately rejected alternatives

- **Raw PCM uplink as the only path.** It works, but 256 kbps leaves no room for
  radio retries, and it forces the connection parameters into a corner where the
  audio quality depends on the phone model. It stays as a fallback only.
- **A second characteristic per direction.** Two characteristics already cover the
  two directions; adding one per message class multiplies the GATT table without
  buying anything the frame type does not already carry.
- **Application-level ACKs for audio.** 60 ms of speech is not worth a round trip;
  the recognizer handles gaps better than a stall would.
- **Two-byte or larger sequence numbers.** One byte is enough to detect loss at
  the rates involved; the counter is diagnostic, not a delivery guarantee.

## Test hooks

- Framing, reassembly, text merging, sequence accounting, and turn state are pure
  logic and belong in host tests on both sides; the Android side mirrors them as
  JVM unit tests.
- A host-side BLE peer (Python plus `bleak`) speaks this protocol against a real
  board without a phone, which is the fastest way to reproduce framing bugs.
- On device, the recomputed loss ratio per turn and the encoder timing are the two
  numbers to log; both are needed to decide whether the Opus path holds on this
  chip.

## Device controls (informative)

Not part of the wire contract, but the app side must not contradict it:

| Input | Behaviour |
| --- | --- |
| Hold `OK` | Start a turn; audio frames flow until release |
| Release `OK` | End the turn; the phone finalizes recognition |
| Short press `OK` | Open the device settings page (device information, brightness, back) |
| `UP` / `DOWN` | Browse conversation history, newest first; inside the settings page they move the highlight or step the value |

Output volume and microphone gain are not adjustable on the device; they come from
the app as the `audio` control command. Brightness stays device-local so the screen
remains usable without a phone.

## Settled and open items

Settled:

- `status` is on demand only. The device stays powered and connected while the
  screen is off, so the phone pulls status when it needs it; there is no periodic
  heartbeat. The device still reports a fresh `status` after any change it applies.
- Output volume and microphone gain belong to the app: they arrive as the `audio`
  control command and are echoed in `status`. Brightness stays device-local so the
  screen remains usable without a phone; the on-device settings page keeps device
  information, brightness, and back.

Still open:

- The PCM fallback frame size (1024 bytes of PCM per frame) is inherited from the
  reference firmware and has not been re-measured since the Opus path became the
  default.
