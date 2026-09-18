// main/c4_link.c —— 联机会话实现(见 c4_link.h 的线程说明)。
#include "c4_link.h"

#include "bsp_ble_link.h"
#include "c4_model.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "c4_link";

// 与 BSP 约定的一段自定义 128 位 UUID:服务 / 对端写入 / 本机通知。
// 布局与 Nordic UART 一致,方便手机或通用 BLE 工具当对端调试。
#define C4_LINK_SVC_UUID \
    { 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
      0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e }
#define C4_LINK_RX_UUID \
    { 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
      0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e }
#define C4_LINK_TX_UUID \
    { 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
      0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e }

#define C4_LINK_NAME_PREFIX "C4-"
#define C4_LINK_RAW_DEPTH   8
#define C4_LINK_EVENT_DEPTH 8

// —— BSP 回调 → 输入任务的原始事件队列 ——
typedef enum {
    C4_RAW_STATE = 0,
    C4_RAW_RX,
} c4_raw_kind_t;

typedef struct {
    uint8_t kind;   // c4_raw_kind_t
    uint8_t state;  // C4_RAW_STATE:bsp_ble_link_state_t
    uint8_t len;    // C4_RAW_RX
    uint8_t data[BSP_BLE_LINK_MAX_PAYLOAD];
} c4_raw_t;

static QueueHandle_t s_raw_queue;
static c4_link_proto_t s_proto;

static bool s_started;
static bool s_transport_up;                  // 本机已判定“通道就绪”
static bool s_failed_reported;
static volatile uint8_t s_bsp_state = BSP_BLE_LINK_IDLE;

static c4_link_event_t s_events[C4_LINK_EVENT_DEPTH];
static uint8_t s_ev_head;
static uint8_t s_ev_tail;

static void push_event(uint8_t kind, uint8_t col, uint8_t ply, uint8_t first_side,
                       uint8_t game_id, uint8_t version)
{
    const uint8_t next = (uint8_t)((s_ev_head + 1) % C4_LINK_EVENT_DEPTH);
    if (next == s_ev_tail) {
        ESP_LOGW(TAG, "事件队列满,丢弃事件 kind=%u", kind);
        return;
    }
    s_events[s_ev_head] = (c4_link_event_t){
        .kind = kind, .col = col, .ply = ply,
        .first_side = first_side, .game_id = game_id, .version = version,
    };
    s_ev_head = next;
}

// —— BSP 回调(NimBLE host 任务):只入队,不做别的 ——
static void on_transport_state(bsp_ble_link_state_t state, void *user)
{
    (void)user;
    s_bsp_state = (uint8_t)state;
    if (!s_raw_queue) return;

    c4_raw_t raw;
    memset(&raw, 0, sizeof(raw));
    raw.kind = C4_RAW_STATE;
    raw.state = (uint8_t)state;
    (void)xQueueSend(s_raw_queue, &raw, 0);
}

static void on_transport_rx(const uint8_t *data, uint16_t len, void *user)
{
    (void)user;
    if (!s_raw_queue || !data || len == 0 || len > BSP_BLE_LINK_MAX_PAYLOAD) return;

    c4_raw_t raw;
    memset(&raw, 0, sizeof(raw));
    raw.kind = C4_RAW_RX;
    raw.len = (uint8_t)len;
    memcpy(raw.data, data, len);
    (void)xQueueSend(s_raw_queue, &raw, 0);
}

static void send_frame(const uint8_t *frame, uint8_t len)
{
    if (len == 0) return;
    const esp_err_t err = bsp_ble_link_send(frame, len);
    if (err != ESP_OK) {
        // 对端刚走或通道还没就绪都会走到这里;可靠层会按超时重传。
        ESP_LOGD(TAG, "发送失败: %s", esp_err_to_name(err));
    }
}

static void deliver(const c4_link_result_t *result)
{
    if (result->tx_len > 0) send_frame(result->tx, result->tx_len);

    switch (result->ev.kind) {
    case C4_LINK_EV_NONE:
        break;
    case C4_LINK_EV_PEER_READY:
        push_event(C4_LINK_EVENT_PEER_READY, 0, 0, result->ev.first_side,
                   result->ev.game_id, 0);
        break;
    case C4_LINK_EV_MOVE:
        push_event(C4_LINK_EVENT_MOVE, result->ev.col, result->ev.ply, 0, 0, 0);
        break;
    case C4_LINK_EV_REMATCH:
        push_event(C4_LINK_EVENT_REMATCH, 0, 0, 0, result->ev.game_id, 0);
        break;
    case C4_LINK_EV_LEAVE:
        push_event(C4_LINK_EVENT_LEAVE, 0, 0, 0, 0, 0);
        break;
    case C4_LINK_EV_LOST:
        ESP_LOGW(TAG, "重传耗尽,判定对端已丢");
        push_event(C4_LINK_EVENT_DOWN, 0, 0, 0, 0, 0);
        break;
    case C4_LINK_EV_BAD_VERSION:
        push_event(C4_LINK_EVENT_BAD_VERSION, 0, 0, 0, 0, result->ev.version);
        break;
    default:
        break;
    }
}

// 用 BSP 的实时状态判定通道起落。状态事件可能因为队列满而丢,
// 所以每次 pump 都直接查 bsp_ble_link_ready() 作为权威判据。
static void sync_transport_state(void)
{
    const bool ready = bsp_ble_link_ready();
    if (ready == s_transport_up) return;

    s_transport_up = ready;
    if (ready) {
        ESP_LOGI(TAG, "通道就绪(本机角色 %s)", bsp_ble_link_is_central() ? "central" : "peripheral");
        push_event(C4_LINK_EVENT_UP, 0, 0, 0, 0, 0);
    } else {
        // 一次连接结束:序号从 0 重新开始,两边都在断开后重置,配对不会错位。
        c4_link_proto_init(&s_proto);
        ESP_LOGW(TAG, "通道断开");
        push_event(C4_LINK_EVENT_DOWN, 0, 0, 0, 0, 0);
    }
}

