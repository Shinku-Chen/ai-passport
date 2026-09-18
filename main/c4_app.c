// main/c4_app.c —— 四子棋控制器实现:设置菜单 + 对局状态机 + 电脑对手 worker + 空闲休眠。
#include "c4_app.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "c4_ai.h"
#include "c4_link.h"
#include "c4_model.h"
#include "c4_settings.h"
#include "c4_sound.h"
#include "c4_ui.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "c4";

// 电脑对手的三档难度。depth 决定上限,墙钟预算决定实际能搜到几层,
// blunder 是“故意走差棋”的比例 —— 低难度靠它放水,而不是靠不堵棋。
typedef struct {
    uint8_t max_depth;
    uint32_t budget_ms;
    uint8_t blunder_percent;
} c4_level_cfg_t;

static const c4_level_cfg_t LEVEL_CFG[C4_LEVEL_COUNT] = {
    [C4_LEVEL_EASY]   = { 3, 200, 35 },
    [C4_LEVEL_MEDIUM] = { 5, 350, 8 },
    [C4_LEVEL_HARD]   = { 7, 500, 0 },
};

#define C4_APP_AI_MAX_NODES     300000u  // 只当保险:真正限时的是墙钟预算
#define C4_APP_AI_TASK_STACK    4096
#define C4_APP_AI_TASK_PRIO     3
#define C4_APP_LVGL_LOCK_MS     500

// 空闲休眠阈值:设置屏/联机屏短一点,对局中给足思考时间。
#define C4_APP_IDLE_MENU_MS     60000u
#define C4_APP_IDLE_PLAY_MS     180000u
#define C4_APP_IDLE_LINK_MS     600000u   // 联机对局中:睡着等于断线,给到 10 分钟
#define C4_APP_SLEEP_NOTICE_MS  400     // 先让 “SLEEPING” 刷上屏再熄背光

// 联机时序:握手超时(通道就绪但一直收不到对端 HELLO)与联机屏重绘节流。
#define C4_APP_LINK_HELLO_TIMEOUT_MS 8000u
#define C4_APP_LINK_RENDER_MS        500u

typedef enum {
    C4_STATE_MENU = 0,
    C4_STATE_LINK,      // 联机屏:搜索/连接/等对端(握手完成后自动开局)
    C4_STATE_PLAY,
    C4_STATE_OVER,
} c4_state_t;

// 联机屏上正在展示的内容。SEARCHING/CONNECTING/HANDSHAKE 由链路状态推导,
// 其余几个是“粘住”的故障提示,直到重新连上才清除。
typedef enum {
    C4_LINK_NOTICE_SEARCHING = 0,
    C4_LINK_NOTICE_CONNECTING,
    C4_LINK_NOTICE_HANDSHAKE,
    C4_LINK_NOTICE_PEER_LEFT,
    C4_LINK_NOTICE_DESYNC,
    C4_LINK_NOTICE_BAD_VERSION,
    C4_LINK_NOTICE_FAILED,
} c4_link_notice_t;

static c4_state_t s_state;
static c4_settings_t s_settings;
static c4_game_t s_game;
static int s_cursor;
static bool s_thinking;
static bool s_ai_ok;
static bool s_ai_ready;          // UI 建好后才允许处理事件
static bool s_ai_pending;        // 本事件处理完后要叫醒 worker
static c4_game_t s_ai_game;      // 交给 worker 的局面快照(写它的人就是发通知的人)

static QueueHandle_t s_input_queue;
static TaskHandle_t s_ai_task;

// 对局代号:每开新局/回菜单都会 +1。worker 在算之前记住它,回复时带回,
// 输入任务据此丢弃“上一局发出的、回来晚了”的结果。
static uint8_t s_generation;
static uint8_t s_ai_generation;
static uint32_t s_idle_ms;
static int64_t s_ai_deadline_us;

