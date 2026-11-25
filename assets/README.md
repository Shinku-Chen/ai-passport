<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, sound effects, and battery profiles, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

## Battery profiles

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
contains the supplied CW2017 profile for the Wanli New Energy 500 mAh cell
(profile2, XIL40X, 2026-10-20). It is an unchanged copy of the project-supplied
file, retained at the maintainer's request for this firmware update. Its
original filename is recorded in the paired Chinese document. No separate
license was included in the supplied file.

The format is 80 lines of two hexadecimal digits, one byte per line in register
order. The BSP embeds those bytes in `components/bsp/src/bsp_battery.c`; the
host gate checks the source file and initializer for an exact match. Text-file
SHA-256: `7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`.
Decoded 80-byte SHA-256:
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`.
This is a fuel-gauge profile, not a charging configuration or a generic profile
for every 500 mAh cell; see the [hardware guide](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md#9-cw2017-fuel-gauge).

## Maitetsu media pack

[`maitetsu/`](maitetsu/) holds the generated media for the Niiroku reader, built from
[`xuanyide01/maitetsu-rw5`](https://github.com/xuanyide01/maitetsu-rw5) at
`9c008406853affe973ceb80f62128696054365e7` (input digest
`082babe851a5efd6a4c6ab28bb4363fc1e8bf35448bea32466abe926ee8d07f2`). Do not edit
these files by hand; regenerate them with the pinned checkout:

```bash
python tools/maitetsu_assets.py --source <maitetsu-rw5>
python tools/maitetsu_assets.py --source <maitetsu-rw5> --check
```

- `maitetsu_media.bin` — 5,773,642 bytes, `MTMEDIA1`. 759 images (353 backgrounds,
  335 composited patches, 35 CGs, 35 64×36 thumbnails, 1 title key visual) as 3,994
  independent 30-row JPEG strips with shared quantisation/Huffman prefixes.
  SHA-256 `29de5b56366a77c069f6d204806544729be30a5f82d0044343ec8e3474439665`.
- `maitetsu_title.bin` — 585,588 bytes. The complete 283.88 s, 4 kHz mono title theme
  as 4-bit IMA ADPCM in 4,436 independent blocks. SHA-256
  `c746e291fa0c65382517eb26f8a4f7d4a15bdf369b1ccedceab3b1fe2478c9e0`.
- `media_manifest.json` — source pin/digest, counts, quality settings and the hashes
  above, regenerated together with the binaries.

Text, artwork, logos and music remain with Lose, HIKARI FIELD and their rights
holders; the repository's MIT license does not cover these assets. See
[the port document](../docs/development/engineering/maitetsu-port.md).
