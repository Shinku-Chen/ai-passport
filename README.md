<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 音效钥匙扣（Voice Keychain）

AI Passport 上的随身音效钥匙扣：开机直接进入播放界面，播放来自几十个角色包的数百条中文语音片段。

## 操作

**UP / DOWN** 在列表中移动，**OK** 进入目录、选择片段或播放，**长按 OK** 进入设置（音量、电量）或返回上一级。

## 功能

- **角色目录** —— 可滚动列表浏览所有角色包，进入后查看片段名。
- **一键播放** —— 内置解码播放 16 kHz 单声道 Opus 音频，连续切换片段复用解码器。
- **流畅浏览** —— 长名称稳定横向滚动；长按上下键连续选择，松手后不再补滚。
- **设置页** —— 显示电量百分比与电压，可调播放音量。
- **自包含固件** —— `voicefs` 数据分区打进同一个 8 MB 镜像。

## 依赖

**单机离线**：不需要手机、网络或账号，所有音频随固件一起烧录。

## 构建与刷写

```bash
./tools/validate.sh --firmware
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/FoloToy-AI-Passport-full.bin
```

## 发布

tag `v1.15.1-voice-keychain`，release 附带已通过真机验收的自包含固件 `FoloToy-AI-Passport-full.bin`。

## 已知限制

- 只能播放随固件打包的片段，不能导入自己的音频。
- 音量与电量只在设置页显示，播放界面不显示。

## 经验文档

- [音频压缩取舍](docs/reference/shinku-chen/audio-compression-trade-offs.zh_CN.md) —— IMA-ADPCM 与体积/音质的实测对比。
- [外设断电与深睡](docs/reference/shinku-chen/deep-sleep-peripheral-power-off.zh_CN.md) —— 睡前关外设省电的做法。


## 许可

MIT，见 [`LICENSE`](LICENSE)。
