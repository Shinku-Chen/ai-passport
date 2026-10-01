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
// 按下后等 App 的 turn_ready(录音/识别真正就绪)的兜底上限:到点仍未收到也切绿,
// 不让用户一直对着红屏等(名称按 UI 行为取,见 oc_ui.h 的两个轮次状态)。
#define OC_UI_TURN_READY_TIMEOUT_MS 800U   // App 就绪(turn_ready)兜底:真机反馈 2500ms 变绿太慢,收到 ack 仍是即时变绿

// 设置页菜单项
enum {
    OC_SET_MENU_REPAIR = 0,
    OC_SET_MENU_INFO,
    OC_SET_MENU_BRIGHT,
    OC_SET_MENU_BACK,
    OC_SET_MENU_COUNT,
};
// 设置页页面
typedef enum {
    OC_SET_PAGE_MENU = 0,
    OC_SET_PAGE_INFO,
    OC_SET_PAGE_BRIGHT,
    OC_SET_PAGE_REPAIR,
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
    bool turn_pressed;         // OK 已按下(可能还没真正开始一轮)
    bool turn_ready_pending;   // 本轮已开始,等 App 的 turn_ready 或兜底超时
    bool playing;              // 正在播放手机推来的 TTS 音频(turn_ready 是上行,这个是下行)
    bool settings_active;
    oc_set_page_t set_page;
    int set_index;

    /** 手机端在 hello 里上报的 App 版本号(设备信息页展示;未上报时为空串)。 */
    char app_version[16];

    /** 本次连接是否已经提示过版本不一致(避免每次重连都刷一条)。 */
    bool version_notice_shown;
    uint32_t battery_poll_ms;
    int64_t turn_ready_deadline_us;   // 等待就绪的绝对截止时刻(esp_timer 单调时钟)
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
    // caps 里的 tts_opus 是下行音频的开关:没它手机必须把回复留在屏幕上(见协议文档)。
    // 解码器没就绪(静态区不够/初始化失败)就不报这个能力,免得手机白推音频。
    const char *downlink = oc_audio_play_available() ? ",\"tts_opus\"" : "";
    snprintf(json, sizeof(json),
             "{\"ev\":\"hello\",\"proto\":%u,\"caps\":[\"opus\",\"pcm\",\"text\",\"time\",\"status\"%s],"
             "\"model\":\"%s\",\"fw\":\"%s\",\"minApp\":\"%s\"}",
             (unsigned)OC_PROTO_VERSION, downlink, oc_link_device_name(), OC_APP_VERSION, OC_APP_VERSION);
    send_event_json(json);
    ESP_LOGI(TAG, "已发 hello(proto=%u, 下行=%s)", (unsigned)OC_PROTO_VERSION, downlink[0] ? "tts_opus" : "无");
}

// ---- 一轮对讲 ----
// 按下 OK 的第一件事就是整屏红:给用户即时反馈,不等链路就绪/音频启动。
// 之后真正能说话时(turn_ready 或兜底超时)才切绿,见 turn_mark_ready。
static void turn_start(void)
{
    if (s_app.turn_pressed) {
        return;
    }
    s_app.turn_pressed = true;

    // 打断(barge-in):设备正在出声时按下 OK 就是“我要说话”。先把喇叭停掉并等采集
    // 真正交还 codec(flush 是同步的),否则第一个字会被自己的喇叭录进去。
    if (s_app.playing || !oc_audio_play_idle()) {
        oc_audio_play_flush();
        s_app.playing = false;   // aborted 事件由应用任务的下一拍补发
    }

    oc_ui_set_state(OC_UI_STATE_RECORDING, NULL);
    oc_ui_note_activity();

    if (!oc_link_ready()) {
        // 链路未就绪:保持红底不闪回,用系统气泡说明原因;松开时由 turn_release 收尾。
        oc_ui_append('R', "设备未连接,请先在 App 里连接");
        oc_ui_note_activity();
        return;
    }
    if (oc_audio_begin_turn() != ESP_OK) {
        oc_ui_append('R', "音频启动失败");
        return;   // 同上:红底保持
    }
    s_app.turn_active = true;
    s_app.turn_ready_deadline_us =
        esp_timer_get_time() + (int64_t)OC_UI_TURN_READY_TIMEOUT_MS * 1000;
    s_app.turn_ready_pending = true;
    send_event_json("{\"ev\":\"turn_start\"}");
    ESP_LOGI(TAG, "本轮开始:等待 App 就绪(turn_ready 或 %ums 兜底)", (unsigned)OC_UI_TURN_READY_TIMEOUT_MS);
}

