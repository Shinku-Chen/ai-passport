// main/steins_mobile.h —— 《命运石之门》手机事件状态机（生产实现）
//
// 目标：把源 `src/common/mobile.js` + `phone.js` + `engine.js(locate/choose)` 的
// 手机语义 1:1 落到 C，直接读写 `steins_checkpoint_t`（不复制影子状态）。
//
// 契约与证据：
//   build/mobile-integration/engine-api.md                        （本模块 API/持久字段/分工）
//   build/review-model/mobile-contract-20261102-mobile/REPORT.md  （源事件契约）
//   build/review-model/mobile-contract-20261102-mobile/logs/oracle.json （S1..S12 期望态）
//
// 设计要点：
//   * 内容读取全部走 `steins_mobile_content_t` 回调，本模块不依赖 pack 结构；
//     生产适配由 `main/steins_mobile_pack.c` 提供（读 `steins_pack.h` 的 v4 访问器）。
//   * `steins_mobile_enter()` 绑定"当前节点的手机事件文件"，对应源里模块级的
//     `setMobileEvents()`；`sync` 在绑定节点与 checkpoint 节点不一致时**什么都不做**
//     （源：`if(mobileNode!==s.chapter)return;`），所以换节点必须先 enter。
//   * 任何写操作失败（容量/参数/内容缺失）都返回 false 且不改动 checkpoint，
//     **绝不静默截断**（源 restore 的上限是 inbox/sent ≤ 700，本模块同）。
#pragma once

#include "steins_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------- 内容枚举
// 与 pack v4 `MEVT.kind` 同序（见 build/mobile-integration/pack-api.md）。
typedef enum {
    STEINS_MOB_EV_SEED = 0,
    STEINS_MOB_EV_RECEIVE = 1,
    STEINS_MOB_EV_ERASE = 2,
    STEINS_MOB_EV_READ_BIT = 3,
    STEINS_MOB_EV_REPLY_BIT = 4,
    STEINS_MOB_EV_EXPIRE = 5,
    STEINS_MOB_EV_QUEUE = 6,
    STEINS_MOB_EV_DISPATCH = 7,
    STEINS_MOB_EV_RULE = 8,
    STEINS_MOB_EV_CALL = 9,
    STEINS_MOB_EV_PRESENTATION = 10,
    STEINS_MOB_EV_KIND_COUNT = 11,
} steins_mob_event_kind_t;

// 选项/动作。**pack 的 action 字节必须使用这组值**（三家共用契约）。
typedef enum {
    STEINS_MOB_ACT_NONE = 0,
    STEINS_MOB_ACT_REPLY = 1,       // 源 'reply'
    STEINS_MOB_ACT_SEND = 2,        // 源 'send'
    STEINS_MOB_ACT_CALL = 3,        // 源 'call'
    STEINS_MOB_ACT_COMPOSE = 4,     // 源 'compose'
    STEINS_MOB_ACT_NO_CALL = 5,     // 源 'noCall'（进关键电话窗口）
    STEINS_MOB_ACT_NO_SEND = 6,     // 源 'noSend'
    STEINS_MOB_ACT_READ_DEFERRED = 7,
    STEINS_MOB_ACT_EXPIRE_DEFERRED = 8,
} steins_mob_action_t;

// 手机屏演示 kind。源是字符串 'draft'/'send'/'view'；**与 pack v4 STEINS_PK_* 同值**
// （draft=0 / send=1 / view=2），避免适配层再做一次映射。
typedef enum {
    STEINS_MOB_PRES_DRAFT = 0,
    STEINS_MOB_PRES_SEND = 1,
    STEINS_MOB_PRES_VIEW = 2,
} steins_mob_pres_kind_t;

// 来电 kind（源 'phone' / 'dmail'）：`finishPresentation` 只在 dmail 时清 call。
typedef enum {
    STEINS_MOB_CALL_PHONE = 0,
    STEINS_MOB_CALL_DMAIL = 1,
} steins_mob_call_kind_t;

// 邮件类型（源 mailIndex[id][2]）。
#define STEINS_MOB_MAIL_TYPE_TEXT 0u
#define STEINS_MOB_MAIL_TYPE_SENT 1u
#define STEINS_MOB_MAIL_TYPE_INCOMING 2u
#define STEINS_MOB_MAIL_TYPE_REPLY 3u

