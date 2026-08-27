// main/dlna_player.c —— DLNA 播放管道编排(ESP-IDF 侧)。
// 依赖:
//   · components/music_decoder  —— MP3 解码(music_decoder.h)
//   · components/bsp            —— bsp_audio_*(I2S 输出)
//   · main/dlna_pipeline.c       —— 纯逻辑环形缓冲
//
// 两个任务,靠环形缓冲解耦(见架构评审):
//   http_pull      prio 8, 栈 4KB —— esp_http_client 流式读,只在 ring 有空时才读
//   audio_pipeline prio 12, 栈 6KB —— ring 取压缩字节 → 解码一帧 → bsp_audio_write(阻塞)
//
// 注意:bsp_audio_set_format 是阻塞的(open/close codec),必须在流开始前、解码循环外调用一次,
// 用解码首帧得到的真实采样率。解码循环里绝不要再调它。
#include "dlna_player.h"
#include "dlna_pipeline.h"
#include "music_decoder.h"
#include "bsp_audio.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "dlna_player";

// 环形缓冲容量:默认 16KB,建档时若 heap 紧张可降(见降级策略)。
#define RING_BYTES          (16 * 1024)
// HTTP client 读缓冲(每次最多从 socket 读这么多)。
#define HTTP_PULL_CHUNK     (2048)
// 解码一帧的压缩缓冲通常 < 1040 字节,给足余量。
#define DECODE_FRAME_MAX    (2080)

// 播放状态与互斥保护(UI 任务与 http_pull/audio_pipeline 并发访问)。
static dlna_player_state_t s_state = DLNA_PLAYER_IDLE;
static char s_title[128];
static int  s_volume = 70;
static SemaphoreHandle_t s_state_mtx;

// 环形缓冲存储(预分配,见内存铁律)。
static uint8_t *s_ring_mem;
static dlna_ring_t s_ring;

// 解码器与 PCM 输出缓冲(预分配)。
static audio_decoder_t *s_decoder;
static int16_t *s_pcm_buf;

// 两个任务句柄。
static TaskHandle_t s_pull_task;
static TaskHandle_t s_audio_task;

// 当前正在进入的 URL(http_pull 启动后读取)。
static char s_url[256];

// 解码参数确认:在解出首帧后设置,避免每帧 set_format。
static int s_sample_rate;
static int s_channels;
static int s_format_ready;

// 暂停标志(读侧会绕过)。
static volatile bool s_paused;
// 停止标志:置 1 时管道任务退出当前流。play 会清 0。
static volatile bool s_stopped;
// 当前是否有活动流(任务据此决定是否等待新任务)。
static volatile bool s_active;

// 解码耗时统计(评审"第一道门"benchmark,真机观察用)。
static uint64_t s_decode_us;
static int64_t  s_decode_peak_us;
static uint32_t s_decode_samples;
static uint32_t s_frames;

// ---- 状态与音量(线程安全) ----
static void state_set(dlna_player_state_t st)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_state_mtx);
}

dlna_player_state_t dlna_player_get_state(void)
{
    dlna_player_state_t st;
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    st = s_state;
    xSemaphoreGive(s_state_mtx);
    return st;
}

int dlna_player_get_title(char *out, size_t max)
{
    if (!out || max == 0) return 0;
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    size_t n = strlen(s_title);
    if (n >= max) n = max - 1;
    memcpy(out, s_title, n);
    out[n] = '\0';
    xSemaphoreGive(s_state_mtx);
    return (int)n;
}

void dlna_player_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_volume = percent;
    xSemaphoreGive(s_state_mtx);
    bsp_audio_set_volume((uint8_t)percent);
}

int dlna_player_get_volume(void)
{
    int v;
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    v = s_volume;
    xSemaphoreGive(s_state_mtx);
    return v;
}

// ---- http_pull 任务:从 URL 拉压缩流写入环形缓冲 ----
// 只在 ring 有空时才读,天然实现背压(见评审)。
static esp_err_t http_event_handler(esp_http_client_event_t *ev)
{
    // 数据到达由 esp_http_client_perform 的分块事件驱动;我们不用回调灌缓冲,
    // 采用"循环 perform 转发"更直观。这里只处理字符集等非关键。
    (void)ev;
    return ESP_OK;
}

