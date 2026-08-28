idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "dlna_app.c"
         "net_prov.c"
         "lv_font_cjk_16.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             custom_dlna dlna_audio miplay esp_http_server esp_system dns_server
)
