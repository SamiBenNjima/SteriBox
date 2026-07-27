SET(SOURCES ui_screeninfo.c
    ui_screenhome.c
    ui_screenconfig.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    ui_events.c
    ui_img_3d_printer_bg_v1_png.c
    ui_img_icn_wifi_png.c
    ui_img_icn_pc_png.c
    ui_img_icn_usb_png.c
    ui_img_btn_print_v2_png.c
    ui_img_btn_setting_png.c
    ui_img_btn_main_off_png.c
    ui_img_btn_main_on_png.c
    ui_img_btn_print_v1_png.c
    ui_img_btn_move_png.c
    ui_img_print_view_bg_png.c
    ui_img_print_view_front_png.c
    ui_img_icn_time_2_png.c
    ui_img_btn_print_down_v2_png.c
    ui_img_btn_print_top_off_png.c
    ui_img_arrow_right_png.c
    fonts/ui_font_hugetext.c
    fonts/ui_font_timefont.c)

add_library(ui ${SOURCES})
