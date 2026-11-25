<p align="right"><a href="port-cafestella-media.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Cafe Stella media integration

This document describes the media path added to the Cafe Stella AI Passport port. The fixed source remains `xuanyide01/cafestella-rw5` @ `90ddb9e020970907b61ad4181f25c65267e6e98b`.

## Source and encoded media

| Item | Source | Encoded |
| --- | --- | --- |
| Title BGM | `src/common/title-bgm.opus`, 1,078,545 B, 48 kHz stereo | 4 kHz mono IMA ADPCM, 516,096 B, 1,027,198 samples / 256.7995 s |
| OP BGM | `src/common/opening-bgm.opus`, 376,222 B, 48 kHz stereo | 8 kHz mono IMA ADPCM, 373,760 B data, 743,959 samples / 92.995 s |
| OP video | `src/common/opening/00..09`, 477 JPEGs, 5 fps, 95.460 s | 287 JPEGs, 224×126 q15, 3 fps, 620,030 B |

`tools/cafe_media.py` generates title 4 kHz and OP 8 kHz IMA raw payloads plus OP frames from the fixed source using ffmpeg/Pillow. `tools/cafe_pack.py` embeds them in `assets/cafe/cafe.pak` as sections 24 (audio table), 25 (audio data) and 26 (OP timeline); OP JPEGs are appended to the image table with IDs 1,997–2,283.

## Playback semantics

- The title track is stored at 4 kHz and doubled to the 8 kHz codec rate; the OP track is stored at 8 kHz and is not doubled. It is restarted only by the title/menu scene lifecycle; it is not truncated to 256.000 s.
- The OP track is stored at full 93 s length. During OP playback the source's pre-EOF guard stops audio at 92.4 s; the 95.460 s video continues to its silent tail. This is an actual source guard, not a claim that all 93 s samples are output in the OP.
- OP video keeps the full 95.460 s timeline. Frame `k` is source frame `floor(k*5/3)`, and the last frame is held to 95.460 s. No stretching or truncation.
- The OP clock comes from the app's monotonic `now_ms`; a lagging tick jumps to the current frame rather than running a catch-up loop, so input remains responsive.
- Audio/codec failure is settled before any short key or long timer is processed; the press/release that surfaced the fault is consumed, and progress/auto/FF/save/CG writes are blocked. Retry preserves the original page, OP frame and media elapsed time, seeks the IMA block and discards its prefix samples, and does not advance the VM. The OP video clock is paused during fault wait; a seek beyond 92.4 s resumes as silent video tail.
- Short press on the OP page skips once and advances the VM once. Natural end does the same once. Long-press release does not add a short action.
- Title music follows title/menu/gallery/settings lifecycle. The music gallery lists and plays the two real tracks. Settings has sound on/off and volume. A failed codec/I2S write stops playback and raises the existing resource fault; there is no busy wait. Stop uses an active-busy acknowledgement, not a stale idle token; a timeout is observable and keeps the device awake rather than deleting a task inside a codec/I2S critical section.

## Band reuse and memory

`cafe_image_render_op` maps a native 224×126 OP JPEG onto the full 224×126 stage using the existing three 224×42 RGB565 bands and `s_stage`. It does not allocate a full OP frame and does not enlarge the LVGL cache. A target RISC-V `-fstack-usage` build reports `decode_jpeg` 176 B, `render_op_band` 160 B, `cafe_image_render_op` 32 B and `out_func` 48 B (max self-frame 176 B); the audio `play_track_at` self-frame is 128 B. The audio task has a 4 KB stack; its decoder/PCM buffers are static (about 5.1 KB) and included in DRAM.

The current candidate app is 8,246,672 B in the 8,323,072 B factory partition (76,400 B free). Static DRAM is 239,250 / 321,296 B (82,046 B free). These are target build measurements, not device runtime guarantees.

## Verification status

Host: packed media CRC/index, all 287 OP frames through the production band path, first/mid/last OP decode, malformed record CRC/JPEG rejection, IMA sample count/reference comparison, 30-screen LVGL UI smoke (64-bit and 32-bit Host HAL), and static tests PASS. Firmware build/merge/verify/archive PASS.

Device: **NOT RUN**. Real ES8311/I2S output, speaker timing, ST7789 refresh with OP frames, sustained CPU/heap/stack high-water marks and OP audio/video synchronization remain unverified.
