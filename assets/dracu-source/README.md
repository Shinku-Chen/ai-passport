<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# DRACU-RIOT! port source assets (shipped with the branch)

This directory holds the **game assets and script data** extracted from the upstream
Mi Band port [`hezdaaa/dracu-riot-miband`](https://github.com/hezdaaa/dracu-riot-miband)
for one purpose only: a clone of this branch can repack and rebuild without network access
and without the retail game. The upstream quick-app sources (`src/pages/*`, `src/app.ux`)
are deliberately not included.

```
assets/dracu-source/
├── src/common/
│   ├── script/scriptData*.txt   106 blocks, global linear page table (52,787 pages)
│   ├── bg/*.jpg                 100 backgrounds (336x480)
│   ├── cimg/*.png               1,096 layered sprite parts (body / expression)
│   ├── evig/*                   475 event CGs and SD characters
│   ├── char_data.txt            sprite composition data (body + expression, scaling)
│   ├── title_bg.jpg             upstream title art (spare; the release art is assets/images/dracu-riot-title.png)
│   └── logo.png                 upstream title logo (spare)
├── <conversion-tools>/branchConfig.js  branch config (noNextPages / noBackPages / hiddenPages / end)
└── src/pages/
    ├── lct/lct.ux               chapter table (route -> first page)
    └── more/more.ux             after-story entry points (heroine -> first page)
```

The conversion-tools directory name is kept in Chinese in the tree; only `branchConfig.js` is used.

`lct.ux` and `more.ux` are read as **data only** (chapter and after-story page numbers),
not as a port of the quick app.

## Repacking

`main/dracu_data/*.bin` in this branch is the byte-identical output of the commands below;
the font subset comes from the third command. `--source` already defaults to
`assets/dracu-source`, so they run as-is:

```bash
# image pack, 4.99 MB (backgrounds q49 / event CGs q37 / sprites 0.46x 96 colours / SD 0.26x 68 colours)
python tools/dracu_pack.py --sprite-scale 0.46 --body-colors 96 --face-colors 40     --sd-scale 0.26 --sd-colors 68 --cg-quality 37 --bg-quality 49     --max-patch-ratio 1.0 --out build/dracu-pack/dracu_pack.bin

# script pack, 1.53 MB (page table + text + branch table, zlib per block)
python tools/dracu_scn_pack.py --out build/dracu-pack/dracu_scn.bin

# 16 px Chinese subset font (3,464 glyphs, 4bpp)
python tools/dracu_font.py --scn build/dracu-pack/dracu_scn.bin
```

Copy both packs into `main/dracu_data/` before flashing (the firmware reads them directly via
`EMBED_FILES`). The 8 MB budget trade-off recorded for this repository is exactly the
parameter set above: sprites 0.46x, CGs q37, SD 0.26x.

## Licensing

The script, characters, sprites, CGs and backgrounds of *DRACU-RIOT!* belong to
**Yuzusoft** and its Chinese publisher. These files come from the fan port named above and
are included for personal study and technical exchange only; please support the official
release. See the repository root README for the project-level statement.
