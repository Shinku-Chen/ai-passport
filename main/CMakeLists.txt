# 《anemoi》AI Passport 横屏阅读器：BSP/LVGL + main/anemoi 应用层。
idf_component_register(
    SRCS "main.c"
         "anemoi/anemoi_pack.c"
         "anemoi/anemoi_script.c"
         "anemoi/anemoi_canvas.c"
         "anemoi/anemoi_image.c"
         "anemoi/anemoi_store.c"
         "anemoi/anemoi_store_nvs.c"
         "anemoi/anemoi_ui.c"
         "anemoi/anemoi_app.c"
         "anemoi/anemoi_render.c"
         "anemoi/anemoi_startup.c"
         "anemoi/tjpgd/tjpgd.c"
         "anemoi_data/anemoi_data.c"
    INCLUDE_DIRS "." "anemoi" "anemoi/tjpgd" "anemoi_data"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_rom
)

# 中文字体子集（assets/fonts/gal_cjk_16.c；字符集来自资源包正文 + 界面文案，
# 由 tools/gal_font.py 生成并逐字形回读校验）。20px 在本卷体积内放不下，不提供。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_16.c"
)

# 资源包：编进固件后由 anemoi_pack.c 直接以指针读取。
target_add_binary_data(${COMPONENT_LIB} "anemoi_data/anemoi_pack.bin" BINARY)