// 通道空值（源用 999）。
#define STEINS_MOB_CHANNEL_EMPTY 999u
// 源 restore() 的硬上限：inbox/sent 各 700。
#define STEINS_MOB_MAX_IDS STEINS_CHK_MAX_INBOX

// ---------------------------------------------------------------- 内容记录
typedef struct {
    uint16_t id;
    uint8_t type;
} steins_mobile_mail_t;

typedef struct {
    uint32_t cursor;   // 事件行游标（源 row[0]）
    uint16_t index;    // 行在节点事件表里的下标（回调取事件时用）
    uint8_t count;     // 该行事件数
} steins_mobile_row_t;

// 事件参数按源顺序压进 arg[]：
//   seed        (argc 1) arg[0]=seed 描述符下标（源 `["seed",[id,...]]` 的有序 ID 流经
//               seed_ids(desc,…) 读出；普通 sync 不消费，只有 migrateMail 用）
//   receive     id, channel, open, owner        （channel=-1 表示无通道）
//   erase       first, last
//   readBit     id, value(0/1)
//   replyBit    id, value(0/1)
//   expire      (0)
//   queue       channel, id
//   dispatch    channel
//   rule        rule_id
//   call        argc=5：arg[0]=CALLSITE 下标, arg[1]=owner, arg[2]=kind,
//               arg[3]=items 描述符下标（event_items(desc,…) 读出有序 items）,
//               arg[4]=waiting。引擎只消费 site/owner/kind/waiting。
//   presentation id, kind, owner
// 源事件参数上界 = 5（call: site/owner/kind/items/waiting），这里留到 8。
#define STEINS_MOB_EVENT_ARGS 8u
typedef struct {
    uint8_t kind;
    uint8_t argc;
    int32_t arg[STEINS_MOB_EVENT_ARGS];
} steins_mobile_event_t;

typedef struct {
    uint16_t node;
    uint16_t id;
    uint16_t limit;
    uint16_t skip;
    uint16_t read;
    uint8_t option_count;
} steins_mobile_window_t;

typedef struct {
    uint8_t action;
    uint16_t id;
    uint16_t to;
} steins_mobile_window_option_t;

typedef struct {
    uint16_t node;
    uint16_t skip;
    uint16_t go;
    uint16_t limit;
    uint16_t after;
    uint16_t sent;      // 0xFFFF = 该窗口不写 sent
} steins_mobile_keyphone_t;

typedef struct {
    uint16_t site;
    uint16_t limit;
    uint8_t owner;
    uint8_t item_count;
} steins_mobile_callsite_t;

typedef struct {
    uint8_t action;
    uint16_t clip;      // clip 节点 id；0xFFFF = 无 clip
                        // （pack v4 的 CALLITEM.clip 是 u8 索引，由生产适配换成节点 id）
    uint8_t join;
    uint16_t id;
} steins_mobile_callitem_t;

typedef struct {
    uint16_t to;
    uint16_t id;
    uint16_t source;    // D-Mail：源串 SGxx_yy 解析出的邮件 id；0xFFFF = 无
    uint8_t action;
} steins_mobile_choice_item_t;

// 事件自带的有序 items（源 call 事件的第 5 个参数）；引擎不消费，包/审计必须保序保动作。
typedef struct {
    uint8_t action;
    uint16_t id;
} steins_mobile_item_ref_t;

