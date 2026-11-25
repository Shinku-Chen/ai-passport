#include "nine_media.h"
#include "nine_image.h"
#include "nine_format.h"
#include <limits.h>
#include <string.h>
#ifndef NINE_MEDIA_HOST
#include "bsp_audio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#endif

static const int s_steps[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
    73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,
    449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,
    2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
    7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,
    24623,27086,29794,32767
};
static const int s_index[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};

bool nine_media_decode_block(const uint8_t *src, size_t len, int16_t pcm[256])
{
    if (!src || !pcm || len != NINE_MEDIA_BLOCK_BYTES || src[2] > 88 || src[3])
        return false;
    int predictor = (int16_t)((uint16_t)src[0] | (uint16_t)src[1] << 8);
    int index = src[2];
    pcm[0] = (int16_t)predictor;
    for (unsigned i = 1; i < NINE_MEDIA_BLOCK_SAMPLES; ++i) {
        unsigned n = i - 1;
        unsigned code = (src[4 + n / 2] >> ((n & 1u) * 4)) & 15u;
        int step = s_steps[index];
        int difference = step >> 3;
        if (code & 1) difference += step >> 2;
        if (code & 2) difference += step >> 1;
        if (code & 4) difference += step;
        predictor += code & 8 ? -difference : difference;
        if (predictor > 32767) predictor = 32767;
        if (predictor < -32768) predictor = -32768;
        index += s_index[code];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        pcm[i] = (int16_t)predictor;
    }
    return true;
}

typedef struct {
    const uint8_t *script, *images;
    size_t script_size, image_size;
    uint32_t table, count, strings, string_size;
    uint32_t requested, current, generation;
    uint32_t block, samples, sample_limit, duration, rate;
    uint32_t frame, last_image;
    bool initialized, playing, failed, opening, closing;
#ifndef NINE_MEDIA_HOST
    int64_t started_us;
#endif
} media_t;
static media_t s_media;
#ifndef NINE_MEDIA_HOST
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static portMUX_TYPE s_mutex = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() taskENTER_CRITICAL(&s_mutex)
#define UNLOCK() taskEXIT_CRITICAL(&s_mutex)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static bool in_range(size_t size, uint32_t off, uint32_t len)
{ return off <= size && len <= size - off; }
static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
    uint32_t crc = UINT32_MAX;
    while (n--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool nine_media_track_info(uint32_t track, nine_media_track_t *out)
{
    if (!s_media.initialized || !out || track >= s_media.count) return false;
    const uint8_t *p = s_media.images + s_media.table + (size_t)track * 32;
    uint32_t name = rd32(p + 4);
    if (name >= s_media.string_size ||
        !memchr(s_media.script + s_media.strings + name, 0, s_media.string_size - name))
        return false;
    out->name = (const char *)s_media.script + s_media.strings + name;
    out->data_offset = rd32(p + 8);
    out->data_size = rd32(p + 12);
    out->duration_ms = rd32(p + 16);
    out->sample_rate = rd32(p + 20);
    // A duration determines the unpadded tail; complete blocks make seeking
    // independent of previous state and bound every decode to 512 PCM bytes.
    uint64_t samples = ((uint64_t)out->duration_ms * out->sample_rate + 999) / 1000;
    return rd32(p) == 1u && rd32(p + 24) == 1 && out->sample_rate == 8000 &&
           out->duration_ms && out->duration_ms <= 24u * 60u * 60u * 1000u &&
           out->data_size && out->data_size % NINE_MEDIA_BLOCK_BYTES == 0 &&
           in_range(s_media.image_size, out->data_offset, out->data_size) &&
           samples <= UINT32_MAX &&
           (samples + 255) / 256 == out->data_size / NINE_MEDIA_BLOCK_BYTES;
}

static bool begin_track(uint32_t track)
{
    nine_media_track_t info;
    if (!nine_media_track_info(track, &info)) return false;
    LOCK();
    s_media.current = track;
    s_media.block = s_media.samples = 0;
    s_media.duration = info.duration_ms;
    s_media.rate = info.sample_rate;
    s_media.sample_limit = (uint32_t)(((uint64_t)info.duration_ms * info.sample_rate + 999) / 1000);
    s_media.playing = true;
#ifndef NINE_MEDIA_HOST
    s_media.started_us = esp_timer_get_time();
#endif
    UNLOCK();
    return true;
}

static size_t decode_next(int16_t *pcm)
{
    nine_media_track_t info;
    if (!nine_media_track_info(s_media.current, &info) || s_media.samples >= s_media.sample_limit)
        return 0;
    uint64_t off = (uint64_t)info.data_offset + (uint64_t)s_media.block * NINE_MEDIA_BLOCK_BYTES;
    if (off + NINE_MEDIA_BLOCK_BYTES > (uint64_t)info.data_offset + info.data_size ||
        !nine_media_decode_block(s_media.images + off, NINE_MEDIA_BLOCK_BYTES, pcm)) return 0;
    uint32_t n = s_media.sample_limit - s_media.samples;
    if (n > 256) n = 256;
    return n;
}

#ifndef NINE_MEDIA_HOST
static void audio_worker(void *arg)
{
    (void)arg;
    int16_t pcm[NINE_MEDIA_BLOCK_SAMPLES];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        LOCK();
        bool closing = s_media.closing;
        uint32_t track = s_media.requested, generation = s_media.generation;
        UNLOCK();
        if (closing) break;
        if (track == NINE_NONE) continue;
        bool ok = begin_track(track);
        if (ok) ok = bsp_audio_init() == ESP_OK && bsp_audio_set_format(8000, 16, 1) == ESP_OK;
        if (ok) bsp_audio_set_volume(65);
        while (ok) {
            LOCK();
            bool changed = s_media.generation != generation || s_media.closing;
            UNLOCK();
            if (changed) break;
            size_t n = decode_next(pcm);
            if (!n) { ok = s_media.samples >= s_media.sample_limit; break; }
            ok = bsp_audio_write(pcm, n * sizeof(*pcm)) == ESP_OK;
            LOCK();
            s_media.samples += (uint32_t)n;
            ++s_media.block;
            UNLOCK();
        }
        LOCK();
        if (s_media.generation == generation) {
            s_media.playing = false;
            s_media.failed = !ok;
        }
        UNLOCK();
    }
    LOCK(); s_media.playing = false; UNLOCK();
    // No source-data read or BSP access occurs after the exit signal.
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}
#endif

