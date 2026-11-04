# 《命运石之门 0》阅读器:只编译本应用的源码。
set(STEINS0_PACK_FILE "${CMAKE_CURRENT_LIST_DIR}/../assets/steins0/steins0_pack.bin" CACHE FILEPATH
    "Steins;Gate 0 资源包(tools/steins0_pack.py 生成)")
if(NOT EXISTS "${STEINS0_PACK_FILE}")
    message(FATAL_ERROR "找不到 ${STEINS0_PACK_FILE}，先运行 tools/steins0_pack.py")
endif()

idf_component_register(
    SRCS "main.c"
         "steins0_pack.c"
         "steins0_text.c"
         "steins0_engine.c"
         "steins0_image.c"
         "steins0_ui.c"
         "steins0_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    EMBED_FILES "${STEINS0_PACK_FILE}"
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/steins0_font16.c"
)
