// main/c4_link.c —— 两机联机的最小 BLE 外设实现(可行性验证用)。
//
// 只在 C4_ENABLE_LINK=1 的构建里编译;默认构建只留桩函数。
#include "c4_link.h"

#include "bsp_display.h"   // bsp_lvgl_lock / bsp_lvgl_unlock
#include "c4_sound.h"
#include "c4_ui.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if C4_ENABLE_LINK

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "c4_link";

// Nordic UART 布局的 128 位 UUID(服务 / 写入 / 通知),对端用通用 BLE 工具或脚本即可。
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static char s_name[16];
static uint8_t s_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle;
static bool s_notify_enabled;
static bool s_started;
static uint8_t s_last_rx;

static int gap_event(struct ble_gap_event *event, void *arg);

// BLE 回调跑在 NimBLE host 任务里:碰 LVGL 必须持锁(短操作,符合仓库既有约定)。
static void set_hint(const char *text)
{
    if (!bsp_lvgl_lock(200)) return;
    c4_ui_set_menu_hint(text);
    bsp_lvgl_unlock();
}

static int notify_byte(uint8_t value)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_notify_enabled) return -1;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(&value, sizeof(value));
    if (!om) return -1;
    return ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
}

static int advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.name = (const uint8_t *)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "广播字段设置失败 rc=%d", rc);
        return rc;
    }

    // 需要可连接:对端要能连进来(基线 demo 只做不可连接广播)。
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "开始广播,设备名 %s", s_name);
        char hint[48];
        snprintf(hint, sizeof(hint), "LINK: %s ADVERTISING", s_name);
        set_hint(hint);
    } else {
        ESP_LOGE(TAG, "广播启动失败 rc=%d", rc);
    }
    return rc;
}

static int on_tx_read(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    // 两个字节:是否已连接 + 最后一次收到的列号,便于对端只读也能判断状态。
    const uint8_t status[2] = { (uint8_t)(s_conn_handle != BLE_HS_CONN_HANDLE_NONE), s_last_rx };
    return os_mbuf_append(ctxt->om, status, sizeof(status)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// 对端写入:最小验证只回显 + 响一声,证明链路是双向的。
static int on_rx_write(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    uint8_t value = 0;
    if (ctxt->om->om_len != 1 ||
        ble_hs_mbuf_to_flat(ctxt->om, &value, sizeof(value), NULL) != 0) {
        ESP_LOGW(TAG, "收到非预期负载,长度=%d", (int)ctxt->om->om_len);
        return BLE_ATT_ERR_UNLIKELY;
    }

    s_last_rx = value;
    ESP_LOGI(TAG, "收到对端写入 col=%u", (unsigned)value);
    c4_sound_play(C4_SOUND_DROP);          // 音频与 BLE 能否共存也是本次要验的
    char hint[48];
    snprintf(hint, sizeof(hint), "LINK: RX COL %u, ECHO...", (unsigned)value);
    set_hint(hint);
    notify_byte(value);                    // 回显 = 双向链路证据
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = on_rx_write,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = on_tx_read,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_notify_enabled = false;
            ESP_LOGI(TAG, "对端已连接 handle=%d,空闲堆=%u", s_conn_handle,
                     (unsigned)esp_get_free_heap_size());
            set_hint("LINK: CONNECTED");
            const uint8_t hello = 0xA5;
            notify_byte(hello);            // 握手字节(通知未订阅时会失败,属正常)
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "对端断开 reason=%d,空闲堆=%u", event->disconnect.reason,
                 (unsigned)esp_get_free_heap_size());
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        set_hint("LINK: DISCONNECTED");
        advertise();                       // 断开后重新广播,便于反复验证
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_notify_enabled = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "对端订阅通知=%d", (int)s_notify_enabled);
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "取本地 BLE 地址失败");
        return;
    }
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();                     // 返回即表示已请求停止
    nimble_port_freertos_deinit();
}

esp_err_t c4_link_start(void)
{
    if (s_started) return ESP_OK;

    // 设备名用 MAC 后两字节,便于对端在扫描列表里分辨两台机器。
    uint8_t mac[6] = { 0 };
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return ESP_FAIL;
    snprintf(s_name, sizeof(s_name), "C4-%02X%02X", mac[4], mac[5]);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(s_name);
    if (rc == 0) rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 服务注册失败 rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;

    ESP_LOGI(TAG, "BLE 启动前空闲堆=%u", (unsigned)esp_get_free_heap_size());
    nimble_port_freertos_init(host_task);
    s_started = true;
    return ESP_OK;
}

bool c4_link_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

#else  // !C4_ENABLE_LINK

esp_err_t c4_link_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool c4_link_connected(void)
{
    return false;
}

#endif  // C4_ENABLE_LINK
