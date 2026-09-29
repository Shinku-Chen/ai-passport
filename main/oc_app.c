// main/oc_app.c —— 对讲机应用编排(见 oc_app.h)。
//
// 设计要点:
//   * 单任务串行:按键、BLE 事件、帧重组、上屏、设置变更全在一个任务里,不需要为它们
//     之间加锁;只有跨任务共享的环形缓冲(oc_link)与 LVGL(oc_ui)各自内部加锁。
//   * 就绪判定来自协议:连接 + 加密 + 订阅三件事都成立后才发 hello 并允许发音频帧。
//   * 丢帧如实上报:音频帧发不出去只计数,不重传;turn_end 里带 dropped,手机侧可见。
#include "oc_app.h"

#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "oc_audio.h"
#include "oc_link.h"
#include "oc_proto.h"
#include "oc_settings.h"
#include "oc_ui.h"

static const char *TAG = "oc_app";

#define OC_APP_TASK_STACK 8192U
#define OC_APP_QUEUE_DEPTH 32U
#define OC_APP_TICK_MS 50U
#define OC_APP_BATTERY_POLL_MS 10000U

// 设置页菜单项
enum {
    OC_SET_MENU_INFO = 0,
    OC_SET_MENU_BRIGHT,
    OC_SET_MENU_BACK,
    OC_SET_MENU_COUNT,
};
// 设置页页面
typedef enum {
    OC_SET_PAGE_MENU = 0,
    OC_SET_PAGE_INFO,
    OC_SET_PAGE_BRIGHT,
} oc_set_page_t;

typedef enum {
    OC_MSG_LINK = 0,
    OC_MSG_BUTTON,
} oc_msg_type_t;

typedef struct {
    oc_msg_type_t type;
    oc_link_event_t link;
    struct {
        bsp_btn_t btn;
        bsp_btn_ev_t ev;
    } button;
} oc_msg_t;

static struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    // 应用任务栈也用静态栈:堆在 NimBLE/LVGL/DMA 之后只剩十几 KB 连续块,
    // 关键任务不应该因为堆碎片建不出来(实测就是这样整个应用静止的)。
    StaticTask_t task_tcb;
    StackType_t task_stack[OC_APP_TASK_STACK / sizeof(StackType_t)];
    oc_reassembler_t rx;
    oc_text_merge_t merge;
    bool running;
    bool link_secure;
    bool link_subscribed;
    bool app_hello;
    bool turn_active;
    bool settings_active;
    oc_set_page_t set_page;
    int set_index;
    uint32_t battery_poll_ms;
    uint32_t heartbeat_ms;
    int battery;
    uint32_t tx_drop_seen;
    char last_gw_reason[96];   // 网关原因系统消息去重
} s_app;

// ---- 发送 ----
// 两个静态组帧缓冲,按调用者分工,避免跨任务竞争:
//   * 音频帧只在音频任务里组(on_audio_frame),缓冲小(≤ 1+512+6);
//   * 文本/事件只在应用任务里组(send_event_json/handle_control),缓冲要装下
//     最长 TEXT 帧(6+2048)。
// 两者同时只有一个任务在写自己那一份,因此不需要互斥量。
static uint8_t s_frame_audio[OC_HEADER_SIZE + 1U + OC_AUDIO_OPUS_MAX];
static uint8_t s_frame_ctrl[OC_FRAME_MAX];

static bool send_frame_buf(uint8_t *buf, size_t cap, uint8_t type, uint8_t flags, const uint8_t *payload,
                           size_t len)
{
    size_t n = oc_encode(buf, cap, type, flags, payload, len);
    if (n == 0) {
        ESP_LOGW(TAG, "组帧失败 type=%u len=%u", type, (unsigned)len);
        return false;
    }
    return oc_link_send(buf, n) == ESP_OK;
}

static bool send_frame(uint8_t type, uint8_t flags, const uint8_t *payload, size_t len)
{
    return send_frame_buf(s_frame_ctrl, sizeof(s_frame_ctrl), type, flags, payload, len);
}

