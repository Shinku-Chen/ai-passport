#include "nine_input.h"

void nine_input_init(nine_input_t *in)
{
    if (!in) return;
    in->have_pending = false;
}

bool nine_input_process(nine_app_t *app, nine_input_t *in)
{
    if (!in || !in->have_pending) return true;
    if (!nine_app_key(app, &in->pending)) return false;
    in->have_pending = false;
    return true;
}
