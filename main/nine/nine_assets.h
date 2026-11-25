// Read-only access to the `assets` flash partition. The container layout is
// produced by tools/nine_container.py: a 64-byte header followed by the script
// pack and the image pack. Every layer is CRC-checked before use.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *container;
    size_t container_size;
    const uint8_t *script;
    uint32_t script_size;
    const uint8_t *images;
    uint32_t images_size;
    const char *error;
} nine_assets_t;

// Pure parser, host-testable. Rejects a short/overlapping/CRC-broken container.
bool nine_assets_parse(const uint8_t *data, size_t size, nine_assets_t *out);

#ifndef NINE_ASSETS_HOST
// Map the `assets` partition and parse it. The mapping lives for the lifetime
// of the application (the packs are immutable). On failure out->error is a
// short user-visible reason and no partially applied pack is returned.
bool nine_assets_map(nine_assets_t *out);
void nine_assets_unmap(nine_assets_t *out);
#endif
