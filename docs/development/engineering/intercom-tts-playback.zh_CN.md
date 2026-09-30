<p align="right">
  <strong>简体中文</strong> · <a href="intercom-tts-playback.md">English</a>
</p>

# 对讲机 TTS 播放（设备侧）

本文是对讲机应用下行音频——设备出声朗读网关回复——的实现清单。线协议本身见
[intercom-wire-protocol.zh_CN.md](intercom-wire-protocol.zh_CN.md)；本文只讲固件为了
满足该协议需要分配什么、运行什么、上报什么。

设计已定稿：帧布局、控制事件与队列深度是要照着实现的决定，不是可以重新讨论的选项；
下面的尺寸是需要**在写第一行实现之前**核实的预算。

## 1. 背景与目标

对讲机已经能把语音上行采集、并把网关回复渲染成文本，缺的另一半是"听见回复"。目标
是：网关答复 → 手机合成语音 → 设备经扬声器播出来。

- 音频来源是**手机侧 TTS**。设备自己不调用语音服务，也不依赖小智云那条路径，因此
  凭据、厂商选型与费用都留在手机侧，固件只负责解码与播放。
- 下行只传 Opus。24 kHz 单声道 16bit 裸 PCM 约 48 KB/s，BLE 在这个方向上承载不了。
- 参考实现是 [`xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) 的 `AudioService`：
  `audio_decode_queue_` 接 `audio_playback_queue_`，
  `MAX_DECODE_PACKETS_IN_QUEUE = 2400 / OPUS_FRAME_DURATION_MS`，
  `MAX_PLAYBACK_TASKS_IN_QUEUE = 2`；解码器是 `OpusDecoderWrapper`，来自 `78/esp-opus`
  组件（`^1.0.5`）——本仓库已经为了上行编码依赖同一个组件（`main/oc_audio.c` 里的
  `opus_encoder_create`，`dependencies.lock` 固定为 `1.0.5`）。此处固定的组件清单
  声明的是 `idf: '>=5.0'`，而参考项目为它自己的构建写的是 `>= 5.3`；本仓库跑的是
  ESP-IDF 5.5.3，两种说法都满足，不需要做版本适配工作。
- `xiaozhi-esp32` 官方构建包含 ESP32-C3，说明 Opus 解码在本芯片上已经被实际跑过。这里
  要判断的是算力与内存够不够，而不是能不能解码。

## 2. 内存与静态预算

这是第一道门。ESP32-C3 没有 PSRAM，而 NimBLE、LVGL 与应用任务都起来之后，运行期空闲
堆只剩约 12.5 KB，因此这条路径上的任何东西都不能在流开始时才分配。

| 项 | 尺寸 | 放置位置 |
| --- | --- | --- |
| Opus 解码器（16 kHz 或 24 kHz、单声道、60 ms） | 约 15–20 KB | 静态 `.bss` |
| 解码队列：24 包 × 约 128 B | 约 3 KB | 静态 `.bss` |
| 播放队列：2 ×（24 kHz × 60 ms × 2 B = 2880 B） | 约 5.8 KB（16 kHz 时约 3.8 KB） | 静态 `.bss` |
| I2S TX DMA | 方案里估 4 KB；BSP 实际配的是 6 个描述符 × 240 帧，32bit 立体声槽位下每个方向约 5.6 KB（`components/bsp/src/bsp_audio.c`） | 已由 `bsp_audio_init()` 分配：它确实占用预算所针对的那片 DRAM，但不是新增需求 |
| **按方案合计** | **约 28–33 KB** | 其中只有解码器与两条队列是新增需求，DMA 本来就有 |

要求：

- 解码器状态是 `.bss` 里的一块静态 `OpusDecoder` 区域（libopus 的
  `opus_decoder_init` 配上静态缓冲，或组件提供的静态分配路径）。**不能用**
  `opus_decoder_create`：等 NimBLE 拿走堆之后，一块约 20 KB 的连续分配就再也满足
  不了——这正是逼着编码器必须在 BLE 栈启动前创建的那条约束（`main/oc_app.c`）。
- 动手实现**之前**先跑 `idf.py size` 并把数字记下来。静态缓冲与堆来自同一片 DRAM，
  所以"静态"并不等于"不占内存"：往 `.bss` 里加 28–33 KB，就等于从启动堆里拿走
  28–33 KB。作为起点，本工作区里最近一次构建的链接映射在 321,296 B 的 DRAM 段里已
  放入 124,368 B 的 `.bss` 与 10,620 B 的 `.data`；该产物可能是旧的，按量级看就好，
  不要当实测值。
- 如果数字放不下，按下面的优先级腾挪，并在变更里给出**腾挪前后**的数字：
  1. LVGL 绘制缓冲（界面就是文字页，现有绘制缓冲比这个页面需要的更大），
  2. 按最坏情况估出来、而不是实测出来的应用/主任务栈，
  3. 上行采集缓冲。
- 不允许为了把账凑平而牺牲安全相关缓冲（I2S DMA 深度、BLE 栈、看门狗余量）。

## 3. 任务与并发

- 解码与播放各由一个任务 `oc_audio_play` 负责。它与采集任务相互独立，这样解码突发
  永远不会拖慢麦克风读取；任务栈用静态 4 KB（与 `main/oc_audio.c` 相同的静态任务
  做法）。
- 优先级**低于**采集任务（`oc_audio` 跑在优先级 4）。丢 60 ms 上行音频是不可接受的，
  而让一个解码块晚几毫秒没有任何影响。
- 数据流：`oc_audio_play_push()` 把一个 Opus 包拷进解码队列后立即返回；任务取包、
  解码成一块 PCM、交给 `bsp_audio_write()`，然后循环。
- `oc_audio_play_push()` 运行在应用任务上下文（控制事件也在那里处理）。它绝不能在
  codec、DMA 或解码任务上阻塞；它唯一的失败模式是队列满，此时丢掉最旧的一包并计数。
- 与上行**半双工**。设备处于播放态时不采集：手机的音频与用户的语音不能同时跑，既
  因为麦克风会把扬声器录进去，也因为链路带宽不是按两路音频设计的。上行采集在
  `tts_start` 暂停，在 `tts_stop`（队列排空后）恢复，或在 `tts_abort` 时立即恢复。
- `rate_khz` 是逐帧的。速率不变时解码器跨包保留 Opus 状态；流中途改速率时为新的
  速率重新初始化解码器；宣布变化的那一包也仍然按新速率解码。
- LVGL 不参与。播放只通过既有的 `oc_ui_set_state()` 路径改状态词，由应用任务在持有
  `bsp_lvgl_lock()` 时调用。任何音频任务都不得触碰 LVGL 对象。

## 4. 固件接口

播放模块的建议签名；实现任务把它们加在现有采集 API 旁边：

```c
esp_err_t oc_audio_play_push(const uint8_t *opus, size_t len, uint8_t rate_khz);
void      oc_audio_play_flush(void);
bool      oc_audio_play_idle(void);
void      oc_audio_play_get_stats(oc_audio_play_stats_t *st);
```

| 函数 | 契约 |
| --- | --- |
| `oc_audio_play_push` | 由应用任务传入一个 `TTS_OPUS` 载荷里的 Opus 包及其 `rate_khz`。把包拷进有界解码队列后返回。包或速率非法返回 `ESP_ERR_INVALID_ARG`；模块未初始化或设备当前不收音频返回 `ESP_ERR_INVALID_STATE`；收下返回 `ESP_OK`。队列满不算错误：丢掉最旧的一包、计数，仍返回 `ESP_OK` |
| `oc_audio_play_flush` | 丢弃队列里的一切，把播放态标记为正在收尾，并在队列逻辑上清空后立即返回。可以在应用任务里、在打断时、在没有播放时安全调用（幂等） |
| `oc_audio_play_idle` | 没有音频在排队或播放时返回 true。应用据此在播放态与常规界面之间切换 |
| `oc_audio_play_get_stats` | 为 `tts_playback_done` 事件拷出本轮统计，之后调用方开始新的一轮。不得阻塞播放任务 |

签名里有 `rate_khz` 却没有 `frame_ms`，是因为设备只实现一种帧长：应用层直接拒绝
`frame_ms` 不为 `60` 的 `TTS_OPUS` 帧（记为非法包，每条流只记一条日志），而不是让
解码器去误解它。以后要支持第二种帧长，就扩展这个签名，而不是静默猜测。

```c
typedef struct {
    uint32_t frames;         /* 从手机收下的包数 */
    uint32_t decoded;        /* 解码成功的包数 */
    uint32_t dropped;        /* 被丢弃的包数:队列溢出 + SEQ 缺口 */
    uint32_t underruns;      /* 播放时无解码数据可用的次数 */
    uint32_t decode_us_max;  /* 单包最长解码耗时(微秒) */
} oc_audio_play_stats_t;
```

## 5. 与既有 turn 流程的交互

- **打断（barge-in）**：播放中按下 `OK` 就是打断。设备在本地 flush 播放队列（效果与
  手机下发 `tts_abort` 相同），上报 `tts_playback_aborted`，恢复采集，然后按现在的做法
  开始新一轮。flush 必须在采集开始之前完成，否则用户说的第一个字会在扬声器还在响的
  时候被录进去。
- **松手**：`RELEASE` 行为不变——带上行计数的 `turn_end`，然后回到常规界面。TTS 播放
  不改变松手路径。
- **顺序**：用户还按着 `OK` 时到达的 `tts_start` 会被忽略并记日志，不接受任何音频；
  两个方向不能同时跑，手机必须先结束本轮再推音频。
- **文本**：播放期间 `TEXT` 照常上屏，路径与音频无关；回复"同时可见可听"是预期行为。
- **`turn_ready`** 保持原义——采集轮次的手机侧反馈，与播放无关。

## 6. 统计与失败处理

- 每条流在 `tts_stop` 之后队列排空时，以 `{"ev":"tts_playback_done",...}` 上报：收下
  的包数、解码成功数、丢弃数、欠载次数、单包最长解码耗时（微秒）。这样手机才能区分
  "已发送"和"已听到"。
- `SEQ` 缺口复用 `main/oc_proto.h` 里已有主机测试覆盖的 `oc_seq_tracker_t`：缺口记为
  丢弃。重复序号忽略，不记为丢失。
- 解码失败只计数，不中断整条流。**连续 3 次**失败后，设备丢弃该流剩余部分并上报
  `tts_playback_aborted`，日志里带上 libopus 的错误码：解码器持续失败说明协商出来的
  采样率或帧长不对，继续下去只会变成噪声。
- 欠载（`tts_stop` 之前任务需要音频但队列已空）记一次，并补一块静音，避免 codec 出现
  "咔"声。反复欠载说明手机侧送得太慢，日志必须如实体现。
- 每一条退出路径——`tts_stop` 排空、`tts_abort`、打断、连续解码失败、链路断开、模块
  关闭——都要把设备带回空闲态并恢复采集。设备卡在"播放态但没音频"是唯一绝对不能存在
  的故障形态。

## 7. 测试计划

主机侧纯逻辑单测（不依赖 ESP-IDF / LVGL，风格与 `tests/test_oc_proto.c` 一致，并接进
`tools/validate.sh --static`）：

- `TTS_OPUS` 载荷解析：3 字节头被正确剥离，`rate_khz` 只接受 16 与 24，`frame_ms`
  只接受 60，payload 长度落在 `4..515` 之外一律拒绝。
- 队列溢出：往深度 `N` 的队列里推 `N + 1` 包时丢**最旧**的一包、保留最新的，并使
  `dropped` 恰好加一。
- 序号统计：缺口计数、重复忽略、256 回绕正确。
- 统计聚合：收下/解码/丢弃/欠载四类计数在一条流内累加，下一条流开始时归零。
- flush：flush 清空队列、报空闲，并计入被丢弃的包数。

真机验收清单：

1. 网关回复在 16 kHz 与 24 kHz 两种速率下都能从扬声器听到。
2. 从 `tts_start` 到第一个可听样本的延迟有测量与日志。
3. 打断：播放中按 `OK` 能迅速停声，且新一轮采集不会丢掉第一个字。
4. 不自听：设备播放期间麦克风通路是停的，手机对下一轮的识别是干净的。
5. `tts_playback_done` 的计数与手机实际发送的量吻合，包括刻意丢一包的情况。
6. 队列满时下发 `tts_abort`，设备回到空闲态并恢复采集。
7. 长回复（约 60 s）播放过程中，除已上报的计数外没有额外丢帧。
8. 坏包不会把播放卡死；连续 3 次失败会 flush 并上报 `tts_playback_aborted`。
9. 前后各记录一次 `idf.py size` 与运行期空闲堆日志。

## 8. 分阶段交付

| 阶段 | 范围 |
| --- | --- |
| M1 | 只做播放通路：`TTS_OPUS` 帧能解码、设备能出声。App 可以先用临时本地 TTS 产音频，统计先不上报 |
| M2 | 打断、播放态与背光：`tts_start` / `tts_stop` / `tts_abort` 驱动状态机，采集暂停与恢复，播放期间屏幕让用户知道发生了什么 |
| M3 | 界面状态词与统计上报：播放态在屏幕上可见，`tts_playback_done` / `tts_playback_aborted` 把计数带给 App 控制台 |

每个阶段都必须保证设备脱离手机仍可用，并且不得让采集路径退化。M1 实际上就是内存的
那道门：静态预算放不下就会在这里失败，而不是等到 UI 与事件都堆上去之后才暴露。

## 9. 未决问题

- **TTS 服务选型**：火山、Azure、Edge-TTS 都能在手机侧产出 Opus 或可转换的格式；这是
  App 侧的选择，不改变本固件契约。
- **重采样**：如果选定的服务只出 24 kHz，设备要么在播放前把 24 kHz 重采样到 16 kHz，
  要么直接按 24 kHz 播。直接按 24 kHz 播改动更小（codec 与 `rate_khz` 字段本来就支持），
  在没有实测问题之前优先这样做；设备侧重采样会吃 CPU 和静态内存，这份预算挤不出来。
- **长回复期间的背光策略**：按 `tts_start` 的约定，播放期间保持背光；如果一条流可能
  持续数分钟，可能需要在流中途就套用常规空闲调光策略，而不是等到 `tts_stop`。

## 相关文档

- [intercom-wire-protocol.zh_CN.md](intercom-wire-protocol.zh_CN.md)：线协议，含
  `TTS_OPUS` 帧与播放事件。
- [coding-conventions.zh_CN.md](coding-conventions.zh_CN.md)：本方案必须满足的栈、
  静态分配与资源评审规则。

## 实现说明(M1)

由实现任务追加;上面各节仍是设计依据,这里只记录固件实际做了什么、以及内存门禁
跑出来的数字。

### 实际落地的固件接口

`main/oc_audio.h`:

| 函数 | 行为 |
| --- | --- |
| `oc_audio_play_push(opus, len, rate_khz)` | 字节级入口:一个 Opus 包加它的速率。不带 `SEQ`,因此无法报告序号缺口 |
| `oc_audio_play_push_frame(payload, len)` | `on_frame` 用的线协议入口:解析 `[SEQ][rate_khz][frame_ms]`,校验载荷长度(`4..515`)、速率(`16`/`24`)与帧长(`60`),再带 `SEQ` 缺口统计进同一个队列 |
| `oc_audio_play_start()` / `oc_audio_play_stop()` | `tts_start` / `tts_stop` 的状态迁移(先暂停采集,再播空收尾) |
| `oc_audio_play_flush()` | 同步:返回时 I2S 写出已停、codec 已挂起、采集已恢复,所以紧接着开始的一轮不可能录到喇叭 |
| `oc_audio_play_available()` | 解码器是否可用。`hello` 只在它为真时才报 `tts_opus` |
| `oc_audio_play_idle()`、`oc_audio_play_get_stats()` | 与设计一致 |
| `oc_audio_play_take_event()` | 粘性 `DONE` / `ABORTED` 事件。应用任务每 50ms 取一次并组 `EVENT` 帧,因为组帧缓冲归应用任务所有 |

`main/oc_tts.c` 放不依赖 ESP-IDF 的那一半(载荷解析、24 包环形队列、计数),由
`tests/test_oc_tts.c` 覆盖;`main/oc_audio.c` 负责解码器、I2S 写出与任务。

### 实测内存(ESP32-C3,ESP-IDF 5.5.3)

| 项目 | 字节 | 位置 |
| --- | --- | --- |
| Opus 解码器状态 | 预留 18432;`opus_decoder_get_size(1)` 实测 ≤ 18148 | 静态 `.bss` |
| 解码队列 24 槽(每槽 516B:长度 + 速率 + 包) | 12384 | 静态 `.bss` |
| 播放队列 2 块(24kHz 60ms) | 5768 | 静态 `.bss` |
| 播放任务栈、TCB 与两个静态信号量 | 4096 + 约 340 | 静态 `.bss` |
| 出队暂存缓冲(`pkt`) | 512 | 静态 `.bss` |
| **新增静态占用(`s_play`)** | **约 41808** | `main/oc_audio.c` |
| LVGL 内置 malloc 池 65536 → 24576 | **-40960** | `lv_mem_core_builtin.c` |
| **DRAM 净变化** | **+908** | 231844/321296 → 232752/321296 |

LVGL 9 里 `CONFIG_LV_MEM_SIZE_KILOBYTES` 已废弃、不生效,所以槽子一直是 65536B,而
`sdkconfig.defaults` 写的是 24KB。改成 `CONFIG_LV_MEM_SIZE=24576` 才真正落实了那个
意图(顺便让 `bsp_display_lvgl.c` 里“24 KB LVGL 池”的说法成立),把 40KB `.bss` 还
给了启动堆。结果是运行期空闲堆比加这个功能之前少约 0.9KB;5 秒心跳会打一行
`lv_mem_monitor()`,真机上可以据此确认新的池子够不够。

### 与上文不同的决定

- **解码失败阈值**:连续 **5** 次才中止,不是 3 次。5 次是确认坏了(约 3 秒垃圾音),
  3 次可能只是丢包后一帧还没对上。
- **静音与闲置**:欠载时用空闲播放槽位生成一块静音(不额外占内存)并计一次欠载。
  连续 1.5 秒没有真音频就自行结束本轮,手机中途断流也不会把设备卡在播放态、把采集
  一直停着。
- **codec 交接**:`tts_start` 先暂停采集,播放任务等采集任务把这一帧收尾、挂起 codec
  之后才碰 codec;`flush()` 同步等同一个握手反向完成,所以打断后的新一轮不会录到喇叭。
- **背光**:播放期间每个应用任务节拍刷新一次 `oc_ui_note_activity()`,回答一直看得见。
  以分钟计的流会一直亮屏,这仍是第 9 节的未决问题。
- **状态词**:播放期间用现有的“接收中”,没有新增居中大字。

### 仍未在真机验证的部分

24KB 的 LVGL 池(界面放得下,但只有真机跑一遍才能确认)、4KB 播放任务栈(每条流结束
时会打栈余量)、音质、端到端延迟(会打 `TTS 首块出声: tts_start 起 Nms`)、欠载表现,
以及播放与录音在半双工下的实际重叠情况。
