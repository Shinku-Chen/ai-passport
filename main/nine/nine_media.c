#include "nine_media.h"
#include "nine_crc.h"
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
    int64_t op_started_us;
    uint32_t op_frames, op_crc_checks, op_jpeg_decodes, op_late_skips;
    uint32_t op_decode_max_us, op_decode_total_us;
    uint32_t op_frame_render_max_us, op_frame_gap_max_ms;
    uint32_t op_audio_writes, op_audio_write_max_us, op_audio_write_total_us;
    uint32_t op_audio_gap_max_us;
    int64_t op_audio_last_start_us;
    int64_t op_last_frame_us;
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
    return ~nine_crc32_update(UINT32_MAX, p, n);
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
    return rd32(p) == 1u && rd32(p + 24) == 1 &&
           (out->sample_rate == 8000 || out->sample_rate == 12000 || out->sample_rate == 16000) &&
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
        nine_media_track_t info;
        bool ok = nine_media_track_info(track, &info);
        if (ok) ok = begin_track(track);
        if (ok) ok = bsp_audio_init() == ESP_OK &&
                     bsp_audio_set_format(info.sample_rate, 16, 1) == ESP_OK;
        if (ok) bsp_audio_set_volume(65);
        while (ok) {
            LOCK();
            bool changed = s_media.generation != generation || s_media.closing;
            UNLOCK();
            if (changed) break;
            size_t n = decode_next(pcm);
            if (!n) { ok = s_media.samples >= s_media.sample_limit; break; }
#ifndef NINE_MEDIA_HOST
            const int64_t write_start = esp_timer_get_time();
            const uint32_t gap_us = s_media.op_audio_last_start_us > 0 &&
                                    write_start > s_media.op_audio_last_start_us ?
                (uint32_t)(write_start - s_media.op_audio_last_start_us) : 0;
            ok = bsp_audio_write(pcm, n * sizeof(*pcm)) == ESP_OK;
            const int64_t write_end = esp_timer_get_time();
            const uint32_t write_us = write_end > write_start ?
                (uint32_t)(write_end - write_start) : 0;
            s_media.op_audio_last_start_us = write_start;
#else
            ok = bsp_audio_write(pcm, n * sizeof(*pcm)) == ESP_OK;
            const uint32_t write_us = 0;
            const uint32_t gap_us = 0;
#endif
            LOCK();
            s_media.samples += (uint32_t)n;
            ++s_media.block;
            ++s_media.op_audio_writes;
            s_media.op_audio_write_total_us += write_us;
            if (write_us > s_media.op_audio_write_max_us)
                s_media.op_audio_write_max_us = write_us;
            if (gap_us > s_media.op_audio_gap_max_us)
                s_media.op_audio_gap_max_us = gap_us;
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
    if (!s_done || xTaskCreate(audio_worker, "nine_audio", 4096, NULL, 6, &s_task) != pdPASS) {
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
    s_media.op_frames = s_media.op_crc_checks = s_media.op_jpeg_decodes = 0;
    s_media.op_late_skips = 0;
    s_media.op_decode_max_us = s_media.op_decode_total_us = 0;
    s_media.op_frame_render_max_us = s_media.op_frame_gap_max_ms = 0;
    s_media.op_audio_writes = 0;
    s_media.op_audio_write_max_us = s_media.op_audio_write_total_us = 0;
    s_media.op_audio_gap_max_us = 0;
    s_media.op_audio_last_start_us = 0;
    s_media.op_last_frame_us = 0;
#ifndef NINE_MEDIA_HOST
    s_media.started_us = esp_timer_get_time();
    s_media.op_started_us = s_media.started_us;
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

bool nine_media_get_op_stats(nine_media_op_stats_t *out)
{
    if (!out || !s_media.initialized) return false;
    LOCK();
    out->position_ms = s_media.rate ? (uint32_t)((uint64_t)s_media.samples * 1000 / s_media.rate) : 0;
#ifndef NINE_MEDIA_HOST
    {
        const int64_t wall = esp_timer_get_time() - s_media.op_started_us;
        out->wall_ms = wall > 0 ? (uint32_t)(wall / 1000) : 0;
    }
#else
    out->wall_ms = out->position_ms;
#endif
    out->duration_ms = s_media.duration;
    out->frames = s_media.op_frames;
    out->crc_checks = s_media.op_crc_checks;
    out->jpeg_decodes = s_media.op_jpeg_decodes;
    out->late_skips = s_media.op_late_skips;
    out->decode_max_us = s_media.op_decode_max_us;
    out->decode_total_us = s_media.op_decode_total_us;
    out->frame_render_max_us = s_media.op_frame_render_max_us;
    out->frame_gap_max_ms = s_media.op_frame_gap_max_ms;
    out->audio_writes = s_media.op_audio_writes;
    out->audio_write_max_us = s_media.op_audio_write_max_us;
    out->audio_write_total_us = s_media.op_audio_write_total_us;
    out->audio_gap_max_us = s_media.op_audio_gap_max_us;
    UNLOCK();
    return true;
}

void nine_media_tick(void)
{
    LOCK(); bool opening = s_media.opening; UNLOCK();
    if (!opening) return;
    uint32_t pos = nine_media_position_ms(), count = nine_image_op_count();
    uint32_t image = NINE_NONE;
    const uint32_t frame_before = s_media.frame;
    while (s_media.frame < count) {
        uint32_t id, timestamp, duration;
        if (!nine_image_op_frame(s_media.frame, &id, &timestamp, &duration)) break;
        if (timestamp > pos) break;
        image = id;
        ++s_media.frame;
    }
    const uint32_t advanced = s_media.frame - frame_before;
    if (advanced > 1) {
        LOCK();
        s_media.op_late_skips += advanced - 1;
        UNLOCK();
    }
    if (image != NINE_NONE && image != s_media.last_image) {
#ifndef NINE_MEDIA_HOST
        const int64_t decode_start = esp_timer_get_time();
#else
        const int64_t decode_start = 0;
        (void)decode_start;
#endif
        nine_image_op_result_t ir;
        const bool ok = nine_image_show_asset_op_ex(image, &ir);
#ifndef NINE_MEDIA_HOST
        const int64_t decode_end = esp_timer_get_time();
        const uint32_t decode_us = decode_end > decode_start ?
            (uint32_t)(decode_end - decode_start) : 0;
        const uint32_t render_us = decode_us;
#else
        const uint32_t decode_us = 0;
        const uint32_t render_us = 0;
#endif
        LOCK();
        s_media.op_crc_checks += ir.crc_checks;
        s_media.op_jpeg_decodes += ir.jpeg_decodes;
        if (ok) ++s_media.op_frames;
        if (decode_us > s_media.op_decode_max_us) s_media.op_decode_max_us = decode_us;
        s_media.op_decode_total_us += decode_us;
        if (render_us > s_media.op_frame_render_max_us) s_media.op_frame_render_max_us = render_us;
        if (ok) {
#ifndef NINE_MEDIA_HOST
            const int64_t now = esp_timer_get_time();
            if (s_media.op_last_frame_us > 0) {
                const int64_t gap = now - s_media.op_last_frame_us;
                const uint32_t gap_ms = gap > 0 ? (uint32_t)(gap / 1000) : 0;
                if (gap_ms > s_media.op_frame_gap_max_ms)
                    s_media.op_frame_gap_max_ms = gap_ms;
            }
            s_media.op_last_frame_us = now;
#endif
        }
        if (!ok) s_media.failed = true;
        UNLOCK();
        if (ok) s_media.last_image = image;
        else {
            // Stop audio immediately, retaining failed=true. The app renders
            // the visible error and must never finish normally after a fault.
            nine_media_stop();
        }
    }
    if (!nine_media_is_playing()) { LOCK(); s_media.opening = false; UNLOCK(); }
}
#ifdef NINE_MEDIA_HOST
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
