# 《CLANNAD》阅读器：只编译本应用的源码；baseline 的 demo_* / gal 不再参与构建。
set(CLANNAD_PACK_FILE "${CMAKE_CURRENT_LIST_DIR}/../assets/clannad/clannad_pack.bin" CACHE FILEPATH
    "CLANNAD 资源包（tools/clannad_pack.py 生成）")
if(NOT EXISTS "${CLANNAD_PACK_FILE}")
    message(FATAL_ERROR "找不到 ${CLANNAD_PACK_FILE}，先运行 tools/clannad_pack.py")
endif()

idf_component_register(
    SRCS "main.c"
         "clannad_pack.c"
         "clannad_vm.c"
         "clannad_text.c"
         "clannad_image.c"
         "clannad_ui.c"
         "clannad_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    EMBED_FILES "${CLANNAD_PACK_FILE}"
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/clannad_cjk_16.c"
)
