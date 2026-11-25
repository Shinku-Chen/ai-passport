cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(FoloToy-AI-Passport)

# Build the single flashable assets image from the committed script/image packs
# and register it at the `assets` partition offset. merge-bin consumes flash_args,
# so both the flash and merge targets depend on this generated file.
idf_build_get_property(nine_python PYTHON)
set(NINE_ASSETS_BIN "${CMAKE_BINARY_DIR}/nine_assets.bin")
set(NINE_ASSETS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/nine")
add_custom_command(
    OUTPUT "${NINE_ASSETS_BIN}"
    COMMAND "${nine_python}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/nine_container.py"
            "${NINE_ASSETS_SRC}" "${NINE_ASSETS_BIN}"
    DEPENDS "${NINE_ASSETS_SRC}/nine_script.bin" "${NINE_ASSETS_SRC}/nine_images.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/nine_container.py"
    VERBATIM
)
add_custom_target(nine_assets ALL DEPENDS "${NINE_ASSETS_BIN}")
esptool_py_flash_to_partition(flash "assets" "${NINE_ASSETS_BIN}")
add_dependencies(flash nine_assets)
add_dependencies(merge-bin nine_assets)
