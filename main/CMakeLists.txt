# 四子棋应用:只编译游戏自己的源码。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里(可随时切回硬件自检 demo),
# 但不再参与本应用的构建,也不再注册到菜单。
idf_component_register(
    SRCS "main.c"
         "c4_model.c"
         "c4_ai.c"
         "c4_layout.c"
         "c4_settings.c"
         "c4_ui.c"
         "c4_sound.c"
         "c4_screenshot.c"
         "c4_link_proto.c"
         "c4_link.c"
         "c4_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_hw_support esp_timer esp_driver_usb_serial_jtag bt
)

# 联机对战要占 73KB 左右的堆;串口截图要在 .bss 里静态预留一整屏
# (320x240x2 = 150KB)。无 PSRAM 的 C3 装不下两份,所以默认发布的是
# “可联机、不带截图”的固件,截图作为开发/出封面用的可选构建:
#   idf.py -DC4_ENABLE_SCREENSHOT=ON build
option(C4_ENABLE_SCREENSHOT "Keep the 150KB serial screenshot buffer (dev/cover builds)" OFF)
if(C4_ENABLE_SCREENSHOT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C4_ENABLE_SCREENSHOT=1)
    message(STATUS "C4 screenshot ON: 150KB internal RAM reserved; the BLE link cannot start")
endif()
