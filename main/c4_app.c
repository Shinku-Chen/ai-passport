// main/c4_app.c —— 四子棋控制器实现:设置菜单 + 对局状态机 + 电脑对手 worker + 空闲休眠。
#include "c4_app.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "c4_ai.h"
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

// 空闲休眠阈值:设置屏短一点,对局中给足思考时间。
#define C4_APP_IDLE_MENU_MS     60000u
#define C4_APP_IDLE_PLAY_MS     180000u
#define C4_APP_SLEEP_NOTICE_MS  400     // 先让 “SLEEPING” 刷上屏再熄背光

typedef enum {
    C4_STATE_MENU = 0,
    C4_STATE_PLAY,
    C4_STATE_OVER,
} c4_state_t;

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

static void render_board(void)
{
    const char *status = "YOUR TURN";
    uint32_t color = 0xFFC53D;

    if (s_game.status == C4_WIN) {
        if (two_player_mode()) {
            status = (s_game.winner == C4_P1) ? "YELLOW WINS  OK:AGAIN" : "RED WINS  OK:AGAIN";
        } else {
            status = (s_game.winner == C4_P1) ? "YOU WIN!  OK:AGAIN" : "AI WINS  OK:AGAIN";
        }
        color = (s_game.winner == C4_P1) ? 0xFFC53D : 0xFF5A4E;
    } else if (s_game.status == C4_DRAW) {
        status = "DRAW  OK:AGAIN";
        color = 0xEAF1FF;
    } else if (s_thinking) {
        status = "THINKING...";
        color = 0x8FA3BF;
    } else if (two_player_mode()) {
        status = (s_game.turn == C4_P1) ? "YELLOW TURN" : "RED TURN";
        color = (s_game.turn == C4_P1) ? 0xFFC53D : 0xFF5A4E;
    } else if (!s_ai_ok) {
        status = "AI OFFLINE";
        color = 0xFF6B6B;
    }

    const int cursor = (s_game.status == C4_ONGOING && !s_thinking) ? s_cursor : -1;
    c4_ui_render(&s_game, cursor, (c4_preview_t)s_settings.preview, status, color);
}

static void start_match(void)
{
    c4_reset(&s_game, C4_P1);   // 先手固定:人机模式玩家先手,双人模式琥珀色先手
    s_cursor = C4_COLS / 2;
    s_thinking = false;
    s_generation++;
    s_state = C4_STATE_PLAY;
    c4_sound_play(C4_SOUND_START);
    c4_ui_show_board();
    render_board();
}

static void goto_menu(void)
{
    s_state = C4_STATE_MENU;
    s_thinking = false;
    s_generation++;
    c4_ui_show_menu();
    render_menu();
}

static void after_move(const c4_move_t *move)
{
    if (move->status == C4_WIN) {
        if (two_player_mode()) c4_sound_play(C4_SOUND_WIN);
        else c4_sound_play(move->winner == C4_P1 ? C4_SOUND_WIN : C4_SOUND_LOSE);
    } else if (move->status == C4_DRAW) {
        c4_sound_play(C4_SOUND_DRAW);
    }

    if (move->status != C4_ONGOING) {
        s_state = C4_STATE_OVER;
        render_board();
        return;
    }
    if (!two_player_mode() && s_game.turn == C4_P2 && s_ai_ok) {
        s_thinking = true;
        s_ai_pending = true;
    }
    render_board();
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
    const int col = s_cursor;
    if (c4_landing_row(&s_game, col) < 0) return;

    const c4_move_t move = c4_play(&s_game, col);
    if (!move.ok) return;

    c4_sound_play(C4_SOUND_DROP);
    c4_ui_drop_anim(move.col, move.row);
    after_move(&move);
}

// 上下键在横屏下就是左右移动:UP 在右手边(设备顺时针转 90 度持握),所以 UP 往右。
#define C4_CURSOR_STEP_UP   (+1)
#define C4_CURSOR_STEP_DOWN (-1)

static void on_menu_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        c4_settings_move(&s_settings, -1);
        render_menu();
    } else if (btn == BSP_BTN_DOWN) {
        c4_settings_move(&s_settings, +1);
        render_menu();
    } else if (btn == BSP_BTN_OK) {
        if (c4_settings_is_start(&s_settings)) {
            start_match();
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

    case C4_STATE_PLAY:
        // 长按确定先处理:即使正在思考,也让玩家能退出去(在飞的搜索结果会被代号丢掉)。
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            goto_menu();
            return;
        }
        if (s_thinking) return;                                   // 电脑思考中忽略其他按键
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) move_cursor(C4_CURSOR_STEP_UP);
        else if (btn == BSP_BTN_DOWN) move_cursor(C4_CURSOR_STEP_DOWN);
        else if (btn == BSP_BTN_OK) human_drop();
        break;

    case C4_STATE_OVER:
        if (btn != BSP_BTN_OK) return;
        if (ev == BSP_BTN_CLICK) start_match();
        else if (ev == BSP_BTN_LONG) goto_menu();
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

    c4_ui_battery_start();
    c4_ui_show_menu();
    render_menu();

    s_ai_ready = true;
}

bool c4_app_idle_tick(uint32_t elapsed_ms)
{
    if (!s_ai_ready) return false;

    s_idle_ms += elapsed_ms;
    const uint32_t limit = (s_state == C4_STATE_PLAY) ? C4_APP_IDLE_PLAY_MS
                                                      : C4_APP_IDLE_MENU_MS;
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
