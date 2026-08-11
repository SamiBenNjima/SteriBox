/**
 * @file sbx_navbar.c
 * SteriBox — Programmatic left navigation sidebar.
 *
 * Layout (75 w × 480 h):
 *   • Dark background (#121821 = SBX_COL_BG) fills the entire strip.
 *   • Three 160-px-tall rows, each centred on a 36×36 LVGL icon image.
 *   • Active row:
 *       - Icon tinted with SBX_COL_ACCENT (0x00D2FF cyan).
 *       - 5 × 92 px cyan bar on the left edge (rounded 3 px).
 *   • Inactive rows:
 *       - Icon at 40 % opacity (dim white look on dark bg).
 */
#include "sbx_navbar.h"
#include "ui.h"   /* SBX_COL_BG, SBX_COL_ACCENT, LV_IMG_DECLARE macros */

/* Icon image descriptors (defined in ui_img_icn_*_png.c) */
LV_IMG_DECLARE(ui_img_icn_home_png);
LV_IMG_DECLARE(ui_img_icn_info_png);
LV_IMG_DECLARE(ui_img_icn_config_png);

/* Nav bar geometry */
#define NAV_W           75
#define NAV_H           480
#define ROW_H           160
#define ACCENT_W        5
#define ACCENT_H        160   /* Full height of the button slot */
#define ACCENT_RADIUS   0   /* Clean full-edge bar */

/* Nav bar colors */
#define SBX_COL_NAV_BG  0x1A2233   /* Original dark sidebar background */
#define COL_ACTIVE      0x00D2FF   /* cyan — matches SBX_COL_ACCENT */

static const lv_img_dsc_t * const nav_icons[3] = {
    &ui_img_icn_home_png,
    &ui_img_icn_info_png,
    &ui_img_icn_config_png,
};

lv_obj_t * sbx_navbar_build(lv_obj_t * screen, int active_row)
{
    /* ── Root panel — solid dark background ──────────────────────── */
    lv_obj_t * nav = lv_obj_create(screen);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, NAV_W, NAV_H);
    lv_obj_set_pos(nav, 0, 0);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(nav, lv_color_hex(SBX_COL_NAV_BG), 0);
    lv_obj_set_style_bg_opa(nav, 255, 0);

    /* Thin right-edge separator line */
    lv_obj_t * sep = lv_obj_create(nav);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 1, NAV_H);
    lv_obj_set_pos(sep, NAV_W - 1, 0);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x1E2838), 0);
    lv_obj_set_style_bg_opa(sep, 255, 0);

    for (int row = 0; row < 3; row++) {
        bool active = (row == active_row);
        int  y_top  = row * ROW_H;

        /* ── Row container (uniform background color for all rows) ── */
        lv_obj_t * row_bg = lv_obj_create(nav);
        lv_obj_remove_style_all(row_bg);
        lv_obj_set_size(row_bg, NAV_W, ROW_H);
        lv_obj_set_pos(row_bg, 0, y_top);
        lv_obj_clear_flag(row_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row_bg, lv_color_hex(SBX_COL_NAV_BG), 0);
        lv_obj_set_style_bg_opa(row_bg, 255, 0);

        /* ── Icon image (scaled to ~48x48 using zoom = 341) ────────── */
        lv_obj_t * icn = lv_img_create(row_bg);
        lv_img_set_src(icn, nav_icons[row]);
        lv_img_set_zoom(icn, 341); /* 256 = 100%, 341 = ~133% (36px -> 48px) */
        lv_obj_align(icn, LV_ALIGN_CENTER, 0, 0);

        if (active) {
            /* Cyan highlight for selected page icon */
            lv_obj_set_style_img_recolor(icn, lv_color_hex(COL_ACTIVE), 0);
            lv_obj_set_style_img_recolor_opa(icn, LV_OPA_COVER, 0);
            lv_obj_set_style_img_opa(icn, LV_OPA_COVER, 0);

            /* Full-height side blue accent bar */
            lv_obj_t * accent = lv_obj_create(nav);
            lv_obj_remove_style_all(accent);
            lv_obj_set_size(accent, ACCENT_W, ACCENT_H);
            lv_obj_set_pos(accent, 0, y_top);
            lv_obj_set_style_radius(accent, ACCENT_RADIUS, 0);
            lv_obj_set_style_bg_color(accent, lv_color_hex(COL_ACTIVE), 0);
            lv_obj_set_style_bg_opa(accent, 255, 0);
        } else {
            /* Dim unselected icon to ~40% opacity */
            lv_obj_set_style_img_opa(icn, LV_OPA_40, 0);
        }
    }

    return nav;
}
