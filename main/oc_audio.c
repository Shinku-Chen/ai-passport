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
        if (!s_aud.turn_requested) {
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
    return ESP_OK;
}

void oc_audio_stop(void)
{
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
