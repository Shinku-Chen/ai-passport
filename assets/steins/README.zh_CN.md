<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# Steins 阅读器资源包

`steins_pack.bin` 是生成物,不要手改。重新生成:

```bash
python tools/steins_pack.py --source <steins-rw5 检出> \
    --out assets/steins/steins_pack.bin --manifest assets/steins/steins_pack.json \
    --pack-scale 0.92 --min-q 51 --bpp-bg 0.070 --bpp-sprite 0.095
```

- 输入:只读的小米手表 5 场景阅读版快照 `steins-rw5`
  (`28e5b52f11c77fc4781f532e2729a5374f5fd8ea`)。快照本身不入库,入库的只有这份
  派生出来的包。
- 内容:UTF-16 正文串、帧/画面/层表、JPEG + 半分辨率 1bpp alpha 遮罩、选择图、
  章节入口、鉴赏分组、世界线表和界面文案。
- 权利:剧情、美术与该派生字形字体的权利归原作者。这个包入库是为了本仓库阅读器的
  本地开发与真机验证,不构成对原作品的再分发许可。
- `steins_pack.json` 是打包器自己的报告(段大小、总数、SHA-256)。独立审计
  (`tools/steins_audit.py`)故意不读它。
