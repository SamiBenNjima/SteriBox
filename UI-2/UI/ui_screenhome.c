// SteriBox - Home screen (redesigned)
// LVGL 8.3   800 x 480 px
//
// Two vertically-stacked "slides" inside a snap-scroll container:
//   Page 1 - CONTROL : chamber view + time + duration slider + START
//   Page 2 - GRAPH   : professional UV-C germ inactivation chart
//                      (log10 reduction over the cycle, per organism)
//
// The container snaps to whichever page is closest after a swipe.
// All public widget handles used by steribox_app.c are preserved.

#include "ui.h"

lv_obj_t * ui_screenhome = NULL;
lv_obj_t * ui_Image4 = NULL;
lv_obj_t * ui_navBar = NULL;
lv_obj_t * ui_BTN_Menu_Print_S4 = NULL;
lv_obj_t * ui_BTN_Menu_Move_S4 = NULL;
lv_obj_t * ui_BTN_Menu_Move_S1 = NULL;
lv_obj_t * ui_Panel_Header3 = NULL;
lv_obj_t * ui_Label_Header3 = NULL;
lv_obj_t * ui_Label_Time3 = NULL;
lv_obj_t * ui_IMG_Wifi3 = NULL;
lv_obj_t * ui_IMG_PC3 = NULL;
lv_obj_t * ui_IMG_USB3 = NULL;
lv_obj_t * ui_S1_Content_Panel3 = NULL;
lv_obj_t * ui_Panel8 = NULL;
lv_obj_t * ui_Slider_Print_View1 = NULL;
lv_obj_t * ui_Number_Print1 = NULL;
lv_obj_t * ui_Display_Time_S2 = NULL;
lv_obj_t * ui_Label_Printing_Time_4 = NULL;
lv_obj_t * ui_IMG_Tine_4 = NULL;
lv_obj_t * ui_Label_Time_5 = NULL;
lv_obj_t * ui_Label_Time_1 = NULL;
lv_obj_t * ui_Label_Time_2 = NULL;
lv_obj_t * ui_Panel_Slider2 = NULL;
lv_obj_t * ui_Slider_Print_Speed2 = NULL;
lv_obj_t * ui_Panel_Buttons_S2 = NULL;
lv_obj_t * ui_BTN_Pause1 = NULL;
lv_obj_t * ui_BTN_Pause_Top1 = NULL;
lv_obj_t * ui_Image_Pause1 = NULL;
lv_obj_t * ui_Label1 = NULL;
lv_obj_t * ui_Chart3 = NULL;

/* page-2 graph extras (file-local) */
static lv_obj_t * ui_graphPage = NULL;
static lv_obj_t * ui_pgdot0    = NULL;
static lv_obj_t * ui_pgdot1    = NULL;

#define HOME_PAGE_W 720
#define HOME_PAGE_H 432

/* Organism palette (shared with the legend) */
#define COL_ECOLI  0x00E0A0   /* E. coli    - UV sensitive  (teal)  */
#define COL_SAUR   0xFFB020   /* S. aureus  - medium        (amber) */
#define COL_ASPER  0xFF5470   /* A. niger   - UV resistant  (red)   */

// event funtions
void ui_event_BTN_Menu_Move_S4(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);

    if(event_code == LV_EVENT_CLICKED) {
        _ui_screen_change(&ui_screeninfo, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_screeninfo_screen_init);
    }
}

void ui_event_BTN_Menu_Move_S1(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);

    if(event_code == LV_EVENT_CLICKED) {
        _ui_screen_change(&ui_screeninfo, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_screeninfo_screen_init);
    }
}

void ui_event_Slider_Print_View1(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t * target = lv_event_get_target(e);

    if(event_code == LV_EVENT_VALUE_CHANGED) {
        _ui_slider_set_text_value(ui_Number_Print1, target, "", "%");
    }
}

