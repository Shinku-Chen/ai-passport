# Cafe Stella（星光咖啡馆与死神之蝶）离线阅读器。
# 数据层：cafe_pack（内嵌 cafe.pak）+ cafe_vm；界面：cafe_ui（320x240 横屏）。
idf_component_register(
    SRCS "main.c"
         "gal/gal_text.c"
         "cafe/cafe_compat.c"
         "cafe/cafe_pack.c"
         "cafe/cafe_vm.c"
         "cafe/cafe_store.c"
         "cafe/cafe_input.c"
         "cafe/cafe_image.c"
         "cafe/cafe_ui.c"
         "cafe/cafe_app.c"
         "cafe/cafe_audio.c"
         "cafe/cafe_audio_codec.c"
         "cafe/tjpgd.c"
    INCLUDE_DIRS "." "gal" "cafe"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_rom
)

# 中文字体子集（tools/gal_font.py 生成；覆盖全部剧本 + UI）。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_20.c"
)

# 把 ~5.4MB 资源包直接链接进 app 分区（factory 0x7f0000，8MB Flash）。
# 这样不需要额外 data 分区，idf.py flash / merge-bin 都会带上资源。
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/cafe/cafe.pak" BINARY)

# 生成目标 RISC-V 的 .su 栈用量文件（不影响镜像内容），用于复核关键调用链。
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
