/*
 * Screen geometry and palette for the galgame reader.
 *
 * The dialogue panel follows the Saya reference port: a flat, fully opaque dark
 * plate spanning the full width, with a single 2 px accent line along its top
 * edge and no rounding. The upstream project's own panel artwork is not used at
 * all -- it is a near-white plate carrying a repeating ornament, and at any
 * usable opacity over scene art it reads as a busy grey band competing with the
 * picture.
 *
 * The panel is 82 px tall, which is four 19 px lines. That is measured rather
 * than preferred: it leaves 206 px of artwork visible (64% of the screen) and
 * covers 52 characters per page, with longer lines paginated instead of hidden.
 * See tools/gal/README.md for the measurement.
 */
#ifndef GAL_LAYOUT_H
#define GAL_LAYOUT_H

/* --- palette ----------------------------------------------------------------- */

/* Shared with the Saya reference port so the two builds feel like one product. */
#define GAL_COLOR_BOX    lv_color_hex(0x101317) /* dialogue panel fill */
#define GAL_COLOR_LINE   lv_color_hex(0x35604A) /* panel top rule */
#define GAL_COLOR_TEXT   lv_color_hex(0xEDF1EE) /* body text */
#define GAL_COLOR_DIM    lv_color_hex(0x87919A) /* secondary text */
#define GAL_COLOR_ACCENT lv_color_hex(0x74D3A0) /* selection and emphasis */
#define GAL_COLOR_NAME   lv_color_hex(0x9FE0BE) /* speaker name */
#define GAL_COLOR_PANEL  lv_color_hex(0x0A0C0F) /* full-screen pages */
#define GAL_COLOR_ROW    lv_color_hex(0x1B2026) /* list row */
#define GAL_COLOR_ROW_SEL lv_color_hex(0x2B5F45) /* selected list row */

/* --- dialogue panel --------------------------------------------------------- */

#define GAL_PANEL_MARGIN_X 0
/* The panel runs all the way to the bottom edge. The display driver masks the
 * rounded corners on every flush, so the panel's own corners are shaped by the
 * screen rather than showing a black gap below it. */
#define GAL_PANEL_BOTTOM   0
#define GAL_PANEL_WIDTH    (240 - 2 * GAL_PANEL_MARGIN_X)

#define GAL_PANEL_RULE_WIDTH 2

/* Inner text block: 10 px side inset, 4 px above the first line and 2 px below
 * the last. */
#define GAL_TEXT_X         10
#define GAL_TEXT_PAD_TOP   4
#define GAL_TEXT_PAD_BOTTOM 2
#define GAL_TEXT_W         (GAL_PANEL_WIDTH - 20)
#define GAL_TEXT_LINES     4

/*
 * Line height per font size. gal_font_16 reports line_height 20 and gal_font_20
 * reports 24; pulling one pixel back gives 19 and 23, which is the leading the
 * Saya reference uses at these sizes.
 *
 * The panel is sized from these, so the four lines of a page always fit whole and
 * the panel grows with the chosen text size instead of clipping the last line.
 * At 16 px it is 82 px (the same as the Saya reference); at 20 px it is 98 px.
 */
#define GAL_LINE_H_SMALL   19
#define GAL_LINE_H_LARGE   23
/* One pixel less than each font's line_height. */
#define GAL_TEXT_LINE_SPACE (-1)

#define GAL_TEXT_H_SMALL   (GAL_TEXT_LINES * GAL_LINE_H_SMALL)
#define GAL_TEXT_H_LARGE   (GAL_TEXT_LINES * GAL_LINE_H_LARGE)
#define GAL_PANEL_H_SMALL  (GAL_TEXT_H_SMALL + GAL_TEXT_PAD_TOP + GAL_TEXT_PAD_BOTTOM)
#define GAL_PANEL_H_LARGE  (GAL_TEXT_H_LARGE + GAL_TEXT_PAD_TOP + GAL_TEXT_PAD_BOTTOM)
#define GAL_PANEL_TOP_SMALL (320 - GAL_PANEL_BOTTOM - GAL_PANEL_H_SMALL)
#define GAL_PANEL_TOP_LARGE (320 - GAL_PANEL_BOTTOM - GAL_PANEL_H_LARGE)

/* The speaker plate keeps the small face at both text sizes, so its geometry is
 * stable while the panel below it moves. */
#define GAL_NAME_HEIGHT 24
#define GAL_NAME_X     8
#define GAL_NAME_Y_SMALL (GAL_PANEL_TOP_SMALL - GAL_NAME_HEIGHT)
#define GAL_NAME_Y_LARGE (GAL_PANEL_TOP_LARGE - GAL_NAME_HEIGHT)
#define GAL_NAME_PAD_X 6
#define GAL_NAME_PAD_Y 1
#define GAL_NAME_RADIUS 6
#define GAL_NAME_OPA   LV_OPA_50

/* --- character sprite -------------------------------------------------------- */

/* Character art is placed relative to this origin. The packer bakes the same
 * values into the sprite offsets it stores. */
#define GAL_SPRITE_X 42
#define GAL_SPRITE_Y 56

/* --- status strip ----------------------------------------------------------- */

#define GAL_STATUS_Y 4

/* Longest packed dialogue line; the packer reports the real value. */
#define GAL_REVEAL_CAPACITY 320

/* --- typesetting ------------------------------------------------------------- */

/*
 * Used to paginate a line that does not fit the panel. One unit is half a
 * full-width character, so a unit is half the font size in pixels. The text block
 * is 220 px wide, which is 27.5 units at 16 px and 22 units at 20 px; the values
 * below leave room for the wider Latin glyphs in Noto Sans SC, so the model never
 * places more on a line than LVGL can actually fit.
 */
#define GAL_UNITS_PER_LINE_SMALL 26
#define GAL_UNITS_PER_LINE_LARGE 21
#define GAL_LINES_PER_PAGE       GAL_TEXT_LINES

/* --- fast forward ------------------------------------------------------------ */

/*
 * Upper bound of the UP key's voltage window, from BSP_BTN_MV_TABLE in
 * bsp_pins.h. Fast forward polls bsp_button_read_mv() and stops as soon as the
 * reading leaves this window, which is how "hold to fast forward, release to
 * stop" is realised without adding a key-release event to the BSP. Update this
 * if the key divider or the table changes.
 */
#define GAL_BTN_UP_MAX_MV 150
#define GAL_FF_INTERVAL_MS 110

#endif /* GAL_LAYOUT_H */