void ui_event_Slider_Print_Speed2(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t * target = lv_event_get_target(e);

    if(event_code == LV_EVENT_VALUE_CHANGED) {
        _ui_slider_set_text_value(ui_Label_Time_1, target, "", "");
    }
}

/*--- Professional chart axis-label formatter ---------------------
 * Y ticks : range is stored x100 (0..600) -> show whole log units 0..6 */
static void home_chart_draw_cb(lv_event_t * e)
{
    lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);
    if(!lv_obj_draw_part_check_type(dsc, &lv_chart_class, LV_CHART_DRAW_PART_TICK_LABEL)) return;
    if(dsc->id == LV_CHART_AXIS_PRIMARY_Y && dsc->text) {
        lv_snprintf(dsc->text, dsc->text_length, "%ld", (long)(dsc->value / 100));
    }
}

/*--- Page indicator: colour the dot of the visible slide ---------*/
static void home_scroll_cb(lv_event_t * e)
{
    lv_obj_t * cont = lv_event_get_target(e);
    lv_coord_t sy = lv_obj_get_scroll_y(cont);
    bool on_graph = sy > (HOME_PAGE_H / 2);
    if(ui_pgdot0) lv_obj_set_style_bg_color(ui_pgdot0, lv_color_hex(on_graph ? 0x3A4152 : 0x00D2FF), 0);
    if(ui_pgdot1) lv_obj_set_style_bg_color(ui_pgdot1, lv_color_hex(on_graph ? 0x00D2FF : 0x3A4152), 0);
}

/*--- Small legend chip (dot + name) ------------------------------*/
static void legend_chip(lv_obj_t * parent, lv_coord_t x, uint32_t color, const char * name)
{
    lv_obj_t * dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_pos(dot, x, 3);
    lv_obj_set_style_radius(dot, 7, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, 255, 0);

    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, name);
    lv_obj_set_pos(lbl, x + 20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xC2CBDE), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
}

// build funtions

