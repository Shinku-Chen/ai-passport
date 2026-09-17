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
         "c4_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_hw_support esp_timer esp_driver_usb_serial_jtag
)
