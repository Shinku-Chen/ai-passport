idf_component_register(
    SRCS "src/dlna_audio.c"
    INCLUDE_DIRS "include"
    # minimp3.h 是单头库:只作为实现用,不暴露给外部(避免多处 IMPLEMENTATION)。
    # esp_aac_dec/esp_mp3_dec 头来自 esp_audio_codec 组件。
    PRIV_REQUIRES bsp esp_http_client esp_timer esp_audio_codec
)
