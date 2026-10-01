// main/oc_link.c —— NUS BLE 外设实现(见 oc_link.h)。
//
// 结构对齐参考实现(同 IDF 5.5.3 + 同板子跑通过),但按本仓约定重写:
//   * 只在"已连接 + 已加密 + 已订阅"时允许发送,其余情况如实返回错误;
//   * 发送走 ring + 独立任务,采集任务永不因射频背压阻塞;
//   * RX 只入环形缓冲,分帧交给应用任务。
#include "oc_link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "oc_link";

// ---- NUS UUID(Nordic UART Service,与手机端逐字一致) ----
#define OC_NUS_SERVICE_UUID_BYTES                                                      \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, \
        0x40, 0x6e
#define OC_NUS_RX_UUID_BYTES                                                           \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, \
        0x40, 0x6e
#define OC_NUS_TX_UUID_BYTES                                                           \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, \
        0x40, 0x6e

#define OC_LINK_TX_CHUNK_MAX 244U          // 单次 notify 上限(MTU 247 - 3)
#define OC_LINK_TX_SLOTS 8U                // 发送 ring 槽数
#define OC_LINK_TX_SLOT_MAX 640U           // 单帧上限:Opus 帧(≤519)+ 帧头 6,留余量
#define OC_LINK_RX_RING 4096U              // RX 环形缓冲:足够装下几个 MTU 的到达字节
#define OC_LINK_TX_TASK_STACK 3072U
#define OC_LINK_TX_TASK_PRIO 5U

typedef struct {
    uint8_t data[OC_LINK_TX_SLOT_MAX];
    size_t len;
} oc_link_tx_slot_t;

static struct {
    SemaphoreHandle_t mutex;              // 保护连接/加密/订阅标志与句柄
    oc_link_event_cb_t event_cb;
    void *event_ctx;
    bool initialized;
    bool host_running;
    bool host_synced;
    bool start_requested;
    bool stop_pending;
    bool encrypted;
    bool secure;                          // 加密 + 已认证 + 已绑定 + 16 字节密钥
    bool notify_subscribed;
    uint16_t conn_handle;
    uint16_t tx_value_handle;
    uint8_t own_addr_type;
    char device_name[24];

    // 发送 ring(生产者=任意任务,消费者=oc_link_tx_task)
    oc_link_tx_slot_t tx_slots[OC_LINK_TX_SLOTS];
    QueueHandle_t tx_free;
    QueueHandle_t tx_filled;
    SemaphoreHandle_t tx_free_sem;
    SemaphoreHandle_t tx_filled_sem;
    TaskHandle_t tx_task;
    uint32_t tx_drop;

    // RX 环形缓冲(生产者=NimBLE host 任务,消费者=应用任务)
    uint8_t rx_ring[OC_LINK_RX_RING];
    size_t rx_head;
    size_t rx_tail;
    // 已有未消费的 RX 唤醒:一次通知可能触发很多个 notify/写,不合并会把事件队列刷满
    // （实测后果:配对码/断开等关键事件被丢掉,界面看不到任何反应）。
    volatile bool rx_signaled;
    // TX_DROP 事件限流:拥塞时每帧一条会把队列刷满
    int64_t last_tx_drop_us;
} s_link;

static const ble_uuid128_t s_nus_service_uuid = BLE_UUID128_INIT(OC_NUS_SERVICE_UUID_BYTES);
static const ble_uuid128_t s_nus_rx_uuid = BLE_UUID128_INIT(OC_NUS_RX_UUID_BYTES);
static const ble_uuid128_t s_nus_tx_uuid = BLE_UUID128_INIT(OC_NUS_TX_UUID_BYTES);

static int oc_gap_event(struct ble_gap_event *event, void *arg);
static int oc_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static void oc_emit(oc_link_event_t *ev);
static int oc_reconcile_advertising(void);
static void oc_tx_task(void *arg);

// ---- 事件上抛 ----
static void oc_emit(oc_link_event_t *ev)
{
    if (s_link.event_cb != NULL) {
        s_link.event_cb(ev, s_link.event_ctx);
    }
}

