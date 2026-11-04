# 《魔法少女的魔女审判》AI Passport 阅读器:只编译本应用的源码。
# baseline 的 demo_*.c / ui_pixel.c / gal 框架骨架仍留在仓库里(可切回硬件自检),
# 但不再参与本应用的构建。
set(MANOSABA_PACK_FILE "${CMAKE_CURRENT_LIST_DIR}/gal_data/manosaba.pack" CACHE FILEPATH
    "Manosaba 资源包")
if(NOT EXISTS "${MANOSABA_PACK_FILE}")
    message(FATAL_ERROR "找不到 ${MANOSABA_PACK_FILE}(先运行 tools/manosaba_pack.py)")
endif()

idf_component_register(
    SRCS "main.c"
         "gal_pack.c"
         "gal_expr.c"
         "gal_vm.c"
         "gal_save.c"
         "gal_store.c"
         "gal_unlock.c"
         "gal_text.c"
         "gal_image.c"
         "gal_ui.c"
         "gal_app.c"
    INCLUDE_DIRS "." "gal_data"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    EMBED_FILES "${MANOSABA_PACK_FILE}"
)

# 16px 完备中文字体(tools/manosaba_font.py 生成;源字体不入库)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/manosaba_16.c"
)