esp_err_t c4_link_start(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;

    if (!s_raw_queue) {
        s_raw_queue = xQueueCreate(C4_LINK_RAW_DEPTH, sizeof(c4_raw_t));
        if (!s_raw_queue) return ESP_ERR_NO_MEM;
    }
    s_ev_head = s_ev_tail = 0;
    s_transport_up = false;
    s_failed_reported = false;
    c4_link_proto_init(&s_proto);

    // 设备名带上 MAC 后两字节,两台机器在扫描列表里能区分开。
    uint8_t mac[6] = { 0 };
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return ESP_FAIL;

    char name[16];
    snprintf(name, sizeof(name), "%s%02X%02X", C4_LINK_NAME_PREFIX, mac[4], mac[5]);

    const bsp_ble_link_cfg_t cfg = {
        .local_name = name,
        .name_prefix = C4_LINK_NAME_PREFIX,
        .service_uuid = C4_LINK_SVC_UUID,
        .rx_uuid = C4_LINK_RX_UUID,
        .tx_uuid = C4_LINK_TX_UUID,
    };

    const esp_err_t err = bsp_ble_link_start(&cfg, on_transport_state,
                                             on_transport_rx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE 链路启动失败: %s", esp_err_to_name(err));
        push_event(C4_LINK_EVENT_FAILED, 0, 0, 0, 0, 0);
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "联机已就绪,设备名 %s", name);
    return ESP_OK;
}

esp_err_t c4_link_stop(void)
{
    if (!s_started) return ESP_OK;

    const esp_err_t err = bsp_ble_link_stop();
    s_started = false;
    s_transport_up = false;
    s_bsp_state = BSP_BLE_LINK_IDLE;
    c4_link_proto_init(&s_proto);
    return err;
}

void c4_link_pump(uint32_t elapsed_ms)
{
    if (!s_raw_queue) return;

    // 先处理 BSP 送来的原始事件(状态事件只用于界面展示,权威判据是 ready())。
    c4_raw_t raw;
    while (xQueueReceive(s_raw_queue, &raw, 0) == pdTRUE) {
        if (raw.kind == C4_RAW_RX && s_transport_up) {
            c4_link_result_t result = c4_link_proto_recv(&s_proto, raw.data, raw.len);
            deliver(&result);
        }
    }

    // 控制器/host 起来但 sync 失败的场合:状态由回调置成 FAILED,这里上报一次。
    if (s_bsp_state == BSP_BLE_LINK_FAILED && !s_failed_reported) {
        s_failed_reported = true;
        push_event(C4_LINK_EVENT_FAILED, 0, 0, 0, 0, 0);
    }

    sync_transport_state();

    if (!s_transport_up) return;

    // 重传计时。
    if (elapsed_ms > 0) {
        c4_link_result_t result = c4_link_proto_tick(&s_proto, elapsed_ms);
        deliver(&result);
    }
}

bool c4_link_next_event(c4_link_event_t *out)
{
    if (!out || s_ev_tail == s_ev_head) return false;
    *out = s_events[s_ev_tail];
    s_ev_tail = (uint8_t)((s_ev_tail + 1) % C4_LINK_EVENT_DEPTH);
    return true;
}

c4_link_state_t c4_link_state(void)
{
    if (!s_started) return C4_LINK_STATE_OFF;
    if (s_bsp_state == BSP_BLE_LINK_FAILED) return C4_LINK_STATE_FAILED;
    if (s_transport_up) return C4_LINK_STATE_READY;
    if (s_bsp_state == BSP_BLE_LINK_CONNECTING) return C4_LINK_STATE_CONNECTING;
    return C4_LINK_STATE_SEARCHING;
}

bool c4_link_ready(void)
{
    return s_transport_up;
}

uint8_t c4_link_local_first_side(uint8_t game_id)
{
    // 主动连接的一方(central)执先手开局,之后逐局交替。game_id 相同 → 两边算出相反结果。
    const bool even = (game_id % 2u) == 0u;
    const bool central_first = !even;
    const bool first = bsp_ble_link_is_central() ? central_first : !central_first;
    return first ? C4_P1 : C4_P2;
}

// 发一个协议包并把结果交给链路层(确认帧/事件都从这里出去)。
static void send_packet(uint8_t type, const uint8_t *payload, uint8_t len)
{
    c4_link_result_t result = c4_link_proto_send(&s_proto, type, payload, len);
    deliver(&result);
}

void c4_link_send_hello(uint8_t game_id)
{
    const uint8_t payload[3] = { C4_LINK_PROTO_VERSION,
                                c4_link_local_first_side(game_id), game_id };
    send_packet(C4_LINK_TYPE_HELLO, payload, sizeof(payload));
}

void c4_link_send_move(uint8_t col, uint8_t ply)
{
    const uint8_t payload[2] = { col, ply };
    send_packet(C4_LINK_TYPE_MOVE, payload, sizeof(payload));
}

void c4_link_send_rematch(uint8_t game_id)
{
    const uint8_t payload[1] = { game_id };
    send_packet(C4_LINK_TYPE_REMATCH, payload, sizeof(payload));
}

void c4_link_send_leave(void)
{
    send_packet(C4_LINK_TYPE_LEAVE, NULL, 0);
}