static bool link_ready_locked(void)
{
    return s_link.conn_handle != BLE_HS_CONN_HANDLE_NONE && s_link.secure && s_link.notify_subscribed;
}

// ---- 配对码:每次配对生成随机 6 位数字并上屏 ----
// 下限必须是 100000:Android 侧对带前导零的 passkey 会在 confirm/random 阶段反复失败
// (参考实现实测:注入 4539 时 ble_sm_inject_io 返回 0 但 Android 仍断链)。
static int oc_inject_passkey(uint16_t conn_handle)
{
    uint32_t passkey = 100000U + (uint32_t)(esp_random() % 900000U);
    struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = passkey };
    int rc = ble_sm_inject_io(conn_handle, &io);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_sm_inject_io 失败: %d", rc);
        return rc;
    }
    oc_link_event_t ev = { .type = OC_LINK_EV_PASSKEY };
    ev.data.passkey.passkey = passkey;
    oc_emit(&ev);
    ESP_LOGI(TAG, "配对码已上屏");
    return 0;
}

// ---- GATT:手机写 RX / 读 TX ----
static int oc_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // TX 特征只用于 notify;读回来几个识别字节,方便通用 BLE 工具确认连对了设备。
        const char tag[4] = { 'O', 'C', '1', 0 };
        return os_mbuf_append(ctxt->om, tag, sizeof(tag)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    bool accept = (s_link.conn_handle == conn_handle) && s_link.secure;
    xSemaphoreGive(s_link.mutex);
    if (!accept) {
        // 特征本身带 ENC 标志;真到这一步说明加密态不一致,拒绝而不是收下脏数据。
        return BLE_ATT_ERR_INSUFFICIENT_ENC;
    }

    size_t total = 0;
    for (struct os_mbuf *m = ctxt->om; m != NULL; m = SLIST_NEXT(m, om_next)) {
        if (m->om_len == 0) {
            continue;
        }
        xSemaphoreTake(s_link.mutex, portMAX_DELAY);
        size_t used = (s_link.rx_head + OC_LINK_RX_RING - s_link.rx_tail) % OC_LINK_RX_RING;
        size_t space = OC_LINK_RX_RING - 1U - used;
        size_t take = m->om_len < space ? m->om_len : space;
        for (size_t i = 0; i < take; i++) {
            s_link.rx_ring[(s_link.rx_head + i) % OC_LINK_RX_RING] = m->om_data[i];
        }
        s_link.rx_head = (s_link.rx_head + take) % OC_LINK_RX_RING;
        xSemaphoreGive(s_link.mutex);
        total += take;
        if (take < m->om_len) {
            ESP_LOGW(TAG, "RX 缓冲满,丢弃 %u 字节", (unsigned)(m->om_len - take));
            break;
        }
    }
    if (total > 0 && !s_link.rx_signaled) {
        s_link.rx_signaled = true;   // 应用任务排空后才允许下一次唤醒
        oc_link_event_t ev = { .type = OC_LINK_EV_RX };
        ev.data.rx.bytes = total;
        oc_emit(&ev);
    }
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // 手机 → 设备:TEXT / CONTROL 帧
                .uuid = &s_nus_rx_uuid.u,
                .access_cb = oc_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                // 设备 → 手机:AUDIO / EVENT 帧(notify)
                .uuid = &s_nus_tx_uuid.u,
                .access_cb = oc_gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .val_handle = &s_link.tx_value_handle,
            },
            {0},
        },
    },
    {0},
};

// ESP-IDF 5.5.3 只给生成的 CCCD 加 WRITE_ENC;这里包一层注册边界,让受保护的 CCCD
// 同时拿到 READ_ENC(否则手机读 CCCD 会被拒,表现为"订阅不上")。
void ble_store_config_init(void);

typedef int oc_att_svr_access_fn_t(uint16_t conn_handle, uint16_t attr_handle,
                                   uint8_t att_op, uint16_t offset,
                                   struct os_mbuf **om, void *arg);

