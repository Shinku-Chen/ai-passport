idf_component_register(
    SRCS "miplay.c"
    INCLUDE_DIRS "include"
    REQUIRES mdns lwip esp_wifi esp_netif esp_timer mbedtls
             esp_driver_gpio
             bsp esp_audio_codec
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