// ---------------------------------------------------------------- 内容后端
// 所有回调：越界/缺失返回 false 或 0，不读越界内存。
typedef struct {
    // mailIndex：type 与后继（原序）。
    bool (*mail_meta)(void *ctx, uint16_t id, steins_mobile_mail_t *out);
    uint8_t (*mail_succ)(void *ctx, uint16_t id, uint16_t *out, uint8_t cap);

    // 节点事件：row_count()==0 表示该节点没有事件文件（源 mobileEventFiles 缺项）。
    uint16_t (*row_count)(void *ctx, uint16_t node);
    bool (*row)(void *ctx, uint16_t node, uint16_t index, steins_mobile_row_t *out);
    bool (*event)(void *ctx, uint16_t node, const steins_mobile_row_t *row, uint8_t index,
                  steins_mobile_event_t *out);
    // seed 有序 ID 流（desc = 事件 arg[0]）：按 offset 分批读，单行最多 282 项。
    uint16_t (*seed_ids)(void *ctx, uint16_t desc, uint16_t offset, uint16_t *out, uint16_t cap);
    // call 事件的有序 items（desc = 事件 arg[3]）；缺失返回 0。
    uint8_t (*event_items)(void *ctx, uint16_t desc, steins_mobile_item_ref_t *out, uint8_t cap);
    // presentation 的稳定 key 编号（q407 的 PRES 目录：index ↔ {source chapter,row,id,kind,owner}）。
    // 返回 0xFFFF = 该事件没有 key（无目录/越界时由适配层决定）。有效范围必须 < count。
    uint16_t (*presentation_key)(void *ctx, uint16_t node, const steins_mobile_row_t *row,
                                 uint8_t event_index);
    // PRES 目录的有效条数（0 = 无 key 目录）。引擎用它校验 key 的真实上界，
    // **不**用常量 STEINS_CHK_MAX_PRES 接受一个不存在的 key。
    uint16_t (*presentation_key_count)(void *ctx);

    // 窗口 / 关键电话 / 通话 / 图
    bool (*window)(void *ctx, uint16_t node, steins_mobile_window_t *out);
    bool (*window_option)(void *ctx, uint16_t node, uint8_t index,
                          steins_mobile_window_option_t *out);
    bool (*keyphone)(void *ctx, uint16_t node, steins_mobile_keyphone_t *out);
    bool (*call_site)(void *ctx, uint16_t site, steins_mobile_callsite_t *out);
    bool (*call_item)(void *ctx, uint16_t site, uint8_t index, steins_mobile_callitem_t *out);

    bool (*node_count)(void *ctx, uint16_t node, uint32_t *count);
    bool (*node_choice_id)(void *ctx, uint16_t node, uint16_t *choice_id);
    uint8_t (*choice_items)(void *ctx, uint16_t node, steins_mobile_choice_item_t *out,
                            uint8_t cap);

    // 通话片段：clip 节点（源 callClips 注册成 graph 节点）的总帧数。
    bool (*clip_frames)(void *ctx, uint16_t node, uint32_t *frames);
    bool (*is_clip_node)(void *ctx, uint16_t node);
} steins_mobile_content_t;

// ---------------------------------------------------------------- 运行时
typedef struct {
    const steins_mobile_content_t *content;
    void *ctx;
    uint16_t events_node;   // 已绑定事件文件的节点
    bool events_bound;
    // 边界归属开关（**同一决策只能有一个所有者**）：
    //   false（默认，本移植现状）= 主图已物化 65 个 choice → 引擎不合成 deferred/keyPhone 边界，
    //                                locate 只按图节点返回 choice；
    //   true（源语义）= 引擎在 w.limit / keyPhone limit 处合成边界并负责跳转。
    // 二者绝不可同时启用，否则同一窗口会被问两次（见 engine-api.md §5）。
    bool dynamic_boundaries;
} steins_mobile_t;

// locate/choose 的结果帧。调用方按 type 决定画什么：
//   TEXT    —— 正常正文（由 app 自己的分块读取负责）
//   CHOICE  —— 图内选择（choice_node 的选项表，经 content->choice_items 读）
//   DEFERRED—— 回复窗口边界（合成选项：window_option(reply_dialog_node) + 读/忽略；
//              由 steins_mobile_deferred_options() 提供统一视图）
//   KEYPHONE—— 关键电话边界（选项来自 choice_items(keyphone_root)，文案改写由
//              steins_mobile_keyphone_options() 提供）
//   END     —— 该节点无后继（源 throw '剧情连接缺失'）
//   ERROR   —— 内容缺失/状态非法
typedef enum {
    STEINS_MOB_FRAME_TEXT = 0,
    STEINS_MOB_FRAME_CHOICE,
    STEINS_MOB_FRAME_DEFERRED,
    STEINS_MOB_FRAME_KEYPHONE,
    STEINS_MOB_FRAME_END,
    STEINS_MOB_FRAME_ERROR,
} steins_mobile_frame_type_t;

