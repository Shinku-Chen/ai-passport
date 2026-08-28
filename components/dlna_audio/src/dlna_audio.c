// components/dlna_audio/src/dlna_audio.c —— 流式音频播放管线实现。
// 一个播放任务持有全部逻辑:HTTP 拉流 → 格式探测 → 解码(MP3=minimp3, AAC=esp_aac)
// → bsp_audio_write。控制命令经队列投递,任务内依次处理。
//
// 适配 ESP32-C3 无 PSRAM 单核:所有缓冲都小(16-32KB),解码与 I2S 串行不重叠。
#include "dlna_audio.h"

#include "bsp_audio.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// minimp3 —— 单头文件 MP3 解码器。
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

// AAC 解码(尽力项):来自 esp_audio_codec 预编译库。
#include "esp_audio_dec_reg.h"
#include "decoder/impl/esp_aac_dec.h"

static const char *TAG = "dlna_audio";

#define PCM_BUF_SAMPLES 8192            // 解码输出缓冲(16bit 单声道采样数)
#define HTTP_BUF_SIZE   16384           // HTTP 读块
#define CMD_QUEUE_LEN   8

typedef enum {
    CMD_PLAY = 0,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP,
    CMD_SEEK,
} cmd_type_t;

typedef struct {
    cmd_type_t     type;
    int            arg;
    char          *uri;        // CMD_PLAY 时指向 strdup 的 URI
} cmd_t;

// 播放器上下文。
typedef struct {
    dlna_audio_state_t state;
    dlna_audio_cb_t    cb;
    void              *cb_user;
    TaskHandle_t       task;
    QueueHandle_t      q;
    volatile bool      running;
    int  volume;
    bool muted;
    char *cur_uri;
    volatile uint32_t pos_ms;
    volatile uint32_t dur_ms;
} audio_ctx_t;

static audio_ctx_t s_ctx;

static void set_state(dlna_audio_state_t st)
{
    if (s_ctx.state != st) {
        s_ctx.state = st;
        if (s_ctx.cb) s_ctx.cb(st, s_ctx.cur_uri, s_ctx.cb_user);
    }
}

// 从 Content-Type 决定 0=MP3 / 1=AAC / 其余-1=未知(默认按 MP3)。
static int detect_from_content_type(const char *ct)
{
    if (!ct) return -1;
    if (strstr(ct, "mpeg") || strstr(ct, "mp3")) return 0;
    if (strstr(ct, "aac") || strstr(ct, "mp4") || strstr(ct, "adts")) return 1;
    return -1;
}

static esp_err_t http_open(esp_http_client_handle_t *out, const char *uri)
{
    esp_http_client_config_t cfg = {
        .url = uri,
        .timeout_ms = 10000,
        .method = HTTP_METHOD_GET,
        .buffer_size = HTTP_BUF_SIZE,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(cli);
        return err;
    }
    *out = cli;
    return ESP_OK;
}

