<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 四子棋（Connect Four）

AI Passport 上的**横屏**四子棋：棋盘 10 列 × 7 行，可对电脑（低 / 中 / 高三档）、双人同机，或两台设备经蓝牙联机对战。

## 操作

上 / 下移动落子列（横屏持握时「上」在右手边，往右移动），**确定**落子，**长按确定**回设置屏。设置屏上用上 / 下选行、确定切换取值（模式：`HUMAN vs AI` / `AI vs HUMAN` / `TWO PLAYERS` / `LINK PLAY`，难度：`EASY` / `MEDIUM` / `HARD`，预览：`LANDING` / `TOP ROW`），选到 `START` 按确定开局。

联机模式不需要选主机 / 加入：两台设备都设成 `LINK PLAY` 并按 `START`，各自广播并扫描，BLE 地址较大的一方主动连接；屏幕依次显示 `SEARCHING...` / `CONNECTING...` / `HANDSHAKE...`，交换完 `HELLO` 自动开局。只有轮到的一方才能落子（顶部 `YOUR TURN` / `PEER TURN`），一局结束后按 **确定** 请求再来一局，**长按确定** 退出联机并关闭射频。

## 功能

- **三档电脑难度** —— 用墙钟搜索预算把每步控制在 1 秒内；低 / 中难度带固定失误率放水。
- **两台设备直接联机** —— 对等发现 + 停等可靠层（逐包序号、捎带确认、超时重传）再加每手落子数校验，丢一包不会让两边棋盘悄悄错位。
- **无素材音效** —— 换列、落子、胜、负、平局音都由正弦表实时合成，不占 flash。
- **空闲深睡** —— 设置屏 60 秒、对局 180 秒后入睡，任意键唤醒；联机时放宽到 10 分钟，入睡前先停 BLE。
- **可选串口截图** —— `FAP_SCREENSHOT_V1` 回传真实 320 × 240 画面，用于抓封面与复查。

## 依赖

**单机离线**即可游玩；联机对战只用设备自带 BLE，不需要手机、网络或账号。PC 上的 `tools/c4_peer.py`（需 `pip install bleak`）可以在没有第二台设备时扮演对端回归协议。

## 构建与刷写

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin            # 生成 merged-binary.bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

默认固件带 `LINK PLAY`、不带串口截图（截图要在 `.bss` 静态预留整屏，与本板无 PSRAM 下的 BLE 堆开销放不进同一份固件）。需要截图时用 `idf.py -DC4_ENABLE_SCREENSHOT=ON build`，该配置下 BLE 起不来，联机屏显示 `BLE UNAVAILABLE`。

## 发布

tag `v1.7.0-connect-four`，release 附带合并固件 `FoloToy-AI-Passport-full.bin`。

## 已知限制

- 联机需要两台同版本设备；BLE 与串口截图不能共存（见上）。
- 联机对局中链路断开只显示 `PEER LEFT` 并继续搜索，不会自动恢复原局面。
- 电脑对手按墙钟预算搜索，极慢的存储卡 / 断电重启后首步会偏保守。

## 经验文档

- [无 PSRAM 的 AI Passport 双机 BLE 联机](docs/reference/shinku-chen/two-device-ble-link.zh_CN.md) —— 实测堆数字、两个只在真机上出现的 NimBLE 陷阱。

## 许可

MIT，见 [`LICENSE`](LICENSE)。
