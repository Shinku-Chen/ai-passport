// CRC table must remain bit-identical to the previous bitwise implementation.
#include "nine_crc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(nine_crc32_zero((const uint8_t *)"123456789", 9, SIZE_MAX, 0) == 0xCBF43926u);
    assert(nine_crc32_update(UINT32_MAX, (const uint8_t *)"123456789", 9) == 0x340BC6D9u);

    // The pack header convention zeros a four-byte field before final inversion.
    const uint8_t with_zero[8] = { 1, 2, 0, 0, 0, 0, 5, 6 };
    const uint8_t replaced[8] = { 1, 2, 9, 9, 9, 9, 5, 6 };
    assert(nine_crc32_zero(with_zero, sizeof(with_zero), 2, 4) ==
           nine_crc32_zero(replaced, sizeof(replaced), 2, 4));

    puts("nine CRC table compatibility: PASS");
    return 0;
}