static void http_pull_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t cfg = {
        .url = s_url,
        .timeout_ms = 8000,
        .buffer_size = HTTP_PULL_CHUNK,
        .event_handler = http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init 失败");
        state_set(DLNA_PLAYER_ERROR);
        s_pull_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open 失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        state_set(DLNA_PLAYER_ERROR);
        s_pull_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "收到流: content-length=%d, state=%s",
             content_length, s_url);

    // 读直至断流或暂停。
    uint8_t tmp[HTTP_PULL_CHUNK];
    size_t free_before = 0;
    while (!s_paused) {
        free_before = dlna_ring_free(&s_ring);
        if (free_before < HTTP_PULL_CHUNK) {
            // 缓冲满,等待解码侧释放(其它任务在消费)。简单忙等即可。
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int n = esp_http_client_read(client, (char *)tmp, (int)(free_before > sizeof(tmp) ? sizeof(tmp) : free_before));
        if (n < 0) {
            ESP_LOGE(TAG, "http read 出错: %s", esp_err_to_name(n));
            break;
        }
        if (n == 0) break;   // 流结束
        size_t w = dlna_ring_write(&s_ring, tmp, (size_t)n);
        if (w == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    // 流结束或暂停:清句柄自删,便于下次 play 重新创建。
    ESP_LOGI(TAG, "http_pull 流结束,句柄清空");
    s_pull_task = NULL;
    vTaskDelete(NULL);
}

// ---- audio_pipeline 任务:取压缩字节 → 解码 → 写 I2S ----
static void audio_pipeline_task(void *arg)
{
    (void)arg;
    uint8_t frame[DECODE_FRAME_MAX];

    state_set(DLNA_PLAYER_PLAYING);

    while (1) {
        if (s_paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        // 用 peek 先窥探而不消费:decode 成功才读走对应字节,失败/数据不足保留在 ring,
        // 避免把"半个帧"读走后永久丢失造成流错位。
        size_t used = dlna_ring_used(&s_ring);
        if (used < 4) {   // MP3 帧头至少 4 字节
            if (dlna_player_get_state() == DLNA_PLAYER_STOPPED) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 窥探连续可读的一段。注意:peek 只能拿到不绕回的连续段;绕回时先处理前段。
        size_t n = dlna_ring_peek(&s_ring, frame, sizeof(frame));
        if (n < 4) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

        audio_decoder_frame_info_t fi = { 0 };
        int64_t t0 = esp_timer_get_time();   // 解码耗时探针(评审"第一道门")
        int consumed = audio_decoder_decode_frame(s_decoder, frame, (int)n, s_pcm_buf, &fi);
        int64_t decode_us = esp_timer_get_time() - t0;

        if (consumed > 0) {
            // 解码算力 benchmark:44.1kHz 一帧播放预算 ≈ 26.1ms(1152/44100)。
            // 只在帧统计里累计平均/峰值,供真机观察;不阻塞播放。
            s_decode_samples += fi.frames_decoded;
            s_decode_us     += decode_us;
            if (decode_us > s_decode_peak_us) s_decode_peak_us = decode_us;
            s_frames++;
            // 每 100 帧打印一次平均/峰值,判断单核是否顶得住该码率。
            if (s_frames % 100 == 0) {
                ESP_LOGI(TAG, "[bench] 平均 %.1f ms/帧, 峰值 %.1f ms, %d frames",
                         s_frames ? (double)s_decode_us / s_frames / 1000.0 : 0.0,
                         s_decode_peak_us / 1000.0, (int)s_frames);
            }

            // 解码成功,消费这 consumed 字节(它一定 <= n)。若 consumed < n,
            // 剩余部分留着作为下一帧的起始(通常是帧间填充/ID3 尾),不要丢弃。
            dlna_ring_read(&s_ring, NULL, (size_t)consumed);

            // 首帧拿到采样率/声道后,只调一次 bsp_audio_set_format(阻塞,不能放循环)。
            if (!s_format_ready) {
                if (fi.sample_rate > 0 && fi.channels > 0) {
                    s_sample_rate = fi.sample_rate;
                    s_channels    = fi.channels;
                    if (bsp_audio_set_format((uint32_t)s_sample_rate, 16, (uint8_t)s_channels) != ESP_OK) {
                        ESP_LOGE(TAG, "set_format 失败");
                        state_set(DLNA_PLAYER_ERROR);
                        break;
                    }
                    bsp_audio_set_volume((uint8_t)dlna_player_get_volume());
                    s_format_ready = 1;
                }
            }
            // 写出解码后的 PCM(阻塞写,I2S DMA 满时让出 CPU)。
            int total_bytes = fi.frames_decoded * s_channels * 2;
            if (total_bytes > 0) {
                bsp_audio_write(s_pcm_buf, (size_t)total_bytes);
            }
        } else if (consumed == 0) {
            // 数据不足一帧:保留在 ring,等 http_pull 写更多。注意 ring 会因此涨到满
            // 并背压 http_pull(符合设计)。
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            // 坏帧 / 非法帧头:peek 数据解不出,丢掉 1 字节重同步。
            ESP_LOGW(TAG, "解码坏帧,跳过 1 字节");
            dlna_ring_read(&s_ring, NULL, 1);
        }
    }

    // 退出:清空 ring、重置解码器、关闭 codec 音频输出留待下次。
    dlna_ring_clear(&s_ring);
    audio_decoder_reset(s_decoder);
    s_format_ready = 0;
    state_set(DLNA_PLAYER_STOPPED);
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

// ---- 对外接口 ----
esp_err_t dlna_player_init(void)
{
    if (s_state_mtx) return ESP_OK;   // 幂等

    s_state_mtx = xSemaphoreCreateMutex();
    if (!s_state_mtx) return ESP_ERR_NO_MEM;
    // 预分配环形缓冲 + PCM 缓冲(内存铁律:启动即占,运行不 malloc)。
    s_ring_mem = (uint8_t *)heap_caps_malloc(RING_BYTES, MALLOC_CAP_8BIT);
    if (!s_ring_mem) return ESP_ERR_NO_MEM;
    dlna_ring_init(&s_ring, s_ring_mem, RING_BYTES);

    s_pcm_buf = (int16_t *)heap_caps_malloc(AUDIO_DECODER_MAX_PCM_BYTES, MALLOC_CAP_8BIT);
    if (!s_pcm_buf) return ESP_ERR_NO_MEM;

    s_decoder = audio_decoder_create();
    if (!s_decoder) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "播放管道初始化完成: ring=%dB pcm=%dB", RING_BYTES, AUDIO_DECODER_MAX_PCM_BYTES);
    return ESP_OK;
}

esp_err_t dlna_player_play(const char *url)
{
    if (!url || !*url) return ESP_ERR_INVALID_ARG;
    if (strlen(url) >= sizeof(s_url)) return ESP_ERR_INVALID_SIZE;

    // 先停止旧的(若有)。
    s_paused = false;
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    dlna_ring_clear(&s_ring);
    s_format_ready = 0;

    {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        strncpy(s_title, "Connecting...", sizeof(s_title) - 1);
        s_title[sizeof(s_title) - 1] = '\0';
        xSemaphoreGive(s_state_mtx);
    }

    state_set(DLNA_PLAYER_CONNECTING);

    if (!s_pull_task) {
        xTaskCreate(http_pull_task, "http_pull", 4096, NULL, 8, &s_pull_task);
    }
    if (!s_audio_task) {
        xTaskCreate(audio_pipeline_task, "audio_pipe", 6144, NULL, 12, &s_audio_task);
    }
    return ESP_OK;
}

esp_err_t dlna_player_pause(void)
{
    s_paused = true;
    state_set(DLNA_PLAYER_PAUSED);
    return ESP_OK;
}

esp_err_t dlna_player_resume(void)
{
    s_paused = false;
    state_set(DLNA_PLAYER_PLAYING);
    return ESP_OK;
}

void dlna_player_stop(void)
{
    s_paused = true;
    state_set(DLNA_PLAYER_STOPPED);
    dlna_ring_clear(&s_ring);
}
