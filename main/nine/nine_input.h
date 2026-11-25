// Serialized input pump shared by the production input task and host tests.
#pragma once
#include "nine_app.h"

typedef struct {
    nine_key_t pending;
    bool have_pending;
} nine_input_t;

void nine_input_init(nine_input_t *in);
// Consume at most one pending event. Returns false when the LVGL lock was busy;
// the caller must retain the same event and retry (FIFO, exactly once).
bool nine_input_process(nine_app_t *app, nine_input_t *in);
