<p align="right">
  <a href="voice-guide.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Voice Keychain Guide

This document explains how the **Voice Keychain** product uses voice assets: where the source audio lives, how it is compressed and packed, and how to build and flash the firmware plus its data partition.

## Navigation and playback responsiveness

UP/DOWN changes the selected clip without interrupting playback; OK restarts
playback with the selected clip. Keep these controls and the existing layout.
Focus moves within a visible window update only the previous and next selected
rows. Text is rebound only when the window scrolls, using immutable names from
the compile-time index rather than allocating a copy for each key press.
Returning to the directory keeps the retained selection visible, and the
selected row alone uses horizontal scrolling for long names.

Button callbacks enqueue events without waiting. A priority-3 application
worker handles navigation; the existing LVGL task (priority 4) and audio worker
(priority 5) retain their priorities. The 16-event queue retains discrete
click/OK ordering. Held-key repeats are coalesced to at most one outstanding
repeat per key. Physical press/release events invalidate older repeat tokens
immediately, and the worker rechecks them after taking the LVGL lock. This
cancels both queued repeats and repeats already waiting for rendering, without
discarding clicks. A draw already accepted before release may finish; there is
no accumulated sequence of old repeats to replay after release. If full, new
events are dropped instead of blocking the shared timer task. Volume changes update only the volume label
and apply codec I2C outside the LVGL lock. Battery polling remains every 30
seconds, and volume limits do not trigger redundant codec writes or redraws.
Stop and deep sleep wait for any input operation to finish before teardown;
short critical sections protect playback-job lifetime from audio preemption.
Display DMA size, SPI clock, audio format, partitions, and assets are unchanged.

### Scrolling text regions and measurement

This firmware displays scrolling clip names, not synchronized lyrics. Native
LVGL partial rendering already avoids a full-screen refresh for each marquee
tick. Each title/name now has a transparent viewport exactly one font line
high (19 px), clipping LVGL's extra label invalidation margin. The row's
background/highlight remains 21 px high. Circular scrolling uses 36 px/s rather
than completing every name in 2000 ms, so longer names do not scroll faster.

The real LVGL 9.5.0 software-rendering test uses the production Chinese font,
240x320 RGB565 display, and the same 9600-byte partial buffer. It replays each
variant for 20 simulated seconds. At equal animation speed the cropped and
previous frames are pixel-identical. Measured row flush pixels fall from
2,709,966 to 2,408,478 (11.1%); title pixels from 2,548,944 to 1,931,122 (24.2%).
The fixed-speed variant reduces the largest sampled offset step from 9 to 2 px
in this replay; this is animation geometry, not measured device frame time.
Static labels cause no further flushes after pending invalidation drains.

After resolving managed dependencies with ESP-IDF, run the rendering regression
independently of the lightweight host gate:

```bash
cmake -S tests/lvgl -B build/marquee-host
cmake --build build/marquee-host
ctest --test-dir build/marquee-host --output-on-failure
```

The same C test can run as WASI with the existing Zig/Node toolchain when a
Windows policy prevents executing newly compiled native test programs.
On hardware, each completed/stopped clip logs `Playback metrics` once:
`max_submit_gap_us` includes blocking I2S writes; `max_prepare_gap_us` covers
the read/decode/scheduling gap after the previous write returned. Compare the
same clip and key sequence on each build, alongside audible glitches.
`write_errors` counts BSP write failures. These are feed diagnostics, not an
I2S underrun detector; zero errors or gaps below the theoretical 90 ms DMA
capacity do not prove glitch-free audio.

`./tools/validate.sh --static` covers focus-only updates, window crossings,
clamped boundaries, empty/short lists, restored selections, held-repeat
coalescing, release cancellation, and old/new press isolation. On the board,
play a long clip while repeatedly moving within and across list windows; check
long-name scrolling, rapid OK selections, returning to a scrolled directory,
and volume holds at both limits. Release UP/DOWN during rapid held scrolling
and check that it stops without a tail of queued movements; also reverse the
direction and verify ordinary clicks/OK remain responsive. Listen for gaps and check input response as
well as display motion. Automated checks do not establish playback continuity
or a measured frame rate.

### Decoder memory and playback recovery

Reserve the mono Opus decoder once at the start of `voice_app_start()`, before
the filesystem, screens, and optional screenshot/input tasks allocate memory.
The current fixed-point decoder needs a contiguous 17,776-byte block. Creating
it per clip can fail with `OPUS_ALLOC_FAIL` (`-7`) after heap fragmentation,
even while navigation continues working. The permanent player reuses this
decoder and calls `OPUS_RESET_STATE` for every new clip. Application stop does
not free state that the permanent worker may still access; reboot releases it.

