idf_component_register(
    SRCS "main.c"
         "oc_proto.c"
         "oc_audio.c"
         "oc_link.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
)

# ESP-IDF 5.5.3 只把 WRITE_ENC 应用到自动生成的 CCCD;包一层注册边界让受保护的 CCCD
# 也拿到 READ_ENC,否则手机读不到 CCCD、表现为"订阅不上通知"。
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_att_svr_register")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,__wrap_ble_att_svr_register")