void ui_screenhome_screen_init(void)
{
    ui_screenhome = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_screenhome, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_SCROLLABLE |
                      LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);     /// Flags
    lv_obj_set_scrollbar_mode(ui_screenhome, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(ui_screenhome, lv_color_hex(SBX_COL_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_screenhome, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_Image4 = NULL;

    /* Left nav sidebar: single baked-in image + invisible touch zones */
    ui_navBar = lv_obj_create(ui_screenhome);
    lv_obj_remove_style_all(ui_navBar);
    lv_obj_set_size(ui_navBar, 75, 480);
    lv_obj_set_pos(ui_navBar, 0, 0);
    lv_obj_clear_flag(ui_navBar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);      /// Flags

    lv_obj_t * navImg = lv_img_create(ui_navBar);
    lv_img_set_src(navImg, &ui_img_nav_bar);
    lv_obj_set_pos(navImg, 0, 0);

    /* Active-tab accent (cyan left edge) — HOME section */
    sbx_nav_accent(ui_navBar, 0);

    /* Invisible touch zones over the image (HOME / INFO / CONFIG) */
    ui_BTN_Menu_Print_S4 = lv_obj_create(ui_navBar);
    lv_obj_remove_style_all(ui_BTN_Menu_Print_S4);
    lv_obj_set_size(ui_BTN_Menu_Print_S4, 75, 160);
    lv_obj_set_pos(ui_BTN_Menu_Print_S4, 0, 0);
    lv_obj_add_flag(ui_BTN_Menu_Print_S4, LV_OBJ_FLAG_CLICKABLE);

    ui_BTN_Menu_Move_S4 = lv_obj_create(ui_navBar);
    lv_obj_remove_style_all(ui_BTN_Menu_Move_S4);
    lv_obj_set_size(ui_BTN_Menu_Move_S4, 75, 160);
    lv_obj_set_pos(ui_BTN_Menu_Move_S4, 0, 160);
    lv_obj_add_flag(ui_BTN_Menu_Move_S4, LV_OBJ_FLAG_CLICKABLE);

    ui_BTN_Menu_Move_S1 = lv_obj_create(ui_navBar);
    lv_obj_remove_style_all(ui_BTN_Menu_Move_S1);
    lv_obj_set_size(ui_BTN_Menu_Move_S1, 75, 160);
    lv_obj_set_pos(ui_BTN_Menu_Move_S1, 0, 320);
    lv_obj_add_flag(ui_BTN_Menu_Move_S1, LV_OBJ_FLAG_CLICKABLE);

    ui_Panel_Header3 = lv_obj_create(ui_screenhome);
    lv_obj_set_width(ui_Panel_Header3, 712);
    lv_obj_set_height(ui_Panel_Header3, 45);
    lv_obj_set_x(ui_Panel_Header3, -6);
    lv_obj_set_y(ui_Panel_Header3, 6);
    lv_obj_set_align(ui_Panel_Header3, LV_ALIGN_TOP_RIGHT);
    lv_obj_clear_flag(ui_Panel_Header3, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                      LV_OBJ_FLAG_SCROLL_CHAIN);     /// Flags
    lv_obj_set_style_bg_color(ui_Panel_Header3, lv_color_hex(SBX_COL_HEADER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Panel_Header3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Panel_Header3, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Panel_Header3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label_Header3 = lv_label_create(ui_Panel_Header3);
    lv_obj_set_width(ui_Label_Header3, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label_Header3, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label_Header3, LV_ALIGN_LEFT_MID);
    lv_label_set_text(ui_Label_Header3, "Home");
    lv_obj_set_style_text_color(ui_Label_Header3, lv_color_hex(0x9098AA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_Header3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_Header3, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Live clock (RTC), driven by steribox_app.c */
    ui_Label_Time3 = lv_label_create(ui_Panel_Header3);
    lv_obj_set_width(ui_Label_Time3, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Label_Time3, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_Label_Time3, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label_Time3, "00:00");
    lv_obj_set_style_text_color(ui_Label_Time3, lv_color_hex(0xC2CBDE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_Time3, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Printer (PC) link indicator */
    ui_IMG_PC3 = lv_img_create(ui_Panel_Header3);
    lv_img_set_src(ui_IMG_PC3, &ui_img_icn_pc_png);
    lv_obj_set_width(ui_IMG_PC3, LV_SIZE_CONTENT);   /// 100
    lv_obj_set_height(ui_IMG_PC3, LV_SIZE_CONTENT);    /// 50
    lv_obj_set_x(ui_IMG_PC3, 0);
    lv_obj_set_y(ui_IMG_PC3, 0);
    lv_obj_set_align(ui_IMG_PC3, LV_ALIGN_RIGHT_MID);
    lv_obj_add_flag(ui_IMG_PC3, LV_OBJ_FLAG_ADV_HITTEST);     /// Flags

    /* USB (export) indicator */
    ui_IMG_USB3 = lv_img_create(ui_Panel_Header3);
    lv_img_set_src(ui_IMG_USB3, &ui_img_icn_usb_png);
    lv_obj_set_width(ui_IMG_USB3, LV_SIZE_CONTENT);   /// 100
    lv_obj_set_height(ui_IMG_USB3, LV_SIZE_CONTENT);    /// 50
    lv_obj_set_x(ui_IMG_USB3, -46);
    lv_obj_set_y(ui_IMG_USB3, 0);
    lv_obj_set_align(ui_IMG_USB3, LV_ALIGN_RIGHT_MID);
    lv_obj_add_flag(ui_IMG_USB3, LV_OBJ_FLAG_ADV_HITTEST);     /// Flags

    /*============================================================
     * SNAP-SCROLL CONTAINER (720 x 432, bottom-right of screen)
     *===========================================================*/
    ui_S1_Content_Panel3 = lv_obj_create(ui_screenhome);
    lv_obj_set_width(ui_S1_Content_Panel3, HOME_PAGE_W);
    lv_obj_set_height(ui_S1_Content_Panel3, HOME_PAGE_H);
    lv_obj_set_align(ui_S1_Content_Panel3, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_clear_flag(ui_S1_Content_Panel3,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                      LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SNAPPABLE);     /// Flags
    lv_obj_add_flag(ui_S1_Content_Panel3, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(ui_S1_Content_Panel3, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(ui_S1_Content_Panel3, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(ui_S1_Content_Panel3, LV_SCROLL_SNAP_START);
    lv_obj_set_style_bg_opa(ui_S1_Content_Panel3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_S1_Content_Panel3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_S1_Content_Panel3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_S1_Content_Panel3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_S1_Content_Panel3, home_scroll_cb, LV_EVENT_SCROLL, NULL);

    /*============================================================
     * PAGE 1 - CONTROL  (reuses ui_Panel8 handle)
     *===========================================================*/
    ui_Panel8 = lv_obj_create(ui_S1_Content_Panel3);
    lv_obj_set_size(ui_Panel8, HOME_PAGE_W, HOME_PAGE_H);
    lv_obj_set_pos(ui_Panel8, 0, 0);
    lv_obj_add_flag(ui_Panel8, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_clear_flag(ui_Panel8, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_opa(ui_Panel8, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Panel8, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_Panel8, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Chamber view (progress dome) - left side */
    ui_Slider_Print_View1 = lv_slider_create(ui_Panel8);
    lv_slider_set_value(ui_Slider_Print_View1, 0, LV_ANIM_OFF);
    lv_obj_set_width(ui_Slider_Print_View1, 320);
    lv_obj_set_height(ui_Slider_Print_View1, 380);
    lv_obj_set_align(ui_Slider_Print_View1, LV_ALIGN_LEFT_MID);
    lv_obj_set_x(ui_Slider_Print_View1, 8);
    lv_obj_set_y(ui_Slider_Print_View1, 0);
    lv_obj_set_style_radius(ui_Slider_Print_View1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Slider_Print_View1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_src(ui_Slider_Print_View1, &ui_img_print_view_bg_png, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Slider_Print_View1, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Slider_Print_View1, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_src(ui_Slider_Print_View1, &ui_img_print_view_front_png, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Slider_Print_View1, 0, LV_PART_KNOB | LV_STATE_DEFAULT);

    ui_Number_Print1 = lv_label_create(ui_Slider_Print_View1);
    lv_obj_set_width(ui_Number_Print1, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Number_Print1, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Number_Print1, 0);
    lv_obj_set_y(ui_Number_Print1, -34);
    lv_obj_set_align(ui_Number_Print1, LV_ALIGN_BOTTOM_MID);
    lv_label_set_text(ui_Number_Print1, "0%");
    lv_obj_set_style_text_color(ui_Number_Print1, lv_color_hex(0x00D2FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Number_Print1, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Time display - top right */
    ui_Display_Time_S2 = lv_obj_create(ui_Panel8);
    lv_obj_set_width(ui_Display_Time_S2, 350);
    lv_obj_set_height(ui_Display_Time_S2, 140);
    lv_obj_set_align(ui_Display_Time_S2, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_x(ui_Display_Time_S2, -14);
    lv_obj_set_y(ui_Display_Time_S2, 15);
    lv_obj_clear_flag(ui_Display_Time_S2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Display_Time_S2, lv_color_hex(0x191D26), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Display_Time_S2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Display_Time_S2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Display_Time_S2, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_color(ui_Display_Time_S2, lv_color_hex(0x414B62), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_opa(ui_Display_Time_S2, 100, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui_Display_Time_S2, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_Display_Time_S2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label_Printing_Time_4 = lv_label_create(ui_Display_Time_S2);
    lv_obj_set_width(ui_Label_Printing_Time_4, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Label_Printing_Time_4, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Label_Printing_Time_4, -8);
    lv_obj_set_y(ui_Label_Printing_Time_4, 6);
    lv_obj_set_align(ui_Label_Printing_Time_4, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_Label_Printing_Time_4, "Time");
    lv_obj_set_style_text_color(ui_Label_Printing_Time_4, lv_color_hex(0x9098AA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_Printing_Time_4, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_IMG_Tine_4 = lv_img_create(ui_Display_Time_S2);
    lv_img_set_src(ui_IMG_Tine_4, &ui_img_icn_time_2_png);
    lv_obj_set_width(ui_IMG_Tine_4, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_IMG_Tine_4, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_IMG_Tine_4, -73);
    lv_obj_set_y(ui_IMG_Tine_4, 12);
    lv_obj_set_align(ui_IMG_Tine_4, LV_ALIGN_TOP_MID);
    lv_obj_add_flag(ui_IMG_Tine_4, LV_OBJ_FLAG_ADV_HITTEST);     /// Flags

    /* Single, tightly-spaced "M:SS" readout (lives in ui_Label_Time_1) */
    ui_Label_Time_1 = lv_label_create(ui_Display_Time_S2);
    lv_obj_set_width(ui_Label_Time_1, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Label_Time_1, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_Label_Time_1, LV_ALIGN_CENTER);
    lv_obj_set_y(ui_Label_Time_1, 24);
    lv_label_set_text(ui_Label_Time_1, "1:00");
    lv_obj_set_style_text_color(ui_Label_Time_1, lv_color_hex(0x00CCFC), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_Time_1, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Legacy colon / seconds labels — hidden (kept for app + destroy refs) */
    ui_Label_Time_5 = lv_label_create(ui_Display_Time_S2);
    lv_label_set_text(ui_Label_Time_5, "");
    lv_obj_add_flag(ui_Label_Time_5, LV_OBJ_FLAG_HIDDEN);

    ui_Label_Time_2 = lv_label_create(ui_Display_Time_S2);
    lv_label_set_text(ui_Label_Time_2, "");
    lv_obj_add_flag(ui_Label_Time_2, LV_OBJ_FLAG_HIDDEN);

    /* Duration slider - middle right */
    ui_Panel_Slider2 = lv_obj_create(ui_Panel8);
    lv_obj_set_width(ui_Panel_Slider2, 376);
    lv_obj_set_height(ui_Panel_Slider2, 64);
    lv_obj_set_align(ui_Panel_Slider2, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_x(ui_Panel_Slider2, -14);
    lv_obj_set_y(ui_Panel_Slider2, 169);
    lv_obj_clear_flag(ui_Panel_Slider2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_opa(ui_Panel_Slider2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Panel_Slider2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Slider_Print_Speed2 = lv_slider_create(ui_Panel_Slider2);
    lv_slider_set_range(ui_Slider_Print_Speed2, 1, 10);
    lv_slider_set_value(ui_Slider_Print_Speed2, 1, LV_ANIM_OFF);
    lv_obj_set_width(ui_Slider_Print_Speed2, 330);
    lv_obj_set_height(ui_Slider_Print_Speed2, 30);
    lv_obj_set_align(ui_Slider_Print_Speed2, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_Slider_Print_Speed2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Slider_Print_Speed2, lv_color_hex(0x222733), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Slider_Print_Speed2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_color(ui_Slider_Print_Speed2, lv_color_hex(0x191D26), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_opa(ui_Slider_Print_Speed2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui_Slider_Print_Speed2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* Inset the knob travel so it is never clipped at the track ends */
    lv_obj_set_style_pad_left(ui_Slider_Print_Speed2, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_Slider_Print_Speed2, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_Slider_Print_Speed2, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Slider_Print_Speed2, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Slider_Print_Speed2, lv_color_hex(0x1DE8FF), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Slider_Print_Speed2, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(ui_Slider_Print_Speed2, lv_color_hex(0x0962B7), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui_Slider_Print_Speed2, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Slider_Print_Speed2, lv_color_hex(0x7689AC), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Slider_Print_Speed2, 255, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui_Slider_Print_Speed2, lv_color_hex(0x28DCFD), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui_Slider_Print_Speed2, 255, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Slider_Print_Speed2, 4, LV_PART_KNOB | LV_STATE_DEFAULT);

    /* START button - bottom right */
    ui_Panel_Buttons_S2 = lv_obj_create(ui_Panel8);
    lv_obj_set_width(ui_Panel_Buttons_S2, 360);
    lv_obj_set_height(ui_Panel_Buttons_S2, 170);
    lv_obj_set_align(ui_Panel_Buttons_S2, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_x(ui_Panel_Buttons_S2, -14);
    lv_obj_set_y(ui_Panel_Buttons_S2, -15);
    lv_obj_clear_flag(ui_Panel_Buttons_S2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_opa(ui_Panel_Buttons_S2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Panel_Buttons_S2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_Panel_Buttons_S2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_BTN_Pause1 = lv_img_create(ui_Panel_Buttons_S2);
    lv_img_set_src(ui_BTN_Pause1, &ui_img_btn_print_down_v2_png);
    lv_obj_set_width(ui_BTN_Pause1, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_BTN_Pause1, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_BTN_Pause1, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_BTN_Pause1, LV_OBJ_FLAG_ADV_HITTEST);     /// Flags

    ui_BTN_Pause_Top1 = lv_img_create(ui_BTN_Pause1);
    lv_img_set_src(ui_BTN_Pause_Top1, &ui_img_btn_print_top_off_png);
    lv_obj_set_width(ui_BTN_Pause_Top1, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_BTN_Pause_Top1, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_BTN_Pause_Top1, 1);
    lv_obj_set_y(ui_BTN_Pause_Top1, -6);
    lv_obj_set_align(ui_BTN_Pause_Top1, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_BTN_Pause_Top1, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CHECKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_style_radius(ui_BTN_Pause_Top1, 30, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui_BTN_Pause_Top1, lv_color_hex(0x00D2FF), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(ui_BTN_Pause_Top1, 255, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(ui_BTN_Pause_Top1, 9, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_spread(ui_BTN_Pause_Top1, 3, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_color(ui_BTN_Pause_Top1, lv_color_hex(0x00D2FF), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(ui_BTN_Pause_Top1, 255, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_BTN_Pause_Top1, 5, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_BTN_Pause_Top1, 5, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_img_recolor(ui_BTN_Pause_Top1, lv_color_hex(0x67799B), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_img_recolor_opa(ui_BTN_Pause_Top1, 255, LV_PART_MAIN | LV_STATE_PRESSED);

    ui_Image_Pause1 = lv_img_create(ui_BTN_Pause_Top1);
    lv_img_set_src(ui_Image_Pause1, &ui_img_arrow_right_png);
    lv_obj_set_width(ui_Image_Pause1, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Image_Pause1, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Image_Pause1, -140);
    lv_obj_set_y(ui_Image_Pause1, 0);
    lv_obj_set_align(ui_Image_Pause1, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Image_Pause1, LV_OBJ_FLAG_ADV_HITTEST);     /// Flags
    lv_obj_set_style_img_recolor_opa(ui_Image_Pause1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label1 = lv_label_create(ui_BTN_Pause_Top1);
    lv_obj_set_width(ui_Label1, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Label1, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Label1, -3);
    lv_obj_set_y(ui_Label1, -1);
    lv_obj_set_align(ui_Label1, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label1, "START");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label1, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);

    /*============================================================
     * PAGE 2 - GRAPH  (professional UV-C inactivation chart)
     *===========================================================*/
    ui_graphPage = lv_obj_create(ui_S1_Content_Panel3);
    lv_obj_set_size(ui_graphPage, HOME_PAGE_W, HOME_PAGE_H);
    lv_obj_set_pos(ui_graphPage, 0, HOME_PAGE_H);
    lv_obj_add_flag(ui_graphPage, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_clear_flag(ui_graphPage, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_opa(ui_graphPage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_graphPage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_graphPage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Card behind the chart for a clean, readable surface */
    lv_obj_t * chartCard = lv_obj_create(ui_graphPage);
    lv_obj_set_size(chartCard, 690, 420);
    lv_obj_set_align(chartCard, LV_ALIGN_CENTER);
    lv_obj_set_y(chartCard, -4);
    lv_obj_clear_flag(chartCard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(chartCard, lv_color_hex(0x141A24), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(chartCard, 235, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(chartCard, lv_color_hex(0x2A3345), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(chartCard, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(chartCard, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(chartCard, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * gtitle = lv_label_create(chartCard);
    lv_label_set_text(gtitle, "UV-C Inactivation");
    lv_obj_set_align(gtitle, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(gtitle, 18, 12);
    lv_obj_set_style_text_color(gtitle, lv_color_hex(0xEAF2FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(gtitle, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * gsub = lv_label_create(chartCard);
    lv_label_set_text(gsub, "log10 reduction over exposure");
    lv_obj_set_align(gsub, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(gsub, 18, 34);
    lv_obj_set_style_text_color(gsub, lv_color_hex(0x7C89A5), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(gsub, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Y-axis caption */
    lv_obj_t * ycap = lv_label_create(chartCard);
    lv_label_set_text(ycap, "log10");
    lv_obj_set_align(ycap, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(ycap, 8, 60);
    lv_obj_set_style_text_color(ycap, lv_color_hex(0x7C89A5), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ycap, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* The chart */
    ui_Chart3 = lv_chart_create(chartCard);
    lv_obj_set_size(ui_Chart3, 600, 250);
    lv_obj_set_align(ui_Chart3, LV_ALIGN_TOP_MID);
    lv_obj_set_x(ui_Chart3, 22);
    lv_obj_set_y(ui_Chart3, 82);
    lv_chart_set_type(ui_Chart3, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ui_Chart3, 16);
    lv_chart_set_range(ui_Chart3, LV_CHART_AXIS_PRIMARY_Y, 0, 700);   /* fixed 0..7 log */
    lv_chart_set_div_line_count(ui_Chart3, 8, 6);
    lv_chart_set_axis_tick(ui_Chart3, LV_CHART_AXIS_PRIMARY_Y, 6, 3, 8, 1, true, 40);
    lv_chart_set_axis_tick(ui_Chart3, LV_CHART_AXIS_PRIMARY_X, 6, 0, 6, 1, false, 20);

    /* Series are created & managed by steribox_app.c (11 toggleable organisms) */

    /* Chart styling - dark, flat, dynamic (thick rounded lines + point dots) */
    lv_obj_set_style_bg_color(ui_Chart3, lv_color_hex(0x0E141C), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Chart3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Chart3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Chart3, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui_Chart3, lv_color_hex(0x263042), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(ui_Chart3, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_Chart3, lv_color_hex(0x8A97B3), LV_PART_TICKS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui_Chart3, lv_color_hex(0x3A4558), LV_PART_TICKS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(ui_Chart3, 3, LV_PART_ITEMS | LV_STATE_DEFAULT);      /* series thickness */
    lv_obj_set_style_line_rounded(ui_Chart3, true, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_size(ui_Chart3, 4, LV_PART_INDICATOR | LV_STATE_DEFAULT);        /* point dots */
    lv_obj_add_event_cb(ui_Chart3, home_chart_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    /* X-axis caption */
    lv_obj_t * xcap = lv_label_create(chartCard);
    lv_label_set_text(xcap, "Exposure time");
    lv_obj_set_align(xcap, LV_ALIGN_TOP_MID);
    lv_obj_set_pos(xcap, 22, 342);
    lv_obj_set_style_text_color(xcap, lv_color_hex(0x7C89A5), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(xcap, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Legend */
    lv_obj_t * legend = lv_obj_create(chartCard);
    lv_obj_remove_style_all(legend);
    lv_obj_set_size(legend, 660, 26);
    lv_obj_set_align(legend, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_y(legend, -8);
    lv_obj_clear_flag(legend, LV_OBJ_FLAG_SCROLLABLE);
    legend_chip(legend, 40,  COL_ECOLI, "E. coli");
    legend_chip(legend, 250, COL_SAUR,  "S. aureus");
    legend_chip(legend, 470, COL_ASPER, "A. niger");

    /*============================================================
     * PAGE INDICATOR (two dots on the right edge)
     *===========================================================*/
    ui_pgdot0 = lv_obj_create(ui_screenhome);
    lv_obj_remove_style_all(ui_pgdot0);
    lv_obj_set_size(ui_pgdot0, 9, 9);
    lv_obj_set_align(ui_pgdot0, LV_ALIGN_RIGHT_MID);
    lv_obj_set_pos(ui_pgdot0, -6, -12);
    lv_obj_set_style_radius(ui_pgdot0, 5, 0);
    lv_obj_set_style_bg_color(ui_pgdot0, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_bg_opa(ui_pgdot0, 255, 0);

    ui_pgdot1 = lv_obj_create(ui_screenhome);
    lv_obj_remove_style_all(ui_pgdot1);
    lv_obj_set_size(ui_pgdot1, 9, 9);
    lv_obj_set_align(ui_pgdot1, LV_ALIGN_RIGHT_MID);
    lv_obj_set_pos(ui_pgdot1, -6, 12);
    lv_obj_set_style_radius(ui_pgdot1, 5, 0);
    lv_obj_set_style_bg_color(ui_pgdot1, lv_color_hex(0x3A4152), 0);
    lv_obj_set_style_bg_opa(ui_pgdot1, 255, 0);

    lv_obj_add_event_cb(ui_BTN_Menu_Move_S4, ui_event_BTN_Menu_Move_S4, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_BTN_Menu_Move_S1, ui_event_BTN_Menu_Move_S1, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Slider_Print_View1, ui_event_Slider_Print_View1, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Slider_Print_Speed2, ui_event_Slider_Print_Speed2, LV_EVENT_ALL, NULL);

    /* Land on the CONTROL slide, not the graph */
    lv_obj_update_layout(ui_S1_Content_Panel3);
    lv_obj_scroll_to_y(ui_S1_Content_Panel3, 0, LV_ANIM_OFF);
}

void ui_screenhome_screen_destroy(void)
{
    if(ui_screenhome) lv_obj_del(ui_screenhome);

    // NULL screen variables
    ui_screenhome = NULL;
    ui_Image4 = NULL;
    ui_navBar = NULL;
    ui_BTN_Menu_Print_S4 = NULL;
    ui_BTN_Menu_Move_S4 = NULL;
    ui_BTN_Menu_Move_S1 = NULL;
    ui_Panel_Header3 = NULL;
    ui_Label_Header3 = NULL;
    ui_Label_Time3 = NULL;
    ui_IMG_Wifi3 = NULL;
    ui_IMG_PC3 = NULL;
    ui_IMG_USB3 = NULL;
    ui_S1_Content_Panel3 = NULL;
    ui_Panel8 = NULL;
    ui_Slider_Print_View1 = NULL;
    ui_Number_Print1 = NULL;
    ui_Display_Time_S2 = NULL;
    ui_Label_Printing_Time_4 = NULL;
    ui_IMG_Tine_4 = NULL;
    ui_Label_Time_5 = NULL;
    ui_Label_Time_1 = NULL;
    ui_Label_Time_2 = NULL;
    ui_Panel_Slider2 = NULL;
    ui_Slider_Print_Speed2 = NULL;
    ui_Panel_Buttons_S2 = NULL;
    ui_BTN_Pause1 = NULL;
    ui_BTN_Pause_Top1 = NULL;
    ui_Image_Pause1 = NULL;
    ui_Label1 = NULL;
    ui_Chart3 = NULL;
    ui_graphPage = NULL;
    ui_pgdot0 = NULL;
    ui_pgdot1 = NULL;
}
