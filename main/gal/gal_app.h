/*
 * The galgame application: title screen, reader, panels and input handling.
 *
 * This is a derivative application, so it owns its whole UI. It does not reuse
 * the baseline hardware-test menu, its pages or the `ui_pixel` shell; it does
 * reuse the BSP (display, LVGL lock, battery, buttons) and the shared LVGL
 * runtime invariants documented in docs/development/ai-guide.md.
 */
#ifndef GAL_APP_H
#define GAL_APP_H

#include <stdbool.h>

#include "bsp_button.h"

/*
 * Create the application UI on the active display. Must be called after
 * bsp_lvgl_init() and with the LVGL lock held. Returns false when the assets
 * partition is missing or unreadable, in which case a diagnostic screen is shown
 * instead of the title.
 */
bool gal_app_start(void);

/* Route one button event. Called from the input task, so it only mutates app
 * state; all LVGL access happens inside the LVGL lock. */
void gal_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

#endif /* GAL_APP_H */
