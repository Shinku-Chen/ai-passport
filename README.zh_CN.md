<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 音效钥匙扣（Voice Keychain）

一个把 AI Passport 变成口袋音频播放器的音效钥匙扣。打开即可播放来自几十个角色包的数百条中文语音片段
——jojo、meme cat、刘华强、哈吉米、奶龙、小明剪膜等等。

这是本 `feature/voice-keychain` 分支承载的应用：固件启动后直接进入音效钥匙扣界面（无 demo 菜单）。

## 功能

- **角色目录**：以可滚动列表浏览所有角色包。每个条目是一个语音包（如 jojo、MC、meme cat、刘华强、
  刘海柱、卡丘美雪、路银、印度阿三、吉一卡哇伊、哈吉米、奶龙、抱抱嘟大磊磊、小团团、小明剪膜）。
- **片段列表**：进入某个包查看其中的片段名。
- **一键播放**：按 OK 播放选中的片段；内置解码播放 8 kHz 单声道 IMA-ADPCM 音频。
- **设置**（长按 OK）：显示当前电量百分比与电压，调节播放音量。

## 交互

三个按键驱动整个应用。顶部栏显示标题，主界面显示电量百分比（如 `97%`）。

- **UP / DOWN**：移动选中项（长按滚动）。
- **OK**：进入目录 / 选择片段 / 播放。
- **OK（长按）**：打开设置，或返回。

长条目横向滚动以便看清全名；选中行高亮为蓝色。

## 固件 / 构建

本分支把音效钥匙扣应用作为开机入口替代 demo 菜单：`main/voice_app.c` / `main/voice_app.h`、改写
`main/main.c`、在 `bsp_button` 暴露可重复触发的 `HOLD` 按键事件、CJK 子集字体
（`main/fonts/voice_cjk.c`）渲染中文，以及一个独立 SPIFFS 数据分区（`voicefs`，3 MB）存放压缩语音。

语音片段存放在挂载于 `/voices` 的 `voicefs` SPIFFS 数据分区，由 `tools/encode_voice.py` 生成
（解码 → 重采样到 8 kHz 单声道 → IMA-ADPCM 4bit → 生成 `main/voice_index.h` + `voicefs.img`）。应用
分别烧录合并固件镜像与该数据分区。

## 来源

- **分支**：[`feature/voice-keychain`](https://github.com/Shinku-Chen/ai-passport/tree/feature/voice-keychain)
- 档案：[`plays/voice-keychain/`](plays/voice-keychain/README.zh_CN.md)
