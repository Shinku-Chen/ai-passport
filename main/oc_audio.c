// main/oc_audio.c —— 16kHz 采集 + Opus 60ms 编码(实现见 oc_audio.h)。
//
// 时序预算(按 60ms 一帧):
//   读 2×480 samples(阻塞 30ms×2) → opus_encode 一次(complexity 0) → 回调入队。
//   编码在 C3 上只占一帧时间的一小部分;真正的余量来自"不做重采样、不做增益、不拷贝大缓冲"。
//
// 关键取舍:
//   * 单任务:采集与编码串行,省一个任务栈与一次槽位传递;回调只入队,所以不会拖慢采集。
//   * 空闲挂起 codec:一轮结束就 bsp_audio_sleep(),下一轮 begin 再 wake,省掉 ES8311 静态功耗。
//     codec 的休眠/唤醒必须与 PCM 读写串行,因此全部在音频任务里做,外部只置标志。
//   * 尾巴补零:松手时不满一帧的残余(pcm_fill)补零编出去,避免把最后一个字的尾音切掉。
#include "oc_audio.h"

#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "opus.h"

static const char *TAG = "oc_audio";

// 音频任务栈:Opus(SILK)编码在定点实现下局部数组很大,栈给小了会直接崩。
// 参考同芯片族固件把 opus 编解码任务开到 2048*13 = 26624 字节,这里给 24KB。
#define OC_AUDIO_TASK_STACK 24576U
#define OC_AUDIO_TASK_PRIO 4U
#define OC_AUDIO_IDLE_POLL_MS 20U
#define OC_AUDIO_END_TURN_TIMEOUT_MS 300U
// 尾巴至少要 10ms 才值得编一帧;更短直接丢(不值得为它多一个包)。
#define OC_AUDIO_TAIL_MIN_SAMPLES (OC_AUDIO_HZ / 100U)

typedef struct {
    oc_audio_frame_cb_t frame_cb;
    void *ctx;
    TaskHandle_t task;
    // 任务栈放在 .bss(静态):24KB 在堆上已经放不下——NimBLE + LVGL 绘制缓冲 + I2S DMA
    // 之后,内部堆只剩约 18KB 的最大连续块,建 24KB 任务会直接 ESP_ERR_NO_MEM。
    // 静态栈从 DRAM 出,不参与堆碎片,也不会因为堆紧张而拿不到。
    StaticTask_t task_tcb;
    StackType_t task_stack[OC_AUDIO_TASK_STACK / sizeof(StackType_t)];
    OpusEncoder *enc;
    SemaphoreHandle_t done_sem;      // end_turn 等待"尾巴已发完、codec 已挂起"
    SemaphoreHandle_t stopped_sem;   // stop 等待采集任务退出(避免在任务还活着时销毁编码器)
    // 采集缓冲:int16,固定 60ms
    int16_t pcm[OC_AUDIO_FRAME_SAMPLES];
    size_t pcm_fill;                 // 已填采样数(0..FRAME_SAMPLES)
    uint8_t seq;                     // 音频帧序号(1 字节回绕,只用于丢帧统计)
    uint8_t opus_buf[OC_AUDIO_OPUS_MAX];
    volatile bool turn_requested;
    volatile bool collecting;        // 音频任务确认正在采集
    volatile bool running;
    // TTS 播放(半双工):为 true 时采集任务不再开始/继续采集,并把 codec 挂起交出去。
    volatile bool capture_pause;
    oc_audio_stats_t stats;
} oc_audio_state_t;

static oc_audio_state_t s_aud;