// —— MP3 解码(用 minimp3)。 ——
static void play_mp3(esp_http_client_handle_t cli)
{
    static uint8_t in[HTTP_BUF_SIZE];
    static short pcm[PCM_BUF_SAMPLES];
    mp3dec_t dec;
    mp3dec_init(&dec);
    bool format_ready = false;

    while (s_ctx.running && !esp_http_client_is_complete_data_received(cli)) {
        int n = esp_http_client_read(cli, (char *)in, sizeof(in));
        if (n <= 0) { if (n < 0) ESP_LOGW(TAG, "http read err: %d", n); break; }

        mp3dec_frame_info_t info;
        // minimp3:解码返回 16bit 采样数,PCM 缓冲固定;info 里只有 hz/channels。
        int samples = mp3dec_decode_frame(&dec, in, n, pcm, &info);
        if (samples > 0) {
            int ch = info.channels ? info.channels : 1;
            if (!format_ready && info.hz) {
                bsp_audio_set_format((uint32_t)info.hz, 16, (uint8_t)ch);
                format_ready = true;
            }
            bsp_audio_write(pcm, (size_t)samples * 2 * ch);
            if (info.hz) {
                s_ctx.pos_ms = (uint32_t)(((int64_t)samples) * 1000 / info.hz);
                if (s_ctx.pos_ms > s_ctx.dur_ms) s_ctx.dur_ms = s_ctx.pos_ms;
            }
        }
        while (s_ctx.state == DLNA_AUDIO_PAUSED && s_ctx.running) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// —— AAC 解码(尽力项,用 esp_aac_dec)。 ——
static void play_aac(esp_http_client_handle_t cli)
{
    static uint8_t in[HTTP_BUF_SIZE];
    static uint8_t pcmbuf[PCM_BUF_SAMPLES * 2 * 2];
    void *dec = NULL;

    // ADTS 流默认参数(open 传 NULL 即可自动解析 ADTS 头)。
    esp_audio_err_t derr = esp_aac_dec_open(NULL, 0, &dec);
    if (derr != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "AAC 解码器打开失败: %d", (int)derr);
        set_state(DLNA_AUDIO_ERROR);
        return;
    }
    ESP_LOGI(TAG, "AAC 解码器已打开");

    while (s_ctx.running && !esp_http_client_is_complete_data_received(cli)) {
        int n = esp_http_client_read(cli, (char *)in, sizeof(in));
        if (n <= 0) { if (n < 0) ESP_LOGW(TAG, "aac http read err: %d", n); break; }

        esp_audio_dec_in_raw_t raw = { .buffer = in, .len = (uint32_t)n, .consumed = 0 };
        while (raw.len > 0) {
            esp_audio_dec_out_frame_t frame = { .buffer = pcmbuf,
                                                .len = sizeof(pcmbuf),
                                                .decoded_size = 0 };
            esp_audio_dec_info_t info = {0};
            esp_audio_err_t ret = esp_aac_dec_decode(dec, &raw, &frame, &info);
            if (ret != ESP_AUDIO_ERR_OK) {
                // 缓冲不足则继续;其余解码错误跳过剩余。
                ESP_LOGW(TAG, "aac decode: %d, consumed=%u remain=%u", ret, raw.consumed, raw.len);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) { raw.consumed = raw.len; continue; }
                break;
            }
            if (frame.decoded_size) {
                if (info.sample_rate && info.channel) {
                    bsp_audio_set_format(info.sample_rate, info.bits_per_sample ? info.bits_per_sample : 16,
                                         info.channel);
                }
                bsp_audio_write(frame.buffer, frame.decoded_size);
                if (info.sample_rate) {
                    s_ctx.pos_ms = (uint32_t)(frame.decoded_size * 1000 /
                                              (uint32_t)(info.sample_rate * info.channel * 2));
                }
            }
            if (raw.consumed == 0) break;  // 防死循环
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }
        while (s_ctx.state == DLNA_AUDIO_PAUSED && s_ctx.running) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    esp_aac_dec_close(dec);
}

// —— 主播放任务。 ——
static void player_task(void *arg)
{
    (void)arg;
    cmd_t cmd;
    while (s_ctx.running) {
        if (xQueueReceive(s_ctx.q, &cmd, portMAX_DELAY) != pdPASS) continue;
        if (!s_ctx.running) break;

        switch (cmd.type) {
        case CMD_STOP:
            if (s_ctx.cur_uri) { free(s_ctx.cur_uri); s_ctx.cur_uri = NULL; }
            s_ctx.pos_ms = 0; s_ctx.dur_ms = 0;
            continue;
        case CMD_PAUSE:  set_state(DLNA_AUDIO_PAUSED);  continue;
        case CMD_RESUME: set_state(DLNA_AUDIO_PLAYING); continue;
        case CMD_SEEK:   ESP_LOGW(TAG, "seek %d 秒(HTTP 流尽力)", cmd.arg); continue;
        case CMD_PLAY:   break;
        default:         continue;
        }

        if (!cmd.uri) continue;
        if (s_ctx.cur_uri) free(s_ctx.cur_uri);
        s_ctx.cur_uri = strdup(cmd.uri);
        s_ctx.pos_ms = 0; s_ctx.dur_ms = 0;
        set_state(DLNA_AUDIO_OPENING);

        esp_http_client_handle_t cli = NULL;
        esp_err_t err = http_open(&cli, cmd.uri);
        free(cmd.uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP 拉流失败: %s", esp_err_to_name(err));
            set_state(DLNA_AUDIO_ERROR);
            continue;
        }

        // 获取响应头以判定格式:先 fetch_headers,再取 Content-Type。
        esp_http_client_fetch_headers(cli);
        int fmt = -1;
        char *ct = NULL;
        if (esp_http_client_get_header(cli, "Content-Type", &ct) == ESP_OK && ct) {
            fmt = detect_from_content_type(ct);
        }

        set_state(DLNA_AUDIO_PLAYING);
        if (fmt == 1) play_aac(cli);
        else         play_mp3(cli);  // 未知默认按 MP3 试

        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        if (s_ctx.running) { s_ctx.pos_ms = 0; set_state(DLNA_AUDIO_IDLE); }
    }
    while (xQueueReceive(s_ctx.q, &cmd, 0) == pdPASS) {
        if (cmd.type == CMD_PLAY && cmd.uri) free(cmd.uri);
    }
    if (s_ctx.cur_uri) { free(s_ctx.cur_uri); s_ctx.cur_uri = NULL; }
}

esp_err_t dlna_audio_init(dlna_audio_cb_t cb, void *user)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cb = cb; s_ctx.cb_user = user;
    s_ctx.volume = 80; s_ctx.running = true;
    bsp_audio_set_volume((uint8_t)s_ctx.volume);

    s_ctx.q = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_t));
    if (!s_ctx.q) return ESP_ERR_NO_MEM;

    if (xTaskCreate(player_task, "dlna_audio", 6144, NULL, 5, &s_ctx.task) != pdPASS) {
        vQueueDelete(s_ctx.q); s_ctx.q = NULL;
        return ESP_ERR_NO_MEM;
    }
    set_state(DLNA_AUDIO_IDLE);
    return ESP_OK;
}

