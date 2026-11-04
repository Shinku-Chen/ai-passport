# 《PARQUET》横屏阅读器：复用框架 BSP/LVGL，应用层是 parquet/* + gal_app.c。
idf_component_register(
    SRCS "main.c"
         "gal/gal_app.c"
         "parquet/parquet_pack.c"
         "parquet/parquet_inflate.c"
         "parquet/parquet_gfx.c"
         "parquet/parquet_game.c"
         "parquet/tjpgd/tjpgd.c"
    INCLUDE_DIRS "." "gal" "parquet" "parquet/tjpgd"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_rom
)

# 中文字体子集（assets/fonts 下由 tools/gal_font.py 生成；源字体不入库）。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_20.c"
)

# 资源包：编进固件后由 parquet_pack.c 直接以只读指针读取（不做文件系统）。
target_add_binary_data(${COMPONENT_LIB} "parquet_data/parquet_img.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "parquet_data/parquet_scn.bin" BINARY)