// —— 联机对战状态(全部由输入任务改写) ——
static bool s_link_mode;         // 本次对局是联机对局
static uint8_t s_game_id = 1;    // 联机局号:两边相同 → 各自算出相反的先手方
static uint8_t s_local_side;     // 本机在这一局执的颜色(C4_P1 / C4_P2)
static bool s_link_up;           // 通道就绪
static bool s_link_peer_ready;   // 收到对端 HELLO
static bool s_link_rematch_mine; // 本机已请求再来一局
static bool s_link_rematch_peer; // 对端已请求再来一局
static uint8_t s_link_notice;    // c4_link_notice_t:联机屏当前展示的提示
static bool s_link_notice_sticky;// 粘住提示(故障类)不被“搜索中”覆盖
static bool s_link_start_pending;   // 放锁后再启动 BLE(启动要百毫秒级)
static bool s_link_teardown_pending;// 放锁后再停 BLE
static uint32_t s_link_hello_ms; // 通道就绪后的等待时长(握手看门狗)
static uint32_t s_link_render_ms;

// RTC 内存里的休眠记号:deep sleep 不会掉电这块内存,复位/掉电重启会。
// 用它区分“唤醒”与“被别的复位打回”。
#define C4_SLEEP_MAGIC 0xC45A7E17u
static RTC_DATA_ATTR uint32_t s_sleep_magic;

bool c4_app_take_sleep_magic(void)
{
    const bool from_sleep = (s_sleep_magic == C4_SLEEP_MAGIC);
    s_sleep_magic = 0;
    return from_sleep;
}

// 搜索分片:每 C4_AI_BUDGET_INTERVAL 个节点回调一次。
// 一是查墙钟预算(超出就收工,改用上一层完整结果),二是 vTaskDelay(1) 让出 CPU ——
// 否则一个优先级 3 的任务连续跑几秒会把 IDLE 任务饿到触发 task watchdog。
static bool ai_budget_cb(void *user)
{
    (void)user;
    vTaskDelay(1);
    return esp_timer_get_time() < s_ai_deadline_us;
}

static const c4_level_cfg_t *level_cfg(void)
{
    const uint8_t level = (s_settings.level < C4_LEVEL_COUNT) ? s_settings.level : C4_LEVEL_MEDIUM;
    return &LEVEL_CFG[level];
}

static bool two_player_mode(void)
{
    return s_settings.mode == C4_MODE_TWO_PLAYER;
}

static bool link_mode(void)
{
    return s_settings.mode == C4_MODE_LINK;
}

static void ai_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const c4_game_t snapshot = s_ai_game;   // 通知前已写好,这里独占读
        const uint8_t generation = s_ai_generation;
        const c4_level_cfg_t cfg = *level_cfg();   // 难度在发起搜索时锁定

        c4_ai_opts_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.max_depth = cfg.max_depth;
        opts.max_nodes = C4_APP_AI_MAX_NODES;
        opts.blunder_percent = cfg.blunder_percent;
        opts.budget_cb = ai_budget_cb;

        const int64_t started_us = esp_timer_get_time();
        s_ai_deadline_us = started_us + (int64_t)cfg.budget_ms * 1000;

        const int col = c4_ai_choose(&snapshot, &opts);
        const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;

        // 太快的“秒回”反而像卡了一下;预算内已经够快时补一点停顿。
        if (elapsed_ms < 300) vTaskDelay(pdMS_TO_TICKS(300 - elapsed_ms));

        c4_event_t event;
        memset(&event, 0, sizeof(event));
        event.kind = C4_EVENT_AI_MOVE;
        event.col = col;
        event.generation = generation;
        if (s_input_queue &&
            xQueueSend(s_input_queue, &event, pdMS_TO_TICKS(250)) != pdTRUE) {
            // 队列满(狂按按键)时丢掉这手会让界面永远停在 THINKING,所以要报错。
            ESP_LOGE(TAG, "电脑走法入队失败,输入队列拥塞");
        }

        ESP_LOGI(TAG, "电脑选列=%d 节点=%u 用时=%lldms 深度上限=%u 失误率=%u%%",
                 col, (unsigned)opts.nodes, (long long)elapsed_ms,
                 (unsigned)cfg.max_depth, (unsigned)cfg.blunder_percent);
    }
}

esp_err_t c4_app_start_worker(QueueHandle_t input_queue)
{
    if (!input_queue) return ESP_ERR_INVALID_ARG;

    s_input_queue = input_queue;
    if (xTaskCreate(ai_worker, "c4_ai", C4_APP_AI_TASK_STACK, NULL,
                    C4_APP_AI_TASK_PRIO, &s_ai_task) != pdPASS) {
        s_ai_task = NULL;
        s_ai_ok = false;
        ESP_LOGE(TAG, "电脑对手任务创建失败:只能玩双人模式");
        return ESP_ERR_NO_MEM;
    }
    s_ai_ok = true;
    return ESP_OK;
}

