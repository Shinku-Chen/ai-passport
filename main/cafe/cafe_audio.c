#include "cafe_audio.h"

#include "bsp_audio.h"
#include "cafe_audio_codec.h"
#include "cafe_pack.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "cafe_audio";

#define OUT_RATE 8000
#define TASK_STACK 4096
#define PCM_CHUNK 512
#define MAX_BLOCK_SAMPLES 2049

static TaskHandle_t s_task;
static SemaphoreHandle_t s_wake;
static SemaphoreHandle_t s_idle;
static volatile uint32_t s_generation;
static volatile uint8_t s_request;     // 0 title, 1 opening, 0xFF none
static volatile bool s_loop;
static volatile uint32_t s_seek_ms;
static volatile bool s_failed;
static volatile bool s_busy;
static volatile bool s_stop_timeout;
static volatile uint8_t s_volume = 60;
static char s_failure[64];
// The audio task owns these buffers; keeping them out of play_track() keeps the
// 4 KB task stack bounded (the codec/JPEG task budget is explicit in docs).
static int16_t s_decoded[MAX_BLOCK_SAMPLES];
static int16_t s_pcm[PCM_CHUNK];
static void cafe_audio_task(void *arg);

static void set_failure_if_current(uint32_t generation, const char *msg)
{
    if (generation != s_generation) return;
    s_failed = true;
    if (msg) {
        strncpy(s_failure, msg, sizeof(s_failure) - 1);
        s_failure[sizeof(s_failure) - 1] = '\0';
    }
    ESP_LOGE(TAG, "%s", msg ? msg : "audio failure");
}

bool cafe_audio_init(void)
{
    if (s_task) return true;
    s_wake = xSemaphoreCreateBinary();
    s_idle = xSemaphoreCreateBinary();
    if (!s_wake || !s_idle) {
        if (s_wake) vSemaphoreDelete(s_wake);
        if (s_idle) vSemaphoreDelete(s_idle);
        s_wake = s_idle = NULL;
        return false;
    }
    if (xTaskCreate(cafe_audio_task, "cafe_audio", TASK_STACK, NULL, 4, &s_task) != pdPASS) {
        vSemaphoreDelete(s_wake);
        vSemaphoreDelete(s_idle);
        s_wake = s_idle = NULL;
        s_task = NULL;
        return false;
    }
    return true;
}

bool cafe_audio_shutdown(void)
{
    if (!s_task) return true;
    if (!cafe_audio_stop_confirmed(1200)) {
        s_stop_timeout = true;
        return false;   // never delete a task still in codec/I2S
    }
    vTaskDelete(s_task);
    s_task = NULL;
    if (s_wake) vSemaphoreDelete(s_wake);
    if (s_idle) vSemaphoreDelete(s_idle);
    s_wake = s_idle = NULL;
    return true;
}

static bool request(uint8_t track, bool loop, uint32_t seek_ms)
{
    if (!s_task || track > 1) return false;
    s_failed = false;
    s_stop_timeout = false;
    s_failure[0] = '\0';
    s_request = track;
    s_loop = loop;
    s_seek_ms = seek_ms;
    ++s_generation;
    (void)xSemaphoreGive(s_wake);
    return true;
}

bool cafe_audio_play_title(bool loop) { return request(0, loop, 0); }
bool cafe_audio_play_op(void) { return request(1, false, 0); }
bool cafe_audio_play_op_at(uint32_t position_ms) { return request(1, false, position_ms); }
bool cafe_audio_play_preview(uint8_t track_id, bool loop) { return request(track_id, loop, 0); }

void cafe_audio_stop(void)
{
    (void)cafe_audio_stop_confirmed(1200);
}

bool cafe_audio_stop_confirmed(uint32_t timeout_ms)
{
    if (!s_task) return true;
    s_stop_timeout = false;
    s_request = 0xFF;
    s_loop = false;
    ++s_generation;
    (void)xSemaphoreGive(s_wake);
    if (!s_busy) {
        /* Drain only our own stale idle token; busy is the proof. */
        (void)xSemaphoreTake(s_idle, 0);
        return true;
    }
    /* A stale binary token can satisfy the first take. Do not accept it as
       proof: take once more and require the task to clear the active flag. */
    for (int attempt = 0; attempt < 2 && s_busy; ++attempt)
        (void)xSemaphoreTake(s_idle, pdMS_TO_TICKS(timeout_ms));
    if (s_busy) {
        s_stop_timeout = true;
        return false;
    }
    return true;
}

bool cafe_audio_stop_timed_out(void) { return s_stop_timeout; }

void cafe_audio_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume = percent;
}
uint8_t cafe_audio_volume(void) { return s_volume; }
bool cafe_audio_failed(void) { return s_failed; }
const char *cafe_audio_failure(void) { return s_failure[0] ? s_failure : "audio"; }