typedef struct {
    steins_mobile_frame_type_t type;
    uint16_t node;        // 帧所属节点（DEFERRED 时是当前节点；KEYPHONE 时是 root）
    uint16_t choice_id;   // CHOICE 的选择 id；DEFERRED 是邮件 id；KEYPHONE 是 root 选择 id
    uint8_t item_count;
} steins_mobile_frame_t;

typedef struct {
    uint16_t node;
    uint32_t cursor;
} steins_mobile_pos_t;

// 边界选项的统一视图（DEFERRED / KEYPHONE）。
typedef struct {
    uint8_t action;       // STEINS_MOB_ACT_*
    uint16_t id;          // reply 的回复邮件 id / read 的父邮件 id
    uint16_t to;          // reply 的落点（源 item.to）
} steins_mobile_boundary_item_t;

// ---------------------------------------------------------------- 生命周期
void steins_mobile_init(steins_mobile_t *m);
// 进入节点（新游戏 / 章节浏览 / 任意跳转）。章节模式从 cursor>0 进入时会置
// lastAt=cursor-1，等价于源的 mobile.presentationEntry（跳过 entry 之前的演示行）。
void steins_mobile_seek(steins_checkpoint_t *chk, uint16_t node, uint32_t cursor);
// 边界归属（默认 false = 物化层）。必须在 enter 之前/之后、locate 之前设置。
void steins_mobile_set_dynamic_boundaries(steins_mobile_t *m, bool enabled);
// 绑定节点事件文件（源 setMobileEvents）。content/ctx 可重复传入。
bool steins_mobile_enter(steins_mobile_t *m, const steins_mobile_content_t *content, void *ctx,
                         uint16_t node);
// 源 syncMobile：处理 (lastAt, cursor] 的事件行，并把游标记为已处理。
// 绑定节点与 chk->node 不同时返回 true 但不做任何事（源行为）。
bool steins_mobile_sync(steins_mobile_t *m, steins_checkpoint_t *chk);

// ---------------------------------------------------------------- locate/choose
// 源 locate() 的手机部分（returnCall → sync → beginDeferred → 边界 → 图选择/END），
// 与源同序。TEXT 只是"可以读正文"，本模块不读正文。
bool steins_mobile_locate(steins_mobile_t *m, steins_checkpoint_t *chk,
                          steins_mobile_frame_t *out);

// 图内选择（源 choose() 的手机部分）：incoming + D-Mail/reply 写入 sent + 落点。
// 已经是 deferred/keyPhone 的边界选择时**不要**走这里，用下面两个 answer_*。
// owner = 源 phoneContext(state).owner（0/1/2/4），由 app 计算后传入。
bool steins_mobile_choose(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t node,
                          uint8_t item_index, uint8_t owner, steins_mobile_pos_t *land);

// ---------------------------------------------------------------- 读信/回复
bool steins_mobile_mark_read(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t id,
                            steins_mobile_pos_t *land);
bool steins_mobile_can_reply(steins_mobile_t *m, const steins_checkpoint_t *chk, uint16_t id);
// 源 sendOptional：can_reply + 后继校验 + 已读/已回复/通道推进。
bool steins_mobile_send_reply(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t parent,
                              uint16_t id, steins_mobile_pos_t *land);

// ---------------------------------------------------------------- 延迟回复边界
// 源 beginDeferred 的守卫：无窗口 / 已有 deferred / cursor < 节点帧数 → false。
bool steins_mobile_defer_begin(steins_mobile_t *m, steins_checkpoint_t *chk);
// 源 deferredBoundary：cursor < limit 时返回 false（还没到边界）。
bool steins_mobile_deferred_boundary(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                     steins_mobile_frame_t *out);
uint8_t steins_mobile_deferred_options(const steins_mobile_t *m, const steins_checkpoint_t *chk,
                                       steins_mobile_boundary_item_t *out, uint8_t cap);
// 源 answerDeferred：**游标不归零、不重置到 0**；reply → 源 item.to，read → w.read，
// 忽略 → w.read（读过）/ w.skip。land 返回目标节点与**当前游标**（源 s.cursor=cursor）。
bool steins_mobile_answer_deferred(steins_mobile_t *m, steins_checkpoint_t *chk, uint8_t action,
                                   uint16_t reply_id, steins_mobile_pos_t *land);
