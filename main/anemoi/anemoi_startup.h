// main/anemoi/anemoi_startup.h —— 启动顺序（纯逻辑，可 host 测；不依赖 LVGL/ESP-IDF）。
//
// 合同：
//   * LVGL 锁失败 → 不初始化、不点背光、不报 ready；
//   * renderer 初始化失败 → 解锁、不点背光、不报 ready；
//   * 资源包/应用初始化失败 → 若显示可用则画一次错误屏（由 ops->error_screen 提供），
//     再点背光让人看得见，绝不“静默暗屏”；不报 ready；
//   * 首帧提交失败 → 不点背光、不报 ready；
//   * 只有 present() 成功（LVGL 提交，不代表物理 flush 完成）后才点背光、才 ready；
//   * 输入队列/任务/按键初始化失败 → 不报 ready。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANEM_START_OK = 0,
    ANEM_START_LOCK_FAIL,
    ANEM_START_RENDER_FAIL,
    ANEM_START_APP_FAIL,
    ANEM_START_PRESENT_FAIL,
    ANEM_START_INPUT_FAIL,
} anem_start_result_t;

typedef struct {
    bool (*lvgl_lock)(int timeout_ms);
    void (*lvgl_unlock)(void);
    bool (*render_init)(void);          // 需在锁内调用
    bool (*app_init)(void);             // 解析资源包
    bool (*present)(void);              // 提交首帧；true = LVGL 已接收
    void (*backlight)(int percent);
    bool (*input_start)(void);          // 队列 + 按键 + 任务（失败要回滚）
    bool (*error_screen)(const char *msg);  // 返回 true 仅当错误屏真的提交成功
    void (*log)(const char *msg);
    bool (*set_landscape)(void);        // 可空：在锁内、render_init 前调用，返回 setter 结果
} anem_startup_ops_t;

// 返回 true 表示 ready（首帧已提交、输入已就绪）。
anem_start_result_t anem_startup_run(const anem_startup_ops_t *ops);

const char *anem_start_result_name(anem_start_result_t r);

#ifdef __cplusplus
}
#endif
