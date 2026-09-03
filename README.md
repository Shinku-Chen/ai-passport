<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Tuner

A chromatic tuner for the FoloToy AI Passport. It samples the onboard microphone
in real time, detects the pitch, and shows the note name, octave, frequency, and
the cents deviation from a reference. Built on the standard BSP demo architecture
(menu + demo pages) with the Tuner as the headlining page.

This is the application built on the `feature/tuner` branch.

## What it does

- **Real-time pitch detection** — the mic is read in a dedicated worker task;
  an integer NSDF pitch tracker turns the samples into a note name, octave, and
  frequency.
- **AUTO mode** — identify the current note automatically; a meter shows how far
  it sits from the nearest equal-tempered pitch (in cents).
- **MANUAL mode** — pick a target note (C4..B4) with UP / DOWN and tune to it;
  the meter shows the deviation from that target, which feels like tuning a
  string.
- **Debug mode** (hold OK) — shows the raw frequency plus RMS / NSDF intermediates
  and lets you adjust the microphone gain with UP / DOWN for on-device
  validation.

## Interaction

- **OK (short)** — switch between AUTO and MANUAL mode.
- **UP / DOWN (short)** — in MANUAL mode, cycle the target note; in debug mode,
  adjust the mic gain.
- **OK (hold)** — toggle debug mode.
- **OK (hold, in Tuner)** — the Tuner page consumes long-OK itself (no menu
  return).

The other demo pages (Display / Button / Audio / Battery / Wi-Fi / BLE / Low
Power) remain available from the menu as BSP references.

## Firmware / build

Standard ESP-IDF project (target `esp32c3`):

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

## Source

- Branch: [`feature/tuner`](https://github.com/Shinku-Chen/ai-passport/tree/feature/tuner)
- Key files: `main/demo_tuner.c` (page + key handling), `main/tuner_engine.c` /
  `main/tuner_engine.h` (integer NSDF pitch detection).
