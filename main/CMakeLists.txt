# 《星之终途》+《渺茫希望的日记》阅读器：只编译本应用的源码。
set(STELLA_PACK_FILE "${CMAKE_CURRENT_LIST_DIR}/../assets/stella/stella_pack.bin" CACHE FILEPATH
    "Stella 资源包（tools/stella_pack.py 生成）")
if(NOT EXISTS "${STELLA_PACK_FILE}")
    message(FATAL_ERROR "找不到 ${STELLA_PACK_FILE}，先运行 tools/stella_pack.py")
endif()

idf_component_register(
    SRCS "main.c"
         "stella_pack.c"
         "stella_model.c"
         "stella_image.c"
         "stella_ui.c"
         "stella_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    EMBED_FILES "${STELLA_PACK_FILE}"
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/stella_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/stella_cjk_20.c"
)
