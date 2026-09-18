<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 四子棋（Connect Four）

AI Passport 上的横屏四子棋：**棋盘 10 列 × 7 行**，可与电脑对战（低 / 中 / 高三档难度，先手可选玩家或电脑）、双人同机轮下，也可以**两台设备通过蓝牙联机对战**。

最新版本：**v1.6.0-connect-four**；分支 tip 新增的 `LINK PLAY` 模式尚未包含在该发布中。

- 分支：[`feature/connect-four`](https://github.com/Shinku-Chen/ai-passport/tree/feature/connect-four)
- 发布：[v1.6.0-connect-four](https://github.com/Shinku-Chen/ai-passport/releases/tag/v1.6.0-connect-four)

## 操作

上 / 下移动落子列（横屏持握时「上」在右手边，往右移动），**确定**落子，**长按确定**回设置屏。设置屏上用上 / 下选行、确定切换取值（模式：`HUMAN vs AI` / `AI vs HUMAN` / `TWO PLAYERS` / `LINK PLAY`，难度：`EASY` / `MEDIUM` / `HARD`，预览：`LANDING` / `TOP ROW`），选到 `START` 按确定开局；`LINK PLAY` 下则进入联机屏。两个电脑模式只差先后手。

## 双机联机对战

1. 两台设备都设成 `MODE = LINK PLAY` 并按 `START`。每台设备都同时广播自己的服务并扫描对端，所以**不需要选主机/加入**：BLE 地址较大的一方主动连接，两边各自算出的角色必然相反。
2. 联机屏会依次显示 `SEARCHING...` / `CONNECTING...` / `HANDSHAKE...`；双方交换完 `HELLO` 后自动开局。
3. 主动连接的一方（BLE 地址较大的一方，central）先手，之后每重开一局先手轮换。只有轮到的一方才能落子——顶部状态行显示 `YOUR TURN` / `PEER TURN`，对端落子同样有滑落动画和音效。
4. 一局结束后按 `OK` 请求再来一局，双方都请求后新局开始；**长按确定**退出联机并关闭射频。对端离开或链路断开时显示 `PEER LEFT`，并继续搜索对端。
5. 局面一致性靠一个停等可靠层保证（逐包序号 + 捎带确认 + 超时重传）再加每手的落子数校验，蓝牙通知丢一包不会让两边棋盘悄悄错位。

[`tools/c4_peer.py`](tools/c4_peer.py) 可以在 PC 上通过 BLE 扮演对端说同一套协议（`python -m pip install bleak`，然后 `python tools/c4_peer.py --list`），只用一台板子就能回归协议。实测堆数字、两个只在真机上出现的 NimBLE 陷阱、以及串口截图为什么改成可选构建，都写在[无 PSRAM 的 AI Passport 双机 BLE 联机](docs/reference/shinku-chen/two-device-ble-link.zh_CN.md)。

## 亮点

- **横屏 320 × 240、棋盘密排** —— 70 个格子、26px 棋子、相邻仅隔 3px，靠 BSP 层的 MADCTL 硬件旋转铺满屏幕。
- **三档电脑难度** —— 用墙钟搜索预算把每步控制在 1 秒内；低 / 中难度带固定失误率放水，保证打得赢。
- **不依赖手机的双机对战** —— BLE 对等发现放在 `components/bsp/bsp_ble_link.c`，报文与可靠投递放在 `main/c4_link_proto.c`（纯 C，有宿主测试）；实测约 73KB 堆开销。
- **无素材音效** —— 换列、落子、胜、负、平局音都由正弦表实时合成，不占 flash 资源。
- **空闲自动深睡** —— 设置屏 60 秒、对局中 180 秒后进入深睡，任意键唤醒（GPIO0 低电平唤醒，已修复 ADC 占用该脚导致的立即自唤醒）；联机对局中放宽到 10 分钟，入睡前会先停掉 BLE。
- **串口截图** —— `FAP_SCREENSHOT_V1` 命令回传真实 320 × 240 画面，本版封面就是这么抓的；现在是可选的开发构建（见下）。

## 构建变体

BLE 联机要占约 73KB 堆，而串口截图要在 `.bss` 里静态预留整屏 320 × 240（150KB）。这块板子没有 PSRAM，两者放不进同一份固件。所以默认固件带 `LINK PLAY`、不带截图：

- `idf.py -DC4_ENABLE_SCREENSHOT=ON build` —— 带串口截图的开发 / 出封面构建；该配置下 BLE 起不来，联机屏会显示 `BLE UNAVAILABLE`。

## 说明

- 本分支只承载一个应用：`feature/*` 分支的 README 只介绍本分支自己的应用，全部项目的目录在 fork `main` 的根 README。
- 烧录使用[在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)或 `esptool` —— 每个 release 都提供合并固件 `FoloToy-AI-Passport-full.bin`，从偏移 `0x0` 写入即可。目标板卡：8 MB Flash。
- 这些发布沉淀的可复用工程经验位于 [`docs/reference/shinku-chen/`](docs/reference/shinku-chen/)。
