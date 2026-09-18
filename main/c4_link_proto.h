// main/c4_link_proto.h —— 联机对战的报文与可靠投递(纯 C,可宿主测试)。
//
// 为什么需要这一层:BSP 的 BLE 链路只保证“尽量把字节送出去”,通知/无响应写都没有
// ATT 层确认。回合制对战不能靠“大概送到了”:某一手丢了,两边棋盘就永久不一致。
// 所以这里做一个最小的停等(stop-and-wait)可靠层:
//
//   * 每个数据包带 7 位序号;接收方去重(重复包只重发确认,不重复交付);
//   * 每一帧都捎带“我已收到的对端序号”(piggyback ack),纯确认帧也走同一格式;
//   * 发送方在 C4_LINK_RETRY_MS 没等到确认就重传,超过 C4_LINK_MAX_RETRIES 报
//     C4_LINK_EV_LOST,由应用层判定“对端丢了”;
//   * 同时最多一个未确认的数据包(停等)。回合制下每回合本来就只有一条消息,
//     所以不需要滑动窗口;急着发新包会覆盖旧包(见 c4_link_proto_send 注释)。
//
// 帧格式(小端无关,固定 5 字节头 + 最多 3 字节负载):
//   [0] 0xC4 魔数        [1] 协议版本        [2] 类型
//   [3] bit7=1 表示带数据包,低 7 位是序号;0 表示纯确认帧
//   [4] bit7=1 表示 ack 有效,低 7 位是已收到的对端序号
//   [5..] 负载
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define C4_LINK_PROTO_MAGIC   0xC4u
#define C4_LINK_PROTO_VERSION 1u
#define C4_LINK_PKT_MAX       8u     // 一帧的固定上限(头 5 + 负载 3)
#define C4_LINK_PAYLOAD_MAX   3u
#define C4_LINK_RETRY_MS      400u   // 未确认时的重传间隔
#define C4_LINK_MAX_RETRIES   5u     // 重传上限,超过即认为链路不可用

// 报文类型(帧格式的 [2])。
typedef enum {
    C4_LINK_TYPE_HELLO = 1,    // 负载 [version, first_side, game_id]
    C4_LINK_TYPE_MOVE = 2,     // 负载 [col, ply]
    C4_LINK_TYPE_REMATCH = 3,  // 负载 [game_id]
    C4_LINK_TYPE_LEAVE = 4,    // 无负载
} c4_link_type_t;

// 交给应用层的事件。
typedef enum {
    C4_LINK_EV_NONE = 0,
    C4_LINK_EV_PEER_READY,    // 收到对端 HELLO,可以开打了
    C4_LINK_EV_MOVE,          // 对端落子
    C4_LINK_EV_REMATCH,       // 对端请求再来一局
    C4_LINK_EV_LEAVE,         // 对端离开
    C4_LINK_EV_LOST,          // 重传耗尽:链路不可用
    C4_LINK_EV_BAD_VERSION,   // 对端协议版本不一致
} c4_link_ev_kind_t;

typedef struct {
    uint8_t kind;        // c4_link_ev_kind_t
    uint8_t col;         // MOVE
    uint8_t ply;         // MOVE:本手之后的落子总数,用于发现失步
    uint8_t first_side;  // PEER_READY:对端在这一局的先手方(C4_P1 / C4_P2)
    uint8_t game_id;     // PEER_READY / REMATCH
    uint8_t version;     // BAD_VERSION
} c4_link_ev_t;

typedef struct {
    uint8_t tx[C4_LINK_PKT_MAX];  // 要立刻发出去的帧
    uint8_t tx_len;               // 0 = 没有要发的
    c4_link_ev_t ev;
} c4_link_result_t;

typedef struct {
    uint8_t next_seq;      // 本机下一个数据序号(0..127 循环)
    uint8_t ack_seq;       // 最近接受的对端序号
    bool have_ack;         // ack_seq 是否有效
    bool have_peer_seq;    // 是否已接受过对端数据包

    uint8_t pending[C4_LINK_PKT_MAX];
    uint8_t pending_len;
    uint8_t pending_seq;
    uint8_t retries;
    uint32_t since_tx_ms;
    bool pending_active;

    bool peer_ready;
    uint8_t peer_side;
    uint8_t peer_game_id;
} c4_link_proto_t;

// 清空状态。两边各自从序号 0 开始。
void c4_link_proto_init(c4_link_proto_t *p);

// 发一个数据包。返回要写进链路层的帧。
// 注意:停等语义 —— 上一个数据包还没确认时,新包会覆盖它并重置重传计数。
// 本应用每回合只发一条消息,所以正常路径不会踩到这个覆盖。
c4_link_result_t c4_link_proto_send(c4_link_proto_t *p, uint8_t type,
                                    const uint8_t *payload, uint8_t payload_len);

// 收到一帧。去重、更新 ack,并把需要回的纯确认帧放在 result.tx 里。
c4_link_result_t c4_link_proto_recv(c4_link_proto_t *p, const uint8_t *frame, uint8_t len);

// 时间推进:到点重传;重传耗尽报 C4_LINK_EV_LOST。
c4_link_result_t c4_link_proto_tick(c4_link_proto_t *p, uint32_t elapsed_ms);

// 是否有未确认的发包。
bool c4_link_proto_busy(const c4_link_proto_t *p);