static void render_menu(void)
{
    c4_ui_render_menu(&s_settings);
}

static uint32_t side_color(uint8_t side)
{
    return (side == C4_P1) ? 0xFFC53D : 0xFF5A4E;
}

static void render_board(void)
{
    const char *status = "YOUR TURN";
    uint32_t color = 0xFFC53D;

    if (s_game.status == C4_WIN) {
        if (link_mode()) {
            color = side_color(s_game.winner);
            if (s_link_rematch_mine && !s_link_rematch_peer) status = "WAITING PEER...";
            else if (s_link_rematch_peer && !s_link_rematch_mine) status = "PEER WANTS REMATCH - OK";
            else status = (s_game.winner == s_local_side) ? "YOU WIN!  OK:REMATCH"
                                                          : "PEER WINS  OK:REMATCH";
        } else if (two_player_mode()) {
            status = (s_game.winner == C4_P1) ? "YELLOW WINS  OK:AGAIN" : "RED WINS  OK:AGAIN";
            color = (s_game.winner == C4_P1) ? 0xFFC53D : 0xFF5A4E;
        } else {
            status = (s_game.winner == C4_P1) ? "YOU WIN!  OK:AGAIN" : "AI WINS  OK:AGAIN";
            color = (s_game.winner == C4_P1) ? 0xFFC53D : 0xFF5A4E;
        }
    } else if (s_game.status == C4_DRAW) {
        color = 0xEAF1FF;
        if (!link_mode()) {
            status = "DRAW  OK:AGAIN";
        } else if (s_link_rematch_mine && !s_link_rematch_peer) {
            status = "WAITING PEER...";
        } else if (s_link_rematch_peer && !s_link_rematch_mine) {
            status = "PEER WANTS REMATCH - OK";
        } else {
            status = "DRAW  OK:REMATCH";
        }
    } else if (s_thinking) {
        status = "THINKING...";
        color = 0x8FA3BF;
    } else if (link_mode()) {
        if (s_game.turn == s_local_side) {
            status = "YOUR TURN";
            color = side_color(s_local_side);
        } else {
            status = "PEER TURN";
            color = 0x8FA3BF;
        }
    } else if (two_player_mode()) {
        status = (s_game.turn == C4_P1) ? "YELLOW TURN" : "RED TURN";
        color = (s_game.turn == C4_P1) ? 0xFFC53D : 0xFF5A4E;
    } else if (!s_ai_ok) {
        status = "AI OFFLINE";
        color = 0xFF6B6B;
    }

    // 联机时只有本机回合并局未结束时才画落子光标(避免“看着能下但其实下不了”)。
    const bool local_can_play = !link_mode() || (s_game.turn == s_local_side);
    const int cursor = (s_game.status == C4_ONGOING && !s_thinking && local_can_play) ? s_cursor : -1;
    c4_ui_render(&s_game, cursor, (c4_preview_t)s_settings.preview, status, color);
}

static void start_match(void)
{
    // 先手由模式决定:AI vs HUMAN 让电脑先下;联机由局号决定;其余模式琥珀色(玩家)先下。
    const bool ai_first = (s_settings.mode == C4_MODE_AI_FIRST) && s_ai_ok;
    if (link_mode()) {
        // 本机执什么颜色由局号算出,对端用同一个局号算出相反结果;
        // 四子棋规则里先手固定是 P1,所以本机是 P2 时就是等对端先下。
        s_local_side = c4_link_local_first_side(s_game_id);
        s_link_rematch_mine = false;
        s_link_rematch_peer = false;
        c4_reset(&s_game, C4_P1);
    } else {
        c4_reset(&s_game, ai_first ? C4_P2 : C4_P1);
    }

    s_cursor = C4_COLS / 2;
    s_thinking = false;
    s_generation++;
    s_state = C4_STATE_PLAY;
    c4_sound_play(C4_SOUND_START);
    c4_ui_show_board();

    if (ai_first) {
        s_thinking = true;      // 开局就点亮思考状态并请电脑走一步
        s_ai_pending = true;
    }
    if (link_mode()) {
        ESP_LOGI(TAG, "联机开局 gen=%u 局号=%u 本机颜色=%s",
                 (unsigned)s_generation, (unsigned)s_game_id,
                 s_local_side == C4_P1 ? "P1" : "P2");
    }
    render_board();
}

