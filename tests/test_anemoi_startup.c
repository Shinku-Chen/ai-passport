// tests/test_anemoi_startup.c —— 启动顺序/失败合同（mock 驱动；不证明真实 LVGL/pool/屏幕）。
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "anemoi_startup.h"

typedef struct {
    int lock_calls, unlock_calls, render_calls, app_calls, present_calls, backlight_calls,
        input_calls, error_calls, log_calls;
    bool lock_ok, render_ok, app_ok, present_ok, input_ok;
    bool error_ok, landscape_ok, landscape_called;
    bool backlight_before_present;
    int backlight_after_present;
    bool ready;
    const char *error_msg;
} mock_t;

static mock_t g;

static bool m_lock(int ms) {
    (void)ms;
    g.lock_calls++;
    return g.lock_ok;
}
static void m_unlock(void) { g.unlock_calls++; }
static bool m_render(void) {
    g.render_calls++;
    return g.render_ok;
}
static bool m_app(void) {
    g.app_calls++;
    return g.app_ok;
}
static bool m_present(void) {
    g.present_calls++;
    if (g.backlight_calls) {
        g.backlight_before_present = true;
    }
    return g.present_ok;
}
static void m_backlight(int pct) {
    (void)pct;
    g.backlight_calls++;
    if (g.present_calls) {
        g.backlight_after_present++;
    }
}
static bool m_input(void) {
    g.input_calls++;
    return g.input_ok;
}
static bool m_error(const char *msg) {
    g.error_calls++;
    g.error_msg = msg;
    return g.error_ok;
}
static bool m_landscape(void) {
    g.landscape_called = true;
    return g.landscape_ok;
}
static void m_log(const char *msg) {
    (void)msg;
    g.log_calls++;
}

static void reset(void) {
    memset(&g, 0, sizeof(g));
    g.lock_ok = g.render_ok = g.app_ok = g.present_ok = g.input_ok = true;
    g.error_ok = true;
    g.landscape_ok = true;
}

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);        \
            fails++;                                                     \
        }                                                                \
    } while (0)

int main(void) {
    static const anem_startup_ops_t ops = {
        m_lock, m_unlock, m_render, m_app, m_present, m_backlight, m_input, m_error, m_log,
        m_landscape,
    };
    int fails = 0;

    // 正常路径：先 present，后 backlight，ready
    reset();
    CHECK(anem_startup_run(&ops) == ANEM_START_OK);
    CHECK(g.present_calls == 1);
    CHECK(g.backlight_calls == 1);
    CHECK(g.backlight_after_present == 1 && !g.backlight_before_present);
    CHECK(g.error_calls == 0);
    CHECK(g.lock_calls == 1 && g.unlock_calls == 1);

    // 锁失败：不初始化、不点背光、不 present
    reset();
    g.lock_ok = false;
    CHECK(anem_startup_run(&ops) == ANEM_START_LOCK_FAIL);
    CHECK(g.render_calls == 0 && g.app_calls == 0 && g.present_calls == 0);
    CHECK(g.backlight_calls == 0 && g.unlock_calls == 0);

    // 横屏设置失败（esp_err_t 当 bool 的回归）：必须报 render-init-failed，且不点背光
    reset();
    g.landscape_ok = false;
    CHECK(anem_startup_run(&ops) == ANEM_START_RENDER_FAIL);
    CHECK(g.unlock_calls == 1 && g.backlight_calls == 0 && g.present_calls == 0);

    // 渲染失败：解锁、不点背光
    reset();
    g.render_ok = false;
    CHECK(anem_startup_run(&ops) == ANEM_START_RENDER_FAIL);
    CHECK(g.unlock_calls == 1 && g.backlight_calls == 0 && g.present_calls == 0);

    // 坏包：显示可用 → 必须出现可见错误屏 + 点背光，但绝不 ready
    reset();
    g.app_ok = false;
    CHECK(anem_startup_run(&ops) == ANEM_START_APP_FAIL);
    CHECK(g.error_calls == 1 && g.error_msg != NULL && g.error_msg[0] != '\0');
    CHECK(g.backlight_calls == 1 && g.present_calls == 0);
    CHECK(g.landscape_called && g.error_calls == 1);

    // 坏包 + 错误屏提交失败：不得点背光（不假装可见）
    reset();
    g.app_ok = false;
    g.error_ok = false;
    CHECK(anem_startup_run(&ops) == ANEM_START_APP_FAIL);
    CHECK(g.backlight_calls == 0);

    // 首帧失败：不点背光、不 ready
    reset();
    g.present_ok = false;
    CHECK(anem_startup_run(&ops) == ANEM_START_PRESENT_FAIL);
    CHECK(g.backlight_calls == 0);

    // 输入初始化失败：背光已开（画面在），但 ready 不能报
    reset();
    g.input_ok = false;
    CHECK(anem_startup_run(&ops) == ANEM_START_INPUT_FAIL);
    CHECK(g.backlight_calls == 1 && g.present_calls == 1);

    // NULL ops 防御
    CHECK(anem_startup_run(NULL) == ANEM_START_LOCK_FAIL);

    printf("test_anemoi_startup: %s (%d failures)\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}