int __real_ble_att_svr_register(const ble_uuid_t *uuid, uint8_t flags, uint8_t min_key_size,
                                uint16_t *handle_id, oc_att_svr_access_fn_t *cb, void *cb_arg);

int __wrap_ble_att_svr_register(const ble_uuid_t *uuid, uint8_t flags, uint8_t min_key_size,
                                uint16_t *handle_id, oc_att_svr_access_fn_t *cb, void *cb_arg)
{
    if (uuid != NULL && ble_uuid_cmp(uuid, BLE_UUID16_DECLARE(0x2902)) == 0) {
        flags |= BLE_ATT_F_READ_AUTHEN | BLE_ATT_F_READ_ENC;
    }
    return __real_ble_att_svr_register(uuid, flags, min_key_size, handle_id, cb, cb_arg);
}

// ---- 广播 ----
static int oc_reconcile_advertising(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields response_fields;
    struct ble_gap_adv_params params;
    bool desired;
    bool physical_link;

    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    physical_link = s_link.conn_handle != BLE_HS_CONN_HANDLE_NONE;
    desired = s_link.start_requested && s_link.host_synced && !s_link.stop_pending && !physical_link;
    xSemaphoreGive(s_link.mutex);

    if (!desired) {
        return ble_gap_adv_active() ? ble_gap_adv_stop() : 0;
    }
    if (ble_gap_adv_active()) {
        return 0;
    }

    // 广播里放服务 UUID(手机按服务过滤扫描),名字放 scan response。
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_nus_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        return rc;
    }

    memset(&response_fields, 0, sizeof(response_fields));
    response_fields.name = (uint8_t *)s_link.device_name;
    response_fields.name_len = strlen(s_link.device_name);
    response_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response_fields);
    if (rc != 0) {
        return rc;
    }

    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(s_link.own_addr_type, NULL, BLE_HS_FOREVER, &params, oc_gap_event, NULL);
}

static int oc_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "连接失败 status=%d,继续广播", event->connect.status);
            return oc_reconcile_advertising();
        }
        xSemaphoreTake(s_link.mutex, portMAX_DELAY);
        if (s_link.stop_pending || s_link.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            xSemaphoreGive(s_link.mutex);
            return ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
        }
        s_link.conn_handle = event->connect.conn_handle;
        s_link.encrypted = false;
        s_link.secure = false;
        s_link.notify_subscribed = false;
        xSemaphoreGive(s_link.mutex);

        oc_link_event_t ev = { .type = OC_LINK_EV_CONNECTED };
        oc_emit(&ev);
        ESP_LOGI(TAG, "已连接,开始配对");

        // 设备无键盘:主动发起安全流程,配对码由 PASSKEY_ACTION(DISP) 注入并上屏。
        int rc = ble_gap_security_initiate(event->connect.conn_handle);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gap_security_initiate 失败: %d", rc);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        bool was_connected;
        xSemaphoreTake(s_link.mutex, portMAX_DELAY);
        was_connected = s_link.conn_handle == event->disconnect.conn.conn_handle;
        if (was_connected) {
            s_link.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_link.encrypted = false;
            s_link.secure = false;
            s_link.notify_subscribed = false;
        }
        xSemaphoreGive(s_link.mutex);
        if (was_connected) {
            oc_link_event_t ev = { .type = OC_LINK_EV_DISCONNECTED };
            ev.data.disconnected.reason = event->disconnect.reason;
            oc_emit(&ev);
            ESP_LOGI(TAG, "已断开 reason=%d", event->disconnect.reason);
        }
        return oc_reconcile_advertising();
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        return oc_reconcile_advertising();

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc = {0};
        bool publish = false;
        oc_link_event_t ev = { .type = OC_LINK_EV_SECURED };

        if (event->enc_change.status == 0 &&
            ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            xSemaphoreTake(s_link.mutex, portMAX_DELAY);
            if (s_link.conn_handle == event->enc_change.conn_handle) {
                s_link.encrypted = desc.sec_state.encrypted;
                // "就绪"要求:加密 + 已认证(MITM)+ 已绑定 + 16 字节密钥。
                s_link.secure = desc.sec_state.encrypted && desc.sec_state.authenticated &&
                                desc.sec_state.bonded && (desc.sec_state.key_size == 16U);
                publish = true;
            }
            xSemaphoreGive(s_link.mutex);
        }
        if (publish) {
            ESP_LOGI(TAG, "加密完成 enc=%d auth=%d bonded=%d key=%u", desc.sec_state.encrypted,
                     desc.sec_state.authenticated, desc.sec_state.bonded, desc.sec_state.key_size);
            oc_emit(&ev);
        } else {
            ESP_LOGW(TAG, "加密未完成 status=%d", event->enc_change.status);
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        bool subscribed = false;
        xSemaphoreTake(s_link.mutex, portMAX_DELAY);
        if (s_link.conn_handle == event->subscribe.conn_handle &&
            event->subscribe.attr_handle == s_link.tx_value_handle) {
            s_link.notify_subscribed = event->subscribe.cur_notify != 0;
            subscribed = s_link.notify_subscribed;
        }
        xSemaphoreGive(s_link.mutex);
        if (subscribed || event->subscribe.attr_handle == s_link.tx_value_handle) {
            oc_link_event_t ev = { .type = OC_LINK_EV_SUBSCRIBED };
            oc_emit(&ev);
            ESP_LOGI(TAG, "通知订阅=%d", (int)subscribed);
        }
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            return oc_inject_passkey(event->passkey.conn_handle);
        }
        return BLE_HS_ENOTSUP;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // 手机侧删除了绑定,或两边密钥不一致:删掉旧记录后重试一次。
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) != 0) {
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        int rc = ble_store_util_delete_peer(&desc.peer_id_addr);
        return rc == 0 ? BLE_GAP_REPEAT_PAIRING_RETRY : BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

    default:
        return 0;
    }
}

