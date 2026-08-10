/**
 * @file sbx_navbar.h
 * SteriBox — Left-side navigation sidebar (programmatic, no baked PNG).
 *
 * Builds a 75×480 panel with three 160-px rows:
 *   row 0  = Home   (icon: ui_img_icn_home_png)
 *   row 1  = Info   (icon: ui_img_icn_info_png)
 *   row 2  = Config (icon: ui_img_icn_config_png)
 *
 * The active row has:
 *   • icon tinted cyan (SBX_COL_ACCENT)
 *   • a 5-px cyan bar on the left edge
 *
 * Inactive rows show the icon at 50 % opacity (dimmed white).
 *
 * Call sbx_navbar_build(screen, active_row) once per screen_init().
 * The returned lv_obj_t* can be used as the parent for nav hit-areas.
 */
#ifndef SBX_NAVBAR_H
#define SBX_NAVBAR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build the left navigation bar on @p screen.
 * @param screen      The screen root object.
 * @param active_row  0=Home, 1=Info, 2=Config.
 * @return The nav panel object (75×480, pos 0,0).
 */
lv_obj_t * sbx_navbar_build(lv_obj_t * screen, int active_row);

#ifdef __cplusplus
}
#endif
#endif /* SBX_NAVBAR_H */
