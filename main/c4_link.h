// main/c4_link.h —— 两机蓝牙联机(可行性验证阶段)。
//
// 阶段目标不是把联机做完,而是先用最小代价验证一件事:在这块无 PSRAM 的板子上,
// “BLE 外设 + 连接 + 双向收发 + LVGL/音频同时跑”能不能稳住。所以这里只实现:
//   * 广播一个自定义 GATT 服务(Nordic UART 布局,便于用通用工具/脚本当对端);
//   * 连上后通知一个握手字节,并把收到的字节回显(证明双向链路);
//   * 把连接状态与收到的列号显示在设置屏提示行上。
// 对端用电脑上的 BLE central 脚本扮演,单台设备即可验证链路。
//
// 默认关闭:发布固件不包含它(也不含它带来的内存开销)。打开方式见 main/CMakeLists.txt
// 的 C4_LINK_EXPERIMENT 选项(会同时关掉 150KB 的串口截图缓冲,腾出堆空间)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifndef C4_ENABLE_LINK
#define C4_ENABLE_LINK 0
#endif

// 启动 BLE 外设并开始广播。未编译进本构建时返回 ESP_ERR_NOT_SUPPORTED。
esp_err_t c4_link_start(void);

// 当前是否有对端连接。
bool c4_link_connected(void);
