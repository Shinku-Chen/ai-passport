// main/gal/gal_music.c —— 标题曲播放（4 kHz 单声道 IMA ADPCM → 16 kHz I2S）。
//
// 媒体包保存原作完整 283.88 秒标题曲的 4 位 ADPCM（每个 256 样本块独立，
// 块头是首样本与起始步长）。播放任务逐块解码、线性插值 4 倍上采样到
// 16 kHz，再交给 bsp_audio_write()。不截短、不用方波替代。
#include "gal_music.h"

#include "bsp_audio.h"
#include "gal_content.h"
#include "maitetsu_pack.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "gal_music";

extern const uint8_t maitetsu_title_bin_start[] asm("_binary_maitetsu_title_bin_start");
extern const uint8_t maitetsu_title_bin_end[] asm("_binary_maitetsu_title_bin_end");

#define AUDIO_HEADER_BYTES 36u
#define AUDIO_BLOCK_SAMPLES 256u
#define AUDIO_BLOCK_BYTES 132u
#define AUDIO_MAGIC "MTAUDIO\0"
#define OUT_RATE 16000
#define OUT_VOLUME 60
// 一个 IMA 块解码后是 1024 个 16 kHz 样本（64 ms）。一次交给 I2S 的却是
// 256 个样本（16 ms）：DMA 一共只有 6 x 240 帧 = 90 ms，按 64 ms 写入时
// 队列最低只剩 26 ms，LVGL 绘制/NVS 提交卡一下就会欠载；按 16 ms 写入时
// 队列最低约 74 ms，同样的卡顿不再断音。
#define OUT_CHUNK 1024
#define OUT_WRITE_SAMPLES 256u
// 高于 LVGL 任务(4)：绘制/解码不能把音频喂数拖过 DMA 余量。
#define MUSIC_TASK_PRIORITY 5

static const int32_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3258, 3583, 3941, 4334, 4766, 5242, 5766, 6342, 6975, 7672, 8438, 9280,
    10205, 11220, 12338, 13570, 14924, 16412, 18052, 19853, 21835, 24015, 26419, 29055,
    32007,
};
static const int8_t IMA_INDEX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static TaskHandle_t s_task;
static SemaphoreHandle_t s_wake;
static volatile int s_current = -1;
static volatile int s_request = -1;

static const uint8_t *s_pack;
static uint32_t s_samples;
static uint32_t s_blocks;
static uint32_t s_block_bytes;

static int16_t s_pcm[AUDIO_BLOCK_SAMPLES];
static int16_t s_out[OUT_CHUNK];

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool load_audio_pack(void)
{
    const uint8_t *data = maitetsu_title_bin_start;
    const size_t size = (size_t)(maitetsu_title_bin_end - maitetsu_title_bin_start);
    if (size < AUDIO_HEADER_BYTES || memcmp(data, AUDIO_MAGIC, 8) != 0) {
        ESP_LOGE(TAG, "音频包标识不匹配");
        return false;
    }
    if (data[8] != 1 || data[9] != 1) {
        ESP_LOGE(TAG, "音频包版本不认识");
        return false;
    }
    const uint32_t rate = read_u32(data + 12);
    const uint32_t samples = read_u32(data + 16);
    const uint16_t block_samples = read_u16(data + 20);
    const uint32_t blocks = read_u32(data + 24);
    const uint32_t payload = read_u32(data + 28);
    if (rate != 4000 || block_samples != AUDIO_BLOCK_SAMPLES) {
        ESP_LOGE(TAG, "音频包采样参数不支持");
        return false;
    }
    if (blocks == 0 || payload != blocks * AUDIO_BLOCK_BYTES ||
        AUDIO_HEADER_BYTES + payload != size) {
        ESP_LOGE(TAG, "音频包长度不一致");
        return false;
    }
    // 整包 CRC：包头 CRC 字段（偏移 32）按 0 计。
    uint8_t header_copy[AUDIO_HEADER_BYTES];
    memcpy(header_copy, data, sizeof(header_copy));
    memset(header_copy + 32, 0, 4);
    const uint32_t stored = read_u32(data + 32);
    uint32_t crc = maitetsu_crc32(header_copy, sizeof(header_copy));
    crc = maitetsu_crc32_update(crc, data + AUDIO_HEADER_BYTES, size - AUDIO_HEADER_BYTES);
    if (crc != stored) {
        ESP_LOGE(TAG, "音频包 CRC 校验失败");
        return false;
    }
    s_pack = data;
    s_samples = samples;
    s_blocks = blocks;
    s_block_bytes = AUDIO_BLOCK_BYTES;
    ESP_LOGI(TAG, "标题曲就绪：%u 样本 / %.2f 秒 / %u 块",
             (unsigned)samples, (double)samples / 4000.0, (unsigned)blocks);
    return true;
}