static void goto_menu(void)
{
    // 退对联机模式:告诉对端一声,再把射频关掉(先置标志,放锁后再停)。
    if (link_mode()) {
        if (c4_link_ready()) c4_link_send_leave();
        s_link_mode = false;
        s_link_teardown_pending = true;
    }
    s_state = C4_STATE_MENU;
    s_thinking = false;
    s_generation++;
    c4_ui_show_menu();
    render_menu();
}

static void after_move(const c4_move_t *move)
{
    if (move->status == C4_WIN) {
        if (link_mode()) c4_sound_play(move->winner == s_local_side ? C4_SOUND_WIN : C4_SOUND_LOSE);
        else if (two_player_mode()) c4_sound_play(C4_SOUND_WIN);
        else c4_sound_play(move->winner == C4_P1 ? C4_SOUND_WIN : C4_SOUND_LOSE);
    } else if (move->status == C4_DRAW) {
        c4_sound_play(C4_SOUND_DRAW);
    }

    if (move->status != C4_ONGOING) {
        s_state = C4_STATE_OVER;
        render_board();
        return;
    }
    if (!link_mode() && !two_player_mode() && s_game.turn == C4_P2 && s_ai_ok) {
        s_thinking = true;
        s_ai_pending = true;
    }
    render_board();
}

// ——— 联机:界面与事件 ————————————————————————————————————————

static void render_link(void)
{
    const char *status = "SEARCHING...";
    const char *hint = "BOTH DEVICES: MODE = LINK PLAY\nLONG OK: BACK TO MENU";
    uint32_t color = 0xEAF1FF;

    switch (s_link_notice) {
    case C4_LINK_NOTICE_CONNECTING:
        status = "CONNECTING...";
        hint = "KEEP BOTH DEVICES CLOSE\nLONG OK: BACK TO MENU";
        break;
    case C4_LINK_NOTICE_HANDSHAKE:
        status = "HANDSHAKE...";
        hint = "PEER FOUND, WAITING FOR ITS HELLO\nLONG OK: BACK TO MENU";
        break;
    case C4_LINK_NOTICE_PEER_LEFT:
        status = "PEER LEFT";
        color = 0xFF6B6B;
        hint = "STILL SEARCHING FOR A PEER\nOK: BACK TO MENU";
        break;
    case C4_LINK_NOTICE_DESYNC:
        status = "LINK DESYNC";
        color = 0xFF6B6B;
        hint = "THE TWO BOARDS DISAGREED, MATCH ABORTED\nOK: BACK TO MENU";
        break;
    case C4_LINK_NOTICE_BAD_VERSION:
        status = "PEER FW MISMATCH";
        color = 0xFF6B6B;
        hint = "BOTH DEVICES MUST RUN THE SAME BUILD\nOK: BACK TO MENU";
        break;
    case C4_LINK_NOTICE_FAILED:
        status = "BLE UNAVAILABLE";
        color = 0xFF6B6B;
        hint = "CHECK THE SERIAL LOG FOR DETAILS\nOK: BACK TO MENU";
        break;
    default:
        break;
    }
    c4_ui_render_link(status, color, hint);
}

// 进入联机屏并展示一个“粘住”的提示(故障或对端离开)。
static void link_show_problem(uint8_t notice)
{
    s_link_notice = notice;
    s_link_notice_sticky = true;
    s_state = C4_STATE_LINK;
    s_thinking = false;
    s_generation++;                 // 丢掉在飞的电脑搜索结果
    c4_ui_show_link();
    render_link();
}

static void start_link_mode(void)
{
    s_link_mode = true;
    s_link_up = false;
    s_link_peer_ready = false;
    s_link_rematch_mine = false;
    s_link_rematch_peer = false;
    s_link_notice = C4_LINK_NOTICE_SEARCHING;
    s_link_notice_sticky = false;
    s_link_hello_ms = 0;
    s_link_render_ms = 0;
    s_state = C4_STATE_LINK;
    s_generation++;
    c4_ui_show_link();
    render_link();
    s_link_start_pending = true;    // BLE 启动要百毫秒级,放锁后再做
}

// 对端已经举过手就沿用它的局号,否则由本机提出下一局。
static void link_request_rematch(void)
{
    if (s_link_rematch_mine) return;
    if (!s_link_rematch_peer) s_game_id++;
    s_link_rematch_mine = true;
    c4_link_send_rematch(s_game_id);
    if (s_link_rematch_peer) start_match();
    else if (s_state == C4_STATE_OVER) render_board();   // 刷新成 “WAITING PEER...”
}

