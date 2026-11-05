<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Tuner

A **landscape** twelve-tone equal-temperament tuner for the AI Passport: the onboard microphone captures audio and the screen shows note name, octave, frequency and the deviation in cents.

## Controls

**Short OK** switches between `AUTO` (detect the current note) and `MANUAL` (pick a target note C4..B4 with UP / DOWN); **short UP / DOWN** cycle the target note in MANUAL. A **long press on OK** opens a debug page showing the raw frequency and the RMS / NSDF intermediates, where UP / DOWN adjust microphone gain.

## Features

- **Real-time pitch detection** — the microphone is read on a dedicated worker task and an integer NSDF tracker yields note, octave and frequency.
- **AUTO mode** — detects the current note and shows cents to the nearest equal-tempered pitch.
- **MANUAL mode** — pick a target note and tune against it, with the header showing the offset.
- **Debug mode** — large frequency readout plus RMS / NSDF, with adjustable gain.

## Dependencies

**Standalone and offline**: it uses only the onboard microphone; no phone, network or account.

## Build and flash

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## Known limitations

- Accuracy is limited by the onboard microphone and speaker echo; the low register (below C3) is noticeably worse.
- MANUAL targets are limited to C4..B4.
- There is no metronome or chord detection.

## License

MIT, see [`LICENSE`](LICENSE).
