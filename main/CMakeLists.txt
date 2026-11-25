# 9-nine- 春色春恋春熙风:离线读者(资源包 + graph VM + 横屏 UI)。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里(可切回硬件自检 demo),
# 但不再参与本应用的构建,也不再注册到菜单。
idf_component_register(
    SRCS "main.c"
         "gal/gal_text.c"
         "gal/gal_settings.c"
         "gal/gal_ui.c"
         "nine/nine_pack.c"
         "nine/nine_crc32.c"
         "nine/nine_vm.c"
         "nine/nine_image.c"
         "nine/nine_media.c"
         "nine/nine_app.c"
         "nine/nine_input.c"
         "nine/tjpgd/tjpgd.c"
    INCLUDE_DIRS "." "gal" "nine" "nine/tjpgd"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_rom
)

# Optional analysis build: emit .su files for the target compiler's real flags.
option(NINE_STACK_USAGE "Emit -fstack-usage for main" OFF)
if(NINE_STACK_USAGE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
endif()

# 320x240 landscape reader layout (gal_ui + nine_app).
target_compile_definitions(${COMPONENT_LIB} PUBLIC NINE_READER=1)

# 中文字体子集(assets/fonts 下由 tools/nine_font.py 生成;源字体不入库)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/nine_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/nine_cjk_20.c"
)

# 本作自己的离线资源包:固定来源快照 6a8907a5 生成的剧情/图像/音频。
target_add_binary_data(${COMPONENT_LIB} "../assets/nine/haruiro/nine_script.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/nine/haruiro/nine_images.bin" BINARY)