bool nine_media_init(const uint8_t *script, size_t script_size,
                     const uint8_t *images, size_t image_size)
{
    if (s_media.initialized || !script || !images || script_size < 256 || image_size < 128 ||
        memcmp(script, NINE_SCRIPT_MAGIC, 8) || memcmp(images, NINE_IMAGE_MAGIC, 8) ||
        rd32(script + 8) != NINE_FORMAT_VERSION || rd32(images + 8) != NINE_FORMAT_VERSION ||
        rd32(script + 12) != script_size || rd32(images + 12) != image_size) return false;
    uint32_t n = rd32(images + 36), table = rd32(images + 40);
    uint32_t strings = rd32(script + 132), string_size = rd32(script + 136);
    if (n > UINT32_MAX / 32 || !in_range(image_size, table, n * 32) ||
        (n && table < 128) || !in_range(script_size, strings, string_size)) return false;
    memset(&s_media, 0, sizeof(s_media));
    s_media.script = script; s_media.script_size = script_size;
    s_media.images = images; s_media.image_size = image_size;
    s_media.table = table; s_media.count = n;
    s_media.strings = strings; s_media.string_size = string_size;
    s_media.current = s_media.requested = NINE_NONE;
    s_media.last_image = NINE_NONE;
    s_media.initialized = true;
    for (uint32_t i = 0; i < n; ++i) {
        nine_media_track_t info;
        const uint8_t *p = images + table + (size_t)i * 32;
        if (!nine_media_track_info(i, &info) ||
            crc32_bytes(images + info.data_offset, info.data_size) != rd32(p + 28)) {
            memset(&s_media, 0, sizeof(s_media)); return false;
        }
        for (uint32_t off = 0; off < info.data_size; off += NINE_MEDIA_BLOCK_BYTES) {
            const uint8_t *block = images + info.data_offset + off;
            if (block[2] > 88 || block[3]) {
                memset(&s_media, 0, sizeof(s_media)); return false;
            }
        }
    }
#ifndef NINE_MEDIA_HOST
    s_done = xSemaphoreCreateBinary();
    if (!s_done || xTaskCreate(audio_worker, "nine_audio", 4096, NULL, 4, &s_task) != pdPASS) {
        if (s_done) vSemaphoreDelete(s_done);
        s_done = NULL; s_task = NULL;
        memset(&s_media, 0, sizeof(s_media)); return false;
    }
#endif
    return true;
}