static void on_link_move(uint8_t col, uint8_t ply)
{
    if (s_state != C4_STATE_PLAY || s_game.status != C4_ONGOING) return;

    // ply 是本手之后的落子总数:对不上说明两边棋盘已经不一致,直接作废这一局。
    if (ply != (uint8_t)(s_game.moves + 1) || c4_landing_row(&s_game, (int)col) < 0) {
        ESP_LOGW(TAG, "对端落子非法: col=%u ply=%u 本机已落 %u",
                 (unsigned)col, (unsigned)ply, (unsigned)s_game.moves);
        link_show_problem(C4_LINK_NOTICE_DESYNC);
        return;
    }

    const c4_move_t move = c4_play(&s_game, (int)col);
    if (!move.ok) return;
    c4_sound_play(C4_SOUND_DROP);
    c4_ui_drop_anim(move.col, move.row);
    after_move(&move);
}

static void on_link_event(const c4_link_event_t *event)
{
    if (!s_link_mode) return;   // 已经退出联机模式:残留事件直接丢

    switch (event->kind) {
    case C4_LINK_EVENT_UP:
        s_link_up = true;
        s_link_peer_ready = false;
        s_link_rematch_mine = false;
        s_link_rematch_peer = false;
        s_link_hello_ms = 0;
        s_link_notice = C4_LINK_NOTICE_HANDSHAKE;
        s_link_notice_sticky = false;
        c4_link_send_hello(s_game_id);
        break;

    case C4_LINK_EVENT_DOWN:
        s_link_up = false;
        s_link_peer_ready = false;
        s_link_rematch_mine = false;
        s_link_rematch_peer = false;
        link_show_problem(C4_LINK_NOTICE_PEER_LEFT);
        break;

    case C4_LINK_EVENT_PEER_READY:
        s_link_peer_ready = true;
        // 两边各自带一个局号:取较大值而不是直接采用对端的 —— 否则两边都在
        // “用对方的局号”,会互换成两个不同的值,颜色/先手就对不上了。
        if (event->game_id > s_game_id) s_game_id = event->game_id;
        if (event->first_side == c4_link_local_first_side(s_game_id)) {
            ESP_LOGW(TAG, "对端声明的先手方与本机一致(局号 %u),按本机计算为准",
                     (unsigned)s_game_id);
        }
        // 已经在同一局里打着时忽略重复握手:对端重传的 HELLO 不应该把局面洗掉。
        if (s_state != C4_STATE_PLAY) start_match();
        break;

    case C4_LINK_EVENT_MOVE:
        on_link_move(event->col, event->ply);
        break;

    case C4_LINK_EVENT_REMATCH:
        // 同样取较大值:对端先提过局号时,本机后来的“同意”不能把它改小。
        if (event->game_id > s_game_id) s_game_id = event->game_id;
        s_link_rematch_peer = true;
        if (s_link_rematch_mine) start_match();
        else if (s_state == C4_STATE_OVER) render_board();
        break;

    case C4_LINK_EVENT_LEAVE:
        ESP_LOGI(TAG, "对端主动离开");
        link_show_problem(C4_LINK_NOTICE_PEER_LEFT);
        break;

    case C4_LINK_EVENT_BAD_VERSION:
        ESP_LOGW(TAG, "对端协议版本不一致: %u", event->version);
        link_show_problem(C4_LINK_NOTICE_BAD_VERSION);
        break;

    case C4_LINK_EVENT_FAILED:
        link_show_problem(C4_LINK_NOTICE_FAILED);
        break;

    default:
        break;
    }
}

static void link_drain_events(void)
{
    c4_link_event_t event;
    while (c4_link_next_event(&event)) on_link_event(&event);
}

// 按链路实际状态更新联机屏上的“搜索中/连接中/握手中”。
static void link_update_notice(void)
{
    if (s_link_notice_sticky) return;

    switch (c4_link_state()) {
    case C4_LINK_STATE_CONNECTING:
        s_link_notice = C4_LINK_NOTICE_CONNECTING;
        break;
    case C4_LINK_STATE_READY:
        s_link_notice = C4_LINK_NOTICE_HANDSHAKE;
        break;
    case C4_LINK_STATE_FAILED:
        s_link_notice = C4_LINK_NOTICE_FAILED;
        s_link_notice_sticky = true;
        break;
    default:
        s_link_notice = C4_LINK_NOTICE_SEARCHING;
        break;
    }
}

