// main/demo.h —— 当前应用的统一接口(本固件只保留「今天吃啥」)。
#pragma once

#include "bsp_button.h"

typedef struct {
    const char *name;
    void (*enter)(void);                          // 建自己的屏并载入
    void (*exit)(void);                           // 删屏、停定时器、释放资源
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);  // 收按键
} demo_entry_t;

// 当前应用(定义在 demo_eat_what.c)
void demo_eat_what_enter(void); void demo_eat_what_exit(void);
void demo_eat_what_key(bsp_btn_t btn, bsp_btn_ev_t ev);
