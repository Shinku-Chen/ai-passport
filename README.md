<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DRACU-RIOT! — a visual novel reader for the AI Passport

A **portrait** offline visual-novel reader that brings the Mi Band fan port of
*DRACU-RIOT!* to the AI Passport: **five heroine routes, each with its own epilogue and
ending**, told across **62 chapter nodes**, with **244 event CGs and 85 backgrounds**
packed into the firmware. The reading engine is written in C on LVGL and driven by the
device's three keys.

## Controls

On lists **UP / DOWN** move the cursor, **OK** selects and **OK (hold)** goes back.
While reading, **UP / DOWN** advance line by line; **UP (hold)** fast-forwards;
**DOWN (hold)** toggles auto-read; **OK** opens the menu (save, load, chapter list,
back to title). Choices use UP / DOWN + OK.

## Features

- Boots straight into the title screen; reading runs from the prologue to an ending.
- Five heroine routes with per-route endings and epilogues, branching on choice history.
- Five manual save slots plus an automatic slot written on scene changes.
- History, chapter list, CG gallery, adjustable text speed; auto-read keeps the screen on.
- Idle behaviour: dim, screen off, then deep sleep; any key wakes the device.

## Build and flash

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash monitor
```

## Release and limits

Released as **v0.1.0-dracu-riot**. Known limits: portrait 240x320 only, no voice or
music, single fixed Chinese font subset.

Experience notes:
[docs/reference/shinku-chen/dracu-riot](https://github.com/Shinku-Chen/ai-passport/tree/docs/dracu-riot-archive-experience/docs/reference/shinku-chen/dracu-riot).

## Credits and licence

Script and art come from the Mi Band fan port of *DRACU-RIOT!*; the original game is
(C) Yuzusoft. This is a personal, non-commercial port — support the original release.