// 通道已就绪却一直收不到对端 HELLO:多半是对端固件版本不同或卡住了。
static void link_watchdog(uint32_t elapsed_ms)
{
    if (!s_link_mode) return;
    if (!s_link_up || s_link_peer_ready) {
        s_link_hello_ms = 0;
        return;
    }

    s_link_hello_ms += elapsed_ms;
    if (s_link_hello_ms < C4_APP_LINK_HELLO_TIMEOUT_MS) return;

    ESP_LOGW(TAG, "通道就绪但 %ums 未收到对端 HELLO", (unsigned)s_link_hello_ms);
    link_show_problem(C4_LINK_NOTICE_PEER_LEFT);
}

// 启动/停止 BLE 都要百毫秒级,统一放在放锁之后执行。
static void link_service_pending(void)
{
    // 先停再起:同一轮里两者都挂起时,顺序反了会把刚起来的射频又关掉。
    if (s_link_teardown_pending) {
        s_link_teardown_pending = false;
        (void)c4_link_stop();
        s_link_up = false;
        s_link_peer_ready = false;
    }
    if (s_link_start_pending) {
        s_link_start_pending = false;
        const esp_err_t err = c4_link_start();
        // INVALID_STATE = 已经在跑(例如上一次联机还没停完),不算故障。
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            s_link_notice = C4_LINK_NOTICE_FAILED;
            s_link_notice_sticky = true;
        }
    }
}

static void move_cursor(int step)
{
    const int next = c4_seek_column(&s_game, s_cursor, step);
    if (next >= 0 && next != s_cursor) {
        s_cursor = next;
        c4_sound_play(C4_SOUND_MOVE);
        render_board();
    }
}

static void human_drop(void)
{
    if (link_mode() && s_game.turn != s_local_side) return;   // 不是本机的回合

    const int col = s_cursor;
    if (c4_landing_row(&s_game, col) < 0) return;

    const c4_move_t move = c4_play(&s_game, col);
    if (!move.ok) return;

    c4_sound_play(C4_SOUND_DROP);
    c4_ui_drop_anim(move.col, move.row);
    // move 已生效,此时的 moves 就是这手之后的落子总数(给对端校验失步用)。
    if (link_mode()) c4_link_send_move((uint8_t)move.col, s_game.moves);
    after_move(&move);
}

// 横屏持握时的方向约定:设备顺时针转 90 度,「上」键在右手边,所以:
//   * 对局中:UP 往右移列,DOWN 往左;
//   * 设置屏列表:右手边的 UP 往下走一行(与对局里“UP = 朝屏幕前进方向”一致)。
// 按竖屏直觉给 UP 配“往上”实测手感是反的,所以列表方向与光标方向在这里统一。
#define C4_CURSOR_STEP_UP   (+1)
#define C4_CURSOR_STEP_DOWN (-1)
#define C4_MENU_STEP_UP     (+1)
#define C4_MENU_STEP_DOWN   (-1)

static void on_menu_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        c4_settings_move(&s_settings, C4_MENU_STEP_UP);
        render_menu();
    } else if (btn == BSP_BTN_DOWN) {
        c4_settings_move(&s_settings, C4_MENU_STEP_DOWN);
        render_menu();
    } else if (btn == BSP_BTN_OK) {
        if (c4_settings_is_start(&s_settings)) {
            if (link_mode()) start_link_mode();
            else start_match();
            return;
        }
        c4_settings_cycle(&s_settings);
        c4_sound_play(C4_SOUND_MOVE);
        render_menu();
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    switch (s_state) {
    case C4_STATE_MENU:
        on_menu_key(btn, ev);
        break;

    case C4_STATE_LINK:
        // 联机屏不需要手动确认开局:握手完成会自动开局。
        // 长按确定退出联机模式;故障提示下短按确定也退回去。
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            goto_menu();
            return;
        }
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK && s_link_notice_sticky) {
            goto_menu();
        }
        break;

    case C4_STATE_PLAY:
        // 长按确定先处理:即使正在思考,也让玩家能退出去(在飞的搜索结果会被代号丢掉)。
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            goto_menu();
            return;
        }
        if (s_thinking) return;                                   // 电脑思考中忽略其他按键
        if (ev != BSP_BTN_CLICK) return;
        if (link_mode() && s_game.turn != s_local_side) {
            // 对端回合:光标可以动(方便提前看),但不允许落子。
            if (btn == BSP_BTN_UP) move_cursor(C4_CURSOR_STEP_UP);
            else if (btn == BSP_BTN_DOWN) move_cursor(C4_CURSOR_STEP_DOWN);
            return;
        }
        if (btn == BSP_BTN_UP) move_cursor(C4_CURSOR_STEP_UP);
        else if (btn == BSP_BTN_DOWN) move_cursor(C4_CURSOR_STEP_DOWN);
        else if (btn == BSP_BTN_OK) human_drop();
        break;

    case C4_STATE_OVER:
        if (btn != BSP_BTN_OK) return;
        if (ev == BSP_BTN_CLICK) {
            if (link_mode()) link_request_rematch();
            else start_match();
        } else if (ev == BSP_BTN_LONG) {
            goto_menu();
        }
        break;
    }
}