static bool play(uint32_t track, bool opening)
{
    nine_media_track_t info;
    if (!nine_media_track_info(track, &info)) return false;
    LOCK();
    if (s_media.closing) { UNLOCK(); return false; }
    ++s_media.generation;
    s_media.requested = track;
    s_media.current = track;
    s_media.samples = 0;
    s_media.duration = info.duration_ms;
    s_media.rate = info.sample_rate;
    s_media.failed = false;
    s_media.playing = true;
    s_media.opening = opening;
    s_media.frame = 0;
    s_media.last_image = NINE_NONE;
#ifndef NINE_MEDIA_HOST
    s_media.started_us = esp_timer_get_time();
#endif
    UNLOCK();
#ifndef NINE_MEDIA_HOST
    xTaskNotifyGive(s_task);
#else
    if (!begin_track(track)) return false;
#endif
    return true;
}
bool nine_media_play(uint32_t track) { return play(track, false); }
bool nine_media_play_op(uint32_t track) { return play(track, true); }

void nine_media_stop(void)
{
    if (!s_media.initialized) return;
    LOCK();
    ++s_media.generation;
    s_media.requested = NINE_NONE;
    s_media.playing = s_media.opening = false;
    UNLOCK();
#ifndef NINE_MEDIA_HOST
    if (s_task) xTaskNotifyGive(s_task);
#endif
}

bool nine_media_deinit(void)
{
    if (!s_media.initialized) return true;
    nine_media_stop();
    LOCK(); s_media.closing = true; UNLOCK();
#ifndef NINE_MEDIA_HOST
    xTaskNotifyGive(s_task);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1500)) != pdTRUE) return false;
    // Worker has signalled no further source/BSP accesses. Its self-deletion may
    // still be pending idle-task cleanup, but no application pointer is retained.
    s_task = NULL;
    vSemaphoreDelete(s_done); s_done = NULL;
#endif
    memset(&s_media, 0, sizeof(s_media));
    return true;
}
bool nine_media_is_playing(void)
{ LOCK(); bool v = s_media.initialized && s_media.playing; UNLOCK(); return v; }
uint32_t nine_media_current_track(void)
{ LOCK(); uint32_t v = s_media.initialized ? s_media.current : NINE_NONE; UNLOCK(); return v; }
bool nine_media_failed(void)
{ LOCK(); bool v = s_media.failed; UNLOCK(); return v; }
uint32_t nine_media_track_count(void) { return s_media.initialized ? s_media.count : 0; }
uint32_t nine_media_position_ms(void)
{
    LOCK();
    uint32_t pos;
#ifndef NINE_MEDIA_HOST
    int64_t elapsed = esp_timer_get_time() - s_media.started_us;
    pos = elapsed <= 0 ? 0 : (uint32_t)(elapsed / 1000);
    if (!s_media.playing && s_media.rate)
        pos = (uint32_t)((uint64_t)s_media.samples * 1000 / s_media.rate);
#else
    pos = s_media.rate ? (uint32_t)((uint64_t)s_media.samples * 1000 / s_media.rate) : 0;
#endif
    if (pos > s_media.duration) pos = s_media.duration;
    UNLOCK(); return pos;
}

void nine_media_tick(void)
{
    // The application owns OP frame selection and calls nine_image_show_asset_top
    // from its single LVGL tick. This compatibility entry point must not render
    // a second timestamped sequence from the audio clock.
    LOCK();
    bool opening = s_media.opening;
    bool playing = s_media.playing;
    if (opening && !playing) s_media.opening = false;
    UNLOCK();
}
#ifdef NINE_MEDIA_HOST
void nine_media_test_fail(int fail)
{
    LOCK();
    s_media.failed = fail != 0;
    if (fail) s_media.playing = false;
    UNLOCK();
}

void nine_media_test_advance(uint32_t ms)
{
    if (!s_media.initialized || !s_media.rate) return;
    uint64_t add = (uint64_t)ms * s_media.rate / 1000u;
    LOCK();
    uint64_t next = (uint64_t)s_media.samples + add;
    if (next > s_media.sample_limit) next = s_media.sample_limit;
    s_media.samples = (uint32_t)next;
    if (s_media.samples >= s_media.sample_limit) s_media.playing = false;
    s_media.failed = false;
    UNLOCK();
}

size_t nine_media_test_pump(int16_t pcm[NINE_MEDIA_BLOCK_SAMPLES])
{
    if (!nine_media_is_playing()) return 0;
    size_t n = decode_next(pcm);
    if (!n) {
        s_media.failed = s_media.samples < s_media.sample_limit;
        s_media.playing = false;
        return 0;
    }
    s_media.samples += (uint32_t)n;
    ++s_media.block;
    if (s_media.samples == s_media.sample_limit) s_media.playing = false;
    return n;
}
#endif
