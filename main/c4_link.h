// main/c4_link.h —— 联机会话:把 BSP 的 BLE 字节链路接上对战协议。
//
// 线程模型与仓库既有约定一致:所有状态都由【输入任务】串行改写。
//   * BSP 的回调跑在 NimBLE host 任务里,只做一件事:把“状态变化/收到字节”
//     塞进本模块的 FreeRTOS 队列后立刻返回;
//   * 输入任务调用 c4_link_pump() 取走队列内容,喂给 c4_link_proto 解码,
//     再通过 c4_link_next_event() 交给控制器;
//   * 因此 c4_link_proto_t 与事件 FIFO 只在输入任务里被碰,不需要加锁。
//
// 连接建立后两边各自发一次 HELLO 完成握手;HELLO/REMATCH 里带 game_id,
// 先手方由 [central 先手, peripheral 后手] 开始、逐局交替,两边用同一个
// game_id 就能各自算出同一套先后手,不需要再协商。
#pragma once

#include "c4_link_proto.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include <stdbool.h>
#include <stdint.h>

// 应用层看到的链路状态。
typedef enum {
    C4_LINK_STATE_OFF = 0,    // 未启动
    C4_LINK_STATE_SEARCHING,  // 广播 + 扫描,还没找到对端
    C4_LINK_STATE_CONNECTING, // 正在连接 / 正在建立 GATT 通道
    C4_LINK_STATE_READY,      // 双向通道就绪
    C4_LINK_STATE_FAILED,     // BLE 起不来(例如堆不够)
} c4_link_state_t;

// 交给控制器的事件。
typedef enum {
    C4_LINK_EVENT_UP = 0,         // 通道就绪:应立刻发 HELLO 完成握手
    C4_LINK_EVENT_DOWN,           // 通道断开,或重传耗尽
    C4_LINK_EVENT_PEER_READY,     // 收到对端 HELLO:可以开局
    C4_LINK_EVENT_MOVE,           // 对端落子(col / ply)
    C4_LINK_EVENT_REMATCH,        // 对端请求再来一局(game_id)
    C4_LINK_EVENT_LEAVE,          // 对端离开
    C4_LINK_EVENT_BAD_VERSION,    // 对端固件协议版本不一致
    C4_LINK_EVENT_FAILED,         // BLE 启动失败
} c4_link_event_kind_t;

typedef struct {
    uint8_t kind;        // c4_link_event_kind_t
    uint8_t col;         // MOVE
    uint8_t ply;         // MOVE
    uint8_t first_side;  // PEER_READY:对端在该局的先手方
    uint8_t game_id;     // PEER_READY / REMATCH
    uint8_t version;     // BAD_VERSION
} c4_link_event_t;

// 启动 BLE 链路(初始化 NimBLE 约需 73KB 堆)。失败时通过 FAILED 事件上报,
// 应用照常可以玩单机模式。可重复调用,已在运行则返回 ESP_ERR_INVALID_STATE。
esp_err_t c4_link_start(void);

// 停链路并释放射频。deep sleep 前必须调用。可重复调用。
esp_err_t c4_link_stop(void);

// 输入任务里调用:取空 BSP 队列、解码报文、推进重传计时。
// elapsed_ms 是本次调用距离上次调用的毫秒数。
void c4_link_pump(uint32_t elapsed_ms);

// 输入任务里调用:取出一个应用事件。没有事件时返回 false。
bool c4_link_next_event(c4_link_event_t *out);

c4_link_state_t c4_link_state(void);

// 通道是否就绪(可以 send)。
bool c4_link_ready(void);

// 本机在 game_id 这一局执的颜色(C4_P1 = 先手/琥珀,C4_P2 = 后手/珊瑚)。
// 两边用同一个 game_id 算出的结果相反,所以逐局交替先手只需要共同推进 game_id。
uint8_t c4_link_local_first_side(uint8_t game_id);

// 发出 HELLO 完成握手(收到 UP 事件后由控制器调用,对端固件版本在 HELLO 里校验)。
void c4_link_send_hello(uint8_t game_id);
void c4_link_send_move(uint8_t col, uint8_t ply);
void c4_link_send_rematch(uint8_t game_id);
void c4_link_send_leave(void);