static void on_ai_move(const c4_event_t *event)
{
    if (!s_thinking) return;                             // 过期的结果(例如中途被重置)
    if (event->generation != s_generation) {
        ESP_LOGW(TAG, "丢弃过期电脑动作 gen=%u(当前 %u)",
                 (unsigned)event->generation, (unsigned)s_generation);
        return;
    }

    s_thinking = false;
    const int col = event->col;
    if (c4_landing_row(&s_game, col) < 0) {
        ESP_LOGW(TAG, "电脑给了非法列 %d,忽略", col);
        render_board();
        return;
    }

    const c4_move_t move = c4_play(&s_game, col);
    if (!move.ok) {
        render_board();
        return;
    }
    c4_sound_play(C4_SOUND_DROP);
    c4_ui_drop_anim(move.col, move.row);
    after_move(&move);
}

void c4_app_init(void)
{
    c4_settings_init(&s_settings);
    c4_reset(&s_game, C4_P1);
    s_cursor = C4_COLS / 2;
    s_state = C4_STATE_MENU;
    s_thinking = false;
    s_ai_pending = false;
    s_generation = 1;
    s_ai_generation = 1;
    s_idle_ms = 0;

    s_link_mode = false;
    s_link_up = false;
    s_link_peer_ready = false;
    s_link_rematch_mine = false;
    s_link_rematch_peer = false;
    s_link_notice = C4_LINK_NOTICE_SEARCHING;
    s_link_notice_sticky = false;
    s_link_start_pending = false;
    s_link_teardown_pending = false;
    s_link_hello_ms = 0;
    s_link_render_ms = 0;
    s_game_id = 1;
    s_local_side = C4_P1;

    c4_ui_battery_start();
    c4_ui_show_menu();
    render_menu();

    s_ai_ready = true;
}

bool c4_app_idle_tick(uint32_t elapsed_ms)
{
    if (!s_ai_ready) return false;

    // 联机:解码收到的报文并推进重传计时。每轮循环都要调 —— 事件密集时队列
    // 会连续非空,重传计时不能因此停滞。
    c4_link_pump(elapsed_ms);

    if (bsp_lvgl_lock(C4_APP_LVGL_LOCK_MS)) {
        link_drain_events();
        link_watchdog(elapsed_ms);
        if (s_state == C4_STATE_LINK) {
            s_link_render_ms += elapsed_ms;
            if (s_link_render_ms >= C4_APP_LINK_RENDER_MS) {
                s_link_render_ms = 0;
                link_update_notice();
                render_link();
            }
        }
        bsp_lvgl_unlock();
    }
    link_service_pending();

    s_idle_ms += elapsed_ms;
    uint32_t limit = (s_state == C4_STATE_PLAY || s_state == C4_STATE_OVER)
                         ? C4_APP_IDLE_PLAY_MS
                         : C4_APP_IDLE_MENU_MS;
    // 联机对阵期间睡着等于断线,给一个更长的窗口。
    if (s_link_mode && (s_link_up || s_state == C4_STATE_PLAY || s_state == C4_STATE_OVER)) {
        limit = C4_APP_IDLE_LINK_MS;
    }
    return s_idle_ms >= limit;
}

