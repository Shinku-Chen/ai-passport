# 9-nine- Episode 4 reader: framework UI + the title's own pack/VM/decoder.
idf_component_register(
    SRCS "main.c"
         "gal/gal_text.c"
         "gal/gal_settings.c"
         "gal/gal_ui.c"
         "nine/nine_app.c"
         "nine/nine_assets.c"
         "nine/nine_pack.c"
         "nine/nine_vm.c"
         "nine/nine_image.c"
         "nine/nine_media.c"
         "nine/nine_store.c"
         "nine/tjpgd/tjpgd.c"
    INCLUDE_DIRS "." "gal" "nine" "nine/tjpgd"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE NINE_READER=1)

# 16/20px LVGL subsets: the complete story + every UI literal in this tree.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/nine/yukiiro/nine_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/nine/yukiiro/nine_cjk_20.c"
)

# Read-only packs live in flash; no heap copy of the 6.1 MB media is made.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/nine/yukiiro/nine_script.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/nine/yukiiro/nine_images.bin" BINARY)
