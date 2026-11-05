<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# What to Eat Today

A **landscape** food-wheel helper for the AI Passport: when you cannot decide what to eat, press a key to start the wheel and release to stop on a random food.

## Controls

**Press any key (UP / DOWN / OK)** to start the wheel spinning and **release** to stop on the last shown frame, which carries the dish or restaurant name. A **long press on OK** switches the render path (LVGL partial refresh versus direct panel flush) for on-device A/B comparison only.

## Features

- **Two wheels** — a place wheel and a food wheel, each a frame sequence, picked with equal probability per draw.
- **The frame is the answer** — dish or restaurant names are baked into the frames, no extra text layer.
- **Non-blocking keys** — button callbacks stay on the button task while the animation runs on an LVGL timer.

## Dependencies

**Standalone and offline**: no phone, network or account.

## Build and flash

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## Known limitations

- Results are purely random; no filtering by taste, distance or opening hours.
- The frame sequences are fixed, so adding food means regenerating frames and reflashing.

## License

MIT, see [`LICENSE`](LICENSE).
