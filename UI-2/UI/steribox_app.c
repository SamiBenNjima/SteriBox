/**
 * @file steribox_app.c
 * SteriBox UV Sterilizer - application logic (portable, LVGL 8.3).
 *
 * Owns:
 *  - the sterilization cycle state machine (door interlock, lamps, buzzer)
 *  - lamp life / cycle counters with persistence
 *  - live refresh of the Info screen (temp / hum / hours / cycles)
 *  - config screen password gate, date & time setting, lamp hour reset
 *  - USB export / print reports
 *
 * It only touches the SquareLine-generated objects through their public
 * handles, so the UI can be re-exported from SquareLine at any time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "steribox_app.h"
#include "steribox_hal.h"

/*==================================================================
 * Configuration
 *=================================================================*/
#define SBX_CHART_POINTS      16      /*dose curve resolution*/
#define SBX_LAMP_WARN_HOURS   100u    /*orange warning threshold*/
#define SBX_UI_REFRESH_MS     500
#define SBX_CYCLE_TICK_MS     1000

/*Max log10 reduction reached at a full effective exposure, per organism
 * (x100 so the chart keeps integer resolution: 600 = 6.00 log).
 * Ordering reflects real UV-C sensitivity: vegetative bacteria fall fast,
 * mould spores are far more resistant. */
#define SBX_LOG_ECOLI   600     /* E. coli    - UV sensitive      */
#define SBX_LOG_SAUR    450     /* S. aureus  - medium resistance */
#define SBX_LOG_ASPER   250     /* A. niger   - UV resistant mould*/

/* UV-C physical dose parameters
 * SBX_UVC_IRRADIANCE_MW_CM2 : combined irradiance at target surface (mW/cm²)
 *   Typical low-pressure 30 W germicidal lamp at ~20 cm : 0.8–2 mW/cm²
 *   Two lamps running simultaneously -> multiply by 2.
 *   Adjust to match your actual lamp datasheet + geometry.
 * Formula: Dose (mJ/cm²) = Irradiance (mW/cm²) × EffectiveTime (s)
 *   (effective time accounts for the 60 s warm-up ramp, same as dose_at) */
#define SBX_UVC_IRRADIANCE_MW_CM2   1.5f   /* per lamp, mW/cm² at target */

/*==================================================================
 * Module state
 *=================================================================*/
static sbx_persist_t persist;
static sbx_state_t   state = SBX_STATE_IDLE;

static uint32_t cycle_total_s;    /*selected duration in seconds*/
static uint32_t cycle_elapsed_s;

static bool pwd_ok;               /*last password attempt result*/

static lv_timer_t * refresh_timer;
static lv_timer_t * cycle_timer;
static lv_timer_t * warmup_timer;      /* 3 s pre-lamp safety countdown */
static uint8_t      warmup_left;
static lv_timer_t * done_timer;        /* holds "DONE" ~3 s, then arms START */

#define SBX_WARMUP_S 3

/* End-of-cycle result popup (defined below, shown from cycle_stop) */
static void show_end_popup(bool aborted);
/* Target-organism toggle list (defined below) */
static void org_open_cb(lv_event_t * e);
static void org_list_create(void);

/*==================================================================
 * Target organisms - UV-C log10 reduction model
 *  log_max = max log10 reduction (x100) reached at a full effective dose,
 *  ordered by UV-C sensitivity (sensitive = high, resistant spores = low).
 *  The first 3 are enabled by default; the rest can be toggled on.
 *=================================================================*/
#define SBX_ORG_COUNT 11

typedef struct {
    const char *        name;
    uint32_t            color;
    uint32_t            log_max;   /* x100 */
    bool                enabled;
    lv_chart_series_t * ser;
} sbx_org_t;

static sbx_org_t organisms[SBX_ORG_COUNT] = {
    { "E. coli",         0x00E0A0, 600, true,  NULL },
    { "S. aureus/MRSA",  0xFFB020, 450, true,  NULL },
    { "A. niger",        0xFF5470, 250, true,  NULL },
    { "P. aeruginosa",   0x4FC3F7, 520, false, NULL },
    { "A. baumannii",    0xBA68C8, 480, false, NULL },
    { "E. hirae",        0x9CCC65, 500, false, NULL },
    { "C. albicans",     0xFFD54F, 350, false, NULL },
    { "C. difficile",    0xF06292, 260, false, NULL },
    { "M. tuberculosis", 0x4DD0E1, 430, false, NULL },
    { "SARS-CoV-2",      0x8FA6FF, 560, false, NULL },
    { "Candida auris",   0xFF8A65, 300, false, NULL },
};

/* Per-organism chart data (log10 x100) */
static lv_coord_t org_data[SBX_ORG_COUNT][SBX_CHART_POINTS];

/*==================================================================
 * Helpers
 *=================================================================*/
static void persist_defaults(void)
{
    memset(&persist, 0, sizeof(persist));
    persist.magic = SBX_PERSIST_MAGIC;
    strncpy(persist.password, SBX_DEFAULT_PASSWORD, sizeof(persist.password) - 1);
}

static uint32_t lamp_remaining_h(uint32_t lamp_seconds)
{
    uint32_t used_h = lamp_seconds / 3600u;
    return (used_h >= SBX_LAMP_LIFE_HOURS) ? 0u : (SBX_LAMP_LIFE_HOURS - used_h);
}

static void set_time_display(uint32_t seconds)
{
    char buf[12];
    lv_snprintf(buf, sizeof(buf), "%u:%02u",
                (unsigned)(seconds / 60u), (unsigned)(seconds % 60u));
    lv_label_set_text(ui_Label_Time_1, buf);   /* single "M:SS" readout */
}

static void set_progress(uint32_t pct)
{
    char buf[8];
    lv_slider_set_value(ui_Slider_Print_View1, (int32_t)pct, LV_ANIM_ON);
    lv_snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    lv_label_set_text(ui_Number_Print1, buf);
}

/** log10 reduction at time t. Germicidal dose accumulates linearly with
 *  exposure (after a 60 s lamp warm-up ramp), and log kill is dose/D90, so
 *  each organism reaches its own maximum (log_max, x100) at a full cycle. */
static uint32_t dose_at(uint32_t t_s, uint32_t total_s, uint32_t log_max)
{
    if(total_s == 0) return 0;
    const uint32_t warm = 60u;
    /*Effective exposure seconds: t - warm/2 lost during ramp*/
    uint32_t eff  = (t_s <= warm) ? (t_s * t_s) / (2u * warm)
                    : t_s - warm / 2u;
    uint32_t full = (total_s <= warm) ? (total_s * total_s) / (2u * warm)
                    : total_s - warm / 2u;
    if(full == 0) return 0;
    return (eff * log_max) / full;   /*0 .. log_max  (log10 x100)*/
}

/** Effective UV-C dose delivered during a completed cycle (mJ/cm²).
 *  Uses the same warmup-ramp model as dose_at() so the value is
 *  consistent with the log-kill chart shown on the Home screen.
 *  active_lamps : number of lamps that were ON (1 or 2). */
static float uvc_dose_mj_cm2(uint32_t elapsed_s, uint8_t active_lamps)
{
    if(elapsed_s == 0) return 0.0f;
    const uint32_t warm = 60u;
    /* Effective seconds (ramp reduces dose during warm-up) */
    float eff_s = (elapsed_s <= warm)
                  ? (float)(elapsed_s * elapsed_s) / (2.0f * (float)warm)
                  : (float)elapsed_s - (float)warm / 2.0f;
    float irr = SBX_UVC_IRRADIANCE_MW_CM2 * (float)active_lamps;
    return irr * eff_s;   /* mW/cm² × s = mJ/cm² */
}

static void chart_reset(void)
{
    for(int o = 0; o < SBX_ORG_COUNT; o++)
        for(int i = 0; i < SBX_CHART_POINTS; i++)
            org_data[o][i] = organisms[o].enabled ? 0 : LV_CHART_POINT_NONE;
    if(ui_Chart3) lv_chart_refresh(ui_Chart3);
}