// ---- 发送:ring + 独立任务 ----
static size_t tx_fragment_size(uint16_t mtu)
{
    if (mtu <= 3U) {
        return 0;
    }
    size_t payload = (size_t)mtu - 3U;
    return payload < OC_LINK_TX_CHUNK_MAX ? payload : OC_LINK_TX_CHUNK_MAX;
}

// 把一帧按 MTU 切片发出。任何一片失败即视为整帧失败(不重传)。
static esp_err_t oc_notify_frame(const uint8_t *data, size_t len)
{
    uint16_t conn;
    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    conn = s_link.conn_handle;
    bool ok = !s_link.stop_pending && link_ready_locked();
    xSemaphoreGive(s_link.mutex);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t frag = tx_fragment_size(ble_att_mtu(conn));
    if (frag == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;
    for (size_t off = 0; off < len;) {
        size_t chunk = len - off;
        if (chunk > frag) {
            chunk = frag;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, (uint16_t)chunk);
        if (om == NULL) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        int rc = ble_gatts_notify_custom(conn, s_link.tx_value_handle, om);
        if (rc != 0) {
            // 控制器/ATT 侧没有发送缓冲:背压,交给上层丢帧计数。
            result = ESP_ERR_NO_MEM;
            break;
        }
        off += chunk;
    }
    return result;
}

static void oc_tx_task(void *arg)
{
    (void)arg;
    int idx = 0;
    for (;;) {
        if (xSemaphoreTake(s_link.tx_filled_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (xQueueReceive(s_link.tx_filled, &idx, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        oc_link_tx_slot_t *slot = &s_link.tx_slots[idx];
        ESP_LOGD(TAG, "发送帧 %u 字节", (unsigned)slot->len);
        if (oc_notify_frame(slot->data, slot->len) != ESP_OK) {
            s_link.tx_drop++;
            // 拥塞时限流上报:每帧一条会把上层事件队列刷满,反而丢掉关键事件。
            int64_t now = esp_timer_get_time();
            if (now - s_link.last_tx_drop_us > 1000000) {
                s_link.last_tx_drop_us = now;
                oc_link_event_t ev = { .type = OC_LINK_EV_TX_DROP };
                oc_emit(&ev);
            }
        }
        xQueueSend(s_link.tx_free, &idx, 0);
        xSemaphoreGive(s_link.tx_free_sem);
    }
}

esp_err_t oc_link_send(const uint8_t *frame, size_t len)
{
    if (frame == NULL || len == 0 || len > OC_LINK_TX_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_link.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int idx = 0;
    if (xSemaphoreTake(s_link.tx_free_sem, 0) != pdTRUE ||
        xQueueReceive(s_link.tx_free, &idx, 0) != pdTRUE) {
        s_link.tx_drop++;
        return ESP_ERR_NO_MEM;   // 队列满:丢这一帧,绝不让调用方等
    }
    oc_link_tx_slot_t *slot = &s_link.tx_slots[idx];
    memcpy(slot->data, frame, len);
    slot->len = len;
    xQueueSend(s_link.tx_filled, &idx, 0);
    xSemaphoreGive(s_link.tx_filled_sem);
    return ESP_OK;
}

uint32_t oc_link_tx_drop_count(void)
{
    return s_link.tx_drop;
}

// ---- RX 给应用 ----
size_t oc_link_read(uint8_t *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    size_t n = 0;
    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    size_t avail = (s_link.rx_head + OC_LINK_RX_RING - s_link.rx_tail) % OC_LINK_RX_RING;
    n = avail < cap ? avail : cap;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_link.rx_ring[(s_link.rx_tail + i) % OC_LINK_RX_RING];
    }
    s_link.rx_tail = (s_link.rx_tail + n) % OC_LINK_RX_RING;
    if (n == 0) {
        s_link.rx_signaled = false;   // 环已排空,下批字节可以再唤醒一次
    }
    xSemaphoreGive(s_link.mutex);
    return n;
}

void oc_link_flush_rx(void)
{
    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    s_link.rx_tail = s_link.rx_head;
    s_link.rx_signaled = false;
    xSemaphoreGive(s_link.mutex);
}

const char *oc_link_device_name(void)
{
    return s_link.device_name;
}

/**
 * 忘记配对(设备侧):清除本机保存的全部 bond 并断开当前连接。
 *
 * 用于设置菜单里的「重新配对」。Android 不允许 App 自行解除绑定,所以手机侧仍需用户
 * 到系统蓝牙里取消配对;设备侧清掉后,下一次连接会重新生成 6 位配对码。
 */
void oc_link_forget_peer(void)
{
    int rc = ble_store_clear();
    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    uint16_t conn = s_link.conn_handle;
    s_link.encrypted = false;
    s_link.secure = false;
    s_link.notify_subscribed = false;
    xSemaphoreGive(s_link.mutex);
    ESP_LOGW(TAG, "已清除配对信息(rc=%d),conn=%u", rc, (unsigned)conn);
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    (void)oc_reconcile_advertising();
}

// ---- host 生命周期 ----
static void oc_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_link.own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "选择 BLE 身份地址失败: %d", rc);
        return;
    }
    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    s_link.host_synced = true;
    xSemaphoreGive(s_link.mutex);

    int adv = oc_reconcile_advertising();
    if (adv != 0) {
        ESP_LOGE(TAG, "启动广播失败: %d", adv);
    }
}

static void oc_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host 复位: %d", reason);
    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    s_link.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_link.encrypted = false;
    s_link.secure = false;
    s_link.notify_subscribed = false;
    s_link.host_synced = false;
    xSemaphoreGive(s_link.mutex);
}

static void oc_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int oc_gatt_init(void)
{
    ble_hs_cfg.reset_cb = oc_on_reset;
    ble_hs_cfg.sync_cb = oc_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // 设备只有显示能力:配对时本机显示随机密码,由手机输入。
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 0;   // 放宽 SC-only:兼容部分 Android 蓝牙栈
    ble_hs_cfg.sm_sec_lvl = 4;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_services);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(s_link.device_name);
    }
    return rc;
}