static esp_err_t post_cmd(cmd_t *cmd)
{
    if (!s_ctx.q) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_ctx.q, cmd, pdMS_TO_TICKS(1000)) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t dlna_audio_play_uri(const char *uri)
{
    if (!uri) return ESP_ERR_INVALID_ARG;
    cmd_t cmd = { .type = CMD_PLAY, .uri = strdup(uri), .arg = 0 };
    if (!cmd.uri) return ESP_ERR_NO_MEM;
    return post_cmd(&cmd);
}
esp_err_t dlna_audio_pause(void)  { cmd_t c = { .type = CMD_PAUSE };  return post_cmd(&c); }
esp_err_t dlna_audio_resume(void) { cmd_t c = { .type = CMD_RESUME }; return post_cmd(&c); }
esp_err_t dlna_audio_seek(int s)  { cmd_t c = { .type = CMD_SEEK, .arg = s }; return post_cmd(&c); }
esp_err_t dlna_audio_stop(void)   { cmd_t c = { .type = CMD_STOP };   return post_cmd(&c); }

dlna_audio_state_t dlna_audio_get_state(void)  { return s_ctx.state; }
uint32_t dlna_audio_get_position_ms(void)      { return s_ctx.pos_ms; }
uint32_t dlna_audio_get_duration_ms(void)      { return s_ctx.dur_ms; }
int dlna_audio_get_volume(void)                { return s_ctx.muted ? 0 : s_ctx.volume; }
bool dlna_audio_is_muted(void)                 { return s_ctx.muted; }

void dlna_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_ctx.volume = percent;
    bsp_audio_set_volume((uint8_t)percent);
}

void dlna_audio_set_mute(bool mute)
{
    s_ctx.muted = mute;
    bsp_audio_set_volume(mute ? 0 : (uint8_t)s_ctx.volume);
}

void dlna_audio_deinit(void)
{
    s_ctx.running = false;
    if (s_ctx.q) { cmd_t c = { .type = CMD_STOP }; xQueueSend(s_ctx.q, &c, 0); }
    if (s_ctx.task) vTaskDelay(pdMS_TO_TICKS(100));
}