// 把当前 pcm 缓冲编成一帧并交出去;pad=true 表示尾巴补零(缓冲里只有 pcm_fill 个有效采样)。
static void encode_and_emit(bool pad)
{
    if (pad && s_aud.pcm_fill < OC_AUDIO_TAIL_MIN_SAMPLES) {
        s_aud.pcm_fill = 0;
        return;
    }
    if (pad) {
        memset(&s_aud.pcm[s_aud.pcm_fill], 0, (OC_AUDIO_FRAME_SAMPLES - s_aud.pcm_fill) * sizeof(int16_t));
    }

    int64_t t0 = esp_timer_get_time();
    int n = opus_encode(s_aud.enc, s_aud.pcm, OC_AUDIO_FRAME_SAMPLES, s_aud.opus_buf, sizeof(s_aud.opus_buf));
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);

    s_aud.pcm_fill = 0;
    if (n <= 0) {
        // 编码失败(参数错/内部状态坏)不该中断整轮对讲:计数并继续采集。
        ESP_LOGW(TAG, "opus_encode 失败: %d", n);
        return;
    }

    s_aud.stats.frames_encoded++;
    s_aud.stats.opus_bytes += (uint32_t)n;
    if (dt > s_aud.stats.encode_us_max) s_aud.stats.encode_us_max = dt;
    // 平均耗时:用已编帧数累计,避免除零
    s_aud.stats.encode_us_avg =
        (s_aud.stats.encode_us_avg * (s_aud.stats.frames_encoded - 1u) + dt) / s_aud.stats.frames_encoded;

    uint8_t seq = s_aud.seq++;
    if (s_aud.frame_cb != NULL && s_aud.frame_cb(seq, s_aud.opus_buf, (size_t)n, s_aud.ctx)) {
        s_aud.stats.frames_delivered++;
    } else {
        s_aud.stats.frames_dropped++;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    bool codec_awake = false;

    while (s_aud.running) {
        // 没开始说话、或者下行播放占了 codec(半双工):收尾并把 codec 挂起交出去。
        if (!s_aud.turn_requested || s_aud.capture_pause) {
            if (codec_awake) {
                // 一轮结束:先把不满一帧的尾巴补零编出去(避免切掉最后一个字的尾音),
                // 再挂起 codec(此时已无 PCM 读写),最后放行 end_turn 的等待者。
                if (s_aud.pcm_fill > 0) {
                    encode_and_emit(true);
                }
                if (bsp_audio_sleep() != ESP_OK) {
                    ESP_LOGW(TAG, "codec 挂起失败(继续运行)");
                }
                codec_awake = false;
                s_aud.collecting = false;
                if (s_aud.done_sem != NULL) {
                    xSemaphoreGive(s_aud.done_sem);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(OC_AUDIO_IDLE_POLL_MS));
            continue;
        }

        if (!codec_awake) {
            if (bsp_audio_wake() != ESP_OK) {
                ESP_LOGE(TAG, "codec 唤醒失败");
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            // 唤醒会重建 codec;同格式重设是廉价操作,确保恢复到 16k/mono/16bit。
            if (bsp_audio_set_format(OC_AUDIO_HZ, 16, 1) != ESP_OK) {
                ESP_LOGE(TAG, "设置采样格式失败");
                codec_awake = true;   // 避免死循环:下一轮 end 时仍会尝试挂起
                continue;
            }
            codec_awake = true;
            s_aud.pcm_fill = 0;
            s_aud.collecting = true;
            // 半双工:采集期间压住 D/A,保证「录音时喇叭不出声」;播放开始时解除
            // (见 oc_audio_play_start)。这只保证不出声,**不治本机底噪** ——
            // 实测底噪在 codec 断电后依旧存在,属常电功放/电源侧,
            // 详见 bsp_audio_set_out_mute 的注释。
            if (bsp_audio_set_out_mute(true) != ESP_OK) {
                ESP_LOGW(TAG, "采集期间静音输出失败(继续运行)");
            }
            ESP_LOGI(TAG, "开始采集");
        }

        int16_t *dst = &s_aud.pcm[s_aud.pcm_fill];
        if (bsp_audio_read(dst, OC_AUDIO_READ_SAMPLES * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGW(TAG, "bsp_audio_read 失败,丢一块");
            s_aud.stats.pcm_overruns++;
            continue;
        }
        s_aud.pcm_fill += OC_AUDIO_READ_SAMPLES;
        if (s_aud.pcm_fill < OC_AUDIO_FRAME_SAMPLES) {
            continue;   // 还没攒够一帧
        }
        encode_and_emit(false);
        // 松手可能正好发生在这一块之后:再等一下,让循环顶部去做收尾。
    }

    if (codec_awake) {
        bsp_audio_sleep();
    }
    if (s_aud.stopped_sem != NULL) {
        xSemaphoreGive(s_aud.stopped_sem);
    }
    vTaskDelete(NULL);
}

// ===================== TTS 下行播放 =====================
// 手机侧的 TTS 音频推下来,设备解码后从喇叭放出来。规范见
// docs/development/engineering/intercom-wire-protocol.md 的 "TTS audio downlink",
// 内存储算与队列深度见 docs/development/engineering/intercom-tts-playback.md。
//
// 三条硬约束(缺一条就会在真机上出事):
//   1. 内存全静态。运行期空闲堆只剩十几 KB,而且这块板子没有 PSRAM:
//      opus_decoder_create() 内部是 malloc(约 18KB 连续),一旦 NimBLE 拿过堆就必然失败,
//      所以解码器状态自己给静态区,再用 opus_decoder_init() 就地初始化(同一套初始化,只差内存来源)。
//   2. 半双工。播放态里停采集:麦克风会录到喇叭,而且双向音频链路也扛不住。
//      codec 的休眠/唤醒有唯一的主人,不能用两个任务同时碰。
//   3. 不许卡住。任何结束路径(tts_stop 播空、tts_abort、用户打断、解码连续失败、长时间没声音、
//      断连、模块关停)都要把状态机送回“采集可用”,并如实上报 tts_playback_aborted。
// 播放任务栈:4096 太小——下行音频解码走的是 SILK(esp-opus 的 silk_decode_core/decode_frame,
// 内部有大块局部数组),真机实测一推 TTS 就 “Stack protection fault” panic 重启:
//   Guru Meditation … Stack protection fault. Detected in task "oc_play"
//   Stack pointer: 0x3fca2c40 / Stack bounds: 0x3fca3500-0x3fca44f0   (SP 已跑到栈下界以下)
//   反解地址: silk_decode_core (decode_core.c:66) ← silk_decode_frame (decode_frame.c:105)
// 取 10KB:实测峰值占用 7232B(栈余量日志 5056/12288),10KB 仍留 ~3KB 余量;
// 同时别把堆吃光 —— 堆只剩几 KB 时 NimBLE 连广播都起不来(真机踩过)。
// 栈余量(uxTaskGetStackHighWaterMark)实测确认还有余量。
#define OC_PLAY_TASK_STACK 10240U
#define OC_PLAY_TASK_PRIO 3U            // 低于采集(OC_AUDIO_TASK_PRIO=4):宁可晚几毫秒出声,不能丢上行
// 解码器状态区:opus_decoder_get_size(1) = silk_decoder(8560)+celt(9388)+OpusDecoder(≤100)
// ≤ 18148 字节;给 18KB 留余量,并在初始化时用 opus_decoder_get_size() 实测校验。
#define OC_PLAY_DECODER_AREA 18432U
#define OC_PLAY_POLL_MS 10U
#define OC_PLAY_IDLE_TIMEOUT_MS 1500U   // 连续这么久没真音频就结束本轮(首包没来 / 手机断流)
#define OC_PLAY_CAPTURE_WAIT_MS 1500U   // 等采集任务交 codec 的上限(I2S 读超时 1000ms + 尾巴)
#define OC_PLAY_FLUSH_TIMEOUT_MS 1200U  // 等播放任务收尾的上限(I2S 写超时 1000ms)
#define OC_PLAY_DECODE_FAIL_MAX 5U      // 连续解码失败这么多次就放弃本轮

// 一块 PCM(60ms 单声道 16bit):24kHz 满块 2880B,16kHz 只用前半块。
typedef struct {
    uint16_t len;                               // 有效字节数
    int16_t  data[OC_PLAY_PCM_BLOCK_SAMPLES];
} oc_play_block_t;

static struct {
    // 解码队列(纯逻辑,24 包 × 516B)与播放队列(2 块 PCM)
    oc_tts_queue_t   q;
    oc_play_block_t  pcm[OC_PLAY_PCM_QUEUE_DEPTH];
    uint32_t         pcm_head, pcm_tail, pcm_count;   // 只由播放任务改动
    // 任务与同步量(全静态:不参与堆碎片,也不会因为堆紧张建不出来)
    StaticTask_t     task_tcb;
    StackType_t      task_stack[OC_PLAY_TASK_STACK / sizeof(StackType_t)];
    TaskHandle_t     task;
    StaticSemaphore_t lock_buf, flush_buf;
    SemaphoreHandle_t lock;           // 保护解码队列(应用任务 push / 播放任务 pop)
    SemaphoreHandle_t flush_sem;      // flush() 等“已收尾”
    uint8_t          decoder_area[OC_PLAY_DECODER_AREA] __attribute__((aligned(4)));
    OpusDecoder     *dec;
    uint8_t          rate_khz;        // 解码器当前速率;0 = 尚未初始化
    uint8_t          pkt[OC_TTS_PACKET_MAX];   // 出队暂存
    uint8_t          push_seq;        // oc_audio_play_push(不带 SEQ)的自增序号
    volatile bool    available;       // 解码器就绪
    volatile bool    stream;          // 播放态(采集已/正在交还 codec)
    volatile bool    accepting;       // 还在收包(tts_start..tts_stop)
    volatile bool    flush_req;       // 立即丢弃并退出
    volatile bool    codec_open;      // codec 归播放任务用(采集不得同时用)
    volatile bool    stop_task;
    volatile bool    task_done;
    volatile oc_audio_play_ev_t ev;
    volatile int64_t start_us;        // tts_start 时刻(算首块出声延迟)
    uint32_t         decode_fail_run;
    uint32_t         overflow_logged;
    bool             bad_frame_logged;
} s_play;

static uint32_t play_pcm_space(void)
{
    return OC_PLAY_PCM_QUEUE_DEPTH - s_play.pcm_count;
}

// 解码队列的读写分属两个任务(应用任务 push / 播放任务 pop),全部在锁里进行:
// 队列的头/尾/计数是共享状态,不靠“只会有一个写者”这类假设。
static void play_note_decode(uint32_t decode_us)
{
    xSemaphoreTake(s_play.lock, portMAX_DELAY);
    oc_tts_queue_note_decode(&s_play.q, decode_us);
    xSemaphoreGive(s_play.lock);
}

static void play_note_underrun(void)
{
    xSemaphoreTake(s_play.lock, portMAX_DELAY);
    oc_tts_queue_note_underrun(&s_play.q);
    xSemaphoreGive(s_play.lock);
}

// 唤醒 codec 并按播放速率复配。每次新速率、每轮第一包都要调:codec 可能上一轮已被挂起,
// 而采样率不同必须重配(esp_codec_dev_open 对已打开的 codec 不会重设采样率)。
static bool play_codec_prepare(uint8_t rate_khz)
{
    if (bsp_audio_wake() != ESP_OK) {
        ESP_LOGE(TAG, "codec 唤醒失败,放弃本轮播放");
        return false;
    }
    if (bsp_audio_set_format((uint32_t)rate_khz * 1000U, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "播放格式(%ukHz)设置失败", (unsigned)rate_khz);
        return false;
    }
    s_play.codec_open = true;
    return true;
}

// 从解码队列取一个包解成一块 PCM。
// EMPTY:队列里暂时没包;FAIL:本轮应当放弃(解码器/编码参数坏了,或连续失败次数超限)。
typedef enum {
    OC_PLAY_STEP_DECODED = 0,
    OC_PLAY_STEP_EMPTY,
    OC_PLAY_STEP_FAIL,
} oc_play_step_t;

static oc_play_step_t play_decode_one(void)
{
    size_t len = 0;
    uint8_t rate_khz = 0;
    xSemaphoreTake(s_play.lock, portMAX_DELAY);
    bool popped = oc_tts_queue_pop(&s_play.q, s_play.pkt, sizeof(s_play.pkt), &len, &rate_khz);
    xSemaphoreGive(s_play.lock);
    if (!popped) {
        return OC_PLAY_STEP_EMPTY;
    }
    bool rate_changed = (s_play.rate_khz != rate_khz);
    if (rate_changed) {
        // 手机可以在流中间换速率(协议允许):重建解码器状态,这一包就按新速率解。
        int err = opus_decoder_init(s_play.dec, (opus_int32)rate_khz * 1000, 1);
        if (err != OPUS_OK) {
            ESP_LOGE(TAG, "opus_decoder_init(%ukHz) 失败: %d", (unsigned)rate_khz, err);
            return OC_PLAY_STEP_FAIL;
        }
    }
    if (rate_changed || !s_play.codec_open) {
        if (!play_codec_prepare(rate_khz)) {
            return OC_PLAY_STEP_FAIL;
        }
        s_play.rate_khz = rate_khz;
    }

    oc_play_block_t *blk = &s_play.pcm[s_play.pcm_head];
    int64_t t0 = esp_timer_get_time();
    int samples = opus_decode(s_play.dec, s_play.pkt, (opus_int32)len, blk->data,
                              OC_PLAY_PCM_BLOCK_SAMPLES, 0);
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    if (samples <= 0) {
        s_play.decode_fail_run++;
        ESP_LOGW(TAG, "opus_decode 失败: %d(连续 %u 次)", samples, (unsigned)s_play.decode_fail_run);
        return (s_play.decode_fail_run < OC_PLAY_DECODE_FAIL_MAX) ? OC_PLAY_STEP_EMPTY : OC_PLAY_STEP_FAIL;
    }
    s_play.decode_fail_run = 0;
    play_note_decode(dt);
    blk->len = (uint16_t)(samples * 2);
    s_play.pcm_head = (s_play.pcm_head + 1U) % OC_PLAY_PCM_QUEUE_DEPTH;
    s_play.pcm_count++;
    return OC_PLAY_STEP_DECODED;
}

static void play_write_block(void)
{
    oc_play_block_t *blk = &s_play.pcm[s_play.pcm_tail];
    // bsp_audio_write 阻塞到 DMA 收下数据(I2S 写超时 1000ms),天然按实时节奏放音。
    if (bsp_audio_write(blk->data, blk->len) != ESP_OK) {
        ESP_LOGW(TAG, "bsp_audio_write 失败,丢一块");
    }
    s_play.pcm_tail = (s_play.pcm_tail + 1U) % OC_PLAY_PCM_QUEUE_DEPTH;
    s_play.pcm_count--;
}

// 欠载时补一块静音:用空闲的 PCM 槽位生成,不额外占静态内存。
static void play_write_silence(void)
{
    if (!s_play.codec_open || s_play.pcm_count != 0 || s_play.rate_khz == 0) {
        return;
    }
    size_t bytes = (size_t)OC_AUDIO_FRAME_MS * s_play.rate_khz * 2U;
    if (bytes > sizeof(s_play.pcm[0].data)) {
        return;
    }
    oc_play_block_t *blk = &s_play.pcm[s_play.pcm_head];
    memset(blk->data, 0, bytes);
    if (bsp_audio_write(blk->data, bytes) != ESP_OK) {
        ESP_LOGW(TAG, "静音写出失败");
    }
}

// 一轮播放结束(正常播空或中止):清队列、挂起 codec、把采集还回去,并上报事件。
static void play_teardown(bool aborted)
{
    s_play.accepting = false;   // 先关门:后面到的包一律拒收,不会收下永远播不到的包
    xSemaphoreTake(s_play.lock, portMAX_DELAY);
    uint32_t left = oc_tts_queue_discard(&s_play.q);   // 没播出去的如实计入 dropped
    s_play.pcm_head = 0;
    s_play.pcm_tail = 0;
    s_play.pcm_count = 0;
    oc_tts_play_stats_t st = { 0 };
    oc_tts_queue_stats(&s_play.q, &st);
    uint32_t overflow = s_play.q.overflow;
    xSemaphoreGive(s_play.lock);

    if (s_play.codec_open) {
        if (bsp_audio_sleep() != ESP_OK) {
            ESP_LOGW(TAG, "codec 挂起失败(继续运行)");
        }
        s_play.codec_open = false;
    }
    // 半双工的另一半:回到可用状态。这一步不依赖任何外部条件,任何结束路径都会走到。
    s_aud.capture_pause = false;
    s_play.flush_req = false;
    s_play.stream = false;
    s_play.rate_khz = 0;   // 下一轮第一包重新初始化解码器
    s_play.decode_fail_run = 0;

    uint32_t stack_free = (s_play.task != NULL) ? (uint32_t)uxTaskGetStackHighWaterMark(NULL) : 0U;
    ESP_LOGI(TAG, "TTS 播放%s: 收=%u 解=%u 丢=%u(溢出=%u) 欠载=%u 解码最长=%uus 剩余=%u 栈余量=%u",
             aborted ? "中止" : "结束", (unsigned)st.frames, (unsigned)st.decoded, (unsigned)st.dropped,
             (unsigned)overflow, (unsigned)st.underruns, (unsigned)st.decode_us_max,
             (unsigned)left, (unsigned)stack_free);
    s_play.ev = aborted ? OC_AUDIO_PLAY_EV_ABORTED : OC_AUDIO_PLAY_EV_DONE;
    xSemaphoreGive(s_play.flush_sem);
}

static void play_stream(void)
{
    bool aborted = false;
    bool played = false;
    int64_t last_audio_us = esp_timer_get_time();
    // 采集任务收到 capture_pause 后会把这一轮的尾巴编完并挂起 codec;在它放手之前绝不能
    // 碰 codec(唤醒/改采样率会与阻塞中的 bsp_audio_read 打架)。
    for (uint32_t waited = 0; s_aud.collecting && waited < OC_PLAY_CAPTURE_WAIT_MS; waited += 5U) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_aud.collecting) {
        ESP_LOGE(TAG, "采集未在 %ums 内停下,放弃本轮播放", (unsigned)OC_PLAY_CAPTURE_WAIT_MS);
        aborted = true;
    }

    while (!aborted && s_play.stream) {
        if (s_play.flush_req) {
            aborted = true;
            break;
        }
        // ① 解码:把播放队列填满(解码只要几毫秒,提前解好才不会在写完一块后断流)
        while (!s_play.flush_req && play_pcm_space() > 0) {
            oc_play_step_t step = play_decode_one();
            if (step == OC_PLAY_STEP_FAIL) {
                ESP_LOGW(TAG, "连续解码失败,放弃本轮播放");
                aborted = true;
                break;
            }
            if (step == OC_PLAY_STEP_EMPTY) {
                break;   // 队列暂时空了
            }
        }
        if (aborted || s_play.flush_req) {
            aborted = true;
            break;
        }
        // ② 播放:写一块(阻塞约一块时长,天然按实时节奏)
        if (s_play.pcm_count > 0) {
            if (!played) {
                played = true;
                ESP_LOGI(TAG, "TTS 首块出声:tts_start 起 %lldms",
                         (esp_timer_get_time() - s_play.start_us) / 1000);
            }
            last_audio_us = esp_timer_get_time();
            play_write_block();
            continue;
        }
        // ③ 队列里没有可播的 PCM
        if (!s_play.accepting) {
            break;   // tts_stop 之后播空 → 正常结束
        }
        int64_t idle_ms = (esp_timer_get_time() - last_audio_us) / 1000;
        // 只在**队列真的空了**、而且静默够久时才结束本轮:真机证据(作者首句/第二句"丢一段")
        // —— 结束时队列里还剩 23 帧(≈1.4s)没播,而它们正好是这一句的尾巴/开头,
        // 于是听起来"读到一半断一下"或"前几个字没了"。有帧在队里就绝不收尾。
        if (idle_ms >= OC_PLAY_IDLE_TIMEOUT_MS && s_play.q.count == 0) {
            ESP_LOGW(TAG, "TTS 连续 %lldms 无音频(已出声=%d),结束本轮", (long long)idle_ms, (int)played);
            aborted = true;
            break;
        }
        if (played && idle_ms >= OC_AUDIO_FRAME_MS) {
            // 欠载:补一块静音,别让 I2S 饿着发出“咔”声;连续补太久由上面的超时兜底。
            // 注意要刷新 last_audio_us:否则静音写完后 idle_ms 仍 ≥60ms,下一圈立刻又计一次欠载,
            // 一段连续饿死会被按 60ms 反复计数(真机曾出现 欠载=57 而实际只有一段静音)。
            play_note_underrun();
            play_write_silence();
            last_audio_us = esp_timer_get_time();
        } else {
            vTaskDelay(pdMS_TO_TICKS(OC_PLAY_POLL_MS));
        }
    }
    play_teardown(aborted);
}

static void play_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_play.stop_task) {
            break;
        }
        if (!s_play.stream) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        play_stream();
        // 每轮播完报一次栈余量:定位“是不是快撞到栈底”靠这个数(单位字节,IDF 下 StackType_t=1B)。
        ESP_LOGI(TAG, "播放任务栈余量: %u 字节(总 %u)",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                 (unsigned)OC_PLAY_TASK_STACK);
    }
    if (s_play.codec_open) {
        bsp_audio_sleep();
        s_play.codec_open = false;
    }
    s_play.task_done = true;
    vTaskDelete(NULL);
}