static void chart_update(void)
{
    if(cycle_total_s == 0) { chart_reset(); return; }
    uint32_t filled = (cycle_elapsed_s * (SBX_CHART_POINTS - 1)) / cycle_total_s;
    if(filled > SBX_CHART_POINTS - 1) filled = SBX_CHART_POINTS - 1;

    for(int o = 0; o < SBX_ORG_COUNT; o++) {
        if(!organisms[o].enabled) {
            for(int i = 0; i < SBX_CHART_POINTS; i++) org_data[o][i] = LV_CHART_POINT_NONE;
            continue;
        }
        for(uint32_t i = 0; i <= filled; i++) {
            uint32_t t = (i * cycle_total_s) / (SBX_CHART_POINTS - 1);
            org_data[o][i] = (lv_coord_t)dose_at(t, cycle_total_s, organisms[o].log_max);
        }
        for(uint32_t i = filled + 1; i < SBX_CHART_POINTS; i++)
            org_data[o][i] = LV_CHART_POINT_NONE;
    }
    if(ui_Chart3) lv_chart_refresh(ui_Chart3);
}

/*==================================================================
 * Info screen refresh
 *=================================================================*/
static void info_screen_refresh(void)
{
    char buf[24];
    float t, h;

    if(sbx_hal_read_env(&t, &h)) {
        lv_snprintf(buf, sizeof(buf), "%d*C", (int)(t + 0.5f));
        lv_label_set_text(ui_Label_Z_Position_Number1, buf);   /*Temp*/
        lv_snprintf(buf, sizeof(buf), "%d%%", (int)(h + 0.5f));
        lv_label_set_text(ui_Label_Z_Position_Number2, buf);   /*Hum*/
    }

    /*Lamp remaining hours: L1 -> Number6, L2 -> Number3*/
    struct { lv_obj_t * label; uint32_t secs; } lamps[2] = {
        { ui_Label_Z_Position_Number6, persist.lamp1_seconds },
        { ui_Label_Z_Position_Number3, persist.lamp2_seconds },
    };
    for(int i = 0; i < 2; i++) {
        uint32_t rem = lamp_remaining_h(lamps[i].secs);
        if(rem == 0) {
            lv_label_set_text(lamps[i].label, "expire");
            lv_obj_set_style_text_color(lamps[i].label, lv_color_hex(0xFF0000), 0);
        }
        else {
            lv_snprintf(buf, sizeof(buf), "%u h", (unsigned)rem);
            lv_label_set_text(lamps[i].label, buf);
            lv_obj_set_style_text_color(lamps[i].label,
                                        rem <= SBX_LAMP_WARN_HOURS ? lv_color_hex(0xFF8800)
                                        : lv_color_hex(0xFFFFFF), 0);
        }
    }

    lv_snprintf(buf, sizeof(buf), "%u h", (unsigned)(persist.total_seconds / 3600u));
    lv_label_set_text(ui_Label_Z_Position_Number5, buf);       /*Total*/
    lv_snprintf(buf, sizeof(buf), "%u", (unsigned)persist.cycles_done);
    lv_label_set_text(ui_Label_Z_Position_Number4, buf);       /*Cycles*/
}

static void usb_icons_refresh(void)
{
    /* --- USB icon: dim when no SD (same hardware slot) --- */
    lv_opa_t opa = sbx_hal_sd_present() ? LV_OPA_COVER : LV_OPA_30;
    lv_obj_set_style_img_opa(ui_IMG_USB1, opa, 0);
    lv_obj_set_style_img_opa(ui_IMG_USB3, opa, 0);
    lv_obj_set_style_img_opa(ui_IMG_USB6, opa, 0);

    /* --- SD-absent indicator: a small red label shown in each header
     *     when no card is inserted.  Created once on first call,
     *     shown/hidden every refresh cycle.
     *     We create one label per screen header that already exists.
     * ----------------------------------------------------------------*/
    static lv_obj_t * sd_lbl_info   = NULL;
    static lv_obj_t * sd_lbl_home   = NULL;
    static lv_obj_t * sd_lbl_config = NULL;
    static bool sd_was_present = true;   /* track transitions for serial msg */

    bool sd_now = sbx_hal_sd_present();

    /* Print one serial message on each transition */
    if(sd_was_present && !sd_now) {
        Serial.println("[SD] No SD card — logging disabled, icon shown");
    }
    sd_was_present = sd_now;

    /* Helper: create the SD-absent label the first time it is needed */
    #define SBX_MAKE_SD_LBL(var, parent) do { \
        if(!(var) && (parent)) { \
            (var) = lv_label_create(parent); \
            lv_label_set_text((var), LV_SYMBOL_SD_CARD "!"); \
            lv_obj_set_align((var), LV_ALIGN_TOP_RIGHT); \
            lv_obj_set_pos((var), -80, 12); \
            lv_obj_set_style_text_color((var), lv_color_hex(0xFF4040), 0); \
            lv_obj_set_style_text_font((var),  &lv_font_montserrat_16, 0); \
        } \
    } while(0)

    SBX_MAKE_SD_LBL(sd_lbl_info,   ui_Panel_Header1);
    SBX_MAKE_SD_LBL(sd_lbl_home,   ui_Panel_Header3);
    SBX_MAKE_SD_LBL(sd_lbl_config, ui_Panel_Header5);

    #undef SBX_MAKE_SD_LBL

    /* Show/hide based on card presence */
    lv_opa_t sd_opa = sd_now ? LV_OPA_TRANSP : LV_OPA_COVER;
    if(sd_lbl_info)   lv_obj_set_style_opa(sd_lbl_info,   sd_opa, 0);
    if(sd_lbl_home)   lv_obj_set_style_opa(sd_lbl_home,   sd_opa, 0);
    if(sd_lbl_config) lv_obj_set_style_opa(sd_lbl_config, sd_opa, 0);
}

/*--- Header clock: one RTC read feeds every screen's clock label --*/
static void clock_refresh(void)
{
    sbx_datetime_t dt;
    sbx_hal_get_datetime(&dt);
    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%02u:%02u", dt.hour, dt.minute);
    if(ui_Label_Time1) lv_label_set_text(ui_Label_Time1, buf);   /*Info  */
    if(ui_Label_Time3) lv_label_set_text(ui_Label_Time3, buf);   /*Home  */
    if(ui_Label_Time5) lv_label_set_text(ui_Label_Time5, buf);   /*Config*/
}

/*==================================================================
 * Sterilization cycle state machine
 *=================================================================*/
static void lamps_set(bool on)
{
    /*Never drive an expired lamp: no germicidal output, wasted ballast*/
    bool l1_ok = lamp_remaining_h(persist.lamp1_seconds) > 0;
    bool l2_ok = lamp_remaining_h(persist.lamp2_seconds) > 0;
    sbx_hal_relay_set(SBX_RELAY_LAMP1, on && l1_ok);
    sbx_hal_relay_set(SBX_RELAY_LAMP2, on && l2_ok);
}

static void lock_slider(bool lock)
{
    if(lock) lv_obj_clear_flag(ui_Slider_Print_Speed2, LV_OBJ_FLAG_CLICKABLE);
    else     lv_obj_add_flag(ui_Slider_Print_Speed2,   LV_OBJ_FLAG_CLICKABLE);
}

/* Set the big button caption. The decorative ">" arrow is hidden for wide
 * words (RESUME) so the text has the full button width, shown otherwise. */
