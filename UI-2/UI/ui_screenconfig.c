// SteriBox – Config screen (redesigned)
// LVGL 8.3   800 × 480 px
// Layout: left sidebar (79 px) | scrollable content area (721 px)
//   Card 1 – General Settings  (date + 3-roller time + Synchronize)
//   Card 2 – Lamp Resets        (arc gauge × 2, Reset buttons)
//   Apply Changes button at the bottom-right

#include "ui.h"
#include "steribox_app.h"

/* Synchronize button: apply the date/time to the RTC without leaving */
static void sync_btn_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) steribox_ev_save_config();
}

/* ── widget handles ─────────────────────────────────────────────── */
lv_obj_t * ui_screenconfig        = NULL;
lv_obj_t * ui_Image2              = NULL;
lv_obj_t * ui_BTN_Menu_Setting_S3 = NULL;
lv_obj_t * ui_BTN_Menu_Move_S7   = NULL;
lv_obj_t * ui_BTN_Menu_Move_S6   = NULL;

/* password popup */
lv_obj_t * ui_popup      = NULL;
lv_obj_t * ui_Panel2     = NULL;
lv_obj_t * ui_Label2     = NULL;
lv_obj_t * ui_pwd        = NULL;
lv_obj_t * ui_Button1    = NULL;
lv_obj_t * ui_Label3     = NULL;
lv_obj_t * ui_Button2    = NULL;
lv_obj_t * ui_Label4     = NULL;
lv_obj_t * ui_Keyboard1  = NULL;

/* main content (hidden until password OK) */
lv_obj_t * ui_mainbody   = NULL;

/* General Settings widgets */
lv_obj_t * ui_TextArea5  = NULL;   /* date field   */
lv_obj_t * ui_Roller1    = NULL;   /* hour         */
lv_obj_t * ui_Roller2    = NULL;   /* minute       */
lv_obj_t * ui_Roller3    = NULL;   /* AM / PM      */

/* calendar overlay */
lv_obj_t * ui_layer2    = NULL;
lv_obj_t * ui_Calendar2 = NULL;

/* Lamp card widgets */
lv_obj_t * ui_arc_lamp1         = NULL;
lv_obj_t * ui_arc_lamp2         = NULL;
lv_obj_t * ui_lamp1_hours_label = NULL;
lv_obj_t * ui_lamp2_hours_label = NULL;
lv_obj_t * ui_lamp1_pct_label   = NULL;
lv_obj_t * ui_lamp2_pct_label   = NULL;
lv_obj_t * ui_lampe_1           = NULL;   /* lamp1 reset btn */
lv_obj_t * ui_lampe_2           = NULL;   /* lamp2 reset btn */

/* confirmation popup */
lv_obj_t * ui_confirm_popup  = NULL;
lv_obj_t * ui_confirm_label  = NULL;
lv_obj_t * ui_confirm_yes    = NULL;
lv_obj_t * ui_confirm_no     = NULL;

/* Apply Changes button */
lv_obj_t * ui_BTN_Apply = NULL;

/* ── stubs for widgets removed in this redesign but still referenced
 *    in steribox_app.c (usb_icons_refresh, panel header, etc.)      */
lv_obj_t * ui_IMG_USB6         = NULL;
lv_obj_t * ui_Label_Time5      = NULL;
lv_obj_t * ui_Panel_Header5    = NULL;
lv_obj_t * ui_Label_Header5    = NULL;

/* page indicator dots (file-local) */
static lv_obj_t * ui_cfg_dot0 = NULL;
static lv_obj_t * ui_cfg_dot1 = NULL;


/* ── colour palette ─────────────────────────────────────────────── */
#define COL_BG       SBX_COL_BG
#define COL_CARD     0x28344A   /* lighter slate so cards read from 2.5 m */
#define COL_CARD2    0x28344A
#define COL_ACCENT   0x00CCFC
#define COL_WARN     0xFF7700
#define COL_TEXT     0xC2CBDE   /* brighter label text for visibility     */
#define COL_VALUE    0xEAF2FF   /* bright value text                      */
#define COL_SUBTEXT  0x9BA6BD
#define COL_BORDER   0x4A587A
#define COL_BTN_BG   0x00CCFC