// 收到 App 的 turn_ready(录音/识别真正就绪)或兜底超时:切绿,明确告诉用户可以说话。
static void turn_mark_ready(void)
{
    if (!s_app.turn_active || !s_app.turn_ready_pending) {
        return;
    }
    s_app.turn_ready_pending = false;
    s_app.turn_ready_deadline_us = 0;
    oc_ui_set_state(OC_UI_STATE_RECORDING_READY, NULL);
    oc_ui_note_activity();
    ESP_LOGI(TAG, "本轮就绪:可以说话了");
}

static void turn_end(void);

// 松开 OK:结束本轮(正常收尾)或收掉没能开始的红底。
static void turn_release(void)
{
    s_app.turn_pressed = false;
    if (s_app.playing) {
        // 按下的瞬间没走到 turn_start(例如设置页里按键被吃掉)时,松开也算打断。
        oc_audio_play_flush();
        s_app.playing = false;
    }
    if (s_app.turn_active) {
        turn_end();
        return;
    }
    // 按下但没能进入本轮(链路未就绪 / 音频启动失败):把红底收掉,回到链路对应的状态。
    // 只在红底还挂着时收尾;期间若已被别的状态覆盖(断开、收到文本),不要抢回去。
    if (oc_ui_get_state() == OC_UI_STATE_RECORDING) {
        oc_ui_set_state(oc_link_ready() ? OC_UI_STATE_READY
                                        : (oc_link_connected() ? OC_UI_STATE_CONNECTING
                                                               : OC_UI_STATE_IDLE),
                        NULL);
    }
}