esp_err_t oc_link_init(oc_link_event_cb_t cb, void *ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_link, 0, sizeof(s_link));
    s_link.event_cb = cb;
    s_link.event_ctx = ctx;
    s_link.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_link.mutex = xSemaphoreCreateMutex();
    s_link.tx_free = xQueueCreate(OC_LINK_TX_SLOTS, sizeof(int));
    s_link.tx_filled = xQueueCreate(OC_LINK_TX_SLOTS, sizeof(int));
    s_link.tx_free_sem = xSemaphoreCreateCounting(OC_LINK_TX_SLOTS, OC_LINK_TX_SLOTS);
    s_link.tx_filled_sem = xSemaphoreCreateCounting(OC_LINK_TX_SLOTS, 0);
    if (s_link.mutex == NULL || s_link.tx_free == NULL || s_link.tx_filled == NULL ||
        s_link.tx_free_sem == NULL || s_link.tx_filled_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < (int)OC_LINK_TX_SLOTS; i++) {
        xQueueSend(s_link.tx_free, &i, 0);
    }

    // 广播名 Passport-<MAC 后两字节>:手机端按前缀过滤扫描结果。
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    snprintf(s_link.device_name, sizeof(s_link.device_name), OC_LINK_NAME_PREFIX "%02X%02X", mac[4], mac[5]);

    s_link.initialized = true;
    ESP_LOGI(TAG, "链路就绪,广播名 %s", s_link.device_name);
    return ESP_OK;
}

