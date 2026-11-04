// main/anemoi_data/anemoi_data.c —— 把 anemoi_pack.bin 暴露成只读指针。
#include "anemoi_data.h"

extern const uint8_t anemoi_pack_bin_start[] asm("_binary_anemoi_pack_bin_start");
extern const uint8_t anemoi_pack_bin_end[] asm("_binary_anemoi_pack_bin_end");

const uint8_t *anemoi_pack_blob(void)
{
    return anemoi_pack_bin_start;
}

size_t anemoi_pack_blob_size(void)
{
    return (size_t)(anemoi_pack_bin_end - anemoi_pack_bin_start);
}
