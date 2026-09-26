idf_component_register(
    # The baseline hardware-test pages are kept compiled even though this
    # application no longer starts them: they stay buildable as the repository's
    # reference demo, their host tests compile them directly, and the linker drops
    # every unreferenced one. main.c boots straight into the galgame.
    SRCS "main.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
         "gal/gal_script.c"
         "gal/gal_assets.c"
         "gal/gal_save.c"
         "gal/gal_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
)

# Generated CJK subsets. Regenerate with tools/gal/gen_font.py when the chapter
# scripts or main/gal/gal_strings.h change; see assets/README.md.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_font_20.c"
)

# The galgame's artwork and scripts are packed into the `assets` data partition.
# Third-party material is not tracked in this repository, so the packer emits a
# placeholder pack when its sources are missing and the build still succeeds.
set(GAL_OUT_DIR "${CMAKE_BINARY_DIR}/gal")
set(GAL_IMAGE "${GAL_OUT_DIR}/gal_assets.bin")
set(GAL_PACKER "${CMAKE_CURRENT_LIST_DIR}/../tools/gal/pack_assets.py")
set(GAL_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../assets/gal-source/common")

file(GLOB_RECURSE GAL_PACK_INPUTS CONFIGURE_DEPENDS
    "${GAL_SOURCE_DIR}/*.png" "${GAL_SOURCE_DIR}/*.txt")

add_custom_command(
    OUTPUT "${GAL_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GAL_OUT_DIR}"
    COMMAND "${PYTHON}" "${GAL_PACKER}"
            --source "${GAL_SOURCE_DIR}" --out "${GAL_OUT_DIR}"
            --max-bytes 4194304
    DEPENDS "${GAL_PACKER}" "${CMAKE_CURRENT_LIST_DIR}/../tools/gal/gal_format.py"
            ${GAL_PACK_INPUTS}
    COMMENT "Packing the galgame asset partition image"
    VERBATIM
)
add_custom_target(gal_assets_image ALL DEPENDS "${GAL_IMAGE}")
add_dependencies(${COMPONENT_LIB} gal_assets_image)

# Flashing and `idf.py merge-bin` both read flash_args, so registering the image
# here keeps the merged image complete.
esptool_py_flash_to_partition(flash "assets" "${GAL_IMAGE}")
