/**
 * @file sbx_topbar.c
 * SteriBox — Shared top-bar icon group implementation
 *
 * Provides three right-anchored status icons for every screen header:
 *   SD card  (green  = present / grey = absent)
 *   USB drive (cyan  = mounted / grey = not mounted)
 *   Printer  (white  = online  / grey = not detected)
 *
 * Also provides export/print action guards with error popups and a 
 * reusable modal popup helper.
 */
#include "sbx_topbar.h"
#include "steribox_hal.h"
#include "ui.h"

#include <string.h>
#include <stdio.h>

/* ---- Forward declarations for the new icon assets ---- */
LV_IMG_DECLARE(ui_img_icn_sd_png);      /* 30×30 SD-card icon  */
LV_IMG_DECLARE(ui_img_icn_usb2_png);   /* 28×28 USB drive icon */
LV_IMG_DECLARE(ui_img_icn_pc_png);     /* existing printer icon */

/* Icon horizontal spacing from the right edge of the header -------------- */
#define ICN_RIGHT_PAD   8   /* px from right edge to printer icon  */
#define ICN_STEP        40  /* px between icon centres             */

/* colour constants */
#define COL_ACTIVE    0x00D2FF   /* cyan-white — device present   */
#define COL_INACTIVE  0x3A4558  /* muted grey — device absent    */
#define COL_SD_OK     0x00E05A  /* green — SD card mounted       */
#define COL_PRINTER   0xC2CBDE  /* light — printer detected      */

/* -------------------------------------------------------------------------
 * sbx_topbar_build
 * Adds three icons inside the header panel, right-anchored as a group.
 * Layout (right→left):  [Printer] [USB] [SD]
 *           offsets:     -8        -48   -88  (from right edge)
 * -------------------------------------------------------------------------*/