static void decode_block(uint32_t index, int16_t *out)
{
    const uint8_t *block = s_pack + AUDIO_HEADER_BYTES + (size_t)index * s_block_bytes;
    int32_t predictor = (int16_t)read_u16(block);
    int32_t step_index = block[2];
    if (step_index > 88) step_index = 88;
    out[0] = (int16_t)predictor;
    uint32_t produced = 1;
    for (uint32_t byte = 4; byte < s_block_bytes && produced < AUDIO_BLOCK_SAMPLES; ++byte) {
        for (int half = 0; half < 2 && produced < AUDIO_BLOCK_SAMPLES; ++half) {
            const uint8_t nibble = half == 0 ? (block[byte] >> 4) : (block[byte] & 0x0Fu);
            const int32_t step = IMA_STEP[step_index];
            int32_t diff = step >> 3;
            if (nibble & 4) diff += step;
            if (nibble & 2) diff += step >> 1;
            if (nibble & 1) diff += step >> 2;
            predictor += (nibble & 8) ? -diff : diff;
            if (predictor > 32767) predictor = 32767;
            if (predictor < -32768) predictor = -32768;
            step_index += IMA_INDEX[nibble & 7];
            if (step_index < 0) step_index = 0;
            if (step_index > 88) step_index = 88;
            out[produced++] = (int16_t)predictor;
        }
    }
}

static void fill_upsampled(int16_t *dst, const int16_t *src, int16_t next)
{
    for (int i = 0; i < (int)AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t a = src[i];
        const int32_t b = (i + 1 < (int)AUDIO_BLOCK_SAMPLES) ? src[i + 1] : next;
        dst[i * 4 + 0] = (int16_t)a;
        dst[i * 4 + 1] = (int16_t)((3 * a + b) / 4);
        dst[i * 4 + 2] = (int16_t)((a + b) / 2);
        dst[i * 4 + 3] = (int16_t)((a + 3 * b) / 4);
    }
}

static bool audio_ready(void)
{
    if (bsp_audio_init() != ESP_OK) return false;
    if (bsp_audio_set_format(OUT_RATE, 16, 1) != ESP_OK) return false;
    bsp_audio_set_volume(OUT_VOLUME);
    return true;
}

static bool write_pcm(const int16_t *pcm, uint32_t samples)
{
    uint32_t done = 0;
    while (done < samples) {
        uint32_t n = samples - done;
        if (n > OUT_WRITE_SAMPLES) n = OUT_WRITE_SAMPLES;
        if (bsp_audio_write(pcm + done, n * sizeof(int16_t)) != ESP_OK) return false;
        done += n;
    }
    return true;
}

static void play_title(void)
{
    if (!s_pack) return;
    if (!audio_ready()) {
        ESP_LOGE(TAG, "音频初始化失败，无法播放");
        return;
    }
    uint32_t block = 0;
    int64_t loop_start_us = 0;
    while (s_current == 0) {
        if (block == 0) loop_start_us = esp_timer_get_time();
        decode_block(block, s_pcm);
        int16_t next;
        if (block + 1 < s_blocks) {
            const uint8_t *nb = s_pack + AUDIO_HEADER_BYTES +
                                (size_t)(block + 1) * s_block_bytes;
            next = (int16_t)read_u16(nb);
        } else {
            next = s_pcm[AUDIO_BLOCK_SAMPLES - 1];
        }
        fill_upsampled(s_out, s_pcm, next);
        // 最后一块可能带填充样本：只写到源曲真实长度，保证 283.88 秒不多不少。
        uint32_t written = OUT_CHUNK;
        const uint32_t played = block * AUDIO_BLOCK_SAMPLES;
        if (played < s_samples && s_samples - played < AUDIO_BLOCK_SAMPLES) {
            written = (s_samples - played) * 4u;
        }
        if (!write_pcm(s_out, written)) {
            ESP_LOGW(TAG, "音频写入失败，停止播放");
            s_current = -1;
            break;
        }
        if (block + 1 == s_blocks) {
            // 有界计时诊断：整曲一轮的墙钟时间。正常应为 283,880 ms 左右；
            // 明显更长说明欠载/写入阻塞，偏短说明采样率链不对。
            const int64_t elapsed_ms = (esp_timer_get_time() - loop_start_us) / 1000;
            ESP_LOGI(TAG, "整曲一轮：%u 样本 / %lld ms（源曲 283.88 秒）",
                     (unsigned)s_samples * 4u, (long long)elapsed_ms);
        }
        block = (block + 1) % s_blocks;
        if (block == 0) {
            // 曲终留一小段静音再从头播放，避免听起来像卡住。
            memset(s_out, 0, sizeof(s_out));
            for (int i = 0; i < 3 && s_current == 0; ++i) {
                if (!write_pcm(s_out, OUT_CHUNK)) break;
            }
        }
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
        play_title();
        s_current = -1;
    }
}

bool gal_music_init(void)
{
    if (s_task) return true;
    if (!load_audio_pack()) return false;
    s_wake = xSemaphoreCreateBinary();
    if (!s_wake) return false;
    if (xTaskCreate(music_task, "gal_music", 4096, NULL, MUSIC_TASK_PRIORITY, &s_task) != pdPASS) {
        vSemaphoreDelete(s_wake);
        s_wake = NULL;
        return false;
    }
    return true;
}

void gal_music_play(int track_index)
{
    if (!s_task || track_index < 0 || track_index >= gal_content_track_count) return;
    if (!s_pack) return;
    s_current = -1;
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