esp_err_t oc_link_start(void)
{
    if (!s_link.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // NimBLE 的 bonding 持久化与 PHY 校准缓存都要 NVS;必须在射频启动之前初始化,
    // 否则每次开机都做一次完整校准(日志里能看到 phy_init 的告警)。
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init 未成功(只影响 bonding/校准缓存): %s", esp_err_to_name(nvs));
    }

    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    if (s_link.stop_pending) {
        xSemaphoreGive(s_link.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_link.start_requested = true;
    bool launch_host = !s_link.host_running;
    if (launch_host) {
        s_link.host_running = true;
    }
    xSemaphoreGive(s_link.mutex);

    if (launch_host) {
        int rc = nimble_port_init();
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init 失败: %d", rc);
            xSemaphoreTake(s_link.mutex, portMAX_DELAY);
            s_link.host_running = false;
            xSemaphoreGive(s_link.mutex);
            return ESP_FAIL;
        }
        rc = oc_gatt_init();
        if (rc != 0) {
            // 常见原因:某条特征缺 access_cb,或服务表写错。报错码容易看成内存不足。
            ESP_LOGE(TAG, "GATT 注册失败: %d", rc);
            xSemaphoreTake(s_link.mutex, portMAX_DELAY);
            s_link.host_running = false;
            xSemaphoreGive(s_link.mutex);
            return ESP_FAIL;
        }
        if (xTaskCreate(oc_tx_task, "oc_link_tx", OC_LINK_TX_TASK_STACK, NULL, OC_LINK_TX_TASK_PRIO,
                        &s_link.tx_task) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        nimble_port_freertos_init(oc_host_task);
        return ESP_OK;
    }
    return oc_reconcile_advertising();
}

esp_err_t oc_link_stop(void)
{
    if (!s_link.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    s_link.stop_pending = true;
    uint16_t conn = s_link.conn_handle;
    xSemaphoreGive(s_link.mutex);

    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }

    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    s_link.stop_pending = false;
    s_link.start_requested = false;
    s_link.encrypted = false;
    s_link.secure = false;
    s_link.notify_subscribed = false;
    xSemaphoreGive(s_link.mutex);
    return ESP_OK;
}

bool oc_link_ready(void)
{
    bool ready;
    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    ready = link_ready_locked();
    xSemaphoreGive(s_link.mutex);
    return ready;
}

bool oc_link_connected(void)
{
    bool connected;
    xSemaphoreTake(s_link.mutex, portMAX_DELAY);
    connected = s_link.conn_handle != BLE_HS_CONN_HANDLE_NONE;
    xSemaphoreGive(s_link.mutex);
    return connected;
}
