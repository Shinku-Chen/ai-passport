// main/anemoi/anemoi_startup.c —— 启动顺序实现（无 LVGL/ESP-IDF 依赖）。
#include "anemoi_startup.h"

#include <stddef.h>

#define STARTUP_LOCK_MS 1000
#define STARTUP_BACKLIGHT 100

const char *anem_start_result_name(anem_start_result_t r)
{
    switch (r) {
    case ANEM_START_OK:
        return "ok";
    case ANEM_START_LOCK_FAIL:
        return "lvgl-lock-failed";
    case ANEM_START_RENDER_FAIL:
        return "render-init-failed";
    case ANEM_START_APP_FAIL:
        return "pack-init-failed";
    case ANEM_START_PRESENT_FAIL:
        return "first-frame-failed";
    case ANEM_START_INPUT_FAIL:
        return "input-init-failed";
    default:
        return "unknown";
    }
}

anem_start_result_t anem_startup_run(const anem_startup_ops_t *ops)
{
    if (ops == NULL || ops->lvgl_lock == NULL || ops->lvgl_unlock == NULL ||
        ops->render_init == NULL || ops->app_init == NULL || ops->present == NULL ||
        ops->backlight == NULL || ops->input_start == NULL) {
        return ANEM_START_LOCK_FAIL;
    }

    // 1) 屏幕 + LVGL 必须真的拿到锁，失败就当作显示不可用，不点背光。
    if (!ops->lvgl_lock(STARTUP_LOCK_MS)) {
        if (ops->log) {
            ops->log("LVGL 加锁失败，显示不可用");
        }
        return ANEM_START_LOCK_FAIL;
    }
    bool landscape_ok = true;
    if (ops->set_landscape) {
        landscape_ok = ops->set_landscape();
    }
    const bool render_ok = landscape_ok && ops->render_init();
    ops->lvgl_unlock();
    if (!render_ok) {
        if (ops->log) {
            ops->log("渲染层初始化失败");
        }
        return ANEM_START_RENDER_FAIL;
    }

    // 2) 资源包解析：显示已经可用，失败要给出可见错误（而不是暗屏 return）。
    if (!ops->app_init()) {
        if (ops->log) {
            ops->log("资源包初始化失败");
        }
        const bool shown = ops->error_screen ? ops->error_screen("资源包损坏\n无法启动") : false;
        if (!shown) {
            if (ops->log) {
                ops->log("错误屏提交失败，保持熄屏");
            }
            return ANEM_START_APP_FAIL;   // 不点亮空屏，不假装可见
        }
        ops->backlight(STARTUP_BACKLIGHT);
        return ANEM_START_APP_FAIL;
    }

    // 3) 首帧提交成功后才点背光；失败则保持暗屏并报错。
    if (!ops->present()) {
        if (ops->log) {
            ops->log("首帧提交失败");
        }
        return ANEM_START_PRESENT_FAIL;
    }
    ops->backlight(STARTUP_BACKLIGHT);

    // 4) 输入（队列+任务+按键）失败不能报 ready。
    if (!ops->input_start()) {
        if (ops->log) {
            ops->log("输入初始化失败");
        }
        return ANEM_START_INPUT_FAIL;
    }
    return ANEM_START_OK;
}