static void send_event_json(const char *json)
{
    if (json == NULL) {
        return;
    }
    send_frame(OC_FRAME_EVENT, 0, (const uint8_t *)json, strlen(json));
}

// 音频帧回调(音频任务上下文):payload = [SEQ][Opus 包]。
static bool on_audio_frame(uint8_t seq, const uint8_t *opus, size_t len, void *ctx)
{
    (void)ctx;
    uint8_t payload[1 + OC_AUDIO_OPUS_MAX];
    if (len > OC_AUDIO_OPUS_MAX) {
        return false;
    }
    payload[0] = seq;
    memcpy(payload + 1, opus, len);
    return send_frame_buf(s_frame_audio, sizeof(s_frame_audio), OC_FRAME_AUDIO_OPUS, OC_FLAG_MORE, payload,
                          len + 1);
}

static void send_status(void)
{
    oc_audio_stats_t st = {0};
    oc_audio_get_stats(&st);
    char json[256];
    snprintf(json, sizeof(json),
             "{\"ev\":\"status\",\"battery\":%d,\"volume\":%u,\"mic_gain_db\":%u,"
             "\"link\":\"%s\",\"fw\":\"%s\",\"tx_drop\":%u}",
             s_app.battery, oc_settings_volume(), oc_settings_mic_gain_db(),
             oc_link_ready() ? "ready" : (oc_link_connected() ? "connecting" : "offline"),
             OC_APP_VERSION, (unsigned)oc_link_tx_drop_count());
    send_event_json(json);
}

static void send_hello(void)
{
    char json[192];
    snprintf(json, sizeof(json),
             "{\"ev\":\"hello\",\"proto\":%u,\"caps\":[\"opus\",\"pcm\",\"text\",\"time\",\"status\"],"
             "\"model\":\"%s\",\"fw\":\"%s\"}",
             (unsigned)OC_PROTO_VERSION, oc_link_device_name(), OC_APP_VERSION);
    send_event_json(json);
    ESP_LOGI(TAG, "已发 hello(proto=%u)", (unsigned)OC_PROTO_VERSION);
}

// ---- 一轮对讲 ----
static void turn_start(void)
{
    if (s_app.turn_active) {
        return;
    }
    if (!oc_link_ready()) {
        oc_ui_append('R', "设备未连接,请先在 App 里连接");
        oc_ui_note_activity();
        return;
    }
    if (oc_audio_begin_turn() != ESP_OK) {
        oc_ui_append('R', "音频启动失败");
        return;
    }
    s_app.turn_active = true;
    send_event_json("{\"ev\":\"turn_start\"}");
    oc_ui_set_state(OC_UI_STATE_RECORDING, NULL);
    oc_ui_note_activity();
}

static void turn_end(void)
{
    if (!s_app.turn_active) {
        return;
    }
    s_app.turn_active = false;
    oc_audio_end_turn();   // 等尾巴编完(codec 也在这里挂起)

    oc_audio_stats_t st = {0};
    oc_audio_get_stats(&st);
    char json[192];
    snprintf(json, sizeof(json),
             "{\"ev\":\"turn_end\",\"frames\":%u,\"codec\":\"opus\",\"dropped\":%u,\"overrun\":%u,"
             "\"encode_us_max\":%u}",
             (unsigned)st.frames_encoded, (unsigned)st.frames_dropped, (unsigned)st.pcm_overruns,
             (unsigned)st.encode_us_max);
    send_event_json(json);
    ESP_LOGI(TAG, "本轮: 帧=%u 丢=%u 溢出=%u 编码最长=%uus 平均=%uus 包均值=%uB", (unsigned)st.frames_encoded,
             (unsigned)st.frames_dropped, (unsigned)st.pcm_overruns, (unsigned)st.encode_us_max,
             (unsigned)st.encode_us_avg,
             st.frames_encoded ? (unsigned)(st.opus_bytes / st.frames_encoded) : 0u);

    oc_ui_set_state(OC_UI_STATE_SENDING, NULL);
}

