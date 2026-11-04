# 《命运石之门》阅读器：只编译本应用的源码。
# 资源包由 tools/steins_pack.py 生成后 EMBED 进应用(整包只读,运行时按偏移读表)。
set(STEINS_PACK_FILE "${CMAKE_CURRENT_LIST_DIR}/../assets/steins/steins_pack.bin" CACHE FILEPATH
    "Steins 资源包（tools/steins_pack.py 生成）")
if(NOT EXISTS "${STEINS_PACK_FILE}")
    message(FATAL_ERROR "找不到 ${STEINS_PACK_FILE}，先运行 tools/steins_pack.py")
endif()

idf_component_register(
    SRCS "main.c"
         "steins_pack.c"
         "steins_model.c"
         "steins_image.c"
         "steins_ui.c"
         "steins_app.c"
         "steins_mobile.c"
         "steins_mobile_pack.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg esp_rom
    EMBED_FILES "${STEINS_PACK_FILE}"
)

# 两张位图字体(正文原字形 + 界面中文字集)与排版度量表,均由 tools/steins_font.py 生成。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/steins_story_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/steins_ui_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/steins_ui_18.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/steins_text_metrics.c"
)