/* ── event callbacks ────────────────────────────────────────────── */
void ui_event_screenconfig(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED)
        _ui_flag_modify(ui_popup, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
}

void ui_event_BTN_Menu_Setting_S3(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED)
        _ui_screen_change(&ui_screenhome, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                          &ui_screenhome_screen_init);
}

void ui_event_BTN_Menu_Move_S7(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED)
        _ui_screen_change(&ui_screeninfo, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                          &ui_screeninfo_screen_init);
}

void ui_event_Panel2(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_PRESSED)
        _ui_flag_modify(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
}

void ui_event_pwd(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        _ui_keyboard_set_target(ui_Keyboard1, ui_pwd);
        _ui_flag_modify(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
    }
}

void ui_event_Button1(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) cancel_pwd(e);
}

void ui_event_Button2(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) confirm_pwd(e);
}

void ui_event_TextArea5(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        _ui_flag_modify(ui_Calendar2, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
        _ui_flag_modify(ui_layer2,    LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
    }
}

/* ── internal helpers ───────────────────────────────────────────── */
static lv_obj_t * make_card(lv_obj_t * parent, lv_coord_t w, lv_coord_t h,
                              uint32_t col)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card,     lv_color_hex(col),        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card,       255,                       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 0,                         LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_color(card, lv_color_hex(COL_BORDER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(card, 4,                        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_opa(card,  100,                       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(card,       12,                        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card,      14,                        LV_PART_MAIN | LV_STATE_DEFAULT);
    return card;
}

static lv_obj_t * make_label(lv_obj_t * p, const char * txt, uint32_t col,
                               const lv_font_t * font)
{
    lv_obj_t * l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    return l;
}

static void style_roller(lv_obj_t * r)
{
    lv_obj_set_style_text_color(r, lv_color_hex(COL_SUBTEXT), LV_PART_MAIN     | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(r,   lv_color_hex(0x162030),    LV_PART_MAIN     | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(r,     255,                        LV_PART_MAIN     | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(r, 0,                        LV_PART_MAIN     | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(r,     8,                          LV_PART_MAIN     | LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(r, 14,                    LV_PART_MAIN     | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(r, lv_color_hex(COL_ACCENT),  LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(r,   lv_color_hex(0x1A2D3E),    LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(r,     255,                        LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(r,  &lv_font_montserrat_36,    LV_PART_MAIN     | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(r,  &lv_font_montserrat_36,    LV_PART_SELECTED | LV_STATE_DEFAULT);
}

/* Colour the page-indicator dot for the visible config slide */
static void cfg_scroll_cb(lv_event_t * e)
{
    lv_obj_t * cont = lv_event_get_target(e);
    bool on_lamps = lv_obj_get_scroll_y(cont) > (439 / 2);
    if(ui_cfg_dot0) lv_obj_set_style_bg_color(ui_cfg_dot0, lv_color_hex(on_lamps ? 0x3A4152 : COL_ACCENT), 0);
    if(ui_cfg_dot1) lv_obj_set_style_bg_color(ui_cfg_dot1, lv_color_hex(on_lamps ? COL_ACCENT : 0x3A4152), 0);
}

static lv_obj_t * make_cyan_btn(lv_obj_t * p, const char * txt,
                                  lv_coord_t w, lv_coord_t h,
                                  const lv_font_t * font)
{
    lv_obj_t * btn = lv_btn_create(p);
    lv_obj_set_size(btn, w, h);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(btn,     lv_color_hex(COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn,       255,                       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn,       10,                        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0,                         LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_align(l, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(l, lv_color_hex(0x06222E),       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(l, font,                          LV_PART_MAIN | LV_STATE_DEFAULT);
    return btn;
}

/* Build one lamp column inside lamp_row and store widget pointers */
static void build_lamp_col(lv_obj_t * row,
                             const char * name,
                             uint32_t arc_col,
                             const char * icon_sym,
                             lv_obj_t ** arc_out,
                             lv_obj_t ** pct_out,
                             lv_obj_t ** hrs_out,
                             lv_obj_t ** rst_out)
{
    lv_obj_t * col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 300, 340);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Arc */
    lv_obj_t * arc = lv_arc_create(col);
    lv_obj_set_size(arc, 190, 190);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_value(arc, 80);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x1A2D3E),  LV_PART_MAIN      | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(arc, 16,                       LV_PART_MAIN      | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(arc, lv_color_hex(arc_col),   LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(arc, 16,                       LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    *arc_out = arc;

    /* Icon centred inside arc */
    lv_obj_t * icon = lv_label_create(arc);
    lv_label_set_text(icon, icon_sym);
    lv_obj_set_align(icon, LV_ALIGN_CENTER);
    lv_obj_set_y(icon, -22);
    lv_obj_set_style_text_color(icon, lv_color_hex(arc_col),   LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Lamp name inside arc */
    lv_obj_t * name_l = lv_label_create(arc);
    lv_label_set_text(name_l, name);
    lv_obj_set_align(name_l, LV_ALIGN_CENTER);
    lv_obj_set_y(name_l, 26);
    lv_obj_set_style_text_color(name_l, lv_color_hex(COL_VALUE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(name_l, &lv_font_montserrat_16,  LV_PART_MAIN | LV_STATE_DEFAULT);

    /* % remaining */
    *pct_out = make_label(col, "-- % Remaining", arc_col, &lv_font_montserrat_16);

    /* hours remaining */
    *hrs_out = make_label(col, "-- h left", COL_SUBTEXT, &lv_font_montserrat_36);

    /* Reset button */
    *rst_out = make_cyan_btn(col, "Reset", 170, 56, &lv_font_montserrat_36);
}

/* ══════════════════════════════════════════════════════════════════
 *  Screen init
 * ══════════════════════════════════════════════════════════════════ */
void ui_screenconfig_screen_init(void)
{
    /* ── root ─────────────────────────────────────────────────────── */
    ui_screenconfig = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_screenconfig, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_screenconfig, lv_color_hex(COL_BG),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_screenconfig, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Left nav sidebar image + active accent (CONFIG) ──────────── */
    ui_Image2 = NULL;
    lv_obj_t * navImg = lv_img_create(ui_screenconfig);
    lv_img_set_src(navImg, &ui_img_nav_bar);
    lv_obj_set_pos(navImg, 0, 0);
    sbx_nav_accent(ui_screenconfig, 2);

    /* ── Header (rounded filled box) ──────────────────────────────── */
    ui_Panel_Header5 = lv_obj_create(ui_screenconfig);
    lv_obj_set_width(ui_Panel_Header5, 712);
    lv_obj_set_height(ui_Panel_Header5, 45);
    lv_obj_set_x(ui_Panel_Header5, -6);
    lv_obj_set_y(ui_Panel_Header5, 6);
    lv_obj_set_align(ui_Panel_Header5, LV_ALIGN_TOP_RIGHT);
    lv_obj_clear_flag(ui_Panel_Header5, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Panel_Header5, lv_color_hex(SBX_COL_HEADER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Panel_Header5, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Panel_Header5, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Panel_Header5, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label_Header5 = lv_label_create(ui_Panel_Header5);
    lv_obj_set_width(ui_Label_Header5, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Label_Header5, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_Label_Header5, LV_ALIGN_LEFT_MID);
    lv_label_set_text(ui_Label_Header5, "Config");
    lv_obj_set_style_text_color(ui_Label_Header5, lv_color_hex(0x9098AA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_Header5, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_Header5, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label_Time5 = lv_label_create(ui_Panel_Header5);
    lv_obj_set_width(ui_Label_Time5, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Label_Time5, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_Label_Time5, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label_Time5, "00:00");
    lv_obj_set_style_text_color(ui_Label_Time5, lv_color_hex(0x9098AA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_Time5, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Printer (PC) link indicator */
    lv_obj_t * cfg_pc = lv_img_create(ui_Panel_Header5);
    lv_img_set_src(cfg_pc, &ui_img_icn_pc_png);
    lv_obj_set_width(cfg_pc, LV_SIZE_CONTENT);
    lv_obj_set_height(cfg_pc, LV_SIZE_CONTENT);
    lv_obj_set_x(cfg_pc, -4);
    lv_obj_set_y(cfg_pc, 0);
    lv_obj_set_align(cfg_pc, LV_ALIGN_RIGHT_MID);
    lv_obj_add_flag(cfg_pc, LV_OBJ_FLAG_ADV_HITTEST);

    /* USB (export) indicator */
    ui_IMG_USB6 = lv_img_create(ui_Panel_Header5);
    lv_img_set_src(ui_IMG_USB6, &ui_img_icn_usb_png);
    lv_obj_set_width(ui_IMG_USB6, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_IMG_USB6, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_IMG_USB6, -50);
    lv_obj_set_y(ui_IMG_USB6, 0);
    lv_obj_set_align(ui_IMG_USB6, LV_ALIGN_RIGHT_MID);
    lv_obj_add_flag(ui_IMG_USB6, LV_OBJ_FLAG_ADV_HITTEST);

    /* ── Nav hit-areas (invisible, keep SquareLine routing) ──────── */
    ui_BTN_Menu_Setting_S3 = lv_img_create(ui_screenconfig);
    lv_img_set_src(ui_BTN_Menu_Setting_S3, &ui_img_btn_setting_png);
    lv_obj_set_size(ui_BTN_Menu_Setting_S3, 79, 160);
    lv_obj_add_flag(ui_BTN_Menu_Setting_S3,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_BTN_Menu_Setting_S3, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(ui_BTN_Menu_Setting_S3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_BTN_Menu_Move_S7 = lv_img_create(ui_screenconfig);
    lv_img_set_src(ui_BTN_Menu_Move_S7, &ui_img_btn_move_png);
    lv_obj_set_size(ui_BTN_Menu_Move_S7, 79, 160);
    lv_obj_set_pos(ui_BTN_Menu_Move_S7, 0, 160);
    lv_obj_add_flag(ui_BTN_Menu_Move_S7,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_BTN_Menu_Move_S7, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(ui_BTN_Menu_Move_S7, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_BTN_Menu_Move_S6 = lv_img_create(ui_screenconfig);
    lv_img_set_src(ui_BTN_Menu_Move_S6, &ui_img_btn_setting_png);
    lv_obj_set_size(ui_BTN_Menu_Move_S6, 79, 160);
    lv_obj_set_pos(ui_BTN_Menu_Move_S6, 0, 320);
    lv_obj_add_flag(ui_BTN_Menu_Move_S6,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_BTN_Menu_Move_S6, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(ui_BTN_Menu_Move_S6, 0, LV_PART_MAIN | LV_STATE_DEFAULT);  /* icon shown by nav_bar */

    /* ── Password overlay ─────────────────────────────────────────── */
    ui_popup = lv_obj_create(ui_screenconfig);
    lv_obj_remove_style_all(ui_popup);
    lv_obj_set_size(ui_popup, 800, 480);
    lv_obj_set_align(ui_popup, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_popup, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_popup, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_popup, 170, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Panel2 = lv_obj_create(ui_popup);
    lv_obj_set_size(ui_Panel2, 440, 210);
    lv_obj_set_align(ui_Panel2, LV_ALIGN_CENTER);
    lv_obj_set_y(ui_Panel2, -70);
    lv_obj_clear_flag(ui_Panel2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Panel2,     lv_color_hex(0x191D26), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Panel2,       255,                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Panel2, lv_color_hex(COL_ACCENT),LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Panel2, 2,                       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Panel2,       14,                      LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label2 = lv_label_create(ui_Panel2);
    lv_label_set_text(ui_Label2, LV_SYMBOL_SETTINGS "  Enter Configuration Password");
    lv_obj_set_align(ui_Label2, LV_ALIGN_TOP_MID);
    lv_obj_set_y(ui_Label2, 14);
    lv_obj_set_style_text_color(ui_Label2, lv_color_hex(COL_TEXT),   LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label2,  &lv_font_montserrat_16,   LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_pwd = lv_textarea_create(ui_Panel2);
    lv_obj_set_size(ui_pwd, 360, 44);
    lv_obj_set_align(ui_pwd, LV_ALIGN_TOP_MID);
    lv_obj_set_y(ui_pwd, 48);
    lv_textarea_set_placeholder_text(ui_pwd, "config_password");
    lv_textarea_set_password_mode(ui_pwd, true);
    lv_textarea_set_one_line(ui_pwd, true);
    lv_obj_set_style_bg_color(ui_pwd,   lv_color_hex(0x101C2A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_pwd, lv_color_hex(COL_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Confirm button */
    ui_Button2 = lv_btn_create(ui_Panel2);
    lv_obj_set_size(ui_Button2, 152, 44);
    lv_obj_set_align(ui_Button2, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_pos(ui_Button2, 16, -14);
    lv_obj_clear_flag(ui_Button2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Button2,     lv_color_hex(COL_ACCENT),   LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button2,       255,                         LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Button2,       8,                           LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button2, 0,                           LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_Label4 = lv_label_create(ui_Button2);
    lv_label_set_text(ui_Label4, "Confirm");
    lv_obj_set_align(ui_Label4, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(ui_Label4, lv_color_hex(0x000000),         LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label4,  &lv_font_montserrat_16,         LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Cancel button */
    ui_Button1 = lv_btn_create(ui_Panel2);
    lv_obj_set_size(ui_Button1, 152, 44);
    lv_obj_set_align(ui_Button1, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_pos(ui_Button1, -16, -14);
    lv_obj_clear_flag(ui_Button1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Button1,     lv_color_hex(0x3A4860),   LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button1,       255,                       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Button1,       8,                         LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button1, 0,                         LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_Label3 = lv_label_create(ui_Button1);
    lv_label_set_text(ui_Label3, "Cancel");
    lv_obj_set_align(ui_Label3, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(ui_Label3, lv_color_hex(COL_TEXT),       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label3,  &lv_font_montserrat_16,       LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Keyboard1 = lv_keyboard_create(ui_popup);
    lv_obj_set_size(ui_Keyboard1, 800, lv_pct(40));
    lv_obj_set_align(ui_Keyboard1, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(ui_Panel2,  ui_event_Panel2,  LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_pwd,     ui_event_pwd,     LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Button1, ui_event_Button1, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Button2, ui_event_Button2, LV_EVENT_ALL, NULL);

    /* ── Main body (revealed after auth) ──────────────────────────── */
    ui_mainbody = lv_obj_create(ui_screenconfig);
    lv_obj_remove_style_all(ui_mainbody);
    lv_obj_set_pos(ui_mainbody, 79, 56);
    lv_obj_set_size(ui_mainbody, 721, 424);
    lv_obj_add_flag(ui_mainbody, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_mainbody, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_mainbody, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Snap-scroll pager: one full-screen card per swipe ───────────
     *  Same behaviour as the Home screen - vertical, snaps to nearest.*/
    lv_obj_t * pager = lv_obj_create(ui_mainbody);
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, 721, 439);
    lv_obj_clear_flag(pager, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_add_flag(pager, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(pager, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(pager, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(pager, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── PAGE 1 : General Settings (fills the screen) ─────────────── */
    lv_obj_t * page1 = lv_obj_create(pager);
    lv_obj_remove_style_all(page1);
    lv_obj_set_size(page1, 721, 439);
    lv_obj_set_pos(page1, 0, 0);
    lv_obj_add_flag(page1, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_clear_flag(page1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card_gs = make_card(page1, 701, 419, COL_CARD);
    lv_obj_set_align(card_gs, LV_ALIGN_CENTER);

    lv_obj_t * gs_title = make_label(card_gs, "General Settings", COL_VALUE, &lv_font_montserrat_36);
    lv_obj_set_align(gs_title, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(gs_title, 6, 2);

    /* --- Date row --- */
    lv_obj_t * row_date = lv_obj_create(card_gs);
    lv_obj_remove_style_all(row_date);
    lv_obj_set_size(row_date, 660, 70);
    lv_obj_set_align(row_date, LV_ALIGN_TOP_MID);
    lv_obj_set_pos(row_date, 0, 62);
    lv_obj_clear_flag(row_date, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_date, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_date, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_date, 16, LV_PART_MAIN | LV_STATE_DEFAULT);

    make_label(row_date, "Date:", COL_TEXT, &lv_font_montserrat_36);

    ui_TextArea5 = lv_textarea_create(row_date);
    lv_obj_set_size(ui_TextArea5, 310, 64);
    lv_textarea_set_text(ui_TextArea5, "09/07/2026");
    lv_textarea_set_placeholder_text(ui_TextArea5, "dd/mm/yyyy");
    lv_textarea_set_one_line(ui_TextArea5, true);
    lv_obj_clear_flag(ui_TextArea5, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(ui_TextArea5,    &lv_font_montserrat_36,  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_TextArea5,   LV_TEXT_ALIGN_CENTER,    LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_TextArea5,     lv_color_hex(0x101C2A),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_TextArea5,   lv_color_hex(COL_VALUE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_TextArea5, lv_color_hex(COL_ACCENT),LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_TextArea5, 2,                        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_TextArea5,       8,                        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_TextArea5, ui_event_TextArea5, LV_EVENT_ALL, NULL);

    lv_obj_t * cal_icon = make_label(row_date, LV_SYMBOL_EDIT, COL_ACCENT, &lv_font_montserrat_36);
    (void)cal_icon;

    /* --- Time row --- */
    lv_obj_t * row_time = lv_obj_create(card_gs);
    lv_obj_remove_style_all(row_time);
    lv_obj_set_size(row_time, 660, 176);
    lv_obj_set_align(row_time, LV_ALIGN_TOP_MID);
    lv_obj_set_pos(row_time, 0, 142);
    lv_obj_clear_flag(row_time, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_time, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_time, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_time, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    make_label(row_time, "Time:", COL_TEXT, &lv_font_montserrat_36);

    ui_Roller1 = lv_roller_create(row_time);
    /* 12-hour clock (AM/PM chosen by Roller3) */
    lv_roller_set_options(ui_Roller1,
        "12\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11",
        LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(ui_Roller1, 3);
    lv_obj_set_width(ui_Roller1, 100);
    style_roller(ui_Roller1);

    lv_obj_t * sep = make_label(row_time, ":", COL_ACCENT, &lv_font_montserrat_48);
    (void)sep;

    ui_Roller2 = lv_roller_create(row_time);
    lv_roller_set_options(ui_Roller2,
        "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n"
        "16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
        "32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
        "48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59",
        LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(ui_Roller2, 3);
    lv_obj_set_width(ui_Roller2, 100);
    style_roller(ui_Roller2);

    ui_Roller3 = lv_roller_create(row_time);
    lv_roller_set_options(ui_Roller3, "AM\nPM", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(ui_Roller3, 3);
    lv_obj_set_width(ui_Roller3, 110);
    style_roller(ui_Roller3);

    /* Synchronize button (centred at the bottom of the card) */
    lv_obj_t * sync_btn = make_cyan_btn(card_gs, "Synchronize", 300, 66, &lv_font_montserrat_36);
    lv_obj_set_align(sync_btn, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_pos(sync_btn, 0, -6);
    lv_obj_add_event_cb(sync_btn, sync_btn_cb, LV_EVENT_ALL, NULL);

    /* ── PAGE 2 : Lamp Resets (fills the screen) ─────────────────── */
    lv_obj_t * page2 = lv_obj_create(pager);
    lv_obj_remove_style_all(page2);
    lv_obj_set_size(page2, 721, 439);
    lv_obj_set_pos(page2, 0, 439);
    lv_obj_add_flag(page2, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_clear_flag(page2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card_lamps = make_card(page2, 701, 419, COL_CARD2);
    lv_obj_set_align(card_lamps, LV_ALIGN_CENTER);

    lv_obj_t * lamp_title = make_label(card_lamps, "Lamp Resets", COL_VALUE, &lv_font_montserrat_36);
    lv_obj_set_align(lamp_title, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(lamp_title, 6, 2);

    lv_obj_t * lamp_row = lv_obj_create(card_lamps);
    lv_obj_remove_style_all(lamp_row);
    lv_obj_set_size(lamp_row, 672, 350);
    lv_obj_set_align(lamp_row, LV_ALIGN_TOP_MID);
    lv_obj_set_pos(lamp_row, 0, 52);
    lv_obj_clear_flag(lamp_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(lamp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lamp_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    build_lamp_col(lamp_row, "Lamp 1", COL_ACCENT,  LV_SYMBOL_EYE_OPEN,
                   &ui_arc_lamp1, &ui_lamp1_pct_label,
                   &ui_lamp1_hours_label, &ui_lampe_1);

    build_lamp_col(lamp_row, "Lamp 2", COL_WARN, LV_SYMBOL_WARNING,
                   &ui_arc_lamp2, &ui_lamp2_pct_label,
                   &ui_lamp2_hours_label, &ui_lampe_2);

    /* Apply Changes removed: date/time is committed by Synchronize, and a
       lamp reset is committed straight from the confirmation dialog.       */
    ui_BTN_Apply = NULL;

    /* ── Page indicator dots (right edge) ─────────────────────────── */
    ui_cfg_dot0 = lv_obj_create(ui_screenconfig);
    lv_obj_remove_style_all(ui_cfg_dot0);
    lv_obj_set_size(ui_cfg_dot0, 9, 9);
    lv_obj_set_align(ui_cfg_dot0, LV_ALIGN_RIGHT_MID);
    lv_obj_set_pos(ui_cfg_dot0, -5, -12);
    lv_obj_set_style_radius(ui_cfg_dot0, 5, 0);
    lv_obj_set_style_bg_color(ui_cfg_dot0, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(ui_cfg_dot0, 255, 0);

    ui_cfg_dot1 = lv_obj_create(ui_screenconfig);
    lv_obj_remove_style_all(ui_cfg_dot1);
    lv_obj_set_size(ui_cfg_dot1, 9, 9);
    lv_obj_set_align(ui_cfg_dot1, LV_ALIGN_RIGHT_MID);
    lv_obj_set_pos(ui_cfg_dot1, -5, 12);
    lv_obj_set_style_radius(ui_cfg_dot1, 5, 0);
    lv_obj_set_style_bg_color(ui_cfg_dot1, lv_color_hex(0x3A4152), 0);
    lv_obj_set_style_bg_opa(ui_cfg_dot1, 255, 0);

    lv_obj_add_event_cb(pager, cfg_scroll_cb, LV_EVENT_SCROLL, NULL);

    /* Land on the General Settings slide, not the lamps */
    lv_obj_update_layout(pager);
    lv_obj_scroll_to_y(pager, 0, LV_ANIM_OFF);

    /* ── Confirmation popup ─────────────────────────────────────────── */
    ui_confirm_popup = lv_obj_create(ui_screenconfig);
    lv_obj_set_size(ui_confirm_popup, 800, 480);
    lv_obj_set_align(ui_confirm_popup, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_confirm_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_confirm_popup, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_confirm_popup, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_confirm_popup, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * c_panel = lv_obj_create(ui_confirm_popup);
    lv_obj_set_size(c_panel, 400, 185);
    lv_obj_set_align(c_panel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(c_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(c_panel,     lv_color_hex(0x1E2D40), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(c_panel,       255,                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(c_panel, lv_color_hex(COL_ACCENT),LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(c_panel, 2,                       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(c_panel,       12,                      LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_confirm_label = lv_label_create(c_panel);
    lv_obj_set_width(ui_confirm_label, 360);
    lv_obj_set_height(ui_confirm_label, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_confirm_label, LV_ALIGN_TOP_MID);
    lv_obj_set_y(ui_confirm_label, 20);
    lv_label_set_text(ui_confirm_label, "Reset lamp hours to 0?");
    lv_label_set_long_mode(ui_confirm_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ui_confirm_label,  lv_color_hex(COL_TEXT),   LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_confirm_label,  LV_TEXT_ALIGN_CENTER,     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_confirm_label,   &lv_font_montserrat_16,   LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_confirm_yes = lv_btn_create(c_panel);
    lv_obj_set_size(ui_confirm_yes, 148, 44);
    lv_obj_set_align(ui_confirm_yes, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_pos(ui_confirm_yes, 14, -14);
    lv_obj_clear_flag(ui_confirm_yes, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_confirm_yes,     lv_color_hex(0xC2003F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_confirm_yes,       255,                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_confirm_yes,       8,                       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_confirm_yes, 0,                       LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * yes_lbl = lv_label_create(ui_confirm_yes);
    lv_obj_set_align(yes_lbl, LV_ALIGN_CENTER);
    lv_label_set_text(yes_lbl, "Yes, Reset");
    lv_obj_set_style_text_color(yes_lbl, lv_color_hex(0xFFFFFF),           LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(yes_lbl,  &lv_font_montserrat_16,           LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_confirm_no = lv_btn_create(c_panel);
    lv_obj_set_size(ui_confirm_no, 148, 44);
    lv_obj_set_align(ui_confirm_no, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_pos(ui_confirm_no, -14, -14);
    lv_obj_clear_flag(ui_confirm_no, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_confirm_no,     lv_color_hex(0x3A4860),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_confirm_no,       255,                      LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_confirm_no,       8,                        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_confirm_no, 0,                        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * no_lbl = lv_label_create(ui_confirm_no);
    lv_obj_set_align(no_lbl, LV_ALIGN_CENTER);
    lv_label_set_text(no_lbl, "Cancel");
    lv_obj_set_style_text_color(no_lbl, lv_color_hex(COL_TEXT),            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(no_lbl,  &lv_font_montserrat_16,            LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Calendar overlay ───────────────────────────────────────────── */
    ui_layer2 = lv_obj_create(ui_screenconfig);
    lv_obj_set_size(ui_layer2, lv_pct(100), lv_pct(100));
    lv_obj_set_align(ui_layer2, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_layer2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_layer2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_layer2,   lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_layer2,     120,                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_layer2, 0,                     LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Calendar2 = lv_calendar_create(ui_layer2);
    lv_calendar_header_arrow_create(ui_Calendar2);
    lv_obj_set_size(ui_Calendar2, 260, 265);
    lv_obj_set_align(ui_Calendar2, LV_ALIGN_CENTER);

    /* ── Screen event ───────────────────────────────────────────────── */
    lv_obj_add_event_cb(ui_screenconfig, ui_event_screenconfig, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_BTN_Menu_Setting_S3, ui_event_BTN_Menu_Setting_S3, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_BTN_Menu_Move_S7, ui_event_BTN_Menu_Move_S7, LV_EVENT_ALL, NULL);
}

void ui_screenconfig_screen_destroy(void) {}
