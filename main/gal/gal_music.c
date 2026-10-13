// main/gal/gal_music.c —— 方波合成 + 播放任务。
#include "gal_music.h"

#include "bsp_audio.h"
#include "gal_content.h"
#include "gal_tone.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "gal_music";

// 合成参数:16kHz / 16bit / 单声道;每块 100ms,一个音符 5 块(500ms)。
#define GAL_MUSIC_RATE 16000
#define GAL_MUSIC_BLOCK_MS 100
#define GAL_MUSIC_BLOCK_SAMPLES (GAL_MUSIC_RATE / 1000 * GAL_MUSIC_BLOCK_MS)
#define GAL_MUSIC_NOTE_BLOCKS 5   // 一个音符 500ms
#define GAL_MUSIC_AMPLITUDE 6000
#define GAL_MUSIC_VOLUME 60

// 音高表:C4 起的自然音阶,内容层的 notes 用它当下标。
static const uint16_t TONE_HZ[] = {
    262, 294, 330, 349, 392, 440, 494,
    523, 587, 659, 698, 784, 880, 988, 1047,
};
#define TONE_COUNT ((int)(sizeof(TONE_HZ) / sizeof(TONE_HZ[0])))

static TaskHandle_t s_task;
static SemaphoreHandle_t s_wake;
static volatile int s_current = -1;   // 正在播放的曲目,-1 = 停止
static int s_request = -1;            // 最近一次请求(任务取走)

// 生成一个音块;hz = 0 表示休止。波形逻辑在 gal_tone.c(可宿主机测试)。
static void fill_tone(int16_t *buf, int samples, uint16_t hz)
{
    gal_tone_fill(buf, samples, GAL_MUSIC_RATE, hz, GAL_MUSIC_AMPLITUDE);
}

static bool audio_ready(void)
{
    if (bsp_audio_init() != ESP_OK) return false;
    if (bsp_audio_set_format(GAL_MUSIC_RATE, 16, 1) != ESP_OK) return false;
    bsp_audio_set_volume(GAL_MUSIC_VOLUME);
    return true;
}

// 循环播放一首曲子,直到 s_current 不再是它。
static void play_track(int index)
{
    const gal_track_t *track = &gal_content_tracks[index];
    if (!track->notes || track->note_count == 0) return;
    if (!audio_ready()) {
        ESP_LOGE(TAG, "音频初始化失败,无法播放");
        s_current = -1;
        return;
    }

    static int16_t buf[GAL_MUSIC_BLOCK_SAMPLES];
    int note = 0;
    while (s_current == index) {
        const uint8_t pitch = track->notes[note];
        const uint16_t hz =
            (pitch == GAL_NOTE_REST || pitch >= TONE_COUNT) ? 0 : TONE_HZ[pitch];
        for (int block = 0; block < GAL_MUSIC_NOTE_BLOCKS && s_current == index; ++block) {
            fill_tone(buf, GAL_MUSIC_BLOCK_SAMPLES, hz);
            if (bsp_audio_write(buf, sizeof(buf)) != ESP_OK) {
                ESP_LOGW(TAG, "音频写入失败,停止播放");
                s_current = -1;
                return;
            }
        }
        note = (note + 1) % track->note_count;
    }
}

static void music_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_wake, portMAX_DELAY) != pdTRUE) continue;
        const int index = s_request;
        if (index < 0 || index >= gal_content_track_count) {
            s_current = -1;
            continue;
        }
        s_current = index;
        play_track(index);
        s_current = -1;
    }
}

bool gal_music_init(void)
{
    if (s_task) return true;
    s_wake = xSemaphoreCreateBinary();
    if (!s_wake) return false;
    if (xTaskCreate(music_task, "gal_music", 4096, NULL, 4, &s_task) != pdPASS) {
        vSemaphoreDelete(s_wake);
        s_wake = NULL;
        return false;
    }
    return true;
}

void gal_music_play(int track_index)
{
    if (!s_task || track_index < 0 || track_index >= gal_content_track_count) return;
    s_current = -1;   // 让上一段循环先退出
    s_request = track_index;
    (void)xSemaphoreGive(s_wake);
}

void gal_music_stop(void)
{
    s_current = -1;
}

int gal_music_current(void)
{
    return s_current;
}