static bool play_track_at(uint8_t id, bool loop, uint32_t generation, uint32_t seek_ms)
{
    cafe_audio_track_t track;
    if (!cafe_audio_track_get(id, &track)) {
        set_failure_if_current(generation, "audio track CRC/index failure");
        return false;
    }
    uint32_t data_len = 0;
    const uint8_t *data = cafe_audio_data(&track, &data_len);
    if (!data || data_len == 0 || track.block_align <= 4) {
        set_failure_if_current(generation, "audio data missing");
        return false;
    }
    if (bsp_audio_init() != ESP_OK ||
        bsp_audio_set_format(OUT_RATE, 16, 1) != ESP_OK) {
        set_failure_if_current(generation, "audio codec/I2S init failed");
        return false;
    }
    bsp_audio_set_volume(s_volume);
    cafe_ima_state_t ima = {0};
    uint32_t guard_samples = 0;
    if (id == 1) {
        cafe_op_t op;
        if (cafe_op_get(&op)) guard_samples = op.audio_guard_ms * track.sample_rate / 1000;
    }
    uint32_t sample_seek = (uint32_t)((uint64_t)seek_ms * track.sample_rate / 1000u);
    if (id == 1 && guard_samples && sample_seek >= guard_samples)
        return true;   // video silent tail after the source audio guard
    if (sample_seek >= track.samples) {
        if (!loop) return true;
        sample_seek = 0;
    }
    const uint32_t block_samples = (track.block_align - 4) * 2 + 1;
    uint32_t offset = 0;
    uint32_t skip = 0;
    if (sample_seek) {
        offset = (sample_seek / block_samples) * track.block_align;
        skip = sample_seek % block_samples;
    }
    uint32_t decoded_samples = sample_seek;
    for (;;) {
        if (generation != s_generation || s_request != id) return true;
        if (decoded_samples >= track.samples) {
            if (!loop) break;
            offset = 0;
            skip = 0;
            decoded_samples = 0;
            ima.predictor = 0;
            ima.step_index = 0;
        }
        if (id == 1 && guard_samples && decoded_samples >= guard_samples)
            break;
        if (offset >= data_len) break;
        const uint32_t block = track.block_align;
        const uint32_t avail = data_len - offset;
        const uint32_t nblock = avail < block ? avail : block;
        const size_t got = cafe_ima_decode_block(data + offset, nblock, &ima,
                                                 s_decoded, MAX_BLOCK_SAMPLES);
        if (got == 0) {
            set_failure_if_current(generation, "IMA block decode failure");
            return false;
        }
        offset += nblock;
        const size_t start = skip > got ? got : skip;
        skip = 0;
        size_t limit = got - start;
        if (decoded_samples + limit > track.samples)
            limit = track.samples - decoded_samples;
        if (id == 1 && guard_samples && decoded_samples + limit > guard_samples)
            limit = guard_samples - decoded_samples;
        const bool upsample = track.sample_rate < OUT_RATE;
        const size_t add = upsample ? 2 : 1;
        size_t out_n = 0;
        for (size_t i = 0; i < limit; ++i) {
            if ((i & 0xFFu) == 0 &&
                (generation != s_generation || s_request != id)) return true;
            const int16_t sample = s_decoded[start + i];
            if (out_n + add > PCM_CHUNK) {
                if (bsp_audio_write(s_pcm, out_n * sizeof(int16_t)) != ESP_OK) {
                    set_failure_if_current(generation, "audio write failed");
                    return generation != s_generation;
                }
                out_n = 0;
            }
            s_pcm[out_n++] = sample;
            if (upsample) s_pcm[out_n++] = sample;
        }
        if (out_n && bsp_audio_write(s_pcm, out_n * sizeof(int16_t)) != ESP_OK) {
            set_failure_if_current(generation, "audio write failed");
            return generation != s_generation;
        }
        decoded_samples += (uint32_t)limit;
        {
            static uint8_t applied = 0xFF;
            if (applied != s_volume) {
                bsp_audio_set_volume(s_volume);
                applied = s_volume;
            }
        }
    }
    return true;
}

/* Keep the three-argument production-loop entry used by controlled HAL probes. */
static bool __attribute__((unused)) play_track(uint8_t id, bool loop, uint32_t generation)
{
    return play_track_at(id, loop, generation, 0);
}

static void cafe_audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_wake, portMAX_DELAY) != pdTRUE) continue;
        const uint8_t id = s_request;
        if (id > 1) {
            xSemaphoreGive(s_idle);
            continue;
        }
        const uint32_t gen = s_generation;
        s_busy = true;
        (void)play_track_at(id, s_loop, gen, s_seek_ms);
        s_busy = false;
        xSemaphoreGive(s_idle);
    }
}