static void set_status(const char * txt)
{
    lv_label_set_text(ui_Label1, txt);
    if(ui_Image_Pause1) {
        if(strcmp(txt, "RESUME") == 0)
            lv_obj_add_flag(ui_Image_Pause1, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(ui_Image_Pause1, LV_OBJ_FLAG_HIDDEN);
    }
}

/* After a completed cycle, revert the button from "DONE" to a ready
 * "START" and reset the readout to the selected duration. */
static void reset_after_done(void)
{
    set_status("START");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
    set_progress(0);
    set_time_display((uint32_t)lv_slider_get_value(ui_Slider_Print_Speed2) * 60u);
    state = SBX_STATE_IDLE;
}

static void done_timer_cb(lv_timer_t * t)
{
    (void)t;
    done_timer = NULL;                 /* one-shot: auto-deleted after this call */
    if(state == SBX_STATE_DONE) reset_after_done();
}

/* Skip the 3 s hold and arm START immediately (e.g. on "Continue"). */
static void done_revert_now(void)
{
    if(done_timer) { lv_timer_del(done_timer); done_timer = NULL; }
    if(state == SBX_STATE_DONE) reset_after_done();
}

/* Terminate the cycle: DONE (finished) or IDLE (manual stop). */
static void cycle_stop(sbx_state_t end_state)
{
    lamps_set(false);
    if(cycle_timer)  { lv_timer_del(cycle_timer);  cycle_timer  = NULL; }
    if(warmup_timer) { lv_timer_del(warmup_timer); warmup_timer = NULL; }

    if(end_state == SBX_STATE_DONE) {
        persist.cycles_done++;
        sbx_hal_buzzer(SBX_BEEP_DONE);
        set_status("DONE");
        lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
        set_progress(100);
    }
    else {   /*manual stop*/
        persist.cycles_aborted++;
        sbx_hal_buzzer(SBX_BEEP_OK);
        set_status("START");
        lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
        set_progress(0);
        set_time_display(cycle_total_s ? cycle_total_s
                         : (uint32_t)lv_slider_get_value(ui_Slider_Print_Speed2) * 60u);
    }

    sbx_hal_storage_save(&persist);

    /* ---- SD log: one row per cycle end ---- */
    {
        float t = 0.0f, h = 0.0f;
        sbx_hal_read_env(&t, &h);
        uint8_t lamps_active = 0;
        if(lamp_remaining_h(persist.lamp1_seconds) > 0) lamps_active++;
        if(lamp_remaining_h(persist.lamp2_seconds) > 0) lamps_active++;
        float dose = uvc_dose_mj_cm2(cycle_elapsed_s, lamps_active);
        uint32_t dur_min = cycle_total_s / 60u;
        uint32_t dur_sec = cycle_total_s % 60u;
        char detail[160];
        snprintf(detail, sizeof(detail),
                 "dur=%um%02us dose=%.1fmJ/cm2 lamps=%u/2 cycles=%u aborted=%u"
                 " L1=%uh L2=%uh T=%dC RH=%d%%",
                 (unsigned)dur_min, (unsigned)dur_sec,
                 (double)dose, (unsigned)lamps_active,
                 (unsigned)persist.cycles_done,
                 (unsigned)persist.cycles_aborted,
                 (unsigned)lamp_remaining_h(persist.lamp1_seconds),
                 (unsigned)lamp_remaining_h(persist.lamp2_seconds),
                 (int)(t + 0.5f), (int)(h + 0.5f));
        sbx_hal_log_event(
            (end_state == SBX_STATE_DONE) ? "CYCLE_DONE" : "CYCLE_ABORT",
            detail);
    }

    lv_obj_clear_state(ui_BTN_Pause_Top1, LV_STATE_CHECKED);
    lock_slider(false);
    state = end_state;
    info_screen_refresh();

    /* Announce the result (Terminee / Arretee) on the Home screen */
    show_end_popup(end_state != SBX_STATE_DONE);

    /* On completion, hold "DONE" ~3 s (button inert) then arm START */
    if(end_state == SBX_STATE_DONE) {
        if(done_timer) lv_timer_del(done_timer);
        done_timer = lv_timer_create(done_timer_cb, 3000, NULL);
        lv_timer_set_repeat_count(done_timer, 1);
    }
}

/* SAFETY: door opened mid-run/warm-up -> freeze, wait for the user. */
static void cycle_pause_door(void)
{
    lamps_set(false);
    if(cycle_timer)  { lv_timer_del(cycle_timer);  cycle_timer  = NULL; }
    if(warmup_timer) { lv_timer_del(warmup_timer); warmup_timer = NULL; }
    sbx_hal_buzzer(SBX_BEEP_ALARM);
    set_status("DOOR !");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFF3030), 0);
    state = SBX_STATE_PAUSED_DOOR;
}

static void cycle_tick_cb(lv_timer_t * timer)
{
    (void)timer;

    /*SAFETY: door interlock - immediate lamp cut-off + pause*/
    if(sbx_hal_door_is_open()) { cycle_pause_door(); return; }

    cycle_elapsed_s++;

    /*Track lamp / device usage every second*/
    if(sbx_hal_relay_get(SBX_RELAY_LAMP1)) persist.lamp1_seconds++;
    if(sbx_hal_relay_get(SBX_RELAY_LAMP2)) persist.lamp2_seconds++;
    persist.total_seconds++;

    uint32_t remaining = (cycle_elapsed_s >= cycle_total_s) ? 0
                         : cycle_total_s - cycle_elapsed_s;
    set_time_display(remaining);
    set_progress((cycle_elapsed_s * 100u) / cycle_total_s);
    chart_update();

    if(cycle_elapsed_s >= cycle_total_s) cycle_stop(SBX_STATE_DONE);
}

/* Energise the lamps and start (or continue) counting down. */
static void cycle_run(void)
{
    set_status("STOP");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
    lamps_set(true);
    sbx_hal_buzzer(SBX_BEEP_OK);
    if(!cycle_timer) cycle_timer = lv_timer_create(cycle_tick_cb, SBX_CYCLE_TICK_MS, NULL);
    state = SBX_STATE_RUNNING;
}

/* 3 s safety warm-up: lamps stay OFF while the button counts 3-2-1. */
static void warmup_tick_cb(lv_timer_t * t)
{
    (void)t;
    if(sbx_hal_door_is_open()) { cycle_pause_door(); return; }

    if(warmup_left > 1) {
        warmup_left--;
        char b[8];
        lv_snprintf(b, sizeof(b), "%u", warmup_left);
        set_status(b);
        sbx_hal_buzzer(SBX_BEEP_KEY);
    }
    else {
        if(warmup_timer) { lv_timer_del(warmup_timer); warmup_timer = NULL; }
        cycle_run();
    }
}

/* Start a fresh cycle (fresh=true) or resume after a door pause (fresh=false),
 * always via the 3 s warm-up delay. */
static void cycle_begin(bool fresh)
{
    /*SAFETY: never energise with the door open*/
    if(sbx_hal_door_is_open()) {
        sbx_hal_buzzer(SBX_BEEP_WARN);
        set_status("DOOR !");
        lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFF3030), 0);
        lv_obj_clear_state(ui_BTN_Pause_Top1, LV_STATE_CHECKED);
        return;
    }
    /*Efficacy: both lamps expired -> dose can't be guaranteed*/
    if(lamp_remaining_h(persist.lamp1_seconds) == 0 &&
       lamp_remaining_h(persist.lamp2_seconds) == 0) {
        sbx_hal_buzzer(SBX_BEEP_WARN);
        set_status("LAMPS !");
        lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFF3030), 0);
        lv_obj_clear_state(ui_BTN_Pause_Top1, LV_STATE_CHECKED);
        return;
    }

    if(fresh) {
        cycle_total_s   = (uint32_t)lv_slider_get_value(ui_Slider_Print_Speed2) * 60u;
        cycle_elapsed_s = 0;
        chart_reset();
        set_progress(0);
        set_time_display(cycle_total_s);
    }
    lock_slider(true);

    /* 3 s warm-up countdown shown on the button, lamps OFF */
    warmup_left = SBX_WARMUP_S;
    state = SBX_STATE_WARMUP;
    lv_obj_add_state(ui_BTN_Pause_Top1, LV_STATE_CHECKED);
    char b[8];
    lv_snprintf(b, sizeof(b), "%u", warmup_left);
    set_status(b);
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x00D2FF), 0);
    sbx_hal_buzzer(SBX_BEEP_KEY);
    if(!warmup_timer) warmup_timer = lv_timer_create(warmup_tick_cb, 1000, NULL);
}

