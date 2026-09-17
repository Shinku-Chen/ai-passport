// main/c4_sound.c —— 用正弦表合成的四子棋音效。
//
// 每个音效是一串「频率段」:段内可做线性扫频(落子那声就是下滑音),并带线性衰减,
// 段首 3ms 淡入避免爆音。所有生成都在音频任务里按块进行,只需要一块 256 采样的
// 栈上缓冲(512 字节),对无 PSRAM 的 C3 很友好。
#include "c4_sound.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "c4_sound";

#define C4_SOUND_HZ        16000      // 采样率
#define C4_SOUND_VOLUME    70         // 输出音量 0..100
#define C4_SOUND_CHUNK     256        // 每块采样数
#define C4_SOUND_ATTACK_MS 3          // 段首淡入,消掉爆音
#define C4_SOUND_TASK_STACK 3072
#define C4_SOUND_TASK_PRIO   4        // 低于输入任务(5),高于 AI worker(3)
#define C4_SOUND_QUEUE_DEPTH 6

typedef struct {
    uint16_t hz_from;
    uint16_t hz_to;      // 与 hz_from 相同即为定频
    uint16_t ms;
    uint16_t amp;        // 峰值(16bit 有符号)
} c4_segment_t;

typedef struct {
    const c4_segment_t *segments;
    uint8_t count;
} c4_sound_t;

// 频率取常见和弦音,音量留足余量避免削顶。
static const c4_segment_t SEG_START[] = { { 700, 700, 90, 6000 }, { 1050, 1050, 140, 6000 } };
static const c4_segment_t SEG_MOVE[]  = { { 1200, 1200, 40, 2200 } };
static const c4_segment_t SEG_DROP[]  = { { 520, 170, 150, 7000 } };
static const c4_segment_t SEG_WIN[]   = { { 660, 660, 110, 7000 }, { 880, 880, 110, 7000 },
                                          { 1320, 1320, 180, 7000 } };
static const c4_segment_t SEG_LOSE[]  = { { 520, 520, 130, 6000 }, { 400, 400, 130, 6000 },
                                          { 300, 300, 220, 6000 } };
static const c4_segment_t SEG_DRAW[]  = { { 500, 500, 120, 6000 }, { 420, 420, 180, 6000 } };

static const c4_sound_t C4_SOUNDS[] = {
    [C4_SOUND_START] = { SEG_START, 2 },
    [C4_SOUND_MOVE]  = { SEG_MOVE, 1 },
    [C4_SOUND_DROP]  = { SEG_DROP, 1 },
    [C4_SOUND_WIN]   = { SEG_WIN, 3 },
    [C4_SOUND_LOSE]  = { SEG_LOSE, 3 },
    [C4_SOUND_DRAW]  = { SEG_DRAW, 2 },
};

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_play_lock;   // 播放期间持有;deep sleep 前由休眠路径夺走
static TaskHandle_t s_task;
static bool s_ready;
static bool s_off;                      // 已进入关机流程,不再接新音效

static int16_t s_sine[256];

static void sine_table_init(void)
{
    for (int i = 0; i < 256; i++) {
        s_sine[i] = (int16_t)lrintf(32767.0f * sinf(2.0f * 3.14159265358979f * (float)i / 256.0f));
    }
}

// 段内第 pos 个采样(共 total 个)的频率与增益(千分比)。
static uint32_t segment_freq(const c4_segment_t *seg, int pos, int total)
{
    const int32_t from = seg->hz_from;
    const int32_t to = seg->hz_to;
    if (from == to || total <= 1) return (uint32_t)from;

    const int32_t span = (to - from) * pos / (total - 1);
    const int32_t hz = from + span;
    return (uint32_t)(hz > 1 ? hz : 1);
}

static int segment_gain(const c4_segment_t *seg, int pos, int total)
{
    const int attack = C4_SOUND_HZ * C4_SOUND_ATTACK_MS / 1000;
    // 1000 -> 250 的线性衰减,再乘段首淡入。
    int gain = 1000 - (750 * pos) / (total > 1 ? total - 1 : 1);
    if (pos < attack) gain = gain * pos / attack;
    return gain;
}

static void play_segment(const c4_segment_t *seg)
{
    const int total = (int)((uint32_t)seg->ms * C4_SOUND_HZ / 1000);
    if (total <= 0) return;

    int16_t buf[C4_SOUND_CHUNK];
    uint32_t phase = 0;

    for (int done = 0; done < total;) {
        int n = total - done;
        if (n > C4_SOUND_CHUNK) n = C4_SOUND_CHUNK;

        for (int i = 0; i < n; i++) {
            const int pos = done + i;
            const uint32_t hz = segment_freq(seg, pos, total);
            const int gain = segment_gain(seg, pos, total);

            const int32_t sample = (int32_t)s_sine[(phase >> 24) & 0xFF];
            buf[i] = (int16_t)(sample * seg->amp / 32767 * gain / 1000);
            phase += (uint32_t)(((uint64_t)hz << 32) / C4_SOUND_HZ);
        }

        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGW(TAG, "I2S 写入失败,跳过剩余音效");
            return;
        }
        done += n;
    }
}

static void sound_task(void *arg)
{
    (void)arg;

    for (;;) {
        c4_sound_id_t id;
        if (xQueueReceive(s_queue, &id, portMAX_DELAY) != pdTRUE) continue;
        if (s_off) continue;
        if (id < 0 || id >= (int)(sizeof(C4_SOUNDS) / sizeof(C4_SOUNDS[0]))) continue;

        if (s_play_lock && xSemaphoreTake(s_play_lock, portMAX_DELAY) != pdTRUE) continue;

        const c4_sound_t *sound = &C4_SOUNDS[id];
        for (uint8_t i = 0; i < sound->count; i++) {
            play_segment(&sound->segments[i]);
        }

        xSemaphoreGive(s_play_lock);
    }
}

bool c4_sound_init(void)
{
    if (s_ready) return true;

    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败:本局静音");
        return false;
    }
    if (bsp_audio_set_format(C4_SOUND_HZ, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "音频格式设置失败:本局静音");
        return false;
    }
    bsp_audio_set_volume(C4_SOUND_VOLUME);

    sine_table_init();

    s_play_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(C4_SOUND_QUEUE_DEPTH, sizeof(c4_sound_id_t));
    if (!s_play_lock || !s_queue) {
        ESP_LOGW(TAG, "音效资源分配失败:本局静音");
        if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
        if (s_play_lock) { vSemaphoreDelete(s_play_lock); s_play_lock = NULL; }
        return false;
    }
    if (xTaskCreate(sound_task, "c4_sound", C4_SOUND_TASK_STACK, NULL,
                    C4_SOUND_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "音效任务创建失败:本局静音");
        vQueueDelete(s_queue);
        s_queue = NULL;
        vSemaphoreDelete(s_play_lock);
        s_play_lock = NULL;
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "音效就绪:%dHz 单声道,音量 %d%%", C4_SOUND_HZ, C4_SOUND_VOLUME);
    return true;
}

void c4_sound_play(c4_sound_id_t id)
{
    if (!s_ready || !s_queue || s_off || s_task == NULL) return;
    (void)xQueueSend(s_queue, &id, 0);   // 队列满就丢,绝不阻塞调用方
}

bool c4_sound_hold_for_sleep(void)
{
    s_off = true;                        // 先关门,再等当前音效写完
    if (!s_ready || !s_play_lock) return true;
    return xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(1000)) == pdTRUE;
}
