<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 《魔法少女的魔女审判》· AI Passport 横屏阅读器

把红米手表 5 的 Vela 移植版《魔法少女的魔女审判》搬到
[FoloToy AI Passport](https://ai-passport.folotoy.cn)（ESP32-C3、320×240 横屏、三键、
8 MB Flash、无 PSRAM）。本分支是一个独立应用，不是上游 demo 菜单的换皮版本。

- 项目说明：[`docs/games/manosaba.zh_CN.md`](docs/games/manosaba.zh_CN.md)
- 分支：`feature/port-manosaba-landscape`
- 源快照：`manosaba-rw5 @ a309a3614837b555741e5d8fd7cfe2b9428e2e49`
- 语言：简体中文；16px 完备字库（2,999 字形，0 缺字）

## 发布

- **版本**：tag [`v1.0.0-manosaba`](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.0.0-manosaba)，
  分支 [`feature/port-manosaba-landscape`](https://github.com/Shinku-Chen/ai-passport/tree/feature/port-manosaba-landscape)；合并固件
  `FoloToy-AI-Passport-full.bin`（app 7,763,280 B / full 7,959,888 B，版本 `1.0.0`）。
- **方向**：320×240 横屏。
- **三键**：正文 UP 推进 / 长按 UP 快进，DOWN 历史 / 长按 DOWN 自动阅读，OK 推进 / 长按 OK 菜单；
  列表与选项 UP/DOWN 移动、OK 确认、长按 OK 返回。
- **功能**：多槽存档 + 自动档、读档、CG 鉴赏、章节列表、历史回看、自动阅读、快进瞬显。
- **空闲**：60 s 调暗、300 s 熄屏、1800 s 深睡，任意键唤醒并回到原进度；自动阅读与快进期间不休眠。
- **已知限制**：分区占用 94.77%，余量 428,720 B；改动时仍需确认 Flash 预算。

## 已实现

- **全部 458 个源剧本文件 / 424 个场景 / 90,243 条指令**打包成一份只读 Flash 包，
  由 C 虚拟机按源 `engine.js` / `scene.js` 语义执行：背景层、立绘实例、焦点、
  条件、`call`/`return`、`offer`/`choice`、地图调查、证据提交、反驳词、审判伤害、
  手册版本、自动存档点、章节入口与 `file:scene/chapter` 两种模式。
- **全量画面**：62 张背景、172 张 CG（61 组）、151 张立绘、19 张机关图、151 张
  手册图、2 张纯色占位图和 2 张标题图，共 559 张；320×240 等比 fit，不裁切；立绘 JPEG + 1bpp 遮罩。
- 作品专属 320×240 横屏 UI：标题、正文、选项、菜单、历史、存读档、设置、
  鉴赏室/CG 全屏、章节选择、魔女手册、结局、关于、错误页。
- 30 个手动存档 + 1 个独立自动档；版本/长度/CRC 校验，写入失败不改旧档；
  全局鉴赏/章节解锁独立版本化存储，只增不减，失败节流重试。
- 三键语义：正文 UP 短按/长按快进，DOWN 历史/长按自动，OK 菜单/反驳词；
  列表 UP/DOWN + OK，长 OK 返回；选项必须 OK 显式确认。

## 构建

需要 ESP-IDF 5.5.3 与 Python 3（Pillow 用于资源/字体生成）。

```bash
python tools/manosaba_pack.py --source <manosaba-rw5> --out main/gal_data/manosaba.pack
python tools/manosaba_font.py --font <manosaba-rw5>/src/common/manosaba.otf \
    --fallback-font "$WINDIR/Fonts/NotoSansSC-VF.ttf" \
    --pack main/gal_data/manosaba.pack --source-root main --out-dir assets/fonts
idf.py build
idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build
```

宿主测试：`CC="$PWD/tools/cc-host.sh" MANOSABA_SOURCE=<manosaba-rw5> \
bash tools/validate.sh --static`。本机 Windows 上 `install_passport_skills`
符号链接测试被跳过，因此宿主门禁显示 PARTIAL；本作 C 测试、源审计、真实
TJpgDec 图像回归均实际执行。

## 当前验证状态

- 软件审核：**通过；待设备验收**（生产冻结于 full `7f90c4e1…`）
- Build：PASS（app 7,442,288 B，full 7,638,896 B；实际 factory@0x30000，余 749,712 B；NVS@0x9000/0x26000）
- Host tests：**PARTIAL**（Gate PASS=42 FAIL=0 SKIP=1；仅 Windows `install_passport_skills` 符号链接测试 SKIP）
- Device tests：**NOT RUN / 待设备验收**（未刷机、未做真机验收）
- 源审计：PASS（657,333 项检查；458 文件 / 90,243 指令 / 5,586 次 if/set + 立绘位置表达式对照）
- 归档：`build/firmware/7f90c4e15f1bc4f8ff179759123b50b9c63fcdf17c1629200c791e7df538e976/`（旧 `99b…`/`a14d…` 保留）

## 许可与来源

上游仓库与 BSP 继续遵循其许可证。本移植代码为独立实现；原游戏剧本、图片、
角色与商标权利属于原权利人，本项目不代表已取得公开再分发原作素材的授权。
字体源为源快照自带的 Noto Sans Mono CJK SC（SIL OFL 1.1），5 个缺字由
系统 Noto Sans SC（SIL OFL 1.1）补齐；详见
[`docs/games/manosaba.zh_CN.md`](docs/games/manosaba.zh_CN.md)。