// 初始化下行播放(由 oc_audio_init 调用)。解码器不可用时只禁用下行,上行不受影响。
static esp_err_t play_init(void)
{
    memset(&s_play, 0, sizeof(s_play));
    s_play.lock = xSemaphoreCreateMutexStatic(&s_play.lock_buf);
    s_play.flush_sem = xSemaphoreCreateBinaryStatic(&s_play.flush_buf);
    if (s_play.lock == NULL || s_play.flush_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    oc_tts_queue_init(&s_play.q);

    // 解码器状态静态化:内存来源换成 .bss,初始化走 opus_decoder_init(与 opus_decoder_create 同源)。
    int need = opus_decoder_get_size(1);
    if (need <= 0 || (size_t)need > sizeof(s_play.decoder_area)) {
        ESP_LOGE(TAG, "Opus 解码器需要 %d 字节,静态区只有 %u 字节:禁用下行播放",
                 need, (unsigned)sizeof(s_play.decoder_area));
        return ESP_ERR_NO_MEM;
    }
    s_play.dec = (OpusDecoder *)s_play.decoder_area;
    int err = opus_decoder_init(s_play.dec, OC_AUDIO_HZ, 1);
    if (err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_init 失败: %d:禁用下行播放", err);
        return ESP_FAIL;
    }
    s_play.rate_khz = (uint8_t)(OC_AUDIO_HZ / 1000U);
    s_play.available = true;
    ESP_LOGI(TAG, "下行播放就绪: 解码器状态 %d B/静态区 %u B, 解码队列 %u×%u B, 播放队列 %u×%u B, 任务栈 %u B",
             need, (unsigned)sizeof(s_play.decoder_area), (unsigned)OC_TTS_QUEUE_DEPTH,
             (unsigned)sizeof(oc_tts_slot_t), (unsigned)OC_PLAY_PCM_QUEUE_DEPTH,
             (unsigned)sizeof(oc_play_block_t), (unsigned)OC_PLAY_TASK_STACK);
    return ESP_OK;
}

static esp_err_t play_task_start(void)
{
    s_play.task = xTaskCreateStatic(play_task, "oc_play", OC_PLAY_TASK_STACK, NULL, OC_PLAY_TASK_PRIO,
                                    s_play.task_stack, &s_play.task_tcb);
    return (s_play.task != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void play_task_stop(void)
{
    if (s_play.task == NULL) {
        return;
    }
    s_play.stop_task = true;
    s_play.accepting = false;
    s_play.flush_req = true;
    xTaskNotifyGive(s_play.task);
    for (uint32_t waited = 0; !s_play.task_done && waited < 500U; waited += 10U) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_play.task_done) {
        ESP_LOGW(TAG, "播放任务未在 500ms 内退出(静态资源保留)");
    }
    s_play.task = NULL;
}

// 入队(应用任务上下文):拷贝 + 记数 + 唤醒播放任务,绝不阻塞在 codec/I2S 上。
static esp_err_t play_enqueue(const uint8_t *opus, size_t len, uint8_t rate_khz, uint8_t seq)
{
    if (!s_play.available || !s_play.accepting) {
        return ESP_ERR_INVALID_STATE;   // 不在播放态:手机必须先 tts_start
    }
    xSemaphoreTake(s_play.lock, portMAX_DELAY);
    // 锁内再确认一次 accepting:收尾时先关 accepting 再清队列,这样不会收下一个永远播不到、
    // 也不计入 dropped 的包。
    bool ok = s_play.accepting && oc_tts_queue_push(&s_play.q, seq, rate_khz, opus, len);
    uint32_t overflow = s_play.q.overflow;
    xSemaphoreGive(s_play.lock);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    if (overflow != s_play.overflow_logged) {
        // 队列满只提醒一次(每一包都刷屏会淹没日志),数字在 tts_playback_done 里看。
        s_play.overflow_logged = overflow;
        ESP_LOGW(TAG, "TTS 解码队列满,已丢最旧的包(累计 %u)", (unsigned)overflow);
    }
    if (s_play.task != NULL) {
        xTaskNotifyGive(s_play.task);
    }
    return ESP_OK;
}

esp_err_t oc_audio_play_push(const uint8_t *opus, size_t len, uint8_t rate_khz)
{
    if (opus == NULL || len == 0 || len > OC_TTS_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rate_khz != 16u && rate_khz != 24u) {
        return ESP_ERR_INVALID_ARG;
    }
    // 这条入口不带 SEQ,用自增序号占位(没有缺口可报,统计里的 dropped 只会来自溢出)。
    return play_enqueue(opus, len, rate_khz, s_play.push_seq++);
}

esp_err_t oc_audio_play_push_frame(const uint8_t *payload, size_t len)
{
    oc_tts_packet_t pkt = { 0 };
    oc_tts_parse_err_t pe = oc_tts_parse(payload, len, &pkt);
    if (pe != OC_TTS_PARSE_OK) {
        if (!s_play.bad_frame_logged) {
            // 手机侧编码错了会每帧都错:每轮只提醒一次,避免刷满日志。
            s_play.bad_frame_logged = true;
            ESP_LOGW(TAG, "TTS_OPUS 帧非法(%s): len=%u rate=%u frame_ms=%u",
                     (pe == OC_TTS_PARSE_ERR_LEN)     ? "长度 4..515"
                     : (pe == OC_TTS_PARSE_ERR_RATE)  ? "速率只认 16/24"
                                                       : "帧长只认 60ms",
                     (unsigned)len, (payload != NULL && len > 1U) ? (unsigned)payload[1] : 0u,
                     (payload != NULL && len > 2U) ? (unsigned)payload[2] : 0u);
        }
        return ESP_ERR_INVALID_ARG;
    }
    return play_enqueue(pkt.opus, pkt.opus_len, pkt.rate_khz, pkt.seq);
}

void oc_audio_play_start(void)
{
    // task 也要在:模块被 oc_audio_stop 停过后又没有重新 start 时,不能进播放态
    // (否则没人收尾,设备会卡在播放态、采集一直停着)。
    if (!s_play.available || s_play.task == NULL) {
        ESP_LOGW(TAG, "下行播放不可用(解码器/任务未就绪),忽略 tts_start");
        return;
    }
    if (s_play.stream) {
        ESP_LOGW(TAG, "已在播放态,忽略重复的 tts_start");
        return;
    }
    xSemaphoreTake(s_play.lock, portMAX_DELAY);
    oc_tts_queue_reset(&s_play.q);
    s_play.pcm_head = 0;
    s_play.pcm_tail = 0;
    s_play.pcm_count = 0;
    xSemaphoreGive(s_play.lock);

    s_play.push_seq = 0;
    s_play.overflow_logged = 0;
    s_play.bad_frame_logged = false;
    s_play.decode_fail_run = 0;
    s_play.ev = OC_AUDIO_PLAY_EV_NONE;
    // 要出声了:解除采集期间对输出级的静音
    if (bsp_audio_set_out_mute(false) != ESP_OK) {
        ESP_LOGW(TAG, "解除输出静音失败(继续运行)");
    }
    s_play.accepting = true;
    s_play.flush_req = false;
    s_play.start_us = esp_timer_get_time();
    s_play.stream = true;          // 先置 stream 再暂停采集:播放任务看到 stream 后会等采集交还 codec
    s_aud.capture_pause = true;    // 半双工:上行让位
    if (s_play.task != NULL) {
        xTaskNotifyGive(s_play.task);
    }
    ESP_LOGI(TAG, "TTS 播放态:采集暂停,等待首包");
}

bool oc_audio_play_available(void)
{
    return s_play.available;
}

void oc_audio_play_stop(void)
{
    if (!s_play.stream) {
        return;
    }
    // 不再收包,把队列里剩下的播完,由播放任务收尾并上报 tts_playback_done。
    s_play.accepting = false;
    if (s_play.task != NULL) {
        xTaskNotifyGive(s_play.task);
    }
}

bool oc_audio_play_idle(void)
{
    if (s_play.stream) {
        return false;
    }
    xSemaphoreTake(s_play.lock, portMAX_DELAY);
    bool idle = oc_tts_queue_empty(&s_play.q) && (s_play.pcm_count == 0);
    xSemaphoreGive(s_play.lock);
    return idle;
}

void oc_audio_play_get_stats(oc_audio_play_stats_t *st)
{
    if (st == NULL) {
        return;
    }
    xSemaphoreTake(s_play.lock, portMAX_DELAY);
    oc_tts_queue_stats(&s_play.q, st);
    xSemaphoreGive(s_play.lock);
}

oc_audio_play_ev_t oc_audio_play_take_event(void)
{
    oc_audio_play_ev_t ev = s_play.ev;
    s_play.ev = OC_AUDIO_PLAY_EV_NONE;
    return ev;
}

void oc_audio_play_flush(void)
{
    if (!s_play.stream) {
        return;   // 幂等:没在播就什么都不做
    }
    ESP_LOGW(TAG, "TTS 播放被打断:丢弃剩余音频");
    s_play.accepting = false;
    s_play.flush_req = true;
    while (xSemaphoreTake(s_play.flush_sem, 0) == pdTRUE) {
    }
    if (s_play.task != NULL) {
        xTaskNotifyGive(s_play.task);
    }
    if (xSemaphoreTake(s_play.flush_sem, pdMS_TO_TICKS(OC_PLAY_FLUSH_TIMEOUT_MS)) == pdTRUE) {
        return;   // 播放任务已收尾:codec 已挂起、采集已恢复
    }
    // 只可能卡在 I2S 写上(写超时 1000ms)。宁可把状态机强行拉回可用,
    // 也不能让设备永远停在播放态、上行永远不恢复。
    ESP_LOGE(TAG, "播放任务未在 %ums 内收尾,强制回到空闲", (unsigned)OC_PLAY_FLUSH_TIMEOUT_MS);
    xSemaphoreTake(s_play.lock, portMAX_DELAY);
    oc_tts_queue_discard(&s_play.q);
    s_play.pcm_head = 0;
    s_play.pcm_tail = 0;
    s_play.pcm_count = 0;
    xSemaphoreGive(s_play.lock);
    if (s_play.codec_open) {
        bsp_audio_sleep();
        s_play.codec_open = false;
    }
    s_aud.capture_pause = false;
    s_play.accepting = false;
    s_play.flush_req = false;
    s_play.stream = false;
    s_play.rate_khz = 0;
    s_play.ev = OC_AUDIO_PLAY_EV_ABORTED;
}

esp_err_t oc_audio_init(oc_audio_frame_cb_t cb, void *ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_aud, 0, sizeof(s_aud));
    s_aud.frame_cb = cb;
    s_aud.ctx = ctx;
    s_aud.done_sem = xSemaphoreCreateBinary();
    s_aud.stopped_sem = xSemaphoreCreateBinary();
    if (s_aud.done_sem == NULL || s_aud.stopped_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t e = bsp_audio_init();
    if (e != ESP_OK) {
        return e;
    }
    e = bsp_audio_set_format(OC_AUDIO_HZ, 16, 1);
    if (e != ESP_OK) {
        return e;
    }

    int err = OPUS_OK;
    s_aud.enc = opus_encoder_create(OC_AUDIO_HZ, 1, OPUS_APPLICATION_VOIP, &err);
    if (s_aud.enc == NULL || err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_create 失败: %d", err);
        return ESP_FAIL;
    }
    // 与参考固件一致:complexity 0(更高档位会把 C3 单核算力吃满),开 DTX(静音时不占空口)。
    opus_encoder_ctl(s_aud.enc, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(s_aud.enc, OPUS_SET_DTX(1));
    ESP_LOGI(TAG, "音频就绪: %uHz/%ums/complexity0/DTX", (unsigned)OC_AUDIO_HZ, (unsigned)OC_AUDIO_FRAME_MS);
    // 下行播放:失败只禁用下行,不影响上行采集(见 play_init 里的日志)。
    if (play_init() != ESP_OK) {
        s_play.available = false;
    }
    return ESP_OK;
}

esp_err_t oc_audio_start(void)
{
    if (s_aud.enc == NULL || s_aud.done_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_aud.running) {
        return ESP_OK;
    }
    s_aud.running = true;
    s_aud.task = xTaskCreateStatic(audio_task, "oc_audio", OC_AUDIO_TASK_STACK, NULL, OC_AUDIO_TASK_PRIO,
                                   s_aud.task_stack, &s_aud.task_tcb);
    if (s_aud.task == NULL) {
        s_aud.running = false;
        return ESP_ERR_NO_MEM;
    }
    // 下行播放任务同样早建:静态栈不占堆,没开始播放时空转。建不出来只影响下行。
    if (play_task_start() != ESP_OK) {
        ESP_LOGE(TAG, "播放任务创建失败:下行不可用");
        s_play.available = false;
    }
    return ESP_OK;
}

void oc_audio_stop(void)
{
    // 先停播放任务:它也在用 codec,且必须自己把采集的暂停标志收回来。
    play_task_stop();
    if (!s_aud.running) {
        return;
    }
    s_aud.turn_requested = false;
    s_aud.running = false;
    // 等任务自己收尾(尾巴 + 挂起 codec)并退出;没等到也不能在任务还活着时销毁编码器。
    if (s_aud.stopped_sem != NULL &&
        xSemaphoreTake(s_aud.stopped_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "采集任务未在 500ms 内退出,保留编码器不释放");
        s_aud.task = NULL;
        return;
    }
    s_aud.task = NULL;
    if (s_aud.enc != NULL) {
        opus_encoder_destroy(s_aud.enc);
        s_aud.enc = NULL;
    }
}

esp_err_t oc_audio_begin_turn(void)
{
    if (!s_aud.running) {
        return ESP_ERR_INVALID_STATE;
    }
    // 播放态里不许开始采集:codec 正被播放任务占着,应用层应先 oc_audio_play_flush()。
    if (s_aud.capture_pause) {
        return ESP_ERR_INVALID_STATE;
    }
    // 本轮统计清零:丢帧率与编码耗时都是"每轮"看的数。
    s_aud.stats.frames_encoded = 0;
    s_aud.stats.frames_delivered = 0;
    s_aud.stats.frames_dropped = 0;
    s_aud.stats.pcm_overruns = 0;
    s_aud.stats.encode_us_max = 0;
    s_aud.stats.encode_us_avg = 0;
    s_aud.stats.opus_bytes = 0;
    s_aud.pcm_fill = 0;
    if (s_aud.done_sem != NULL) {
        xSemaphoreTake(s_aud.done_sem, 0);   // 清掉上一轮可能残留的信号
    }
    s_aud.turn_requested = true;
    return ESP_OK;
}

void oc_audio_end_turn(void)
{
    if (!s_aud.turn_requested) {
        return;
    }
    // 收尾(尾巴补零 + 挂起 codec + 计数)全部由采集任务在两次读之间完成:
    // 缓冲只能属于任务,外部直接动它会与阻塞读竞态。这里只置标志并等它放信号。
    s_aud.turn_requested = false;
    if (s_aud.done_sem != NULL) {
        xSemaphoreTake(s_aud.done_sem, pdMS_TO_TICKS(OC_AUDIO_END_TURN_TIMEOUT_MS));
    }
}

bool oc_audio_turn_active(void)
{
    return s_aud.collecting;
}

esp_err_t oc_audio_set_mic_gain_db(uint8_t db)
{
    return bsp_audio_set_mic_gain((float)db);
}

void oc_audio_get_stats(oc_audio_stats_t *out)
{
    if (out != NULL) {
        *out = s_aud.stats;
    }
}

bool oc_audio_capture_paused(void)
{
    return s_aud.capture_pause;
}
