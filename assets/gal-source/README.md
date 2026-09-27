<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Upstream game material

The artwork and script this reader packs into `main/limelight_data/`, kept in the
repository so the packers can rebuild the shipped images from a plain checkout.

## Source

- **Upstream**: [`hezdaaa/limelight-lemonade-jam-miband`](https://github.com/hezdaaa/limelight-lemonade-jam-miband)
  — a Xiaomi Band port of the commercial visual novel *limelight lemonade jam*.
  `skdkzzx/limelight-lemonade-jam-xiaomi-band10` is a fork of it, and its script files
  are byte-identical to this copy.
- **Layout**: the upstream `src/common/` tree, unchanged. `script/` holds 137
  `scriptDataN.txt` files (68,229 dialogue entries), `bcgi/` the backgrounds, `cimg/`
  the character sprites, `evig/` the event CGs, and the `title_bg*.jpg` files the title
  screen uses. `gallery_cgs.txt` lists the 633 event CGs the upstream gallery page
  references, so the packer can tell story CGs from gallery-only ones without that
  quick-app page. Six small band-UI icons (`back.png`, `bt.png`, `go.png`, `hd.png`,
  `logo.png`, `reset.png`) are not included: this reader does not draw them, and no
  quick-app code is committed.
- **Licence**: the upstream repositories declare none. The original work is
  **SAGA PLANETS**' *limelight lemonade jam*; this copy exists only so the fork can be
  rebuilt and reviewed, and the credit belongs to the original authors and the two
  ports above.

## Rebuilding the packs

```bash
python tools/limelight_material_pack.py --source assets/gal-source --out main/limelight_data/limelight_pack.bin
python tools/limelight_script_pack.py --source assets/gal-source --out main/limelight_data/limelight_script.bin
python tools/limelight_lvgl_font.py --font <NotoSansSC-Regular.otf> \
    --pack main/limelight_data/limelight_script.bin --out-dir assets/fonts
```

The material pack is deterministic: rebuilding it from this directory reproduces the
committed `limelight_pack.bin` byte for byte. The font needs the Noto Sans SC (SIL OFL
1.1) file supplied separately, which is why only its generated output is committed.
