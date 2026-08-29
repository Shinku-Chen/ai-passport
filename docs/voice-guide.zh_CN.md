<p align="right">
  <strong>简体中文</strong> · <a href="voice-guide.md">English</a>
</p>

# 音效钥匙扣指南

本文说明**音效钥匙扣**产品如何使用语音素材：原始音频放在哪、如何压缩打包、以及如何构建并烧录固件与其数据分区。

## 原始素材

原始音频位于 `assets/project/<目录>/*.mp3|ogg|wav`。每个目录对应钥匙扣 UI 里的一个顶层项目；每个音频文件对应一段可选择的音效。目录名与文件名会显示在 UI 中，请保持其可读。

- 原始媒体是用户自有素材；提交原始文件前应记录来源与再分发许可。本仓库倾向只保留转码产物（见下文），以保持精简并避免打包原始二进制。
- 文件名中的非 BMP 字符（如 emoji `🐦`）会被转码器替换成可读中文，因为嵌入的 CJK 子集字体没有对应字形。

## 转码管线

新增或改动原始音频后运行转码器。Opus 编码需带 `libopus` 的 `ffmpeg`，并复用 `encode_voice.py` 做显示名清洗与 C 索引生成：

```bash
python tools/encode_opus.py
```

脚本对每段执行：

1. 用 `ffmpeg`（libopus）解码 mp3/ogg/wav 并编码为 Opus 8kbps / 16kHz 单声道。
2. 响度归一化（均值 RMS 向目标靠拢、峰值限幅），使所有片段听感一致。
3. 剥离 Ogg 容器为裸 Opus 包流（每个包 = 2 字节小端长度 + 帧），正是固件解码器读取的格式。
4. 写入 `assets/audio/dirNN/clipMM.opus`，更新 `assets/audio/voice_index.json`，重新生成 `main/voice_index.h`（固件使用的编译期路径/中文名/长度表），并用 ESP-IDF 的 `spiffsgen.py` 打包 `voicefs.img`。

`assets/audio/dirNN/*.opus` 与 `main/voice_index.h` 已入库，因此即使没有原始素材也能复现数据分区。`tools/pack_voicefs.py` 直接从已提交的片段重新打包 `voicefs.img`（无需重编码 / ffmpeg），构建与 CI 流程即使用它。

## 存储布局

`partitions.csv` 新增一个独立的 SPIFFS 数据分区：

```csv
voicefs, data, spiffs, 0x210000, 0x5F0000,
```

固件通过 `esp_vfs_spiffs_register` 将其挂载到 `/voices`，并用普通的 `fopen`/`fread` 读取每段。SPIFFS 是 ESP-IDF 内置组件，因此无需外部 Managed Component。`voicefs.img` 即该分区的内容（约 5.9 MB）；它由构建生成，故在 `.gitignore` 中、不入库。

## 构建与烧录

`./tools/validate.sh --firmware` 会构建应用、打包数据分区，并把完整镜像（bootloader `0x0`、分区表 `0x8000`、应用 `0x10000`、voicefs 数据 `0x210000`）合并成 `build/FoloToy-AI-Passport-full.bin`（8 MB）。这就是应从 `0x0` 烧录的固件。

若只烧数据分区（或作参考），把 `voicefs.img` 烧到其偏移：

```bash
python -m esptool --chip esp32c3 -p <端口> write_flash 0x210000 assets/audio/voicefs.img
```

首次启动时以 `format_if_mount_failed = true` 挂载，因此空白数据分区会在读取前自动格式化。

## 更新素材

当新增、重命名或删除原始音频时：

1. 把文件放入 `assets/project/<目录>/`。
2. 运行 `python tools/encode_opus.py` 重新生成各段、`voice_index.h` 与 `voicefs.img`。
3. 重新构建固件（`voice_index.h` 表已改变）并重新烧录数据分区——或直接烧录合并后的 `build/FoloToy-AI-Passport-full.bin`（`0x0`）。
