# 《纸上的魔法使》横屏阅读器：复用框架 BSP/LVGL，应用层是 paper/* + gal_app.c。
idf_component_register(
    SRCS "main.c"
         "gal/gal_app.c"
         "gal/gal_music.c"
         "paper/paper_pack.c"
         "paper/paper_gfx.c"
         "paper/paper_game.c"
         "paper/tjpgd/tjpgd.c"
    INCLUDE_DIRS "." "gal" "paper" "paper/tjpgd"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_rom
)

# 中文字体子集（tools/gal_font.py 由 assets/fonts/paper_cjk_symbols.txt 生成）。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_16.c"
)

# 资源包：编进固件后由 paper_pack.c 直接以指针读取（不做文件系统）。
target_add_binary_data(${COMPONENT_LIB} "paper_data/paper_img.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "paper_data/paper_scn.bin" BINARY)
