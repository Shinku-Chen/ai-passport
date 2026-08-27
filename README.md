<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Voice Keychain

A sound-effects keychain that turns the AI Passport into a pocket audio player.
Open it and instantly play one of hundreds of Chinese voice clips from dozens of
character packs — jojo, meme cat, Liu Huaqiang, Haji Mi, Nailong, Xiao Ming
Jian Mo, and more.

This is the application built on this `feature/voice-keychain` branch. The
firmware boots straight into the voice keychain app (no demo menu).

## What it does

- **Character directory**: browse all character packs as a scrollable list. Each
  entry is a pack of voice clips (e.g. jojo, MC, meme cat, Liu Huaqiang,
  Liu Haizhu, Kaqiu Mixue'er, Luyin, Indian A-san, Ji Yi Kawai, Haji Mi,
  Nailong, Bao Bao Duda Leilei, Xiao TuanTuan, Xiao Ming Jian Mo).
- **Clip list**: enter a pack to see its clips by name.
- **One-tap playback**: press OK to play the selected clip; built-in decoding
  plays 8 kHz mono IMA-ADPCM audio.
- **Settings** (hold OK): show current battery percentage and voltage, and
  adjust the playback volume.

## Interaction

Three keys drive the whole app. A top bar shows the title and, on the home
screen, the battery percentage (e.g. `97%`).

- **UP / DOWN**: move selection (hold to scroll).
- **OK**: enter a directory / select a clip / play.
- **OK (hold)**: open settings, or go back.

Long entries scroll horizontally so the full name is readable; the selected row
is highlighted in blue.

## Firmware / build

This branch adds the voice keychain application as a replacement for the demo
menu: `main/voice_app.c` / `main/voice_app.h`, a rewired `main/main.c`, a
re-triggerable `HOLD` button event in `bsp_button`, a CJK subset font
(`main/fonts/voice_cjk.c`) for Chinese names, and a dedicated SPIFFS data
partition (`voicefs`, 3 MB) that holds the compressed clips.

The audio clips live in the `voicefs` SPIFFS data partition mounted at
`/voices`, built via `tools/encode_voice.py` (decode → resample to 8 kHz mono →
IMA-ADPCM 4-bit → build `main/voice_index.h` + `voicefs.img`). The app flashes
the merged firmware image and that data partition separately.

## Source

- **Branch**: [`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain)
- Archive: [`plays/voice-keychain/`](plays/voice-keychain/README.md)
