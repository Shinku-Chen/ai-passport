# 《天使☆騒々 RE-BOOT!》阅读器:只编译本应用的源码。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里(可切回硬件自检 demo),
# 但不再参与本应用的构建,也不再注册到菜单。
idf_component_register(
    SRCS "main.c"
         "tsxx_pack.c"
         "tsxx_model.c"
         "tsxx_image.c"
         "tsxx_save.c"
         "tsxx_ui.c"
         "tsxx_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg esp_partition
)

# 资源包(剧本 + 背景 + 立绘 + 事件图,由 tools/tsxx_pack.py 生成)不再编进 app:
# 5.86 MiB 的 rodata 段会超出 ESP32-C3 的 Flash MMU 容量,引导加载器映射时直接复位。
# 现在把它作为独立镜像烧到 assets 分区,固件运行时用 esp_partition_mmap 窗口化读取。
# 先复制进构建目录(esptool_py_flash_to_partition 要求镜像在 build dir 内),
# 再注册到 flash 目标 —— 这样 flash_args 与 idf.py merge-bin 都会带上它。
set(TSXX_PACK_SOURCE "${CMAKE_CURRENT_LIST_DIR}/tsxx_data/tsxx_pack.bin")
set(TSXX_PACK_IMAGE "${CMAKE_BINARY_DIR}/tsxx_assets.bin")
add_custom_command(
    OUTPUT "${TSXX_PACK_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TSXX_PACK_SOURCE}" "${TSXX_PACK_IMAGE}"
    DEPENDS "${TSXX_PACK_SOURCE}"
    COMMENT "Copying tsxx_pack.bin into the assets partition image"
    VERBATIM
)
add_custom_target(tsxx_assets_image ALL DEPENDS "${TSXX_PACK_IMAGE}")
esptool_py_flash_to_partition(flash "assets" "${TSXX_PACK_IMAGE}")
add_dependencies(flash tsxx_assets_image)

# 中文字体子集(assets/fonts 下由 tools/tsxx_font.py 生成)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/tsxx_cjk_16.c"
)
