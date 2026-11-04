// Intentionally broken translation unit for the negative gate:
// the gate must fail when the image test cannot compile.
#include "gal_image.h"
int manosaba_negative_gate_should_not_compile(void)
{
    this_is_not_a_valid_c_expression;
    return 0;
}