static void turn_end(void)
{
    if (!s_app.turn_active) {
        return;
    }
    s_app.turn_active = false;
    s_app.turn_ready_pending = false;   // 已经松开,不必再等 turn_ready
    s_app.turn_ready_deadline_us = 0;
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
        const char *items[OC_SET_MENU_COUNT] = { "重新配对", "设备信息", "亮度", "返回" };
        int off = 0;
        for (int i = 0; i < OC_SET_MENU_COUNT; i++) {
            off += snprintf(body + off, sizeof(body) - (size_t)off, "%s%s\n", i == s_app.set_index ? "> " : "  ",
                            items[i]);
        }
        oc_ui_settings_update(body);
        break;
    }
    case OC_SET_PAGE_INFO: {
        char body[384];
        snprintf(body, sizeof(body),
                 "设备名: %s\n固件: %s\n协议: v%u\n手机 App: %s\n链路: %s\n电量: %s\n音量: %u%%\n麦克风: %udB\n"
                 "发帧丢弃: %u",
                 oc_link_device_name(), OC_APP_VERSION, (unsigned)OC_PROTO_VERSION,
                 s_app.app_version[0] != '\0' ? s_app.app_version : "未上报",
                 oc_link_ready() ? "就绪" : (oc_link_connected() ? "加密中" : "未连接"),
                 s_app.battery >= 0 ? "见状态栏" : "不可用",
                 oc_settings_volume(), oc_settings_mic_gain_db(), (unsigned)oc_link_tx_drop_count());
        oc_ui_settings_update(body);
        break;
    }
    case OC_SET_PAGE_REPAIR: {
        // 设备侧的配对信息已清掉:剩下要用户做的是在手机上取消配对,然后重新扫描连接。
        oc_ui_settings_update(
            "已清除本机配对信息。\n\n"
            "接下来在手机上：\n"
            "1) 设置 → 蓝牙 → 找到本设备 → 取消配对\n"
            "2) 回到 App 设备页点「扫描并连接」\n"
            "3) 按小屏新显示的 6 位码完成配对");
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
    oc_ui_settings_open("设置", "  设备信息\n  亮度\n  返回", "UP/DOWN 选择，OK 确认，长按 OK 返回");
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
            if (s_app.set_index == OC_SET_MENU_REPAIR) {
                // 重新配对:设备侧清掉 bond 并断开,手机侧需自行取消配对(见页面提示)
                oc_link_forget_peer();
                s_app.set_page = OC_SET_PAGE_REPAIR;
                oc_ui_settings_open("重新配对", "", "OK / 长按 OK 返回");
                settings_render();
                return true;
            }
            if (s_app.set_index == OC_SET_MENU_INFO) {
                s_app.set_page = OC_SET_PAGE_INFO;
                oc_ui_settings_open("设备信息", "", "OK 返回，长按 OK 退出设置");
            } else if (s_app.set_index == OC_SET_MENU_BRIGHT) {
                s_app.set_page = OC_SET_PAGE_BRIGHT;
                oc_ui_settings_open("亮度", "", "UP/DOWN 调节，OK 确认，长按 OK 退出设置");
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
            oc_ui_settings_open("设置", "", "UP/DOWN 选择，OK 确认，长按 OK 返回");
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
            oc_ui_settings_open("设置", "", "UP/DOWN 选择，OK 确认，长按 OK 返回");
        }
    } else if (s_app.set_page == OC_SET_PAGE_REPAIR) {
        if (btn == BSP_BTN_OK) {
            s_app.set_page = OC_SET_PAGE_MENU;
            oc_ui_settings_open("设置", "", "UP/DOWN 选择，OK 确认，长按 OK 返回");
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
    if (btn == BSP_BTN_OK && ev == BSP_BTN_PRESS) {
        turn_start();   // 按下瞬间就红(不再等长按 500ms)
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_RELEASE) {
        turn_release();
        return;
    }
    // OK 的长按不再触发开始(PRESS 已先触发,否则 500ms 后会重复开始)。
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

// ---- TTS 下行播放 ----
// 手机侧的 TTS 音频经 BLE 推下来,设备解码后从喇叭放出来。规范见
// docs/development/engineering/intercom-wire-protocol.md 的 "TTS audio downlink";
// 解码/播放/半双工由 oc_audio 的播放任务负责,应用任务只做三件事:
//   * 把 tts_start/tts_stop/tts_abort 转成 oc_audio_play_*() 的状态迁移;
//   * 把 TTS_OPUS 帧交给 oc_audio_play_push_frame();
//   * 把播放结果组帧上报(tts_playback_done / tts_playback_aborted)。
// 状态词沿用现有的“接收中”,不新增居中大字(用户明确要求)。
static void handle_tts_start(void)
{
    if (s_app.turn_active || s_app.turn_pressed) {
        // 两边不能同时跑:用户还按着 OK 时来的 tts_start 直接忽略,手机应先结束本轮。
        ESP_LOGW(TAG, "本轮录音未结束,忽略 tts_start");
        return;
    }
    if (s_app.playing) {
        ESP_LOGW(TAG, "已在播放 TTS,忽略重复的 tts_start");
        return;
    }
    oc_audio_play_start();
    s_app.playing = true;
    oc_ui_set_state(OC_UI_STATE_RECEIVING, NULL);
    oc_ui_note_activity();
    ESP_LOGI(TAG, "手机开始推 TTS 音频");
}

static void handle_tts_stop(void)
{
    // 幂等:没在播就是空操作(手机重复发 tts_stop 也不会出错)。
    oc_audio_play_stop();
    if (!s_app.playing) {
        ESP_LOGD(TAG, "没有在播的 TTS,忽略 tts_stop");
        return;
    }
    ESP_LOGI(TAG, "TTS 音频结束,等队列播空");
}

static void handle_tts_abort(void)
{
    if (!s_app.playing && oc_audio_play_idle()) {
        return;
    }
    // flush 是同步的:返回时 I2S 已停、codec 已挂起、上行采集已恢复。
    oc_audio_play_flush();
    s_app.playing = false;   // aborted 事件由应用任务的下一拍补发
    ESP_LOGW(TAG, "手机要求中止 TTS 播放");
}

// 播放任务把结果放在一个粘性事件里,由应用任务(唯一组帧者)发出:
// 组帧缓冲 s_frame_ctrl 归应用任务所有,不能在播放任务里直接用。
static void report_playback_event(oc_audio_play_ev_t ev)
{
    oc_audio_play_stats_t st = { 0 };
    oc_audio_play_get_stats(&st);
    if (ev == OC_AUDIO_PLAY_EV_DONE) {
        char json[192];
        snprintf(json, sizeof(json),
                 "{\"ev\":\"tts_playback_done\",\"frames\":%u,\"decoded\":%u,\"dropped\":%u,"
                 "\"underruns\":%u,\"decode_us_max\":%u}",
                 (unsigned)st.frames, (unsigned)st.decoded, (unsigned)st.dropped,
                 (unsigned)st.underruns, (unsigned)st.decode_us_max);
        send_event_json(json);
        ESP_LOGI(TAG, "TTS 播完: 收=%u 解=%u 丢=%u 欠载=%u 解码最长=%uus", (unsigned)st.frames,
                 (unsigned)st.decoded, (unsigned)st.dropped, (unsigned)st.underruns,
                 (unsigned)st.decode_us_max);
    } else {
        // 规范里这个事件不带计数;数字留在设备日志里(串口能看到)。
        send_event_json("{\"ev\":\"tts_playback_aborted\"}");
        ESP_LOGW(TAG, "TTS 播放中止: 收=%u 解=%u 丢=%u 欠载=%u", (unsigned)st.frames,
                 (unsigned)st.decoded, (unsigned)st.dropped, (unsigned)st.underruns);
    }
    s_app.playing = false;
    // 播完回到常规状态。但用户此刻可能正按着 OK(打断):那就不要抢掉红/绿屏。
    if (!s_app.turn_pressed && !s_app.turn_active) {
        oc_ui_set_state(oc_link_ready() ? OC_UI_STATE_READY
                                        : (oc_link_connected() ? OC_UI_STATE_CONNECTING
                                                               : OC_UI_STATE_IDLE),
                        NULL);
    }
    oc_ui_note_activity();
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
    // App → 设备的新事件 turn_ready:手机侧录音/识别已真正开始,设备可以切绿了。
    // 手机用 CONTROL 帧承载该事件(payload 与设备侧 EVENT 同形),所以事件名在 ev 字段;
    // 少数实现可能写成 {"cmd":"turn_ready"},一并接受。
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(root, "ev");
    if ((cJSON_IsString(ev) && strcmp(ev->valuestring, "turn_ready") == 0) ||
        (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "turn_ready") == 0)) {
        ESP_LOGI(TAG, "手机上报 turn_ready");
        turn_mark_ready();
        cJSON_Delete(root);
        return;
    }
    // 下行 TTS 的三个事件:与 turn_ready 同形(手机→设备都是 CONTROL + ev 字段)。
    if (cJSON_IsString(ev)) {
        const char *name = ev->valuestring;
        if (strcmp(name, "tts_start") == 0) {
            handle_tts_start();
            cJSON_Delete(root);
            return;
        }
        if (strcmp(name, "tts_stop") == 0) {
            handle_tts_stop();
            cJSON_Delete(root);
            return;
        }
        if (strcmp(name, "tts_abort") == 0) {
            handle_tts_abort();
            cJSON_Delete(root);
            return;
        }
    }
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd->valuestring, "hello") == 0) {
        const cJSON *proto = cJSON_GetObjectItemCaseSensitive(root, "proto");
        s_app.app_hello = true;
        // 手机端可选上报 App 版本号(设备信息页展示),并检查固件/App 是否配套:
        // 两者按同一版本号发布,不一致只提示、不阻断(旧 App 仍能对话,但会看到提示)。
        const cJSON *app = cJSON_GetObjectItemCaseSensitive(root, "app");
        if (cJSON_IsString(app) && app->valuestring != NULL) {
            snprintf(s_app.app_version, sizeof(s_app.app_version), "%s", app->valuestring);
            if (strcmp(app->valuestring, OC_APP_VERSION) != 0 && !s_app.version_notice_shown) {
                s_app.version_notice_shown = true;
                char msg[112];
                snprintf(msg, sizeof(msg), "版本提示：手机 App %s 与本机固件 %s 不一致，请更新 APP",
                         app->valuestring, OC_APP_VERSION);
                ESP_LOGW(TAG, "%s", msg);
                oc_ui_append('A', msg);
            }
        }
        if (cJSON_IsNumber(proto) && (unsigned)proto->valueint < OC_PROTO_VERSION &&
            !s_app.version_notice_shown) {
            s_app.version_notice_shown = true;
            char msg[112];
            snprintf(msg, sizeof(msg), "版本提示：手机端协议 v%d 低于本机 v%u，请更新 APP",
                     proto->valueint, (unsigned)OC_PROTO_VERSION);
            ESP_LOGW(TAG, "%s", msg);
            oc_ui_append('A', msg);
        }
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
            if (s_app.merge.role == 'A' && !s_app.playing) {
                // 正在放 TTS 时保持“接收中”:播完由 report_playback_event 回到常规状态。
                oc_ui_set_state(OC_UI_STATE_READY, NULL);
            }
            oc_ui_note_activity();
        }
    } else if (type == OC_FRAME_CONTROL) {
        handle_control(payload, len);
    } else if (type == OC_FRAME_EVENT) {
        // 手机 → 设备的事件帧:协议里“手机下发”走 CONTROL,但 App 侧实现用 EVENT 帧型
        // (payload 形状与 CONTROL 相同,都是 JSON)。两种帧型都接受,避免 turn_ready 被静默丢弃。
        handle_control(payload, len);
    } else if (type == OC_FRAME_TTS_OPUS) {
        // 手机 → 设备的下行 TTS:载荷 [SEQ][rate_khz][frame_ms] + Opus 包。
        // 长度边界在这里先挡一道(4..515),具体校验/解码队列在 oc_audio 里。
        if (len < OC_TTS_OPUS_HEADER + 1u || len > OC_TTS_OPUS_FRAME_MAX) {
            ESP_LOGW(TAG, "TTS_OPUS 载荷长度非法: %u", (unsigned)len);
        } else if (oc_audio_play_push_frame(payload, len) != ESP_OK) {
            // 没在播放态/参数非法:一行 DEBUG 就够(手机上会看到没有 tts_playback_done)
            ESP_LOGD(TAG, "TTS_OPUS 未接受(播放态=%d)", (int)s_app.playing);
        }
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
        s_app.version_notice_shown = false;   // 新一次连接:版本提示重新计一次
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
        // 加密完成 = 配对已经成功:此刻就把配对码面板收起。
        // 之前只在“订阅完成”时隐藏,而订阅要等手机走完 MTU 协商,
        // 中间任何一步慢一步或失败,面板就会一直挡在屏幕中间。
        oc_ui_hide_pairing();
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
        if (s_app.playing) {
            // 断连也要立刻把喇叭停下、把采集还给用户;事件发不出去就只记日志。
            oc_audio_play_flush();
            (void)oc_audio_play_take_event();
            s_app.playing = false;
            ESP_LOGW(TAG, "链路断开:中止 TTS 播放");
        }
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

        // 下行播放的结果(播完/中止):在这里组帧上报,保证 s_frame_ctrl 只被应用任务写。
        oc_audio_play_ev_t pev = oc_audio_play_take_event();
        if (pev != OC_AUDIO_PLAY_EV_NONE) {
            report_playback_event(pev);
        }
        // 播放期间保持背光:用户要看得见正在朗读的那段回答。
        if (s_app.playing) {
            oc_ui_note_activity();
        }

        // 兜底:App 的 turn_ready 迟迟不来也要切绿,不让用户一直对着红屏。
        // 用单调时钟比较绝对截止时刻,不受队列繁忙时循环快慢的影响。
        if (s_app.turn_ready_pending && esp_timer_get_time() >= s_app.turn_ready_deadline_us) {
            turn_mark_ready();
        }

        // 心跳:用来判断应用任务是否还在正常消费事件(队列满/界面卡住时靠它定位)。
        s_app.heartbeat_ms += OC_APP_TICK_MS;
        if (s_app.heartbeat_ms >= 5000U) {
            s_app.heartbeat_ms = 0;
            ESP_LOGI(TAG, "心跳 队列空闲=%u 堆=%u 说话=%d 设置页=%d",
                     (unsigned)uxQueueSpacesAvailable(s_app.queue),
                     (unsigned)esp_get_free_heap_size(), (int)s_app.turn_active, (int)s_app.settings_active);
            // LVGL 池水位:下行音频的静态内存是从这个池子让出来的,真机上靠这条日志确认余量。
            oc_ui_log_memory();
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
