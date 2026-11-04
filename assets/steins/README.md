<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Steins reader resource pack

`steins_pack.bin` is generated, not hand-edited. Regenerate it with:

```bash
python tools/steins_pack.py --source <steins-rw5 checkout> \
    --out assets/steins/steins_pack.bin --manifest assets/steins/steins_pack.json \
    --pack-scale 0.92 --min-q 51 --bpp-bg 0.070 --bpp-sprite 0.095
```

- Input: the read-only Xiaomi Watch 5 scene-reader snapshot `steins-rw5`
  (`28e5b52f11c77fc4781f532e2729a5374f5fd8ea`). The snapshot is not copied into
  this repository; only this derived pack is committed.
- Contents: UTF-16 story strings, frame/view/layer tables, JPEG + half-resolution
  1 bpp alpha masks, choice graph, chapter starts, gallery groups, worldline
  table and the user-interface strings.
- Rights: the story text, artwork and the derived glyph font belong to the
  original rights holders. This pack is committed for local development and
  device validation of this repository's reader; it is not a redistribution
  licence for the original work.
- `steins_pack.json` is the generator's own report (section sizes, totals,
  SHA-256). The independent audit (`tools/steins_audit.py`) deliberately does not
  read it.
