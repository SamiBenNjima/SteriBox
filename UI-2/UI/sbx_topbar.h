/**
 * @file sbx_topbar.h
 * SteriBox — Shared top-bar helper + device status API
 *
 * Provides:
 *   sbx_topbar_build()   — builds the right-side icon group
 *                           (SD card + USB drive + Printer)
 *   sbx_topbar_refresh() — call every lv_timer tick to update
 *                           icon visibility / colour from HAL
 *   sbx_export_or_popup()/ sbx_print_or_popup() — action guards
 */
#ifndef SBX_TOPBAR_H
#define SBX_TOPBAR_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Icon set attached to one header panel ----------------------------------- */
typedef struct {
    lv_obj_t * icn_sd;      /* SD card  icon   (green = present) */
    lv_obj_t * icn_usb;     /* USB drive icon  (cyan = mounted)  */
    lv_obj_t * icn_printer; /* Printer icon    (white = online)  */
} sbx_topbar_icons_t;

/* Add the three right-side icons to an existing header panel.
 * header  — the lv_obj panel (width ~712, height 45)
 * icons   — caller-allocated struct, filled in by this call           */
void sbx_topbar_build(lv_obj_t * header, sbx_topbar_icons_t * icons);

/* Refresh icon tint/visibility from the current HAL state.
 * Call once per second from steribox_app.c clock_task.               */
void sbx_topbar_refresh(const sbx_topbar_icons_t * icons);

/* Export-report guard:
 *   • SD absent  → shows error popup "No SD card inserted"
 *   • SD present → calls sbx_hal_usb_export(filename, text)
 * Returns true if file was written.                                   */
bool sbx_try_export(lv_obj_t * parent, const char * filename,
                    const char * text);

/* Print-report guard:
 *   • Printer absent → shows error popup "No printer connected"
 *   • Printer present → calls sbx_hal_usb_print(text)
 * Returns true if print command was sent.                             */
bool sbx_try_print(lv_obj_t * parent, const char * text);

/* Simple modal error popup (used by the guards above).
 * Closes itself after 3 s or on tap.                                 */
void sbx_popup_error(lv_obj_t * parent, const char * msg);

#ifdef __cplusplus
}
#endif
#endif /* SBX_TOPBAR_H */
