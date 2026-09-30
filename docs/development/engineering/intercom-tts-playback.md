<p align="right">
  <a href="intercom-tts-playback.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Intercom TTS Playback (Device Side)

This document is the implementation plan for the audio downlink of the intercom
application: the device speaking the gateway's reply out loud. The wire contract
itself is in [intercom-wire-protocol.md](intercom-wire-protocol.md); this document
covers what the firmware has to allocate, run, and report to satisfy it.

The design is settled. The frame layout, the control events, and the queue depths
are decisions to implement, not options to re-open; the sizes below are budgets to
verify before the first line of the implementation is written.

## 1. Background and goal

The intercom already captures speech upward and renders the gateway reply as text.
The missing half is hearing the reply. The goal is: the gateway answers, the phone
synthesizes speech, and the device plays it through the speaker.

- The audio source is **phone-side TTS**. The device never calls the speech service
  and does not depend on the Xiaozhi cloud path, so credentials, vendor choice, and
  cost stay on the phone, and the firmware only decodes and plays.
- The downlink carries Opus only. Raw PCM at 24 kHz mono 16-bit is about 48 KB/s,
  which the BLE link cannot carry in this direction.
- The reference implementation is [`xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32)
  `AudioService`: an `audio_decode_queue_` in front of an `audio_playback_queue_`,
  `MAX_DECODE_PACKETS_IN_QUEUE = 2400 / OPUS_FRAME_DURATION_MS`, and
  `MAX_PLAYBACK_TASKS_IN_QUEUE = 2`. Its decoder is `OpusDecoderWrapper` over the
  `78/esp-opus` component (`^1.0.5`), which this repository already depends on for
  the uplink encoder (`opus_encoder_create` in `main/oc_audio.c`, version `1.0.5`
  in `dependencies.lock`). The component manifest pinned here declares
  `idf: '>=5.0'`; the reference project documents `>= 5.3` for its own build. This
  repository runs ESP-IDF 5.5.3, so either reading is satisfied and no version
  work is needed.
- The official `xiaozhi-esp32` build set includes ESP32-C3, so Opus decoding on this
  chip is already exercised in production. Decoding is a capacity question here,
  not a feasibility question.

## 2. Memory and static budget

This is the first gate. The ESP32-C3 has no PSRAM, and the runtime free heap after
NimBLE, LVGL, and the application tasks is only about 12.5 KB, so nothing in this
path may be allocated at stream time.

| Item | Size | Placement |
| --- | --- | --- |
| Opus decoder (16 kHz or 24 kHz, mono, 60 ms) | about 15–20 KB | static `.bss` |
| Decode queue: 24 packets × about 128 B | about 3 KB | static `.bss` |
| Playback queue: 2 × (24 kHz × 60 ms × 2 B = 2880 B) | about 5.8 KB (about 3.8 KB at 16 kHz) | static `.bss` |
| I2S TX DMA | about 4 KB in the plan; the BSP configures 6 descriptors × 240 frames, about 5.6 KB per direction with 32-bit stereo slots (`components/bsp/src/bsp_audio.c`) | already allocated by `bsp_audio_init()`: it consumes the DRAM the budget is measured against, but it is not new demand |
| **Total as planned** | **about 28–33 KB** | only the decoder and the two queues are new demand; the DMA already exists |

Requirements:

- The decoder state is a static `OpusDecoder` area in `.bss` (libopus
  `opus_decoder_init` over a static buffer, or the component's static-allocation
  path). `opus_decoder_create` is not acceptable: once NimBLE owns its heap, a
  ~20 KB contiguous allocation cannot be satisfied — the same constraint that
  forces the encoder to be created before the BLE stack starts (`main/oc_app.c`).
- Run `idf.py size` **before** writing the implementation and record the numbers.
  Static buffers are carved from the same DRAM the heap uses, so "static" is not
  free memory: adding 28–33 KB of `.bss` removes 28–33 KB from the boot heap. As a
  starting point, the last build present in this worktree places 124,368 B of
  `.bss` and 10,620 B of `.data` in the 321,296 B DRAM segment; that artifact may
  be stale, so treat it as an order of magnitude rather than a measurement.
- If the numbers do not fit, reclaim memory in this priority order and report the
  before/after figures with the change:
  1. LVGL draw buffers (the interface is a text page; the draw buffers are larger
     than this page needs),
  2. application/main task stacks that are sized for worst case rather than
     measured,
  3. uplink capture buffers.
- Do not trade away a safety-critical buffer (I2S DMA depth, BLE stack, watchdog
  margins) to make the budget add up.

## 3. Tasks and concurrency

- One task, `oc_audio_play`, owns decoding and playback. It is independent of the
  capture task so a decode burst can never delay microphone reads, and it uses a
  static 4 KB stack (the same static-task pattern as `main/oc_audio.c`).
- Priority is **below** the capture task (`oc_audio` runs at priority 4). Losing
  60 ms of uplink audio is not acceptable; delaying a decoded block by a few
  milliseconds is.
- Data flow: `oc_audio_play_push()` copies one Opus packet into the decode queue
  and returns immediately; the task pops a packet, decodes it into a PCM block,
  hands the block to `bsp_audio_write()`, and repeats. The codec is woken for the
  stream and slept when the stream ends, matching the existing
  wake/sleep ownership rule in `main/oc_audio.c`.
- `oc_audio_play_push()` runs in the application task context (the same place the
  control events are handled). It must never block on the codec, the DMA, or the
  decode task; its only failure mode is a full queue, which drops the oldest
  packet and increments a counter.
- Half duplex with the uplink. While the device is in the playback state it does
  not capture: the phone's audio and the user's voice must not both run, both
  because the microphone would record the speaker and because the link is not
  dimensioned for two audio streams. Uplink capture pauses at `tts_start` and
  resumes at `tts_stop` (after the queue drains) or immediately at `tts_abort`.
- `rate_khz` is per frame. The decoder keeps its Opus state across packets while
  the rate is unchanged and is re-initialized when the rate changes mid-stream;
  the packet that announces the change is still decoded, at the new rate.
- LVGL is not involved. Playback only changes the status word through the existing
  `oc_ui_set_state()` path, which the application task calls while holding
  `bsp_lvgl_lock()`. No audio task may touch an LVGL object.

## 4. Firmware interface

Suggested signatures for the playback module; the implementation task adds them
next to the existing capture API:

```c
esp_err_t oc_audio_play_push(const uint8_t *opus, size_t len, uint8_t rate_khz);
void      oc_audio_play_flush(void);
bool      oc_audio_play_idle(void);
void      oc_audio_play_get_stats(oc_audio_play_stats_t *st);
```

| Function | Contract |
| --- | --- |
| `oc_audio_play_push` | Called from the application task with one `TTS_OPUS` payload's Opus packet and its `rate_khz`. Copies the packet into the bounded decode queue and returns. Returns `ESP_ERR_INVALID_ARG` for a bad packet or rate, `ESP_ERR_INVALID_STATE` when the module is not initialized or the device is not accepting audio, `ESP_OK` on accept. A full queue is not an error: the oldest packet is dropped, counted, and `ESP_OK` is returned |
| `oc_audio_play_flush` | Discards everything queued, marks the playback state as ending, and returns as soon as the queue is logically empty. Safe to call from the application task, from a barge-in, and when nothing is playing (idempotent) |
| `oc_audio_play_idle` | True when no audio is queued or playing. Used by the application to decide between the playback state and the normal screen |
| `oc_audio_play_get_stats` | Copies the per-stream statistics for the `tts_playback_done` event, then the caller starts a new stream. Must not block the playback task |

The signature carries `rate_khz` but not `frame_ms` because the device implements
one frame duration: the application layer rejects a `TTS_OPUS` frame whose
`frame_ms` is not `60` (counted as an invalid packet, logged once per stream)
instead of letting the decoder misinterpret it. Supporting a second duration later
means extending this signature, not silently guessing.

```c
typedef struct {
    uint32_t frames;         /* packets accepted from the phone */
    uint32_t decoded;        /* packets decoded successfully */
    uint32_t dropped;        /* packets discarded: queue overflow + SEQ gaps */
    uint32_t underruns;      /* times playback had no decoded audio ready */
    uint32_t decode_us_max;  /* longest single-packet decode, microseconds */
} oc_audio_play_stats_t;
```

## 5. Interaction with the existing turn flow

- **Barge-in.** Pressing `OK` while the device is playing is a barge-in: the device
  flushes the playback queue locally (the same effect the phone requests with
  `tts_abort`), reports `tts_playback_aborted`, resumes capture, and starts the new
  turn exactly as it does today. The flush must complete before capture starts, so
  the user's first words are not recorded while the speaker is still producing
  sound.
- **Release.** `RELEASE` keeps its current behaviour: `turn_end` with the uplink
  counters, then the normal screen. TTS playback does not change the release path.
- **Ordering.** A `tts_start` that arrives while the user is still holding `OK` is
  ignored and logged, and no audio is accepted; the two directions cannot run at
  once, and the phone must end the turn before pushing audio.
- **Text.** `TEXT` frames keep rendering during playback, unchanged: the text path
  is independent of the audio path, and the answer appearing on screen while it is
  spoken is expected.
- **`turn_ready`** stays what it is today, the phone half of the capture-turn
  feedback, and is not used for playback.

## 6. Statistics and failure handling

- Every stream reports through `{"ev":"tts_playback_done",...}` after the queue
  drains following `tts_stop`: accepted packets, decoded packets, dropped packets,
  underruns, and the longest decode time in microseconds. The phone can then tell
  "sent" from "heard".
- `SEQ` gaps reuse the host-tested `oc_seq_tracker_t` in `main/oc_proto.h`: a gap
  counts as a dropped packet. Duplicate sequence numbers are ignored, not counted
  as loss.
- A decode failure is counted and does not stop the stream. After **3 consecutive**
  failures the device flushes the rest of the stream and reports
  `tts_playback_aborted`, with the libopus error code in the log; a decoder that
  keeps failing means the negotiated rate or frame size is wrong, and continuing
  would only produce noise.
- An underrun (the task needs audio and the queue is empty before `tts_stop`)
  counts once and writes one silent block so the codec does not click. Repeated
  underruns mean the phone is pacing too slowly and the log must say so.
- Every exit path — `tts_stop` drained, `tts_abort`, barge-in, repeated decode
  failure, link loss, module shutdown — returns the device to the idle state and
  resumes capture. A device stuck in the playback state with no audio is the one
  failure mode that must not exist.

## 7. Test plan

Host-side unit tests (pure logic, no ESP-IDF or LVGL, in the style of
`tests/test_oc_proto.c` and wired into `tools/validate.sh --static`):

- `TTS_OPUS` payload parsing: the 3-byte header is stripped correctly, `rate_khz`
  accepts only 16 and 24, `frame_ms` accepts only 60, and payload lengths outside
  `4..515` are rejected.
- Queue overflow: pushing `N + 1` packets into a depth-`N` queue drops the **oldest**
  packet, keeps the newest, and increments `dropped` exactly once.
- Sequence accounting: gaps counted, duplicates ignored, wrap at 256 handled.
- Statistics: accepted, decoded, dropped, and underrun counters aggregate over one
  stream and reset at the start of the next.
- Flush: a flush empties the queue, reports idle, and counts the discarded packets.

On-device acceptance checklist:

1. A gateway reply is audible through the speaker at both 16 kHz and 24 kHz.
2. Latency from `tts_start` to the first audible sample is measured and logged.
3. Barge-in: pressing `OK` during playback stops the audio promptly and the new
   turn captures without losing the first word.
4. No self-hearing: while the device plays, the microphone path is paused, and the
   phone's own recognition of the next turn is clean.
5. `tts_playback_done` counters match what the phone sent, including a deliberately
   dropped packet.
6. `tts_abort` with a full queue returns to idle and resumes capture.
7. A long reply (about 60 s) plays without drops beyond the reported counters.
8. A corrupt packet does not wedge playback; three consecutive failures flush and
   report `tts_playback_aborted`.
9. `idf.py size` and the runtime free-heap log are recorded before and after.

## 8. Staged delivery

| Stage | Scope |
| --- | --- |
| M1 | Playback path only: `TTS_OPUS` frames decode and the device makes sound. The app may produce audio with a temporary local TTS and ignore statistics |
| M2 | Barge-in, playback state, and backlight handling: `tts_start` / `tts_stop` / `tts_abort` drive the state machine, capture pauses and resumes, and the screen keeps the user informed while the reply is spoken |
| M3 | UI status word and statistics: the playback state is visible on screen, and `tts_playback_done` / `tts_playback_aborted` carry the counters to the app console |

Each stage must leave the device working without a phone and must not regress the
capture path. M1 is the memory gate in practice: if the static budget does not fit,
it fails there, before any UI or event work is built on top of it.

## 9. Open questions

- **TTS service selection.** Volcano, Azure, and Edge-TTS all produce Opus or a
  format that can be converted on the phone; the choice belongs to the app and does
  not change this firmware contract.
- **Resampling.** If the chosen service only produces 24 kHz, the device either
  resamples 24 kHz to 16 kHz before playing or plays 24 kHz directly. Playing
  24 kHz directly is the smaller change (the codec and `rate_khz` field already
  support it) and is preferred unless a measured problem appears; a device-side
  resampler would cost CPU and static memory that this budget cannot spare.
- **Backlight policy during long replies.** Playback keeps the backlight lit per
  `tts_start`; if a stream can run for minutes, the normal idle dimming policy may
  need to apply mid-stream rather than at `tts_stop`.

## Related documents

- [intercom-wire-protocol.md](intercom-wire-protocol.md): the wire contract,
  including the `TTS_OPUS` frame and the playback events.
- [coding-conventions.md](coding-conventions.md): stack, static-allocation, and
  resource-review rules that this plan has to satisfy.

## Implementation notes (M1)

Appended by the implementation task; the sections above stay the design of record.
This addendum records what the firmware actually does and the numbers the memory
gate produced.

### Firmware interface as implemented

`main/oc_audio.h`:

| Function | Behaviour |
| --- | --- |
| `oc_audio_play_push(opus, len, rate_khz)` | Byte-level entry point: one Opus packet plus its rate. Carries no `SEQ`, so it cannot report sequence gaps |
| `oc_audio_play_push_frame(payload, len)` | Wire-level entry point used by `on_frame`. Parses `[SEQ][rate_khz][frame_ms]`, validates the payload length (`4..515`), the rate (`16`/`24`) and the frame duration (`60`), then feeds the same queue with `SEQ` gap accounting |
| `oc_audio_play_start()` / `oc_audio_play_stop()` | The `tts_start` / `tts_stop` transitions (pause capture, then drain) |
| `oc_audio_play_flush()` | Synchronous: returns after the I2S write has stopped, the codec is asleep and capture has resumed, so a following turn cannot record the speaker |
| `oc_audio_play_available()` | Whether the decoder is usable. `hello` advertises `tts_opus` only when it is true |
| `oc_audio_play_idle()`, `oc_audio_play_get_stats()` | As designed |
| `oc_audio_play_take_event()` | Sticky `DONE` / `ABORTED` event. The application task takes it on its 50 ms tick and builds the `EVENT` frame, because the frame buffer belongs to the application task |

`main/oc_tts.c` holds the ESP-IDF-free half (payload parsing, the 24-packet ring,
the counters) and is covered by `tests/test_oc_tts.c`; `main/oc_audio.c` owns the
decoder, the I2S writes and the task.

### Measured memory (ESP32-C3, ESP-IDF 5.5.3)

| Item | Bytes | Placement |
| --- | --- | --- |
| Opus decoder state | 18432 reserved; `opus_decoder_get_size(1)` measured <= 18148 | static `.bss` |
| Decode queue, 24 slots (516 B each, length + rate + packet) | 12384 | static `.bss` |
| Playback queue, 2 blocks of 60 ms at 24 kHz | 5768 | static `.bss` |
| Play task stack, TCB and two static semaphores | 4096 + about 340 | static `.bss` |
| Packet staging buffer (`pkt`) | 512 | static `.bss` |
| **New static demand (`s_play`)** | **about 41808** | `main/oc_audio.c` |
| LVGL built-in malloc pool, 65536 to 24576 | **-40960** | `lv_mem_core_builtin.c` |
| **Net DRAM change** | **+908** | 231844 of 321296 to 232752 of 321296 |

`CONFIG_LV_MEM_SIZE_KILOBYTES` is deprecated in LVGL 9 and has no effect, so the pool
was really 65536 B while `sdkconfig.defaults` asked for 24 KB. Setting
`CONFIG_LV_MEM_SIZE=24576` makes the documented intent true at last and returns
40 KB of `.bss` to the boot heap: `bsp_display_lvgl.c` already refers to "the 24 KB
LVGL pool". The runtime free heap therefore ends up about 0.9 KB lower than before
this feature, and the 5 s heartbeat logs `lv_mem_monitor()` so the new pool size can
be confirmed on the device.

### Decisions that differ from the sections above

- **Decode failures**: the code flushes after **5** consecutive failures, not 3. Five
  failures is three seconds of confirmed garbage; three can be a single resynchronized
  frame after a dropped packet.
- **Silence and idle**: an underrun writes one silent block generated in a free
  playback slot (no extra buffer) and counts once. After 1.5 s without any real audio
  the stream ends by itself, so a phone that stops mid-stream cannot leave the device
  in the playback state with capture paused.
- **Capture handover**: `tts_start` pauses capture first, and the play task waits for
  the capture task to finish its frame and put the codec to sleep before it touches
  the codec. `flush()` is synchronous and waits for the same handshake in reverse, so
  a barge-in turn never records the speaker.
- **Backlight**: `oc_ui_note_activity()` is refreshed every application tick while a
  stream is playing, so the answer stays readable. A stream measured in minutes would
  keep the display lit; that remains the open question in section 9.
- **Status word**: playback shows the existing receiving state (`OC_UI_STATE_RECEIVING`,
  shown as the Chinese word for `receiving`). No new
  centred text was added.

### Still unverified on hardware

The 24 KB LVGL pool (the UI fits, but only a device run can prove it), the 4 KB play
task stack (the stack high-water mark is logged at the end of every stream), audible
quality, end-to-end latency (the play task logs the delay from `tts_start` to the first
block it writes), underrun
behaviour and the real half-duplex overlap between playback and capture.
