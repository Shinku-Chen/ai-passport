// main/c4_screenshot.h —— FAP_SCREENSHOT_V1 串口截图(发布到社区需要“设备真实画面”)。
//
// 协议:主机在 USB-CDC 上发一行 `FAP_SCREENSHOT_V1\n`,设备回
//   `FAP_SCREENSHOT_V1 <width> <height> RGB565LE <bytes>\n` + 恰好 <bytes> 字节 LE RGB565。
// 实现要点与踩坑记录见 docs/reference/y2lin/serial-screenshot-protocol.md:
//   * 必须先 usb_serial_jtag_driver_install() + usb_serial_jtag_vfs_use_driver(),
//     否则读命令会踩空驱动对象;
//   * 全屏快照缓冲必须在编译期静态预留(150KB 在运行时堆里拿不到连续块);
//   * 负载要按 tx 环形缓冲大小分块写,二进制窗口内必须静音日志。
//
// 只读命令:不重启、不刷机、不改设置。不截图时几乎不占 CPU。
#pragma once

#include "esp_err.h"

// 截图功能会在 .bss 里静态预留一帧完整画面(320x240 RGB565 = 150KB,无 PSRAM 的板子
// 上堆里拿不到这么大的连续块,所以必须静态留)。默认开启;想把这 150KB 还给堆时,
// 用 -DC4_ENABLE_SCREENSHOT=0 构建(此时 c4_screenshot_start() 直接返回不支持)。
#ifndef C4_ENABLE_SCREENSHOT
#define C4_ENABLE_SCREENSHOT 1
#endif

// 启动截图服务(装 USB-serial-JTAG 驱动 + 读命令任务)。失败只返回错误,游戏照常运行。
esp_err_t c4_screenshot_start(void);