/* Poll the door each refresh: pause on open, offer RESUME once closed. */
static void door_monitor(void)
{
    bool open = sbx_hal_door_is_open();
    if((state == SBX_STATE_RUNNING || state == SBX_STATE_WARMUP) && open) {
        cycle_pause_door();
    }
    else if(state == SBX_STATE_PAUSED_DOOR) {
        if(open) {
            set_status("DOOR !");
            lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFF3030), 0);
        } else {
            set_status("RESUME");
            lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x22DD88), 0);
        }
    }
}

/*==================================================================
 * Event callbacks added on top of the generated UI
 *=================================================================*/
static void start_btn_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    switch(state) {
        case SBX_STATE_RUNNING:
        case SBX_STATE_WARMUP:
            cycle_stop(SBX_STATE_IDLE);   /* manual stop */
            break;
        case SBX_STATE_PAUSED_DOOR:
            cycle_begin(false);           /* resume remaining time (3 s warm-up) */
            break;
        case SBX_STATE_DONE:
            /* Inert while "DONE" is held; the 3 s timer arms START */
            break;
        default:                          /* IDLE */
            cycle_begin(true);            /* fresh cycle (3 s warm-up) */
            break;
    }
}

static void duration_slider_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    if(state == SBX_STATE_RUNNING) return;
    /*Show the selected duration as "M:00" in the single readout*/
    set_time_display((uint32_t)lv_slider_get_value(ui_Slider_Print_Speed2) * 60u);
    set_status("START");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
    set_progress(0);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

/*--- Info screen: export / print ---------------------------------*/
static void build_report(char * buf, int len, const char * period /*NULL = none*/)
{
    sbx_datetime_t dt;
    sbx_hal_get_datetime(&dt);
    float t = 0, h = 0;
    sbx_hal_read_env(&t, &h);

    /* --- UV-C dose for the last completed cycle --- */
    uint8_t lamps_active = 0;
    if(lamp_remaining_h(persist.lamp1_seconds) > 0) lamps_active++;
    if(lamp_remaining_h(persist.lamp2_seconds) > 0) lamps_active++;
    float dose_mj  = uvc_dose_mj_cm2(cycle_elapsed_s, lamps_active);
    uint32_t dur_min = cycle_total_s / 60u;
    uint32_t dur_sec = cycle_total_s % 60u;

    /* Log-kill at end of cycle (x100 stored -> divide for display) */
    uint32_t kill_ecoli = dose_at(cycle_elapsed_s, cycle_total_s, SBX_LOG_ECOLI);
    uint32_t kill_saur  = dose_at(cycle_elapsed_s, cycle_total_s, SBX_LOG_SAUR);
    uint32_t kill_asper = dose_at(cycle_elapsed_s, cycle_total_s, SBX_LOG_ASPER);

    snprintf(buf, len,
             "=== SteriBox UV Sterilizer ===\r\n"
             "Date     : %02u/%02u/%04u  %02u:%02u\r\n"
             "%s"
             "------------------------------\r\n"
             "Duree cycle  : %u min %02u s\r\n"
             "Lampes actives: %u/2\r\n"
             "------------------------------\r\n"
             "DOSE UV-C recue\r\n"
             "  Estimee    : %.1f mJ/cm2\r\n"
             "  Irradiance : %.1f mW/cm2\r\n"
             "------------------------------\r\n"
             "Reduction microbienne (log10)\r\n"
             "  E. coli    : %u.%02u log\r\n"
             "  S. aureus  : %u.%02u log\r\n"
             "  A. niger   : %u.%02u log\r\n"
             "------------------------------\r\n"
             "Temperature  : %.1f C\r\n"
             "Humidite     : %.0f %%\r\n"
             "Lampe L1 rest: %u h\r\n"
             "Lampe L2 rest: %u h\r\n"
             "Tps total app: %u h\r\n"
             "Cycles OK    : %u\r\n"
             "Cycles abort.: %u\r\n"
             "==============================\r\n",
             dt.day, dt.month, dt.year, dt.hour, dt.minute,
             period ? period : "",
             dur_min, dur_sec,
             (unsigned)lamps_active,
             (double)dose_mj,
             (double)(SBX_UVC_IRRADIANCE_MW_CM2 * lamps_active),
             kill_ecoli / 100u, kill_ecoli % 100u,
             kill_saur  / 100u, kill_saur  % 100u,
             kill_asper / 100u, kill_asper % 100u,
             (double)t, (double)h,
             (unsigned)lamp_remaining_h(persist.lamp1_seconds),
             (unsigned)lamp_remaining_h(persist.lamp2_seconds),
             (unsigned)(persist.total_seconds / 3600u),
             (unsigned)persist.cycles_done,
             (unsigned)persist.cycles_aborted);
}

static void print_btn_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char report[640];
    build_report(report, sizeof(report), NULL);
    sbx_hal_buzzer(sbx_hal_usb_print(report) ? SBX_BEEP_OK : SBX_BEEP_WARN);
}

/*==================================================================
 * End-of-cycle result popup (Home screen)
 *=================================================================*/
static lv_obj_t * end_overlay;
static lv_obj_t * end_title;
static lv_obj_t * end_meta;
static lv_obj_t * end_dose;
static lv_obj_t * end_logs;

static void end_print_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char report[640];
    build_report(report, sizeof(report), NULL);
    sbx_hal_buzzer(sbx_hal_usb_print(report) ? SBX_BEEP_OK : SBX_BEEP_WARN);
}

static void end_cont_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_add_flag(end_overlay, LV_OBJ_FLAG_HIDDEN);
    done_revert_now();                 /* continuing arms START immediately */
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

/* One cyan section-title label */
static lv_obj_t * end_section(lv_obj_t * p, const char * txt, lv_coord_t y)
{
    lv_obj_t * l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, 22, y);
    lv_obj_set_style_text_color(l, lv_color_hex(0x00CCFC), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    return l;
}