// 源 deferredLimit。
bool steins_mobile_deferred_limit(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                  uint16_t *limit);

// ---------------------------------------------------------------- 关键电话
bool steins_mobile_defer_key_phone(steins_mobile_t *m, steins_checkpoint_t *chk, uint8_t owner);
bool steins_mobile_keyphone_boundary(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                     steins_mobile_frame_t *out);
uint8_t steins_mobile_keyphone_options(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                       steins_mobile_boundary_item_t *out, uint8_t cap);
bool steins_mobile_answer_key_phone(steins_mobile_t *m, steins_checkpoint_t *chk, uint8_t index,
                                    steins_mobile_pos_t *land);

// ---------------------------------------------------------------- 来电 / 返回
bool steins_mobile_call_current(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                uint16_t *site, uint8_t *item_count);
bool steins_mobile_take_call(steins_mobile_t *m, steins_checkpoint_t *chk, uint8_t index,
                             steins_mobile_pos_t *land);
bool steins_mobile_return_call(steins_mobile_t *m, steins_checkpoint_t *chk,
                               steins_mobile_pos_t *land);

// ---------------------------------------------------------------- 手机屏演示
bool steins_mobile_plot_presentation(const steins_checkpoint_t *chk, uint16_t *id, uint8_t *kind,
                                     uint8_t *owner);
bool steins_mobile_finish_presentation(steins_mobile_t *m, steins_checkpoint_t *chk);
// 源 nextPresentationBoundary：把章节跳转截断到下一条 presentation 事件行。
bool steins_mobile_next_presentation_boundary(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                              uint32_t to, uint32_t *out);

// ---------------------------------------------------------------- 查询（UI 用）
uint16_t steins_mobile_inbox_count(const steins_checkpoint_t *chk);
bool steins_mobile_inbox_at(const steins_checkpoint_t *chk, uint16_t index, uint16_t *id);
uint16_t steins_mobile_sent_count(const steins_checkpoint_t *chk);
bool steins_mobile_sent_at(const steins_checkpoint_t *chk, uint16_t index, uint16_t *id);
uint16_t steins_mobile_unread_count(const steins_checkpoint_t *chk);
bool steins_mobile_unread_at(const steins_checkpoint_t *chk, uint16_t index, uint16_t *id);
// 源 m.open[id]：返回通道号；未打开返回 -1。
int16_t steins_mobile_open_channel(const steins_checkpoint_t *chk, uint16_t id);
uint16_t steins_mobile_channel(const steins_checkpoint_t *chk, uint8_t index);
bool steins_mobile_owner(const steins_checkpoint_t *chk, uint16_t id, uint8_t *owner);
uint16_t steins_mobile_last_node(const steins_checkpoint_t *chk);
uint32_t steins_mobile_last_at(const steins_checkpoint_t *chk);
// 源 pendingMail(owner)：存在未读、类型 0/2、owner 匹配（owner<0 表示不限）。
uint16_t steins_mobile_pending_count(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                     int owner);
// 源 markOrdinaryInboxRead(owner)：把该 owner 的普通未读全部标记已读，返回条数。
uint16_t steins_mobile_mark_ordinary_read(steins_mobile_t *m, steins_checkpoint_t *chk,
                                          uint8_t owner);

// presentation key 空间：**用 qfic 的公共常量**（0..55 有效，对应 pres_read_bits 的 56 位），
// 与待播队列容量 `STEINS_CHK_MAX_PRES (64)` 是两件事：56..63 是非法 key，不是"更大的队列"。
#define STEINS_MOB_PRES_KEY_NONE 0xFFu   // 源里不存在；旧档 / 无 PRES 目录的降级值

// 源 mailLifeVersion：新存档写入 1；只有更早的存档（version 0）才需要 migrate。
#define STEINS_MOB_MAIL_LIFE_VERSION 1u

// 源 migrateMail：按 seed 有序 ID 流 + receive 的后继闭包 + erase 区间重建 allowed 集合，
// 再过滤 inbox/sent/unread/open。**只有 mailLifeVersion != 1 的旧档才需要调用**；
// 新存档一律写 1，因此生产路径上它是幂等 no-op（本函数返回 true）。
bool steins_mobile_migrate(steins_mobile_t *m, steins_checkpoint_t *chk, bool legacy_archive);
