<p align="right">
  <a href="protected-flash-layout.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Protected Flash Layout

This repository reserves the per-device identity region so derivative firmware
does not overwrite provisioned `cardid` data.

## Mandatory layout

Derivative projects must preserve all of the following:

- ESP32-C3, 8 MB Flash, ESP-IDF 5.5.3.
- A merged ESP image starting at `0x0`, produced as
  `build/FoloToy-AI-Passport-full.bin`.
- One main application image at `0x10000`, no larger than `0x300000` bytes.
- `cardid`: data/NVS at `0x356000`, size `0x4000`.
- A valid partition-table MD5 marker and no partition overlap with the
  protected `cardid` region.
- No device-specific `cardid` payload in a community artifact.

Applications may add resource partitions, but they must not overlap protected
`cardid`. Required resource partitions must be included in the merged
artifact rather than declared empty.

## Enforced validation

Run:

```bash
./tools/validate.sh --firmware
```

The check builds in an isolated directory, creates the merged image, verifies
the bootloader/table/application offsets, parses the partition table, checks its
MD5 and the protected `cardid` range, enforces the 3 MB application limit, and
rejects device-specific identity bytes. CI runs the same gate. Do not publish
an artifact when this command fails.

Upload only `build/FoloToy-AI-Passport-full.bin`; the similarly named app-only
`build/FoloToy-AI-Passport.bin` does not contain the complete validated layout.

## Flashing safety during development

Never run `idf.py erase-flash` on a provisioned device. It destroys the
per-device identity. Prefer segmented `idf.py flash`, which does not write an
image for protected `cardid`. A
raw single-file write from `0x0` is safe only when its byte range ends before
`cardid`; a merged artifact containing later resource partitions spans the gap
and must not be raw-flashed to a provisioned device.
