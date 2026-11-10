<p align="right">
  <strong>简体中文</strong> · <a href="voice-guide.md">English</a>
</p>

# 音效钥匙扣指南

本文说明**音效钥匙扣**产品如何使用语音素材：原始音频放在哪、如何压缩打包、以及如何构建并烧录固件与其数据分区。

## 导航与播放流畅度

UP/DOWN 切换选中的音效，不打断播放；OK 停止当前播放并从头播放所选音效。
保持这些操作与现有布局。光标在可见窗口内移动时，只更新前后两条选中行；
只有窗口滚动时才重新绑定文字，并直接引用编译期索引中的不可变名称，
避免每次按键都分配文字副本。返回目录时保持原选中项可见，
只有选中行会横向滚动显示长名称。

按键回调只做非阻塞入队，由优先级 3 的应用 worker 处理导航；
现有 LVGL 任务（优先级 4）与音频 worker（优先级 5）保持不变。
16 个事件的队列保持普通点击与 OK 的顺序，每个键最多保留一个待处理的长按重复事件。
物理按下或松开时立即使旧的重复事件失效，worker 取得 LVGL 锁后还会重新校验，
同时取消仍在队列中和已出队但等待绘制的旧重复事件，不丢弃普通点击。
松开前已经确认的一次绘制仍可能完成，但不会再逐个补执行积压的连续滚动。
队列满时丢弃新事件，不阻塞共享定时器任务。
音量变化只更新音量文字，并在 LVGL 锁外写入 codec I2C。
电量仍每 30 秒刷新一次，到达音量上下限后不重复写 codec 或重绘。
停止应用与深睡会先等待正在处理的输入结束，再关闭相关资源；
短临界区保护播放任务对象的生命周期，避免音频抢占后访问已释放对象。
显示 DMA 大小、SPI 时钟、音频格式、分区与素材均不变。

### 滚动文字区域与测量

本固件滚动显示音效名称，并无同步歌词功能。原生 LVGL 局部渲染已经避免了
每次跑马灯更新都刷新整屏。现在为标题和名称增加恰好一行字高（19 px）的透明视口，
裁掉 LVGL 标签额外的失效边界；行背景与高亮仍为 21 px 高。
循环滚动改为 36 px/s，不再让所有名称都在 2000 ms 内滚完，避免名称越长滚动越快。

真实 LVGL 9.5.0 软件渲染测试采用生产中文字库、240x320 RGB565 显示，
以及相同的 9600 字节局部缓冲。每个版本回放 20 秒模拟时间。
在相同动画速度下，裁剪前后画面逐帧像素一致。
列表行刷屏像素从 2,709,966 降至 2,408,478（减少 11.1%）；
标题从 2,548,944 降至 1,931,122（减少 24.2%）。
固定像素速度后，此回放中最大的单次位移从 9 px 降为 2 px；
这是动画位移数据，并非设备实测帧时间。
静态文字在待刷新区域清空后不再刷屏。

用 ESP-IDF 解析托管依赖后，独立于轻量主机检查运行渲染回归：

```bash
cmake -S tests/lvgl -B build/marquee-host
cmake --build build/marquee-host
ctest --test-dir build/marquee-host --output-on-failure
```

Windows 策略禁止执行新编译的原生测试程序时，也可用现有 Zig/Node 工具链
将同一份 C 测试编译为 WASI 后运行。
真机上，每段音效结束或停止后会输出一次 `Playback metrics`：
`max_submit_gap_us` 包含 I2S 写入阻塞时间；`max_prepare_gap_us` 表示上次写入返回后，
读取、解码与调度造成的准备间隔。应在不同构建上播放同一段素材、重复相同按键动作，
并结合实际听感比较。`write_errors` 记录 BSP 写入失败数。
这些是供给链路诊断，并非 I2S 欠载检测；错误数为零或间隔小于理论 DMA 容量
90 ms，均不能证明完全没有断音。

`./tools/validate.sh --static` 覆盖仅焦点变化、跨窗口滚动、边界钳位、
空列表、短列表、恢复选中项、重复事件合并、松键取消及前后两次按键的隔离。
真机验收时，播放较长音效并连续在窗口内及跨窗口移动，
检查长名称滚动、快速 OK 切换、返回已滚动的目录，以及音量两端的连续长按。
长按 UP/DOWN 快速滚动后松手，检查不会继续补滚，并测试反向切换、普通点击与 OK 响应。
同时听是否有断音，观察输入响应与屏幕运动。
自动化检查不能证明播放连续性或实际帧率。

### 解码器内存与播放恢复

在 `voice_app_start()` 最前面一次性预留单声道 Opus 解码器，先于文件系统、
屏幕及可选截图、输入任务的内存分配。当前定点解码器需要连续的 17,776 字节。
逐段创建解码器可能在堆内存碎片化后返回 `OPUS_ALLOC_FAIL`（`-7`），
即使此时上下导航仍然正常。常驻播放器复用这一个解码器，
每次新音效开始前调用 `OPUS_RESET_STATE`。停止应用时不释放仍可能被常驻 worker
访问的状态，重启后由系统回收。

这会在空闲时保留解码器内存，而非增加播放峰值内存。
启动日志中的 `Decoder reserve`、`App ready` 输出剩余堆和最大连续块；
真机应同时核对文件系统、输入及截图任务初始化，不能只看预留成功。
内存分配失败会明确记录。

真实编解码器回归用两段已提交素材，对比全新解码器基线的采样数及 PCM 校验值；
随后禁止任何新的 Opus 内存分配，通过生产解码器模块交替播放 12 次，
并覆盖重复初始化。运行方式：

```bash
cmake -S tests/opus -B build/decoder-host
cmake --build build/decoder-host
ctest --test-dir build/decoder-host --output-on-failure
```

主机编译器需兼容 GCC/Clang（包括 Zig），因为生产定点组件使用 C99 变长数组及相关编译选项。
独立主机测试在优化编译时也明确保留断言。同一编解码器回归也可在 WASI 下运行，
只需授予测试对仓库素材的读取权限。真机验收需覆盖长时间滚动后的播放、
重新播放与切换音效，以及此前修复的松键停止行为。

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