static void end_popup_create(void)
{
    end_overlay = lv_obj_create(ui_screenhome);
    lv_obj_remove_style_all(end_overlay);
    lv_obj_set_size(end_overlay, 800, 480);
    lv_obj_set_style_bg_color(end_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(end_overlay, 170, 0);
    lv_obj_add_flag(end_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(end_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(end_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * p = lv_obj_create(end_overlay);
    lv_obj_set_size(p, 600, 396);
    lv_obj_center(p);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x18202E), 0);
    lv_obj_set_style_bg_opa(p, 255, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0x00CCFC), 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_style_pad_all(p, 0, 0);

    end_title = lv_label_create(p);
    lv_label_set_text(end_title, LV_SYMBOL_OK "  Sterilisation Terminee");
    lv_obj_set_pos(end_title, 22, 14);
    lv_obj_set_style_text_color(end_title, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_font(end_title, &lv_font_montserrat_36, 0);

    end_meta = lv_label_create(p);
    lv_label_set_text(end_meta, "");
    lv_obj_set_pos(end_meta, 22, 70);
    lv_obj_set_style_text_color(end_meta, lv_color_hex(0xC2CBDE), 0);
    lv_obj_set_style_text_font(end_meta, &lv_font_montserrat_16, 0);

    end_section(p, "DOSE UV-C RECUE", 130);
    end_dose = lv_label_create(p);
    lv_label_set_text(end_dose, "");
    lv_obj_set_pos(end_dose, 22, 156);
    lv_obj_set_style_text_color(end_dose, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_font(end_dose, &lv_font_montserrat_16, 0);

    end_section(p, "REDUCTION LOGARITHMIQUE ESTIMEE", 216);

    /* Toggle which target organisms are listed (shared dropdown) */
    lv_obj_t * btgt = lv_btn_create(p);
    lv_obj_set_size(btgt, 150, 34);
    lv_obj_set_align(btgt, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(btgt, -16, 210);
    lv_obj_set_style_bg_color(btgt, lv_color_hex(0x2A3A52), 0);
    lv_obj_set_style_radius(btgt, 8, 0);
    lv_obj_set_style_shadow_width(btgt, 0, 0);
    lv_obj_add_event_cb(btgt, org_open_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * ltgt = lv_label_create(btgt);
    lv_label_set_text(ltgt, "Cibles " LV_SYMBOL_DOWN);
    lv_obj_center(ltgt);
    lv_obj_set_style_text_color(ltgt, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_font(ltgt, &lv_font_montserrat_16, 0);

    end_logs = lv_label_create(p);
    lv_label_set_text(end_logs, "");
    lv_obj_set_pos(end_logs, 22, 242);
    lv_obj_set_style_text_color(end_logs, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_font(end_logs, &lv_font_montserrat_16, 0);

    /* Print Ticket (left) */
    lv_obj_t * bp = lv_btn_create(p);
    lv_obj_set_size(bp, 250, 56);
    lv_obj_set_align(bp, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_pos(bp, 16, -14);
    lv_obj_set_style_bg_color(bp, lv_color_hex(0x2A3A52), 0);
    lv_obj_set_style_radius(bp, 10, 0);
    lv_obj_set_style_shadow_width(bp, 0, 0);
    lv_obj_add_event_cb(bp, end_print_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * lp = lv_label_create(bp);
    lv_label_set_text(lp, LV_SYMBOL_SD_CARD "  Print Ticket");
    lv_obj_center(lp);
    lv_obj_set_style_text_color(lp, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_font(lp, &lv_font_montserrat_16, 0);

    /* Continue (right) */
    lv_obj_t * bc = lv_btn_create(p);
    lv_obj_set_size(bc, 250, 56);
    lv_obj_set_align(bc, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_pos(bc, -16, -14);
    lv_obj_set_style_bg_color(bc, lv_color_hex(0x00CCFC), 0);
    lv_obj_set_style_radius(bc, 10, 0);
    lv_obj_set_style_shadow_width(bc, 0, 0);
    lv_obj_add_event_cb(bc, end_cont_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * lc = lv_label_create(bc);
    lv_label_set_text(lc, "Continue");
    lv_obj_center(lc);
    lv_obj_set_style_text_color(lc, lv_color_hex(0x06222E), 0);
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_16, 0);
}

static void show_end_popup(bool aborted)
{
    if(!end_overlay) return;
    char buf[128];

    lv_snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  Sterilisation %s",
                aborted ? "Arretee" : "Terminee");
    lv_label_set_text(end_title, buf);
    lv_obj_set_style_text_color(end_title,
        lv_color_hex(aborted ? 0xFFB020 : 0x22DD88), 0);

    sbx_datetime_t dt;
    sbx_hal_get_datetime(&dt);
    lv_snprintf(buf, sizeof(buf),
                "Date  : %02u/%02u/%04u   %02u:%02u\n"
                "Duree : %u min %02u s        Cycle N: %u",
                dt.day, dt.month, dt.year, dt.hour, dt.minute,
                (unsigned)(cycle_total_s / 60u), (unsigned)(cycle_total_s % 60u),
                (unsigned)persist.cycles_done);
    lv_label_set_text(end_meta, buf);

    uint8_t lamps_active = 0;
    if(lamp_remaining_h(persist.lamp1_seconds) > 0) lamps_active++;
    if(lamp_remaining_h(persist.lamp2_seconds) > 0) lamps_active++;
    float dose_mj = uvc_dose_mj_cm2(cycle_elapsed_s, lamps_active);
    lv_snprintf(buf, sizeof(buf),
                "Estimee    : %d.%d mJ/cm2      Irradiance : %d.%d mW/cm2",
                (int)dose_mj, (int)(dose_mj * 10) % 10,
                (int)(SBX_UVC_IRRADIANCE_MW_CM2 * lamps_active),
                (int)(SBX_UVC_IRRADIANCE_MW_CM2 * lamps_active * 10) % 10);
    lv_label_set_text(end_dose, buf);

    /* Log10 reduction for each ENABLED target organism */
    char logs[400];
    logs[0] = '\0';
    for(int o = 0; o < SBX_ORG_COUNT; o++) {
        if(!organisms[o].enabled) continue;
        uint32_t k = dose_at(cycle_elapsed_s, cycle_total_s, organisms[o].log_max);
        char line[56];
        lv_snprintf(line, sizeof(line), "%s : %u.%u log10\n",
                    organisms[o].name, k / 100u, (k / 10u) % 10u);
        strncat(logs, line, sizeof(logs) - strlen(logs) - 1);
    }
    lv_label_set_text(end_logs, logs);

    lv_obj_clear_flag(end_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(end_overlay);
}

/* Info PRINT button: go Home, then show the last result popup so the
 * user confirms Print Ticket / Continue there. */
static void info_print_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _ui_screen_change(&ui_screenhome, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                      &ui_screenhome_screen_init);
    show_end_popup(state != SBX_STATE_DONE);
}

/*==================================================================
 * Target-organism toggle list (shared "dropdown", on the top layer)
 *=================================================================*/
static lv_obj_t * org_overlay;

static void org_redraw(void)
{
    if(cycle_total_s == 0) chart_reset(); else chart_update();
}

static void org_toggle_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    organisms[idx].enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    org_redraw();
    /* If the result popup is open, refresh its organism list too */
    if(end_overlay && !lv_obj_has_flag(end_overlay, LV_OBJ_FLAG_HIDDEN))
        show_end_popup(state != SBX_STATE_DONE);
}

static void org_close_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_add_flag(org_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void org_open_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_clear_flag(org_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(org_overlay);
}

static void org_list_create(void)
{
    org_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(org_overlay);
    lv_obj_set_size(org_overlay, 800, 480);
    lv_obj_set_style_bg_color(org_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(org_overlay, 160, 0);
    lv_obj_add_flag(org_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(org_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * p = lv_obj_create(org_overlay);
    lv_obj_set_size(p, 470, 440);
    lv_obj_center(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x18202E), 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0x00CCFC), 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(p, 16, 0);
    lv_obj_set_style_pad_row(p, 8, 0);

    lv_obj_t * title = lv_label_create(p);
    lv_label_set_text(title, "Bacteries cibles");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00CCFC), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    for(int o = 0; o < SBX_ORG_COUNT; o++) {
        lv_obj_t * row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 36);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 16, 16);
        lv_obj_set_align(dot, LV_ALIGN_LEFT_MID);
        lv_obj_set_style_radius(dot, 8, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(organisms[o].color), 0);
        lv_obj_set_style_bg_opa(dot, 255, 0);

        lv_obj_t * name = lv_label_create(row);
        lv_label_set_text(name, organisms[o].name);
        lv_obj_set_align(name, LV_ALIGN_LEFT_MID);
        lv_obj_set_pos(name, 28, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0xEAF2FF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);

        lv_obj_t * sw = lv_switch_create(row);
        lv_obj_set_align(sw, LV_ALIGN_RIGHT_MID);
        if(organisms[o].enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x00CCFC), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, org_toggle_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)o);
    }

    lv_obj_t * close = lv_btn_create(p);
    lv_obj_set_size(close, lv_pct(100), 48);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x00CCFC), 0);
    lv_obj_set_style_radius(close, 10, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_add_event_cb(close, org_close_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * lcl = lv_label_create(close);
    lv_label_set_text(lcl, "Fermer");
    lv_obj_center(lcl);
    lv_obj_set_style_text_color(lcl, lv_color_hex(0x06222E), 0);
    lv_obj_set_style_text_font(lcl, &lv_font_montserrat_16, 0);
}

/*--- Info screen: export date-range panel -------------------------
 * Export no longer writes immediately: a modal panel asks for the
 * From / To dates of the data to export, with Export / Cancel.     */
static lv_obj_t * exp_overlay;      /*full-screen modal layer*/
static lv_obj_t * exp_panel;
static lv_obj_t * exp_from_btn, * exp_from_lbl;
static lv_obj_t * exp_to_btn,   * exp_to_lbl;
static lv_obj_t * exp_cal;
static lv_obj_t * exp_active_lbl;   /*date label the calendar edits*/

static void exp_set_label_date(lv_obj_t * lbl, const sbx_datetime_t * dt)
{
    char buf[16];
    lv_snprintf(buf, sizeof(buf), "%02u/%02u/%04u", dt->day, dt->month, dt->year);
    lv_label_set_text(lbl, buf);
}

static void exp_date_btn_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    exp_active_lbl = (lv_event_get_target(e) == exp_from_btn) ? exp_from_lbl : exp_to_lbl;
    lv_obj_clear_flag(exp_cal, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

static void exp_cal_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_calendar_date_t date;
    if(lv_calendar_get_pressed_date(exp_cal, &date) == LV_RES_OK && exp_active_lbl) {
        char buf[16];
        lv_snprintf(buf, sizeof(buf), "%02u/%02u/%04u",
                    (unsigned)date.day, (unsigned)date.month, (unsigned)date.year);
        lv_label_set_text(exp_active_lbl, buf);
        lv_obj_add_flag(exp_cal, LV_OBJ_FLAG_HIDDEN);
        sbx_hal_buzzer(SBX_BEEP_KEY);
    }
}

static void exp_cancel_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_add_flag(exp_cal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(exp_overlay, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

static bool exp_parse_date(lv_obj_t * lbl, unsigned * d, unsigned * m, unsigned * y)
{
    return sscanf(lv_label_get_text(lbl), "%u/%u/%u", d, m, y) == 3;
}

static void exp_confirm_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    unsigned fd, fm, fy, td, tm, ty;
    if(!exp_parse_date(exp_from_lbl, &fd, &fm, &fy) ||
       !exp_parse_date(exp_to_lbl,   &td, &tm, &ty)) {
        sbx_hal_buzzer(SBX_BEEP_WARN);
        return;
    }

    /*From must not be after To*/
    uint32_t from_key = fy * 10000u + fm * 100u + fd;
    uint32_t to_key   = ty * 10000u + tm * 100u + td;
    if(from_key > to_key) {
        lv_obj_set_style_text_color(exp_from_lbl, lv_color_hex(0xFF5050), 0);
        lv_obj_set_style_text_color(exp_to_lbl,   lv_color_hex(0xFF5050), 0);
        sbx_hal_buzzer(SBX_BEEP_WARN);
        return;
    }
    lv_obj_set_style_text_color(exp_from_lbl, lv_color_hex(0xDBE6FF), 0);
    lv_obj_set_style_text_color(exp_to_lbl,   lv_color_hex(0xDBE6FF), 0);

    char period[80];
    snprintf(period, sizeof(period),
             "Period           : %02u/%02u/%04u - %02u/%02u/%04u\r\n",
             fd, fm, fy, td, tm, ty);

    char report[640];
    build_report(report, sizeof(report), period);

    char fname[64];
    snprintf(fname, sizeof(fname), "steribox_%04u%02u%02u-%04u%02u%02u.txt",
             fy, fm, fd, ty, tm, td);

    bool ok = sbx_hal_usb_export(fname, report);
    sbx_hal_buzzer(ok ? SBX_BEEP_OK : SBX_BEEP_WARN);
    if(ok) lv_obj_add_flag(exp_overlay, LV_OBJ_FLAG_HIDDEN);
}

/*Row: caption label + clickable date button, returns the button*/
static lv_obj_t * exp_make_date_row(lv_obj_t * parent, lv_coord_t y,
                                    const char * caption, lv_obj_t ** out_lbl)
{
    lv_obj_t * cap = lv_label_create(parent);
    lv_label_set_text(cap, caption);
    lv_obj_set_pos(cap, 24, y + 14);
    lv_obj_set_style_text_color(cap, lv_color_hex(0x9098AA), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_16, 0);

    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 240, 48);
    lv_obj_set_pos(btn, -24, y);
    lv_obj_set_align(btn, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x191D26), 0);
    lv_obj_set_style_bg_opa(btn, 255, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x414B62), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, exp_date_btn_cb, LV_EVENT_ALL, NULL);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "--/--/----");
    lv_obj_set_align(lbl, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDBE6FF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    *out_lbl = lbl;
    return btn;
}

static void export_panel_create(void)
{
    /*Modal layer swallowing all clicks behind it*/
    exp_overlay = lv_obj_create(ui_screeninfo);
    lv_obj_remove_style_all(exp_overlay);
    lv_obj_set_size(exp_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(exp_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(exp_overlay, 150, 0);
    lv_obj_add_flag(exp_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(exp_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(exp_overlay, LV_OBJ_FLAG_HIDDEN);

    exp_panel = lv_obj_create(exp_overlay);
    lv_obj_set_size(exp_panel, 460, 320);
    lv_obj_center(exp_panel);
    lv_obj_clear_flag(exp_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(exp_panel, lv_color_hex(0x22293A), 0);
    lv_obj_set_style_bg_opa(exp_panel, 255, 0);
    lv_obj_set_style_border_color(exp_panel, lv_color_hex(0x00CAFF), 0);
    lv_obj_set_style_border_width(exp_panel, 2, 0);
    lv_obj_set_style_radius(exp_panel, 14, 0);

    lv_obj_t * title = lv_label_create(exp_panel);
    lv_label_set_text(title, LV_SYMBOL_SD_CARD " Export Data Report");
    lv_obj_set_align(title, LV_ALIGN_TOP_MID);
    lv_obj_set_y(title, 6);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00CAFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    exp_from_btn = exp_make_date_row(exp_panel, 52,  "From :", &exp_from_lbl);
    exp_to_btn   = exp_make_date_row(exp_panel, 118, "To :",   &exp_to_lbl);

    /*Cancel (grey, bottom-left)*/
    lv_obj_t * btn_cancel = lv_btn_create(exp_panel);
    lv_obj_set_size(btn_cancel, 160, 52);
    lv_obj_set_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_pos(btn_cancel, 12, -12);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x3A3F4B), 0);
    lv_obj_set_style_bg_opa(btn_cancel, 255, 0);
    lv_obj_set_style_radius(btn_cancel, 10, 0);
    lv_obj_set_style_shadow_width(btn_cancel, 0, 0);
    lv_obj_add_event_cb(btn_cancel, exp_cancel_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_align(lbl_cancel, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(0xDBE6FF), 0);
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_16, 0);

    /*Export (cyan, bottom-right)*/
    lv_obj_t * btn_export = lv_btn_create(exp_panel);
    lv_obj_set_size(btn_export, 160, 52);
    lv_obj_set_align(btn_export, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_pos(btn_export, -12, -12);
    lv_obj_set_style_bg_color(btn_export, lv_color_hex(0x00CAFF), 0);
    lv_obj_set_style_bg_opa(btn_export, 255, 0);
    lv_obj_set_style_radius(btn_export, 10, 0);
    lv_obj_set_style_shadow_width(btn_export, 0, 0);
    lv_obj_add_event_cb(btn_export, exp_confirm_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * lbl_export = lv_label_create(btn_export);
    lv_label_set_text(lbl_export, LV_SYMBOL_USB " Export");
    lv_obj_set_align(lbl_export, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lbl_export, lv_color_hex(0x101820), 0);
    lv_obj_set_style_text_font(lbl_export, &lv_font_montserrat_16, 0);

    /*Shared calendar for both date fields (on the overlay, above the panel)*/
    exp_cal = lv_calendar_create(exp_overlay);
    lv_obj_set_size(exp_cal, 320, 320);
    lv_obj_center(exp_cal);
    lv_calendar_header_arrow_create(exp_cal);
    lv_obj_add_event_cb(exp_cal, exp_cal_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(exp_cal, LV_OBJ_FLAG_HIDDEN);
}

static void export_btn_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /*Pre-fill both dates with today, then open the range picker*/
    sbx_datetime_t dt;
    sbx_hal_get_datetime(&dt);
    exp_set_label_date(exp_from_lbl, &dt);
    exp_set_label_date(exp_to_lbl,   &dt);
    lv_obj_set_style_text_color(exp_from_lbl, lv_color_hex(0xDBE6FF), 0);
    lv_obj_set_style_text_color(exp_to_lbl,   lv_color_hex(0xDBE6FF), 0);
    lv_calendar_set_today_date(exp_cal, dt.year, dt.month, dt.day);
    lv_calendar_set_showed_date(exp_cal, dt.year, dt.month);
    lv_obj_add_flag(exp_cal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(exp_overlay, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

/*--- Config screen ------------------------------------------------*/
static void pwd_post_cb(lv_event_t * e)
{
    /*Runs AFTER the generated handler that hides the popup:
      re-arm the popup when the password was wrong.*/
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(pwd_ok) {
        lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_mainbody, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_popup, LV_OBJ_FLAG_HIDDEN);
        sbx_hal_buzzer(SBX_BEEP_OK);
    }
    else {
        lv_obj_add_flag(ui_mainbody, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_popup, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(ui_pwd, "");
        lv_textarea_set_placeholder_text(ui_pwd, "wrong password !");
        sbx_hal_buzzer(SBX_BEEP_WARN);
    }
}

static int lamp_to_reset = 0;

/* Refresh both arc widgets from persist data */
/* Status colour for a lamp given its remaining hours:
   0 -> red (expired), <= warn -> orange, else the lamp's normal colour. */
static uint32_t lamp_status_color(uint32_t remaining_h, uint32_t normal_col)
{
    if(remaining_h == 0)                    return 0xFF3030;   /*expired  - red   */
    if(remaining_h <= SBX_LAMP_WARN_HOURS)  return 0xFF8800;   /*low      - orange*/
    return normal_col;
}

/* Update one lamp's arc + labels with value, colour and expired state. */
static void refresh_one_lamp(lv_obj_t * arc, lv_obj_t * pct_lbl, lv_obj_t * hrs_lbl,
                             uint32_t lamp_seconds, uint32_t normal_col)
{
    char buf[32];
    uint32_t used_h    = lamp_seconds / 3600u;
    uint32_t remaining = lamp_remaining_h(lamp_seconds);
    uint32_t pct = (used_h >= SBX_LAMP_LIFE_HOURS) ? 0u
                 : (uint32_t)((SBX_LAMP_LIFE_HOURS - used_h) * 100u / SBX_LAMP_LIFE_HOURS);
    uint32_t col = lamp_status_color(remaining, normal_col);

    if(arc) {
        lv_arc_set_value(arc, (int16_t)pct);
        lv_obj_set_style_arc_color(arc, lv_color_hex(col), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }
    if(pct_lbl) {
        lv_snprintf(buf, sizeof(buf), "%u%% Remaining", (unsigned)pct);
        lv_label_set_text(pct_lbl, buf);
        lv_obj_set_style_text_color(pct_lbl, lv_color_hex(col), 0);
    }
    /*Remaining hours: Config matches Info + exported report; red EXPIRED at 0.*/
    if(hrs_lbl) {
        if(remaining == 0) lv_label_set_text(hrs_lbl, "EXPIRED");
        else { lv_snprintf(buf, sizeof(buf), "%u h left", (unsigned)remaining);
               lv_label_set_text(hrs_lbl, buf); }
        lv_obj_set_style_text_color(hrs_lbl, lv_color_hex(col), 0);
    }
}

static void refresh_lamp_arcs(void)
{
    refresh_one_lamp(ui_arc_lamp1, ui_lamp1_pct_label, ui_lamp1_hours_label,
                     persist.lamp1_seconds, 0x00CCFC);   /*Lamp 1 normal: cyan  */
    refresh_one_lamp(ui_arc_lamp2, ui_lamp2_pct_label, ui_lamp2_hours_label,
                     persist.lamp2_seconds, 0xFF7700);   /*Lamp 2 normal: amber */
}

static void confirm_yes_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(lamp_to_reset == 1) {
        persist.lamp1_seconds = 0;
        sbx_hal_storage_save(&persist);
        sbx_hal_log_event("CFG_LAMP_RESET", "lamp=1");
        sbx_hal_buzzer(SBX_BEEP_OK);
    }
    else if(lamp_to_reset == 2) {
        persist.lamp2_seconds = 0;
        sbx_hal_storage_save(&persist);
        sbx_hal_log_event("CFG_LAMP_RESET", "lamp=2");
        sbx_hal_buzzer(SBX_BEEP_OK);
    }
    lamp_to_reset = 0;
    lv_obj_add_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    refresh_lamp_arcs();
    info_screen_refresh();
}

static void confirm_no_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lamp_to_reset = 0;
    lv_obj_add_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

static void lamp1_reset_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lamp_to_reset = 1;
    lv_label_set_text(ui_confirm_label, "Reset Lamp 1 hours to 0?");
    lv_obj_clear_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

static void lamp2_reset_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lamp_to_reset = 2;
    lv_label_set_text(ui_confirm_label, "Reset Lamp 2 hours to 0?");
    lv_obj_clear_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

static void calendar_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_calendar_date_t date;
    if(lv_calendar_get_pressed_date(ui_Calendar2, &date) == LV_RES_OK) {
        char buf[16];
        lv_snprintf(buf, sizeof(buf), "%02u/%02u/%04u",
                    (unsigned)date.day, (unsigned)date.month, (unsigned)date.year);
        lv_textarea_set_text(ui_TextArea5, buf);
        lv_obj_add_flag(ui_layer2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Calendar2, LV_OBJ_FLAG_HIDDEN);
        sbx_hal_buzzer(SBX_BEEP_KEY);
    }
}

static void calendar_overlay_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t * target = lv_event_get_target(e);
    if(target == ui_layer2) {
        lv_obj_add_flag(ui_layer2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_Calendar2, LV_OBJ_FLAG_HIDDEN);
        sbx_hal_buzzer(SBX_BEEP_KEY);
    }
}

static void config_screen_loaded_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_SCREEN_LOADED) return;
    /* Re-arm password gate */
    pwd_ok = false;
    lv_textarea_set_text(ui_pwd, "");
    lv_textarea_set_placeholder_text(ui_pwd, "config_password");
    lv_obj_add_flag(ui_layer2,        LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Calendar2,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_mainbody,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_popup,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Keyboard1,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);

    /* Pre-fill date / time from RTC */
    sbx_datetime_t dt;
    sbx_hal_get_datetime(&dt);
    char buf[32];
    lv_snprintf(buf, sizeof(buf), "%02u/%02u/%04u", dt.day, dt.month, dt.year);
    lv_textarea_set_text(ui_TextArea5, buf);
    /* Roller1 is 12-hour ("12,01..11"): index = hour24 % 12 */
    lv_roller_set_selected(ui_Roller1, dt.hour % 12u, LV_ANIM_OFF);
    lv_roller_set_selected(ui_Roller2, dt.minute,     LV_ANIM_OFF);
    /* Roller3 (AM/PM): 0=AM(hour<12), 1=PM */
    lv_roller_set_selected(ui_Roller3, (dt.hour >= 12) ? 1u : 0u, LV_ANIM_OFF);

    /* Refresh arc gauges and labels */
    refresh_lamp_arcs();
}

/*--- Home nav fix: settings icon must open Config, not Info ------*/
static void home_settings_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _ui_screen_change(&ui_screenconfig, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                      &ui_screenconfig_screen_init);
}

/*==================================================================
 * Backends for the SquareLine named event hooks (see ui_events.c)
 *=================================================================*/
void steribox_ev_confirm_pwd(void)
{
    pwd_ok = (strcmp(lv_textarea_get_text(ui_pwd), persist.password) == 0);
    /*pwd_post_cb() (added after the generated handler) applies the result*/
}

void steribox_ev_cancel_pwd(void)
{
    /*No password -> leave the config area*/
    _ui_screen_change(&ui_screenhome, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                      &ui_screenhome_screen_init);
}

void steribox_ev_save_config(void)
{
    sbx_datetime_t dt;
    sbx_hal_get_datetime(&dt);

    unsigned d, m, y;
    if(sscanf(lv_textarea_get_text(ui_TextArea5), "%u/%u/%u", &d, &m, &y) == 3) {
        dt.day   = (uint8_t)d;
        dt.month = (uint8_t)m;
        dt.year  = (uint16_t)y;
    }

    char buf[8];
    lv_roller_get_selected_str(ui_Roller1, buf, sizeof(buf));
    uint8_t h = (uint8_t)atoi(buf);
    lv_roller_get_selected_str(ui_Roller2, buf, sizeof(buf));
    dt.minute = (uint8_t)atoi(buf);
    /* Roller3: 0=AM, 1=PM – convert hour to 24h */
    uint16_t ampm = lv_roller_get_selected(ui_Roller3);
    if(ampm == 1 && h < 12)       dt.hour = (uint8_t)(h + 12u);
    else if(ampm == 0 && h == 12) dt.hour = 0u;
    else                          dt.hour = h;

    sbx_hal_set_datetime(&dt);
    sbx_hal_storage_save(&persist);

    /* ---- SD log: config change ---- */
    {
        char detail[48];
        snprintf(detail, sizeof(detail), "set=%02u/%02u/%04u %02u:%02u",
                 (unsigned)dt.day, (unsigned)dt.month, (unsigned)dt.year,
                 (unsigned)dt.hour, (unsigned)dt.minute);
        sbx_hal_log_event("CFG_DATETIME", detail);
    }

    sbx_hal_buzzer(SBX_BEEP_OK);
}

/* Callback adapter for Apply Changes / Sync buttons */
static void save_config_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        steribox_ev_save_config();
        
        /* Check if it's the Apply Changes button to return home */
        lv_obj_t * target = lv_event_get_target(e);
        if(target == ui_BTN_Apply) {
            _ui_screen_change(&ui_screenhome, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                              &ui_screenhome_screen_init);
        }
    }
}

/*==================================================================
 * Periodic UI refresh
 *=================================================================*/
static void refresh_cb(lv_timer_t * timer)
{
    (void)timer;
    info_screen_refresh();
    usb_icons_refresh();
    clock_refresh();
    door_monitor();
}

/*==================================================================
 * Init
 *=================================================================*/
sbx_state_t steribox_app_get_state(void)
{
    return state;
}

void steribox_app_init(void)
{
    if(!sbx_hal_storage_load(&persist)) {
        persist_defaults();
        sbx_hal_storage_save(&persist);
    }

    /*--- Home screen ---*/
    /*Progress slider is display-only*/
    lv_obj_clear_flag(ui_Slider_Print_View1, LV_OBJ_FLAG_CLICKABLE);
    /*Settings nav icon: generated code wrongly targets the Info screen*/
    lv_obj_remove_event_cb(ui_BTN_Menu_Move_S1, ui_event_BTN_Menu_Move_S1);
    lv_obj_add_event_cb(ui_BTN_Menu_Move_S1, home_settings_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_BTN_Pause_Top1, start_btn_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Slider_Print_Speed2, duration_slider_cb, LV_EVENT_ALL, NULL);

    /*Chart: create one line series per organism, bound to org_data[o]*/
    lv_chart_set_point_count(ui_Chart3, SBX_CHART_POINTS);
    lv_chart_set_range(ui_Chart3, LV_CHART_AXIS_PRIMARY_Y, 0, 700);   /* fixed 0..7 log */
    for(int o = 0; o < SBX_ORG_COUNT; o++) {
        organisms[o].ser = lv_chart_add_series(ui_Chart3,
                              lv_color_hex(organisms[o].color), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_ext_y_array(ui_Chart3, organisms[o].ser, org_data[o]);
    }
    chart_reset();
    org_list_create();

    /* "Cibles" button on the graph card to open the organism toggle list */
    lv_obj_t * gcard = lv_obj_get_parent(ui_Chart3);
    lv_obj_t * gbtn = lv_btn_create(gcard);
    lv_obj_set_size(gbtn, 150, 34);
    lv_obj_set_align(gbtn, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(gbtn, -12, 12);
    lv_obj_set_style_bg_color(gbtn, lv_color_hex(0x2A3A52), 0);
    lv_obj_set_style_radius(gbtn, 8, 0);
    lv_obj_set_style_shadow_width(gbtn, 0, 0);
    lv_obj_add_event_cb(gbtn, org_open_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * glbl = lv_label_create(gbtn);
    lv_label_set_text(glbl, "Cibles " LV_SYMBOL_DOWN);
    lv_obj_center(glbl);
    lv_obj_set_style_text_color(glbl, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_font(glbl, &lv_font_montserrat_16, 0);

    /*Initial time display from the duration slider*/
    set_time_display((uint32_t)lv_slider_get_value(ui_Slider_Print_Speed2) * 60u);
    set_progress(0);

    /*--- Home end-of-cycle result popup ---*/
    end_popup_create();

    /*--- Info screen ---*/
    export_panel_create();
    lv_obj_add_flag(ui_BTN_Reset2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_BTN_Reset1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_BTN_Reset2, export_btn_cb, LV_EVENT_ALL, NULL);   /*Export*/
    lv_obj_add_event_cb(ui_BTN_Reset1, info_print_cb, LV_EVENT_ALL, NULL);   /*PRINT -> Home popup*/

    /*--- Config screen ---*/
    lv_obj_add_event_cb(ui_Button2,      pwd_post_cb,           LV_EVENT_ALL, NULL);
    lv_obj_add_flag(ui_lampe_1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_lampe_2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_lampe_1,      lamp1_reset_cb,        LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_lampe_2,      lamp2_reset_cb,        LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Calendar2,    calendar_cb,           LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_screenconfig, config_screen_loaded_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_confirm_yes,  confirm_yes_cb,        LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_confirm_no,   confirm_no_cb,         LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_layer2,       calendar_overlay_cb,   LV_EVENT_ALL, NULL);
    lv_obj_add_flag(ui_layer2, LV_OBJ_FLAG_CLICKABLE);
    /* Apply Changes + Synchronize buttons */
    if(ui_BTN_Apply) lv_obj_add_event_cb(ui_BTN_Apply, save_config_cb, LV_EVENT_ALL, NULL);

    /*--- Global periodic refresh ---*/
    refresh_timer = lv_timer_create(refresh_cb, SBX_UI_REFRESH_MS, NULL);
    (void)refresh_timer;

    info_screen_refresh();
    usb_icons_refresh();

    /*Start on the Home screen (generated ui_init loads Info first)*/
    lv_disp_load_scr(ui_screenhome);

    /* ---- BOOT marker in syslog ---- */
    {
        char detail[96];
        snprintf(detail, sizeof(detail),
                 "cycles=%u aborted=%u tot=%uh L1=%uh L2=%uh sd=%s",
                 (unsigned)persist.cycles_done,
                 (unsigned)persist.cycles_aborted,
                 (unsigned)(persist.total_seconds / 3600u),
                 (unsigned)lamp_remaining_h(persist.lamp1_seconds),
                 (unsigned)lamp_remaining_h(persist.lamp2_seconds),
                 sbx_hal_sd_present() ? "ok" : "absent");
        sbx_hal_log_event("BOOT", detail);
    }
}