This retains the decoder's memory while idle rather than increasing playback
peak memory. Startup logs `Decoder reserve` and `App ready` with free heap and
the largest block; check filesystem, input, and screenshot-task initialization
as well as successful reservation on the board. Allocation failures are logged.

The real-codec regression compares decoded sample counts and PCM hashes for
two committed clips against fresh-decoder baselines. It then denies all
further Opus allocations while replaying 12 alternating clips through the
production decoder owner, including repeated initialization. Run it with:

```bash
cmake -S tests/opus -B build/decoder-host
cmake --build build/decoder-host
ctest --test-dir build/decoder-host --output-on-failure
```

Use a GCC/Clang-compatible host compiler (including Zig), since the production
fixed-point component uses C99 variable-length arrays and compiler options.
Optimized standalone host tests explicitly retain their assertions. The same
codec regression can run under WASI, using read access to the repository's
assets. On-device acceptance must cover playback after long scrolling, clip
restart/switching, and the previously fixed stop-on-release behavior.

## Source assets

Raw audio lives in `assets/project/<directory>/*.mp3|ogg|wav`. Each directory becomes a top-level item in the keychain UI; each audio file becomes a selectable clip. The folder and file names are displayed in the UI, so keep them human-readable.

- Source media are the user's own assets; record their source and redistribution permission before committing raw files. The repository prefers to keep only the transcoded results (see below) to stay slim and avoid shipping original binaries.
- Non-BMP characters (for example emoji such as `🐦`) are replaced with readable Chinese by the transcoder, because the embedded CJK subset font has no glyph for them.

## Transcoding pipeline

Run the transcoder after adding or changing source audio. The Opus encode needs `ffmpeg` built with `libopus` and reuses `encode_voice.py` for display-name cleaning and the C-index generation:

```bash
python tools/encode_opus.py
```

For each clip the script:

1. Decodes mp3/ogg/wav with `ffmpeg` (libopus) and encodes Opus at 8 kbps / 16 kHz mono.
2. Normalizes loudness (mean RMS toward a target, peak-capped) so all clips sound equally loud.
3. Strips the Ogg container into raw Opus packets (each packet = 2-byte little-endian length + frame), the exact format the firmware decoder reads.
4. Writes `assets/audio/dirNN/clipMM.opus`, updates `assets/audio/voice_index.json`, regenerates `main/voice_index.h` (the compile-time path/name/length table the firmware uses), and packs a `voicefs.img` with the ESP-IDF `spiffsgen.py`.

`assets/audio/dirNN/*.opus` and `main/voice_index.h` are committed, so the data partition stays reproducible even without the raw source. `tools/pack_voicefs.py` repacks `voicefs.img` from those committed clips (no re-encode, no ffmpeg), which is what the build/CI flow uses.

## Storage layout

`partitions.csv` adds a dedicated SPIFFS data partition:

```csv
voicefs, data, spiffs, 0x210000, 0x5F0000,
```

The firmware mounts it via `esp_vfs_spiffs_register` under `/voices` and reads each clip with ordinary `fopen`/`fread`. SPIFFS is an ESP-IDF built-in component, so no external Managed Component is required. The `voicefs.img` is the content of that partition (about 5.9 MB); it is generated by the build, so it is gitignored and never committed.

## Building and flashing

`./tools/validate.sh --firmware` builds the application, packs the data partition, and merges the complete image — bootloader at `0x0`, partition table at `0x8000`, application at `0x10000`, voicefs data at `0x210000` — into `build/FoloToy-AI-Passport-full.bin` (8 MB). That is the firmware intended to flash from `0x0`.

To flash only the data partition (or as a reference), write `voicefs.img` to its offset:

```bash
python -m esptool --chip esp32c3 -p <PORT> write_flash 0x210000 assets/audio/voicefs.img
```

On first boot the partition is mounted with `format_if_mount_failed = true`, so a blank data partition is formatted automatically before reading clips.

## Updating assets

When you add, rename, or remove source audio:

1. Put the files in `assets/project/<directory>/`.
2. Run `python tools/encode_opus.py` to regenerate the clips, `voice_index.h`, and `voicefs.img`.
3. Rebuild the firmware (the `voice_index.h` table changed) and re-flash the data partition — or flash the merged `build/FoloToy-AI-Passport-full.bin` from `0x0`.
