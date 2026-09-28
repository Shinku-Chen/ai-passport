// main/oc_link.h —— 对讲机的 BLE 外设链路(Nordic UART Service)。
//
// 手机是中央端,设备只做外设:广播 Passport-XXXX → 配对加密(设备显示 6 位密码)→
// 订阅 TX 通知 → 双向搬字节。链路只懂"字节",分帧/重组在 oc_proto 里做。
//
// 三条约定(协议文档里的硬规则):
//   * 只有「已连接 + 已加密(16 字节密钥、MITM)+ 已订阅通知」三件事都成立,才算就绪;
//     未就绪时 send 返回 ESP_ERR_INVALID_STATE,上层据此丢弃音频帧并计数,不重传。
//   * 发送走独立任务 + 有界 ring:射频忙/缓冲满时丢帧而不是让采集任务阻塞。
//   * RX 字节在回调里只拷贝进环形缓冲,由应用任务取走,回调里不做任何重活。
//
// 线程:事件回调运行在 NimBLE host 任务里,必须短小 —— 只允许拷贝、入队、置标志。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 广播名前缀(与手机端扫描过滤一致)。
#define OC_LINK_NAME_PREFIX "Passport-"

typedef enum {
    OC_LINK_EV_CONNECTED,      // 物理连接建立(尚未加密)
    OC_LINK_EV_DISCONNECTED,
    OC_LINK_EV_SECURED,        // 配对/加密完成,可以写受保护的特征
    OC_LINK_EV_SUBSCRIBED,     // 手机订阅了 TX 通知,可以发数据
    OC_LINK_EV_PASSKEY,        // 需要在设备屏上显示 6 位配对码
    OC_LINK_EV_RX,             // 收到了字节(已入环形缓冲,应用任务来取)
    OC_LINK_EV_TX_DROP,        // 一帧因为未就绪/背压被丢弃
} oc_link_event_type_t;

typedef struct {
    oc_link_event_type_t type;
    union {
        struct { uint32_t passkey; } passkey;      // OC_LINK_EV_PASSKEY
        struct { size_t bytes; } rx;               // OC_LINK_EV_RX
        struct { int reason; } disconnected;       // OC_LINK_EV_DISCONNECTED(HCI 原因码)
    } data;
} oc_link_event_t;

// 事件回调(NimBLE host 任务上下文):只允许置标志/入队/唤醒任务。
typedef void (*oc_link_event_cb_t)(const oc_link_event_t *ev, void *ctx);

// 初始化:保存回调、算广播名。不碰射频。
esp_err_t oc_link_init(oc_link_event_cb_t cb, void *ctx);

// 启动射频:初始化 NVS(RF 校准缓存)→ NimBLE host → GATT 服务 → 开始广播。
// 重复调用是安全的;失败返回错误,可重试。
esp_err_t oc_link_start(void);

// 停止:断开连接、停广播、停 NimBLE host 并释放控制器(约 60KB 堆)。
esp_err_t oc_link_stop(void);

// 链路是否就绪(已连接 + 已加密 + 已订阅)。只有就绪才允许发帧。
bool oc_link_ready(void);

bool oc_link_connected(void);

// 发一整帧(已含 magic 帧头):内部按 MTU 分片后 notify。
// 就绪性/缓冲不足时返回 ESP_ERR_INVALID_STATE 或 ESP_ERR_NO_MEM,由上层计数丢弃。
esp_err_t oc_link_send(const uint8_t *frame, size_t len);

// 从 RX 环形缓冲取字节。返回实际取到的字节数(0 表示暂时没有)。
size_t oc_link_read(uint8_t *out, size_t cap);

// 丢弃 RX 缓冲里已到达的字节(断开连接/重新订阅时清场,避免半截帧影响新链路)。
void oc_link_flush_rx(void);

// 本机广播名(设置页显示用),如 "Passport-3A2B"。
const char *oc_link_device_name(void);

// 发送侧丢弃计数(未就绪 / 背压),用于诊断。
uint32_t oc_link_tx_drop_count(void);
