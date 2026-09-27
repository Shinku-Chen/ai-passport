<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

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
| [`images/senren-banka-title.png`](images/senren-banka-title.png) | 402 × 560, PNG | Official key visual of Senren * Banka (SAGA PLANETS), supplied by the developer on 2026-09-27. Build-time source of the port's title screen: `tools/senren_pack.py --title-art` cover-scales it to 240 × 320 and packs it as the background-space entry named by `main/senren_pack.h`'s `SENREN_TITLE_ART_NAME`. Artwork copyright SAGA PLANETS; the port is non-commercial and for study use only, see the packer docstring. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Port source material

Archived upstream sources for the fan ports live here in the layout the packers
read directly, so every packed blob in a firmware image can be rebuilt offline
without touching the network.

`senren-source/` holds the sources of the *Senren \* Banka* port:

| Path | Contents |
| --- | --- |
| `senren-source/bg/` | 92 backgrounds |
| `senren-source/ch/` | 123 character sprites (several poses each) |
| `senren-source/ev/` | 570 event illustrations and SD images |
| `senren-source/scn/` | 112 script chunks |
| `senren-source/MANIFEST.json` | Upstream repository, ref and a SHA-256 per file, written by the fetch tool |

Source: the Mi Band fan port [`hrk666666/Senren-Banka-MiBand-10`](https://github.com/hrk666666/Senren-Banka-MiBand-10),
fetched with `tools/senren_fetch_source.py`. Repacking from this directory is
byte-identical to the committed packs:

```bash
python3 tools/senren_pack.py --source assets/senren-source \
  --out build/repack/senren_pack.bin --json build/repack/senren_pack.json \
  --sprite-max-h 320 --quality-scale 1.15 --sd-min-refs 8
python3 tools/senren_scn_pack.py --source assets/senren-source \
  --out build/repack/senren_scn.bin --json build/repack/senren_scn.json
```

Both commands reproduce the committed SHA-256 values (`a0f89a22…` for the
5,263,756-byte image pack and `a0d7d3cf…` for the 1,432,148-byte script pack).
The port is non-commercial and for personal study only; the artwork belongs to
its original publisher and the upstream project is credited in the branch README.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