static void log_shutdown_step(const char *step, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "休眠前 %s 失败: %s", step, esp_err_to_name(err));
    }
}

// 低电平唤醒的坑:按键脚在被 ADC 接管后没有内部上拉,进入 deep sleep 时如果读成低电平,
// 唤醒条件在入睡瞬间就已成立,设备会立刻醒回来(实测就是这样)。入睡前显式补上拉。
// C3 没有 RTC IO API(SOC_RTCIO_PIN_COUNT=0),所以直接用普通 GPIO 上拉 —— 它会跟着
// 进入睡眠态;与板上 10k 外部上拉并联,松开态仍为高。
void c4_app_enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)", (unsigned)s_idle_ms);

    // 唤醒源配不上就干脆不睡,否则睡下去就再也醒不来。
    const esp_err_t wake_err = esp_deep_sleep_enable_gpio_wakeup(
        1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        s_idle_ms = 0;
        return;
    }

    // 在 RTC 内存里留记号:唤醒重启后据此区分“真·deep sleep 唤醒”与“复位/掉电重启”。
    s_sleep_magic = C4_SLEEP_MAGIC;

    // 联机时直接睡下去对端只会看到断线;先停掉 BLE 释放射频再走既有流程。
    log_shutdown_step("BLE link stop", c4_link_stop());

    // I2S 写入必须在 ES8311 暂停前结束,所以先等当前音效收尾。
    if (!c4_sound_hold_for_sleep()) {
        ESP_LOGW(TAG, "音效仍在写入,本次不休眠");
        return;
    }

    // 先把提示刷上屏,再依次停外设 —— 用户能看到设备是去睡了而不是卡住。
    if (bsp_lvgl_lock(C4_APP_LVGL_LOCK_MS)) {
        c4_ui_show_sleeping();
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(C4_APP_SLEEP_NOTICE_MS));

    // 与 demo 验证过的顺序一致:CW2017 与 ES8311 共用 I2C,先完成电量计写入/回读。
    log_shutdown_step("CW2017 suspend", bsp_battery_sleep());
    log_shutdown_step("ES8311 suspend", bsp_audio_sleep());
    // 即使 codec 寄存器操作失败,也继续停时钟并释放引脚。
    log_shutdown_step("I2S pin release", bsp_audio_prepare_deep_sleep());
    // 按键:释放 ADC 并把唤醒脚切回带上拉的数字输入(否则会立即自唤醒)。
    int wake_level = 0;
    log_shutdown_step("button release", bsp_button_prepare_deep_sleep(&wake_level));
    log_shutdown_step("shared I2C pin release", bsp_i2c_prepare_deep_sleep());

    // 到这里按键已经不可用:按键脚读回低电平说明有人按着,宁愿重启也不要刚睡就醒。
    if (wake_level == 0) {
        ESP_LOGW(TAG, "唤醒脚仍为低电平,放弃本次休眠并重启");
        esp_restart();
    }

    // 持锁挡住后续刷屏,然后停 ST7789 并进休眠。
    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "deep sleep 前无法停止 LVGL 刷屏,重启恢复外设");
        esp_restart();
    }
    log_shutdown_step("ST7789 suspend", bsp_display_prepare_deep_sleep());

    esp_deep_sleep_start();
    // 从 deep-sleep 准备接口返回后总线已不可在本次运行中恢复。
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回,重启恢复外设");
    esp_restart();
}

void c4_app_handle_event(const c4_event_t *event)
{
    if (!event || !s_ai_ready) return;

    s_idle_ms = 0;   // 任何事件都算“有人在场”

    if (!bsp_lvgl_lock(C4_APP_LVGL_LOCK_MS)) {
        ESP_LOGW(TAG, "拿不到 LVGL 锁,丢弃事件 kind=%d", (int)event->kind);
        return;
    }

    if (event->kind == C4_EVENT_AI_MOVE) {
        on_ai_move(event);
    } else {
        on_key(event->btn, event->ev);
    }

    bsp_lvgl_unlock();

    // 联机的启停(百毫秒级)放在放锁之后。
    link_service_pending();

    // 起 worker 必须在放锁之后:搜索要跑几百毫秒,不能占着 LVGL 锁。
    if (s_ai_pending) {
        s_ai_pending = false;
        if (s_ai_task) {
            s_ai_game = s_game;
            s_ai_generation = s_generation;
            xTaskNotifyGive(s_ai_task);
        }
    }
}