// ---- 设置页 ----
static void settings_render(void)
{
    if (!s_app.settings_active) {
        return;
    }
    switch (s_app.set_page) {
    case OC_SET_PAGE_MENU: {
        char body[256];
        const char *items[OC_SET_MENU_COUNT] = { "设备信息", "亮度", "返回" };
        int off = 0;
        for (int i = 0; i < OC_SET_MENU_COUNT; i++) {
            off += snprintf(body + off, sizeof(body) - (size_t)off, "%s%s\n", i == s_app.set_index ? "> " : "  ",
                            items[i]);
        }
        oc_ui_settings_update(body);
        break;
    }
    case OC_SET_PAGE_INFO: {
        char body[320];
        snprintf(body, sizeof(body),
                 "设备名: %s\n固件: %s\n链路: %s\n电量: %s\n音量: %u%%\n麦克风: %udB\n"
                 "发帧丢弃: %u",
                 oc_link_device_name(), OC_APP_VERSION,
                 oc_link_ready() ? "就绪" : (oc_link_connected() ? "加密中" : "未连接"),
                 s_app.battery >= 0 ? "见状态栏" : "不可用",
                 oc_settings_volume(), oc_settings_mic_gain_db(), (unsigned)oc_link_tx_drop_count());
        oc_ui_settings_update(body);
        break;
    }
    case OC_SET_PAGE_BRIGHT: {
        char body[96];
        uint8_t b = oc_settings_brightness();
        int bars = b / 10;
        char bar[12];
        for (int i = 0; i < 10; i++) {
            bar[i] = i < bars ? '#' : '-';
        }
        bar[10] = '\0';
        snprintf(body, sizeof(body), "亮度 %u%%\n[%s]\n\nUP/DOWN 调节,OK 确认", b, bar);
        oc_ui_settings_update(body);
        break;
    }
    default:
        break;
    }
}

static void settings_open(void)
{
    s_app.settings_active = true;
    s_app.set_page = OC_SET_PAGE_MENU;
    s_app.set_index = 0;
    oc_ui_settings_open("设置", "  设备信息\n  亮度\n  返回", "UP/DOWN 选择 · OK 确认 · 长按 OK 返回");
    settings_render();
}

static void settings_close(void)
{
    s_app.settings_active = false;
    oc_ui_settings_close();
}

// 返回 true 表示这次按键已在设置页内处理掉。
static bool settings_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_app.settings_active) {
        return false;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        settings_close();
        return true;
    }
    if (ev != BSP_BTN_CLICK) {
        return true;   // 设置页内不响应 PRESS/RELEASE/DOUBLE
    }
    if (s_app.set_page == OC_SET_PAGE_MENU) {
        if (btn == BSP_BTN_UP) {
            s_app.set_index = s_app.set_index > 0 ? s_app.set_index - 1 : OC_SET_MENU_COUNT - 1;
        } else if (btn == BSP_BTN_DOWN) {
            s_app.set_index = (s_app.set_index + 1) % OC_SET_MENU_COUNT;
        } else if (btn == BSP_BTN_OK) {
            if (s_app.set_index == OC_SET_MENU_INFO) {
                s_app.set_page = OC_SET_PAGE_INFO;
                oc_ui_settings_open("设备信息", "", "OK 返回 · 长按 OK 退出设置");
            } else if (s_app.set_index == OC_SET_MENU_BRIGHT) {
                s_app.set_page = OC_SET_PAGE_BRIGHT;
                oc_ui_settings_open("亮度", "", "UP/DOWN 调节 · OK 确认 · 长按 OK 退出设置");
            } else {
                settings_close();
                return true;
            }
        }
    } else if (s_app.set_page == OC_SET_PAGE_BRIGHT) {
        uint8_t b = oc_settings_brightness();
        if (btn == BSP_BTN_UP) {
            b = b + 10 <= 100 ? b + 10 : 100;
        } else if (btn == BSP_BTN_DOWN) {
            b = b >= 20 ? b - 10 : 10;
        } else if (btn == BSP_BTN_OK) {
            s_app.set_page = OC_SET_PAGE_MENU;
            oc_ui_settings_open("设置", "", "UP/DOWN 选择 · OK 确认 · 长按 OK 返回");
            settings_render();
            return true;
        }
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            oc_settings_set_brightness(b);
            oc_ui_apply_brightness();
            oc_ui_note_activity();
        }
    } else if (s_app.set_page == OC_SET_PAGE_INFO) {
        if (btn == BSP_BTN_OK) {
            s_app.set_page = OC_SET_PAGE_MENU;
            oc_ui_settings_open("设置", "", "UP/DOWN 选择 · OK 确认 · 长按 OK 返回");
        }
    }
    oc_ui_note_activity();
    settings_render();
    return true;
}

