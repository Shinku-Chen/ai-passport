# DRACU-RIOT! 移植源素材（随分支提交）

这份目录是从上游手环移植工程 [`hezdaaa/dracu-riot-miband`](https://github.com/hezdaaa/dracu-riot-miband)
里取出的**游戏素材与剧本数据**，目的只有一个：让这个分支 **clone 下来就能重新打包、重新构建**，
不需要联网、不需要正版游戏本体（原仓库里那些需要正版资源才能跑的转换脚本没有收录）。

只放了打包器真正会读的东西；上游的**快应用源码（`src/pages/*`、`src/app.ux` 等）没有收录**。

```
assets/dracu-source/
├── src/common/
│   ├── script/scriptData*.txt   106 块全局线性页表（52,787 页）
│   ├── bg/*.jpg                 100 张背景（336x480）
│   ├── cimg/*.png               1,096 张立绘分层素材（身体 / 表情差分）
│   ├── evig/*                   475 张事件 CG / SD 小人
│   ├── char_data.txt            立绘合成数据（身体 + 表情坐标、缩放）
│   ├── title_bg.jpg             上游标题图（备用，正式标题图见 assets/images/dracu-riot-title.png）
│   └── logo.png                 上游标题 logo（备用）
├── 转换工具/branchConfig.js     分支配置（noNextPages / noBackPages / hiddenPages / end）
└── src/pages/
    ├── lct/lct.ux               章节表数据（每条路线的章节起始页）
    └── more/more.ux             后日谈入口数据（女主 → 起始页）
```

`lct.ux` / `more.ux` 只当**数据**用（打包器从中读章节与后日谈的页码），不是把快应用搬进来。

## 重新打包

分支里 `main/dracu_data/*.bin` 就是下面两条命令的输出（逐字节一致，可复现）；
字库用第三条重新生成。默认 `--source` 已经是 `assets/dracu-source`，
所以直接跑就行：

```bash
# 图片包 4.99 MB（背景 q49 / 事件 CG q37 / 立绘 0.46 倍 96 色 / SD 0.26 倍 68 色）
python tools/dracu_pack.py --sprite-scale 0.46 --body-colors 96 --face-colors 40 \
    --sd-scale 0.26 --sd-colors 68 --cg-quality 37 --bg-quality 49 \
    --max-patch-ratio 1.0 --out build/dracu-pack/dracu_pack.bin

# 剧本包 1.53 MB（页表 + 正文 + 分支表，逐块 zlib）
python tools/dracu_scn_pack.py --out build/dracu-pack/dracu_scn.bin

# 16 px 中文子集字库（3,464 字形，4bpp）
python tools/dracu_font.py --scn build/dracu-pack/dracu_scn.bin
```

刷进分支前把两个包拷到 `main/dracu_data/`（固件用 `EMBED_FILES` 直接读这两个文件）。
本仓库曾经的 8 MB 预算取舍就是上面这组参数：立绘 0.46 倍、CG q37、SD 0.26 倍。

## 版权

《DRACU-RIOT!》的剧本、角色、立绘、CG、背景版权归 **Yuzusoft（柚子社）** 及中文版发行方所有；
这批文件来自上述同人移植工程，**仅供个人学习与技术交流，请支持正版**。详见仓库根 README 的说明。
