<p align="right"><a href="port-cafestella-media.md">English</a> · <strong>简体中文</strong></p>

# Cafe Stella 媒体整合

本文说明星光咖啡馆 AI Passport 移植新增的媒体路径。固定源仍为 `xuanyide01/cafestella-rw5` @ `90ddb9e020970907b61ad4181f25c65267e6e98b`。

## 源与编码

| 项目 | 源 | 编码后 |
| --- | --- | --- |
| 标题曲 | `src/common/title-bgm.opus`，1,078,545 B，48 kHz 立体声 | 4 kHz 单声道 IMA ADPCM，516,096 B，1,027,198 samples / 256.7995 s |
| OP 曲 | `src/common/opening-bgm.opus`，376,222 B，48 kHz 立体声 | 8 kHz 单声道 IMA ADPCM，373,760 B 数据，743,959 samples / 92.995 s |
| OP 视频 | `src/common/opening/00..09`，477 JPEG，5 fps，95.460 s | 287 JPEG，224×126 q15，3 fps，620,030 B |

`tools/cafe_media.py` 用 ffmpeg/Pillow 从固定源生成 IMA 与 OP 帧；`tools/cafe_pack.py` 将其写入 `assets/cafe/cafe.pak` 的 section 24/25/26；OP JPEG 追加到 image 表 1,997–2,283。

## 播放语义

- 标题曲保存完整解码样本；仅由标题/菜单场景生命周期重启，不截到 256.000 s。
- OP 曲保存完整 93 s；播放时按源在 92.4 s pre-EOF guard 停止，95.460 s 视频继续到静音尾。这是源的实际 guard，不声称 OP 中输出全部 93 s 样本。
- OP 视频保持完整 95.460 s 时间轴：第 k 帧取源 `floor(k*5/3)`，末帧持有到 95.460 s；不拉伸/不截断。
- OP 时钟使用 app 单调 `now_ms`；落后时直接跳到当前帧，不做追赶长循环，输入保持响应。
- 音频/codec 故障先于任何短按键或长按定时处理；触发故障的同一次 press/release 被消费，进度/auto/FF/存档/CG 写入被阻断。重试保留原页面、OP 帧与媒体 elapsed，定位 IMA 块并丢弃块内前缀样本，不推进 VM；故障等待期间 OP 视频时钟暂停；越过 92.4 s guard 时以静音视频尾恢复。
- OP 短按跳过只推进 VM 一次；自然结束也只推进一次；长按松手不追加短按。
- 标题曲跟随标题/菜单/鉴赏/设置页面生命周期；音乐鉴赏列出并播放真实两轨；设置含声音开关和音量。codec/I2S 写失败停止播放并进入现有 resource fault，不忙等。停止使用 active-busy 确认，不把过期 idle token 当成功；超时可观察并保持设备唤醒，不在 codec/I2S 临界区内删除任务。

## 条带复用与内存

`cafe_image_render_op` 把 224×126 OP JPEG 等比映射到完整 224×126 舞台，复用现有三条 224×42 RGB565 条带和 `s_stage`；不分配整幅 OP 帧，不扩大 LVGL 缓存。target RISC-V `-fstack-usage`：`decode_jpeg` 176 B、`render_op_band` 160 B、`cafe_image_render_op` 32 B、`out_func` 48 B（最大自帧 176 B）。音频任务栈 4 KB；解码/PCM 静态缓冲约 5.1 KB，已计入 DRAM。

最终固件 8,246,672 B / factory 8,323,072 B，余 76,400 B。静态 DRAM 239,250 / 321,296 B，余 82,046 B；扣除既有条带与输入任务栈后动态余量约 21.6 KB。这些是 target 构建测量，不是设备运行保证。

## 验证状态

Host：packed media CRC/index、全部 287 帧生产条带解码、首/中/末 OP 对照、坏 record CRC/JPEG 拒绝、IMA 样本数/独立参考比较、30 屏 LVGL smoke（64/32-bit Host HAL）、static 全过；firmware build/merge/verify/archive PASS。

Device：**NOT RUN**。真实 ES8311/I2S 输出、扬声器时序、ST7789 OP 帧刷新、持续 CPU/heap/stack 高水位、OP 音画同步仍未验证。