// ---- 按键 ----
static void handle_button(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    oc_ui_note_activity();   // 任意按键都点亮背光(背光超时后的第一次按键也会走到这)

    if (settings_handle_key(btn, ev)) {
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        turn_start();
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_RELEASE) {
        turn_end();
        return;
    }
    // 短按 OK 只用来把屏幕点亮(背光已在函数开头恢复),不再打开设置页:
    // 设置页改成长按 UP 打开,避免想看一眼屏幕/误碰时误入菜单。
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        return;
    }
    if (btn == BSP_BTN_UP && ev == BSP_BTN_LONG) {
        settings_open();
        return;
    }
    if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        oc_ui_scroll(btn == BSP_BTN_UP ? 1 : -1);
    }
}

// ---- 手机下发的帧 ----
static void handle_control(const uint8_t *payload, size_t len)
{
    char json[OC_JSON_PAYLOAD_MAX + 1];
    size_t n = len < OC_JSON_PAYLOAD_MAX ? len : OC_JSON_PAYLOAD_MAX;
    memcpy(json, payload, n);
    json[n] = '\0';

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "CONTROL 解析失败: %s", json);
        return;
    }
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd->valuestring, "hello") == 0) {
        const cJSON *proto = cJSON_GetObjectItemCaseSensitive(root, "proto");
        s_app.app_hello = true;
        ESP_LOGI(TAG, "手机 hello(proto=%d)", cJSON_IsNumber(proto) ? proto->valueint : -1);
        send_status();   // 握手完成即回报一次状态
    } else if (strcmp(cmd->valuestring, "status") == 0) {
        send_status();
    } else if (strcmp(cmd->valuestring, "audio") == 0) {
        const cJSON *vol = cJSON_GetObjectItemCaseSensitive(root, "volume");
        const cJSON *mic = cJSON_GetObjectItemCaseSensitive(root, "mic_gain_db");
        if (cJSON_IsNumber(vol)) {
            uint8_t v = (uint8_t)(vol->valueint < 0 ? 0 : (vol->valueint > 100 ? 100 : vol->valueint));
            oc_settings_set_volume(v);
            bsp_audio_set_volume(v);
            ESP_LOGI(TAG, "音量设为 %u%%", v);
        }
        if (cJSON_IsNumber(mic)) {
            uint8_t m = (uint8_t)(mic->valueint < 0 ? 0 : (mic->valueint > 32 ? 32 : mic->valueint));
            oc_settings_set_mic_gain_db(m);
            oc_audio_set_mic_gain_db(m);
            ESP_LOGI(TAG, "麦克风增益设为 %udB", m);
        }
        send_status();   // 回显,App 面板据此确认生效
    } else if (strcmp(cmd->valuestring, "time") == 0) {
        const cJSON *epoch = cJSON_GetObjectItemCaseSensitive(root, "epoch");
        if (cJSON_IsNumber(epoch)) {
            oc_ui_set_time((int64_t)epoch->valuedouble);
        }
    } else if (strcmp(cmd->valuestring, "clear_display") == 0) {
        oc_ui_clear_conversation();
    } else if (strcmp(cmd->valuestring, "gateway") == 0) {
        // 手机上报网关状态。设备自己连不上网关,左上角把「设备」与「网关」分开显示,
        // 两者都就绪才显示单个「就绪」;这里的 detail 是网关异常时的可读原因。
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
        const cJSON *detail = cJSON_GetObjectItemCaseSensitive(root, "detail");
        const char *st = cJSON_IsString(state) ? state->valuestring : "";
        const char *dt = (cJSON_IsString(detail) && detail->valuestring[0] != '\0') ? detail->valuestring : NULL;
        if (strcmp(st, "ready") == 0) {
            oc_ui_set_gateway_state(OC_UI_GATEWAY_READY, NULL);
        } else if (strcmp(st, "connecting") == 0) {
            oc_ui_set_gateway_state(OC_UI_GATEWAY_CONNECTING, dt);
        } else if (strcmp(st, "working") == 0) {
            // 已连上、agent 正在跑(带阶段):与 App 状态卡显示同一组词(工作中)。
            oc_ui_set_gateway_state(OC_UI_GATEWAY_WORKING, dt);
        } else {
            oc_ui_set_gateway_state(OC_UI_GATEWAY_OFFLINE, dt);
            // 底部提示行已去掉:网关不可用的原因作为系统消息上屏,同一条只加一次,
            // 避免网络抖动时把对话区刷满。
            if (dt != NULL && strcmp(dt, s_app.last_gw_reason) != 0) {
                snprintf(s_app.last_gw_reason, sizeof(s_app.last_gw_reason), "%s", dt);
                oc_ui_append('R', dt);
            }
        }
        ESP_LOGI(TAG, "网关状态: %s%s%s", st, dt ? " / " : "", dt ? dt : "");
    } else if (strcmp(cmd->valuestring, "turn_start") == 0) {
        const cJSON *codec = cJSON_GetObjectItemCaseSensitive(root, "codec");
        ESP_LOGI(TAG, "手机接受本轮(codec=%s)", cJSON_IsString(codec) ? codec->valuestring : "?");
    } else {
        ESP_LOGD(TAG, "未知控制命令: %s", cmd->valuestring);   // 新版手机对旧设备下发,忽略即可
    }
    cJSON_Delete(root);
}

