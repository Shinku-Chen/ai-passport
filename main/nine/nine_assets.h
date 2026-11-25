#pragma once

#include <stddef.h>
#include <stdint.h>

// Flash-embedded packs, bound at link time by main/CMakeLists.txt.
const uint8_t *nine_assets_script(size_t *size);
const uint8_t *nine_assets_images(size_t *size);