void sbx_topbar_build(lv_obj_t * header, sbx_topbar_icons_t * icons)
{
    /* Printer icon (rightmost -8) */
    icons->icn_printer = lv_img_create(header);
    lv_img_set_src(icons->icn_printer, &ui_img_icn_pc_png);
    lv_obj_set_align(icons->icn_printer, LV_ALIGN_RIGHT_MID);
    lv_obj_set_x(icons->icn_printer, -ICN_RIGHT_PAD);
    lv_obj_set_y(icons->icn_printer, 0);
    lv_obj_add_flag(icons->icn_printer, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_style_img_recolor(icons->icn_printer, lv_color_hex(COL_INACTIVE), 0);
    lv_obj_set_style_img_recolor_opa(icons->icn_printer, 255, 0);

    /* USB drive icon (-48) */
    icons->icn_usb = lv_img_create(header);
    lv_img_set_src(icons->icn_usb, &ui_img_icn_usb2_png);
    lv_obj_set_align(icons->icn_usb, LV_ALIGN_RIGHT_MID);
    lv_obj_set_x(icons->icn_usb, -(ICN_RIGHT_PAD + ICN_STEP));
    lv_obj_set_y(icons->icn_usb, 0);
    lv_obj_add_flag(icons->icn_usb, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_style_img_recolor(icons->icn_usb, lv_color_hex(COL_INACTIVE), 0);
    lv_obj_set_style_img_recolor_opa(icons->icn_usb, 255, 0);

    /* SD card icon (-88) */
    icons->icn_sd = lv_img_create(header);
    lv_img_set_src(icons->icn_sd, &ui_img_icn_sd_png);
    lv_obj_set_align(icons->icn_sd, LV_ALIGN_RIGHT_MID);
    lv_obj_set_x(icons->icn_sd, -(ICN_RIGHT_PAD + 2 * ICN_STEP));
    lv_obj_set_y(icons->icn_sd, 0);
    lv_obj_add_flag(icons->icn_sd, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_style_img_recolor(icons->icn_sd, lv_color_hex(COL_INACTIVE), 0);
    lv_obj_set_style_img_recolor_opa(icons->icn_sd, 255, 0);
}

/* -------------------------------------------------------------------------
 * sbx_topbar_refresh
 * Update icon tint from HAL state. Call once per second.
 * -------------------------------------------------------------------------*/
void sbx_topbar_refresh(const sbx_topbar_icons_t * icons)
{
    if (!icons) return;

    /* SD card */
    bool sd = sbx_hal_sd_present();
    if (icons->icn_sd) {
        uint32_t col = sd ? COL_SD_OK : COL_INACTIVE;
        lv_obj_set_style_img_recolor(icons->icn_sd, lv_color_hex(col), 0);
        lv_obj_set_style_img_recolor_opa(icons->icn_sd, 255, 0);
    }

    /* USB drive (SD card mounted = USB drive is the SD, same API) */
    bool usb = sbx_hal_usb_present();
    if (icons->icn_usb) {
        uint32_t col = usb ? COL_ACTIVE : COL_INACTIVE;
        lv_obj_set_style_img_recolor(icons->icn_usb, lv_color_hex(col), 0);
        lv_obj_set_style_img_recolor_opa(icons->icn_usb, 255, 0);
    }

    /* Printer */
    bool printer = sbx_hal_printer_present();
    if (icons->icn_printer) {
        uint32_t col = printer ? COL_PRINTER : COL_INACTIVE;
        lv_obj_set_style_img_recolor(icons->icn_printer, lv_color_hex(col), 0);
        lv_obj_set_style_img_recolor_opa(icons->icn_printer, 255, 0);
    }
}

/* -------------------------------------------------------------------------
 * sbx_popup_error  — modal message box, auto-closes in 3 s or on tap
 * -------------------------------------------------------------------------*/
static void popup_close_cb(lv_event_t * e)
{
    lv_obj_t * box = lv_event_get_current_target(e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED ||
        lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        lv_obj_del(box);
    }
}

static void popup_timer_cb(lv_timer_t * t)
{
    lv_obj_t * box = (lv_obj_t *)t->user_data;
    if (box && lv_obj_is_valid(box)) lv_obj_del(box);
    lv_timer_del(t);
}

void sbx_popup_error(lv_obj_t * parent, const char * msg)
{
    /* Dark semi-transparent backdrop */
    lv_obj_t * overlay = lv_obj_create(parent ? parent : lv_scr_act());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 380, 140);
    lv_obj_set_align(overlay, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(overlay, 240, 0);
    lv_obj_set_style_radius(overlay, 16, 0);
    lv_obj_set_style_border_color(overlay, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_border_width(overlay, 2, 0);
    lv_obj_set_style_shadow_color(overlay, lv_color_hex(0xFF2020), 0);
    lv_obj_set_style_shadow_width(overlay, 20, 0);
    lv_obj_set_style_shadow_opa(overlay, 180, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* ⚠ icon */
    lv_obj_t * icn = lv_label_create(overlay);
    lv_label_set_text(icn, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(icn, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_text_font(icn, &lv_font_montserrat_36, 0);
    lv_obj_set_align(icn, LV_ALIGN_TOP_MID);
    lv_obj_set_y(icn, 10);

    /* Message */
    lv_obj_t * lbl = lv_label_create(overlay);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xC2CBDE), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, 340);
    lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_y(lbl, -14);

    /* Hint */
    lv_obj_t * hint = lv_label_create(overlay);
    lv_label_set_text(hint, "Tap to dismiss");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x4A5568), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_align(hint, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_pos(hint, -8, -2);

    lv_obj_add_event_cb(overlay, popup_close_cb, LV_EVENT_CLICKED, NULL);
    lv_timer_create(popup_timer_cb, 3000, overlay);
}

/* -------------------------------------------------------------------------
 * sbx_try_export — export guard
 * -------------------------------------------------------------------------*/
bool sbx_try_export(lv_obj_t * parent, const char * filename, const char * text)
{
    if (!sbx_hal_sd_present()) {
        sbx_popup_error(parent,
            "No SD card inserted.\nInsert an SD card to export.");
        return false;
    }
    bool ok = sbx_hal_usb_export(filename, text);
    if (!ok) {
        sbx_popup_error(parent,
            "Export failed.\nCheck SD card and try again.");
    }
    return ok;
}

/* -------------------------------------------------------------------------
 * sbx_try_print — print guard
 * -------------------------------------------------------------------------*/
bool sbx_try_print(lv_obj_t * parent, const char * text)
{
    if (!sbx_hal_printer_present()) {
        sbx_popup_error(parent,
            "No printer connected.\nConnect a USB printer to print.");
        return false;
    }
    bool ok = sbx_hal_usb_print(text);
    if (!ok) {
        sbx_popup_error(parent,
            "Print failed.\nCheck printer connection.");
    }
    return ok;
}