static void on_frame(uint8_t type, uint8_t flags, const uint8_t *payload, size_t len, void *ctx)
{
    (void)ctx;
    if (type == OC_FRAME_TEXT) {
        if (oc_text_merge_push(&s_app.merge, flags, payload, len)) {
            oc_ui_set_state(OC_UI_STATE_RECEIVING, NULL);
            oc_ui_append(s_app.merge.role, s_app.merge.text);
            if (s_app.merge.overflow) {
                ESP_LOGW(TAG, "本条文本超出显示上限,已截断");
            }
            if (s_app.merge.role == 'A') {
                oc_ui_set_state(OC_UI_STATE_READY, NULL);
            }
            oc_ui_note_activity();
        }
    } else if (type == OC_FRAME_CONTROL) {
        handle_control(payload, len);
    } else if (type == OC_FRAME_AUDIO_PCM || type == OC_FRAME_AUDIO_OPUS) {
        ESP_LOGD(TAG, "忽略上行帧的本地回环 type=%u", type);
    }
}

static void drain_rx(void)
{
    uint8_t buf[256];
    for (;;) {
        size_t n = oc_link_read(buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        oc_reassembler_push(&s_app.rx, buf, n);
    }
}

// ---- 链路事件 ----
static void on_link_event(const oc_link_event_t *ev, void *ctx)
{
    (void)ctx;
    if (s_app.queue == NULL) {
        return;
    }
    oc_msg_t msg = {0};
    msg.type = OC_MSG_LINK;
    msg.link = *ev;
    if (xQueueSend(s_app.queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "事件队列满,丢弃事件 %d", (int)ev->type);
    }
}

static void handle_link_event(const oc_link_event_t *ev)
{
    switch (ev->type) {
    case OC_LINK_EV_CONNECTED:
        s_app.link_secure = false;
        s_app.link_subscribed = false;
        s_app.app_hello = false;
        ESP_LOGI(TAG, "事件: 已连接");
        oc_ui_set_state(OC_UI_STATE_CONNECTING, NULL);
        break;
    case OC_LINK_EV_PASSKEY:
        ESP_LOGI(TAG, "事件: 配对码 %06u", (unsigned)ev->data.passkey.passkey);
        oc_ui_set_state(OC_UI_STATE_PAIRING, NULL);
        oc_ui_show_pairing(ev->data.passkey.passkey);
        break;
    case OC_LINK_EV_SECURED:
        s_app.link_secure = oc_link_ready() || oc_link_connected();
        ESP_LOGI(TAG, "事件: 已加密");
        oc_ui_set_state(OC_UI_STATE_CONNECTING, NULL);
        break;
    case OC_LINK_EV_SUBSCRIBED:
        s_app.link_subscribed = oc_link_ready();
        ESP_LOGI(TAG, "事件: 订阅=%d", (int)s_app.link_subscribed);
        oc_ui_hide_pairing();
        if (s_app.link_subscribed) {
            oc_ui_set_state(OC_UI_STATE_READY, NULL);
            send_hello();
        }
        break;
    case OC_LINK_EV_DISCONNECTED:
        s_app.link_secure = false;
        s_app.link_subscribed = false;
        s_app.app_hello = false;
        ESP_LOGI(TAG, "事件: 断开 reason=%d", ev->data.disconnected.reason);
        if (s_app.turn_active) {
            turn_end();   // 断开时把当前轮收尾,避免手机侧永远等 turn_end
        }
        oc_ui_hide_pairing();
        oc_ui_set_gateway_state(OC_UI_GATEWAY_UNKNOWN, NULL);   // 链路断了,手机不会再上报网关状态
        oc_ui_set_state(OC_UI_STATE_IDLE, NULL);
        oc_reassembler_reset(&s_app.rx);   // 半截帧作废
        oc_link_flush_rx();
        break;
    case OC_LINK_EV_RX:
        drain_rx();
        break;
    case OC_LINK_EV_TX_DROP:
        if ((oc_link_tx_drop_count() / 20U) > s_app.tx_drop_seen) {
            s_app.tx_drop_seen = oc_link_tx_drop_count() / 20U;
            ESP_LOGW(TAG, "发送丢帧累计 %u", (unsigned)oc_link_tx_drop_count());
            oc_ui_append('R', "链路拥塞,部分内容可能丢失");
        }
        break;
    default:
        break;
    }
}

// ---- 主任务 ----
static void poll_battery(void)
{
    int soc = bsp_battery_soc();
    if (soc != s_app.battery) {
        s_app.battery = soc;
        oc_ui_set_battery(soc);
    }
}

static void app_task(void *arg)
{
    (void)arg;
    for (;;) {
        oc_msg_t msg;
        if (xQueueReceive(s_app.queue, &msg, pdMS_TO_TICKS(OC_APP_TICK_MS)) == pdTRUE) {
            if (msg.type == OC_MSG_LINK) {
                if (msg.link.type == OC_LINK_EV_RX) {
                    drain_rx();   // 兜底:即使事件被丢也能把字节消化掉
                } else {
                    handle_link_event(&msg.link);
                }
            } else if (msg.type == OC_MSG_BUTTON) {
                handle_button(msg.button.btn, msg.button.ev);
            }
        }
        drain_rx();   // 每拍兜底一次,保证不积压

        // 心跳:用来判断应用任务是否还在正常消费事件(队列满/界面卡住时靠它定位)。
        s_app.heartbeat_ms += OC_APP_TICK_MS;
        if (s_app.heartbeat_ms >= 5000U) {
            s_app.heartbeat_ms = 0;
            ESP_LOGI(TAG, "心跳 队列空闲=%u 堆=%u 说话=%d 设置页=%d",
                     (unsigned)uxQueueSpacesAvailable(s_app.queue),
                     (unsigned)esp_get_free_heap_size(), (int)s_app.turn_active, (int)s_app.settings_active);
        }

        s_app.battery_poll_ms += OC_APP_TICK_MS;
        if (s_app.battery_poll_ms >= OC_APP_BATTERY_POLL_MS) {
            s_app.battery_poll_ms = 0;
            poll_battery();
        }
        if (!s_app.running) {
            break;
        }
    }
    vTaskDelete(NULL);
}

esp_err_t oc_app_start(void)
{
    memset(&s_app, 0, sizeof(s_app));
    s_app.battery = -1;
    s_app.running = true;

    if (oc_settings_load() != ESP_OK) {
        ESP_LOGW(TAG, "设置读取异常,使用默认值继续");
    }
    oc_text_merge_init(&s_app.merge);
    oc_reassembler_init(&s_app.rx, on_frame, NULL);

    // 堆预算在这块板子上很紧(无 PSRAM):NimBLE 约 60KB、音频任务 24KB、LVGL 绘制缓冲与
    // DMA 缓冲都要从内部堆拿。每步都打印剩余堆与最大连续块,分配失败时能直接看出卡在哪。
    ESP_LOGI(TAG, "启动堆: 空闲=%u 最大块=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

    s_app.queue = xQueueCreate(OC_APP_QUEUE_DEPTH, sizeof(oc_msg_t));
    if (s_app.queue == NULL) {
        ESP_LOGE(TAG, "事件队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    // 先把小而关键的任务建出来(静态栈,不占堆),再建音频任务。
    s_app.task = xTaskCreateStatic(app_task, "oc_app", OC_APP_TASK_STACK, NULL, 5, s_app.task_stack,
                                   &s_app.task_tcb);
    if (s_app.task == NULL) {
        ESP_LOGE(TAG, "应用任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    // 先把音量/麦克风增益套用到硬件:设备重启后不依赖 App 再下发一次。
    bsp_audio_set_volume(oc_settings_volume());
    bsp_audio_set_mic_gain((float)oc_settings_mic_gain_db());

    // 音频要早初始化:Opus 编码器状态是一块几十 KB 的连续分配,必须在 NimBLE 拿走
    // ~70KB 之前拿到;放到后面就只剩十几 KB 的连续块,opus_encoder_create 直接
    // 返回 OPUS_ALLOC_FAIL(-7)。采集任务本身是静态栈,早启动也没关系(没开始说话时空转)。
    esp_err_t e = oc_audio_init(on_audio_frame, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s(空闲=%u 最大块=%u)", esp_err_to_name(e),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    } else if ((e = oc_audio_start()) != ESP_OK) {
        ESP_LOGE(TAG, "音频任务启动失败: %s", esp_err_to_name(e));
    }
    ESP_LOGI(TAG, "音频后堆: 空闲=%u 最大块=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

    e = oc_link_init(on_link_event, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "链路初始化失败: %s", esp_err_to_name(e));
        return e;
    }
    e = oc_link_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "链路启动失败: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "链路后堆: 空闲=%u 最大块=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

    e = oc_ui_init(oc_link_device_name());
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "界面初始化失败: %s", esp_err_to_name(e));
        return e;
    }
    oc_ui_set_state(OC_UI_STATE_IDLE, NULL);
    ESP_LOGI(TAG, "界面后堆: 空闲=%u 最大块=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败,状态栏不显示电量");
    }
    poll_battery();

    ESP_LOGI(TAG, "对讲机应用启动(设备 %s,固件 %s) 空闲堆=%u", oc_link_device_name(), OC_APP_VERSION,
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

void oc_app_key(int btn, int ev)
{
    if (s_app.queue == NULL) {
        return;
    }
    oc_msg_t msg = {0};
    msg.type = OC_MSG_BUTTON;
    msg.button.btn = (bsp_btn_t)btn;
    msg.button.ev = (bsp_btn_ev_t)ev;
    (void)xQueueSend(s_app.queue, &msg, 0);
}
