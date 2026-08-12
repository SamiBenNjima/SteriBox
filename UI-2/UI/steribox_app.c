/**
 * @file steribox_app.c
 * SteriBox UV Sterilizer - application logic (portable, LVGL 8.3).
 *
 * Owns:
 *  - the sterilization cycle state machine (door interlock, lamps, buzzer)
 *  - UV-C dose integration and per-organism log10 reduction model
 *  - lamp life / cycle counters with persistence
 *  - live refresh of the Info screen (temp / hum / hours / cycles)
 *  - config screen password gate, date & time setting, lamp hour reset
 *  - USB export / print reports
 *
 * It only touches the SquareLine-generated objects through their public
 * handles, so the UI can be re-exported from SquareLine at any time.
 *
 * Hardware assumptions (see the UV-C physics block below):
 *   Chamber  : 550 x 550 x 550 mm, mirror-polished 304 stainless,
 *              soda-lime glass door
 *   Lamps    : 2 x Philips TUV PL-L 95W HO, 28.1 W UV-C @ 253.7 nm @ 100 h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sbx_topbar.h"
#include "steribox_app.h"
#include "steribox_hal.h"
#include "ui.h"

#include <math.h>

/*==================================================================
 * Configuration
 *=================================================================*/
#define SBX_CHART_POINTS 16      /*dose curve resolution*/
#define SBX_LAMP_WARN_HOURS 100u /*orange warning threshold*/
#define SBX_UI_REFRESH_MS 500
#define SBX_CYCLE_TICK_MS 1000

/*==================================================================
 * UV-C physics
 *
 * Everything below is derived from the lamp datasheet and the chamber
 * geometry. Change these six numbers and the whole dose / log-kill
 * model follows - no other edit is needed.
 *=================================================================*/

/* Internal chamber area: 6 faces x 55 x 55 cm */
#define SBX_CHAMBER_AREA_CM2 18150.0f

/* Effective 254 nm reflectance of the cavity.
 *   5 faces mirror-polished 304 stainless : rho = 0.27
 *   1 face soda-lime glass door           : rho = 0.06
 *   rho_eff = (5*0.27 + 0.06) / 6 = 0.235
 *
 * IMPORTANT: mirror polish changes SPECULARITY, not reflectance. The
 * chromium-oxide passive layer absorbs UV-C, so stainless sits at
 * ~22-30% at 254 nm whatever the finish (brushed, BA, #8, electro-
 * polished). If UV-grade aluminium panels (Alanod Miro-Silver, rho
 * ~0.88) are later fitted to the 5 metal faces, set this to 0.74 -
 * cycle times then drop by roughly a factor 8. */
#define SBX_RHO_EFF 0.235f

/* Mean irradiance from ONE lamp, at 100 h, integrating-cavity model:
 *   E = P / (A * (1 - rho)) = 28100 mW / (18150 cm2 * 0.765)
 *     = 2.02 mW/cm2
 * Cross-checked against a line-source model at 25 cm (~2.0 mW/cm2). */
#define SBX_IRRADIANCE_PER_LAMP_MW_CM2 2.02f

/* Worst-point to mean ratio. A fully shadowed face only receives the
 * reflected component, so in an integrating cavity the ratio equals
 * rho_eff. Every dose guarantee below is computed on the WORST point,
 * never the mean - that is what sets the cycle time.
 *
 * REPLACE THIS with the measured min/mean once the chamber has been
 * mapped with a 254 nm radiometer (8 corners + centre + under a test
 * object). It is the only figure in this model that cannot be
 * computed reliably. */
#define SBX_UNIFORMITY SBX_RHO_EFF

/* Datasheet: UV-C output falls 15% over the 9000 h rated life. */
#define SBX_LAMP_EOL_FACTOR 0.85f

/* Amalgam preheat. The lamps are ENERGISED during this window and
 * their output ramps from ~25% to 100% over 180 s. The dose delivered
 * during the ramp is real dose and is counted. Never cut the lamps
 * between preheat and run: a hot-restruck amalgam needs 5-10 min to
 * recover. */
#define SBX_WARMUP_S 180u
#define SBX_WARMUP_START_FRAC 0.25f

/*==================================================================
 * Module state
 *=================================================================*/
static sbx_persist_t persist;
static sbx_state_t state = SBX_STATE_IDLE;

static uint32_t cycle_total_s; /*selected duration in seconds*/
static uint32_t cycle_elapsed_s;
/* Amalgam cool-down time constant. The UV-C output follows the
 * amalgam spot temperature, which has real thermal mass: after a
 * short outage the lamp is still essentially warm. Preheat credit
 * decays as exp(-t_off / TAU), so a 10 s door interruption costs
 * ~8% of the ramp instead of a full 180 s restart.
 * Measure with a radiometer and adjust: cut the lamps for 30/60/120 s
 * and log how long the output takes to come back. */
#define SBX_AMALGAM_TAU_S 120.0f

/* Minimum delay before re-energising, even on a fully warm lamp:
 * ballast restrike + time for the operator to clear the chamber. */
#define SBX_MIN_RESTRIKE_S 3u

<<<<<<< HEAD
static lv_timer_t * refresh_timer;
static lv_timer_t * cycle_timer;
static lv_timer_t * warmup_timer;      /* 3min s pre-lamp safety countdown */
static uint8_t      warmup_left;
static lv_timer_t * done_timer;        /* holds "DONE" ~3 s, then arms START */
static lv_timer_t * pause_timeout_timer; /* 10 s pause timeout before auto-abort */

#define SBX_WARMUP_S 180
#define SBX_PAUSE_TIMEOUT_MS 10000     /* 10 s max pause window */
=======
/* Dose accounting for the current cycle.
 * cycle_eff_s  : full-power-equivalent lamp-seconds delivered so far,
 *                integrated one second at a time so that door pauses
 *                and the resulting re-warmups are handled exactly.
 * lamp_streak_s: seconds since the lamps were last energised, drives
 *                the ramp fraction. Reset to 0 whenever they go off. */
static float cycle_eff_s;
static uint32_t lamp_streak_s; /* burn seconds credited to the ramp */
static uint32_t lamp_off_tick; /* lv_tick when the lamps last went off */

static bool pwd_ok; /*last password attempt result*/

static lv_timer_t *refresh_timer;
static lv_timer_t *cycle_timer;
static lv_timer_t *warmup_timer; /* 180 s amalgam preheat, lamps ON */
static uint16_t warmup_left;
static lv_timer_t *done_timer; /* holds "DONE" ~3 s, then arms START */
static lv_timer_t
    *pause_timeout_timer; /* 10 s pause timeout before auto-abort */

#define SBX_PAUSE_TIMEOUT_MS 10000 /* 10 s max pause window */
>>>>>>> dfeea45e2bc20f827e0b9e9dd96edf40f4175a84

/* End-of-cycle result popup (defined below, shown from cycle_stop) */
static void show_end_popup(bool aborted);
/* Target-organism toggle list (defined below) */
static void org_open_cb(lv_event_t *e);
static void org_list_create(void);

/*==================================================================
 * Target organisms - UV-C D10 model
 *
 * d10_x10 = dose for a 1 log10 reduction, in mJ/cm2, stored x10 so the
 * table stays integer. Dry-surface literature values at 254 nm; expect
 * a factor 2 spread depending on strain, substrate and humidity.
 *
 * The design drivers are A. niger (60 mJ/cm2 per log, pigmented mould
 * spores) and C. auris (30). Anything that clears those two clears the
 * rest with a very wide margin.
 *
 * The first 3 are enabled by default; the rest can be toggled on.
 *=================================================================*/
#define SBX_ORG_COUNT 11

typedef struct {
  const char *name;
  uint32_t color;
  uint16_t d10_x10; /* mJ/cm2 per log10, x10 */
  bool enabled;
  lv_chart_series_t *ser;
} sbx_org_t;

static sbx_org_t organisms[SBX_ORG_COUNT] = {
    {"E. coli", 0x00E0A0, 30, true, NULL},          /*  3.0 mJ/cm2 */
    {"S. aureus/MRSA", 0xFFB020, 40, true, NULL},   /*  4.0        */
    {"A. niger", 0xFF5470, 600, true, NULL},        /* 60.0 spores */
    {"P. aeruginosa", 0x4FC3F7, 30, false, NULL},   /*  3.0        */
    {"A. baumannii", 0xBA68C8, 35, false, NULL},    /*  3.5        */
    {"E. hirae", 0x9CCC65, 70, false, NULL},        /*  7.0        */
    {"C. albicans", 0xFFD54F, 120, false, NULL},    /* 12.0        */
    {"C. difficile", 0xF06292, 250, false, NULL},   /* 25.0 spores */
    {"M. tuberculosis", 0x4DD0E1, 60, false, NULL}, /*  6.0        */
    {"SARS-CoV-2", 0x8FA6FF, 20, false, NULL},      /*  2.0        */
    {"Candida auris", 0xFF8A65, 300, false, NULL},  /* 30.0        */
};

/* Per-organism chart data (log10 x100) */
static lv_coord_t org_data[SBX_ORG_COUNT][SBX_CHART_POINTS];

/*==================================================================
 * Helpers
 *=================================================================*/
static void persist_defaults(void) {
  memset(&persist, 0, sizeof(persist));
  persist.magic = SBX_PERSIST_MAGIC;
  strncpy(persist.password, SBX_DEFAULT_PASSWORD, sizeof(persist.password) - 1);
}

static uint32_t lamp_remaining_h(uint32_t lamp_seconds) {
  uint32_t used_h = lamp_seconds / 3600u;
  return (used_h >= SBX_LAMP_LIFE_HOURS) ? 0u : (SBX_LAMP_LIFE_HOURS - used_h);
}

<<<<<<< HEAD
static inline uint32_t slider_get_time_s(void)
{
    return (uint32_t)lv_slider_get_value(ui_Slider_Print_Speed2) * 5u;
=======
static inline uint32_t slider_get_time_s(void) {
  return (uint32_t)lv_slider_get_value(ui_Slider_Print_Speed2) * 15u;
>>>>>>> dfeea45e2bc20f827e0b9e9dd96edf40f4175a84
}

static void set_time_display(uint32_t seconds) {
  char buf[12];
  lv_snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(seconds / 60u),
              (unsigned)(seconds % 60u));
  lv_label_set_text(ui_Label_Time_1, buf); /* single "M:SS" readout */
}

static void set_progress(uint32_t pct) {
  char buf[8];
  lv_slider_set_value(ui_Slider_Print_View1, (int32_t)pct, LV_ANIM_ON);
  lv_snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
  lv_label_set_text(ui_Number_Print1, buf);
}

/*==================================================================
 * UV-C dose model
 *=================================================================*/

/** Instantaneous output fraction after s seconds of continuous burn. */
static float lamp_output_frac(uint32_t s) {
  if (s >= SBX_WARMUP_S)
    return 1.0f;
  return SBX_WARMUP_START_FRAC +
         (1.0f - SBX_WARMUP_START_FRAC) * (float)s / (float)SBX_WARMUP_S;
}

/** Full-power-equivalent seconds after t_s of UNINTERRUPTED burn.
 *  Integral of lamp_output_frac: f0*t + (1-f0)*t^2/(2W) during the ramp,
 *  then W*(1+f0)/2 + (t-W). Used to PROJECT the chart curve; the value
 *  actually reported comes from cycle_eff_s, which is integrated live. */
static float uvc_eff_seconds(uint32_t t_s) {
  const float W = (float)SBX_WARMUP_S;
  const float f0 = SBX_WARMUP_START_FRAC;
  float t = (float)t_s;
  if (t <= W)
    return f0 * t + (1.0f - f0) * t * t / (2.0f * W);
  return W * (1.0f + f0) * 0.5f + (t - W); /* 112.5 s + run time */
}

/** Output derating from lamp ageing: -15% linear over the rated life. */
static float lamp_derate(uint32_t lamp_seconds) {
  float h = (float)lamp_seconds / 3600.0f;
  float f =
      1.0f - (1.0f - SBX_LAMP_EOL_FACTOR) * (h / (float)SBX_LAMP_LIFE_HOURS);
  return (f < SBX_LAMP_EOL_FACTOR) ? SBX_LAMP_EOL_FACTOR : f;
}

/** Worst-point irradiance (mW/cm2) with the lamps currently usable,
 *  ageing included. */
static float uvc_irradiance_now(void) {
  float e = 0.0f;
  if (lamp_remaining_h(persist.lamp1_seconds) > 0)
    e += SBX_IRRADIANCE_PER_LAMP_MW_CM2 * lamp_derate(persist.lamp1_seconds);
  if (lamp_remaining_h(persist.lamp2_seconds) > 0)
    e += SBX_IRRADIANCE_PER_LAMP_MW_CM2 * lamp_derate(persist.lamp2_seconds);
  return e * SBX_UNIFORMITY;
}

/** Dose (mJ/cm2) for a given number of full-power-equivalent seconds. */
static float uvc_dose_from_eff(float eff_s) {
  return uvc_irradiance_now() * eff_s;
}

/** log10 reduction from dose, biphasic.
 *  A resistant sub-population (~0.1%: cell aggregates, soiling, optical
 *  shadowing) survives, so past 4 log the slope collapses by ~12x. A
 *  pure exponential would grossly overstate what a real surface
 *  achieves - never report more than this model gives. */
static float log_reduction(float dose_mj, uint16_t d10_x10) {
  if (d10_x10 == 0)
    return 0.0f;
  float lr = dose_mj * 10.0f / (float)d10_x10;
  if (lr > 4.0f)
    lr = 4.0f + (lr - 4.0f) / 12.0f;
  return (lr > 6.0f) ? 6.0f : lr;
}

/** Inverse of log_reduction: dose needed for a target log10 reduction. */
static float dose_for_log(float lr, uint16_t d10_x10) {
  float eq = (lr <= 4.0f) ? lr : 4.0f + (lr - 4.0f) * 12.0f;
  return eq * (float)d10_x10 / 10.0f;
}

/** Minimum RUN time (s, preheat excluded) to reach target_log on the
 *  most resistant ENABLED organism at the current lamp state.
 *  Returns 0 when the 180 s preheat alone already delivers it. */
static uint32_t cycle_time_for_log(float target_log) {
  float e = uvc_irradiance_now();
  if (e <= 0.0f)
    return 0u;

  float dmax = 0.0f;
  for (int o = 0; o < SBX_ORG_COUNT; o++) {
    if (!organisms[o].enabled)
      continue;
    float d = dose_for_log(target_log, organisms[o].d10_x10);
    if (d > dmax)
      dmax = d;
  }
  float need_eff = dmax / e;
  float warm_eff = (float)SBX_WARMUP_S * (1.0f + SBX_WARMUP_START_FRAC) * 0.5f;
  if (need_eff <= warm_eff)
    return 0u;
  return (uint32_t)(need_eff - warm_eff + 0.5f);
}

/*==================================================================
 * Chart
 *=================================================================*/
static void chart_reset(void) {
  for (int o = 0; o < SBX_ORG_COUNT; o++)
    for (int i = 0; i < SBX_CHART_POINTS; i++)
      org_data[o][i] = organisms[o].enabled ? 0 : LV_CHART_POINT_NONE;
  if (ui_Chart3)
    lv_chart_refresh(ui_Chart3);
}

/** Draw each enabled organism's log-kill curve. x = 0 is the START of
 *  the preheat, so the curve already rises before the countdown begins. */
static void chart_update(void) {
  if (cycle_total_s == 0) {
    chart_reset();
    return;
  }
  uint32_t filled = (cycle_elapsed_s * (SBX_CHART_POINTS - 1)) / cycle_total_s;
  if (filled > SBX_CHART_POINTS - 1)
    filled = SBX_CHART_POINTS - 1;

  for (int o = 0; o < SBX_ORG_COUNT; o++) {
    if (!organisms[o].enabled) {
      for (int i = 0; i < SBX_CHART_POINTS; i++)
        org_data[o][i] = LV_CHART_POINT_NONE;
      continue;
    }
    for (uint32_t i = 0; i <= filled; i++) {
      uint32_t t = SBX_WARMUP_S + (i * cycle_total_s) / (SBX_CHART_POINTS - 1);
      float dose = uvc_dose_from_eff(uvc_eff_seconds(t));
      float lr = log_reduction(dose, organisms[o].d10_x10);
      org_data[o][i] = (lv_coord_t)(lr * 100.0f + 0.5f);
    }
    for (uint32_t i = filled + 1; i < SBX_CHART_POINTS; i++)
      org_data[o][i] = LV_CHART_POINT_NONE;
  }
  if (ui_Chart3)
    lv_chart_refresh(ui_Chart3);
}

/*==================================================================
 * Info screen refresh
 *=================================================================*/
static void info_screen_refresh(void) {
  char buf[24];
  float t, h;

  if (sbx_hal_read_env(&t, &h)) {
    lv_snprintf(buf, sizeof(buf), "%d*C", (int)(t + 0.5f));
    lv_label_set_text(ui_Label_Z_Position_Number1, buf); /*Temp*/
    lv_snprintf(buf, sizeof(buf), "%d%%", (int)(h + 0.5f));
    lv_label_set_text(ui_Label_Z_Position_Number2, buf); /*Hum*/
  }

  /*Lamp remaining hours: L1 -> Number6, L2 -> Number3*/
  struct {
    lv_obj_t *label;
    uint32_t secs;
  } lamps[2] = {
      {ui_Label_Z_Position_Number6, persist.lamp1_seconds},
      {ui_Label_Z_Position_Number3, persist.lamp2_seconds},
  };
  for (int i = 0; i < 2; i++) {
    uint32_t rem = lamp_remaining_h(lamps[i].secs);
    if (rem == 0) {
      lv_label_set_text(lamps[i].label, "expire");
      lv_obj_set_style_text_color(lamps[i].label, lv_color_hex(0xFF0000), 0);
    } else {
      lv_snprintf(buf, sizeof(buf), "%u h", (unsigned)rem);
      lv_label_set_text(lamps[i].label, buf);
      lv_obj_set_style_text_color(lamps[i].label,
                                  rem <= SBX_LAMP_WARN_HOURS
                                      ? lv_color_hex(0xFF8800)
                                      : lv_color_hex(0xFFFFFF),
                                  0);
    }
  }

  lv_snprintf(buf, sizeof(buf), "%u h",
              (unsigned)(persist.total_seconds / 3600u));
  lv_label_set_text(ui_Label_Z_Position_Number5, buf); /*Total*/
  lv_snprintf(buf, sizeof(buf), "%u", (unsigned)persist.cycles_done);
  lv_label_set_text(ui_Label_Z_Position_Number4, buf); /*Cycles*/
}

static void usb_icons_refresh(void) {
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
  static lv_obj_t *sd_lbl_info = NULL;
  static lv_obj_t *sd_lbl_home = NULL;
  static lv_obj_t *sd_lbl_config = NULL;
  static bool sd_was_present = true; /* track transitions for serial msg */

  bool sd_now = sbx_hal_sd_present();

  /* Print one serial message on each transition */
  if (sd_was_present && !sd_now) {
    // Serial.println("[SD] No SD card — logging disabled, icon shown");
  }
  sd_was_present = sd_now;

/* Helper: create the SD-absent label the first time it is needed */
#define SBX_MAKE_SD_LBL(var, parent)                                           \
  do {                                                                         \
    if (!(var) && (parent)) {                                                  \
      (var) = lv_label_create(parent);                                         \
      lv_label_set_text((var), LV_SYMBOL_SD_CARD "!");                         \
      lv_obj_set_align((var), LV_ALIGN_TOP_RIGHT);                             \
      lv_obj_set_pos((var), -80, 12);                                          \
      lv_obj_set_style_text_color((var), lv_color_hex(0xFF4040), 0);           \
      lv_obj_set_style_text_font((var), &lv_font_montserrat_16, 0);            \
    }                                                                          \
  } while (0)

  SBX_MAKE_SD_LBL(sd_lbl_info, ui_Panel_Header1);
  SBX_MAKE_SD_LBL(sd_lbl_home, ui_Panel_Header3);
  SBX_MAKE_SD_LBL(sd_lbl_config, ui_Panel_Header5);

#undef SBX_MAKE_SD_LBL

  /* Show/hide based on card presence */
  lv_opa_t sd_opa = sd_now ? LV_OPA_TRANSP : LV_OPA_COVER;
  if (sd_lbl_info)
    lv_obj_set_style_opa(sd_lbl_info, sd_opa, 0);
  if (sd_lbl_home)
    lv_obj_set_style_opa(sd_lbl_home, sd_opa, 0);
  if (sd_lbl_config)
    lv_obj_set_style_opa(sd_lbl_config, sd_opa, 0);

  /* --- Refresh right-side icon group on every visible screen --- */
  /* ui_topbar_icons_* are declared in the respective screen files  */
  extern sbx_topbar_icons_t ui_topbar_icons_home;
  extern sbx_topbar_icons_t ui_topbar_icons_info;
  extern sbx_topbar_icons_t ui_topbar_icons_cfg;
  sbx_topbar_refresh(&ui_topbar_icons_home);
  sbx_topbar_refresh(&ui_topbar_icons_info);
  sbx_topbar_refresh(&ui_topbar_icons_cfg);
}

/*--- Header clock: one RTC read feeds every screen's clock label --*/
static void clock_refresh(void) {
  sbx_datetime_t dt;
  sbx_hal_get_datetime(&dt);
  char buf[8];
  lv_snprintf(buf, sizeof(buf), "%02u:%02u", dt.hour, dt.minute);
  if (ui_Label_Time1)
    lv_label_set_text(ui_Label_Time1, buf); /*Info  */
  if (ui_Label_Time3)
    lv_label_set_text(ui_Label_Time3, buf); /*Home  */
  if (ui_Label_Time5)
    lv_label_set_text(ui_Label_Time5, buf); /*Config*/
}

/*==================================================================
 * Sterilization cycle state machine
 *=================================================================*/
static void lamps_set(bool on) {
  bool l1_ok = lamp_remaining_h(persist.lamp1_seconds) > 0;
  bool l2_ok = lamp_remaining_h(persist.lamp2_seconds) > 0;

  /* Stamp the moment the lamps go off so the amalgam cool-down can
   * be credited on the next restrike. Do NOT clear lamp_streak_s:
   * a short outage barely cools the amalgam. */
  bool were_on =
      sbx_hal_relay_get(SBX_RELAY_LAMP1) || sbx_hal_relay_get(SBX_RELAY_LAMP2);
  if (!on && were_on)
    lamp_off_tick = lv_tick_get();

  sbx_hal_relay_set(SBX_RELAY_LAMP1, on && l1_ok);
  sbx_hal_relay_set(SBX_RELAY_LAMP2, on && l2_ok);
}

/** Preheat credit surviving the current off-time, in equivalent burn
 *  seconds. Newton cooling on the amalgam spot: credit *= exp(-toff/TAU).
 *  Returns 0 on a cold start (lamp_off_tick still 0). */
static uint32_t warm_credit(void) {
  if (lamp_streak_s == 0 || lamp_off_tick == 0)
    return 0u;
  float toff = (float)lv_tick_elaps(lamp_off_tick) / 1000.0f;
  if (toff > 10.0f * SBX_AMALGAM_TAU_S)
    return 0u; /* long cold */
  uint32_t s = (lamp_streak_s > SBX_WARMUP_S) ? SBX_WARMUP_S : lamp_streak_s;
  float kept = (float)s * expf(-toff / SBX_AMALGAM_TAU_S);
  return (uint32_t)(kept + 0.5f);
}

/** One second of lamp-on time: bump usage counters and integrate the
 *  delivered dose at the current ramp fraction. Called from both the
 *  preheat tick and the run tick. */
static void dose_tick(void) {
  bool any = false;
  if (sbx_hal_relay_get(SBX_RELAY_LAMP1)) {
    persist.lamp1_seconds++;
    any = true;
  }
  if (sbx_hal_relay_get(SBX_RELAY_LAMP2)) {
    persist.lamp2_seconds++;
    any = true;
  }
  persist.total_seconds++;
  if (any) {
    lamp_streak_s++;
    cycle_eff_s += lamp_output_frac(lamp_streak_s);
  }
}

static void lock_slider(bool lock) {
  if (lock)
    lv_obj_clear_flag(ui_Slider_Print_Speed2, LV_OBJ_FLAG_CLICKABLE);
  else
    lv_obj_add_flag(ui_Slider_Print_Speed2, LV_OBJ_FLAG_CLICKABLE);
}

/* Set the big button caption. The decorative ">" arrow is hidden for wide
 * words (RESUME) so the text has the full button width, shown otherwise. */
static void set_status(const char *txt) {
  lv_label_set_text(ui_Label1, txt);
  if (ui_Image_Pause1) {
    if (strcmp(txt, "RESUME") == 0)
      lv_obj_add_flag(ui_Image_Pause1, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_clear_flag(ui_Image_Pause1, LV_OBJ_FLAG_HIDDEN);
  }
}

/* After a completed cycle, revert the button from "DONE" to a ready
 * "START" and reset the readout to the selected duration. */
static void reset_after_done(void) {
  set_status("START");
  lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
  set_progress(0);
  set_time_display(slider_get_time_s());
  state = SBX_STATE_IDLE;
  sbx_hal_set_system_state((uint8_t)state);
}

static void done_timer_cb(lv_timer_t *t) {
  (void)t;
  done_timer = NULL; /* one-shot: auto-deleted after this call */
  if (state == SBX_STATE_DONE)
    reset_after_done();
}

/* Skip the 3 s hold and arm START immediately (e.g. on "Continue"). */
static void done_revert_now(void) {
  if (done_timer) {
    lv_timer_del(done_timer);
    done_timer = NULL;
  }
  if (state == SBX_STATE_DONE)
    reset_after_done();
}

/* Terminate the cycle: DONE (finished) or IDLE (manual stop). */
static void cycle_stop(sbx_state_t end_state) {
  lamps_set(false);
  if (cycle_timer) {
    lv_timer_del(cycle_timer);
    cycle_timer = NULL;
  }
  if (warmup_timer) {
    lv_timer_del(warmup_timer);
    warmup_timer = NULL;
  }
  if (pause_timeout_timer) {
    lv_timer_del(pause_timeout_timer);
    pause_timeout_timer = NULL;
  }

  if (end_state == SBX_STATE_DONE) {
    persist.cycles_done++;
    sbx_hal_buzzer(SBX_BEEP_DONE);
    set_status("DONE");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
    set_progress(100);
  } else { /*manual stop*/
    persist.cycles_aborted++;
    sbx_hal_buzzer(SBX_BEEP_OK);
    set_status("START");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
    set_progress(0);
    set_time_display(cycle_total_s ? cycle_total_s : slider_get_time_s());
  }

  sbx_hal_storage_save(&persist);

  /* ---- SD log: one row per cycle end ---- */
  {
    float t = 0.0f, h = 0.0f;
    sbx_hal_read_env(&t, &h);
    uint8_t lamps_active = 0;
    if (lamp_remaining_h(persist.lamp1_seconds) > 0)
      lamps_active++;
    if (lamp_remaining_h(persist.lamp2_seconds) > 0)
      lamps_active++;
    float dose = uvc_dose_from_eff(cycle_eff_s);
    float irr = uvc_irradiance_now();
    uint32_t dur_min = cycle_total_s / 60u;
    uint32_t dur_sec = cycle_total_s % 60u;
    char detail[224];
    snprintf(detail, sizeof(detail),
             "dur=%um%02us warm=%us dose=%.1fmJ/cm2 E=%.2fmW/cm2 eff=%.0fs"
             " lamps=%u/2 cycles=%u aborted=%u L1=%uh L2=%uh T=%dC RH=%d%%",
             (unsigned)dur_min, (unsigned)dur_sec, (unsigned)SBX_WARMUP_S,
             (double)dose, (double)irr, (double)cycle_eff_s,
             (unsigned)lamps_active, (unsigned)persist.cycles_done,
             (unsigned)persist.cycles_aborted,
             (unsigned)lamp_remaining_h(persist.lamp1_seconds),
             (unsigned)lamp_remaining_h(persist.lamp2_seconds), (int)(t + 0.5f),
             (int)(h + 0.5f));
    sbx_hal_log_event(
        (end_state == SBX_STATE_DONE) ? "CYCLE_DONE" : "CYCLE_ABORT", detail);
  }

  lv_obj_clear_state(ui_BTN_Pause_Top1, LV_STATE_CHECKED);
  lock_slider(false);
  state = end_state;
  sbx_hal_set_system_state((uint8_t)state);
  info_screen_refresh();

  /* Announce the result (Terminee / Arretee) on the Home screen */
  show_end_popup(end_state != SBX_STATE_DONE);

  /* On completion, hold "DONE" ~3 s (button inert) then arm START */
  if (end_state == SBX_STATE_DONE) {
    if (done_timer)
      lv_timer_del(done_timer);
    done_timer = lv_timer_create(done_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(done_timer, 1);
  }
}

/* 10 s pause timeout callback: aborts cycle if not resumed in time */
static void pause_timeout_cb(lv_timer_t *t) {
  (void)t;
  pause_timeout_timer = NULL;
  if (state == SBX_STATE_PAUSED_DOOR) {
    cycle_stop(SBX_STATE_IDLE); /* pause timed out (10 s) -> auto abort */
  }
}

/* SAFETY: door opened mid-run/preheat -> freeze, arm 10 s timeout.
 * The dose already integrated in cycle_eff_s is kept; the amalgam
 * cools, so lamps_set(false) resets the ramp and the resume restarts
 * a full 180 s preheat. */
static void cycle_pause_door(void) {
  lamps_set(false);
  char d[64];
  snprintf(d, sizeof(d), "eff=%.0fs streak=%us elapsed=%us",
           (double)cycle_eff_s, (unsigned)lamp_streak_s,
           (unsigned)cycle_elapsed_s);
  sbx_hal_log_event("DOOR_PAUSE", d);
  if (cycle_timer) {
    lv_timer_del(cycle_timer);
    cycle_timer = NULL;
  }
  if (warmup_timer) {
    lv_timer_del(warmup_timer);
    warmup_timer = NULL;
  }
  if (pause_timeout_timer) {
    lv_timer_del(pause_timeout_timer);
    pause_timeout_timer = NULL;
  }

  /* Arm 10 s pause timeout to auto-abort if user doesn't resume */
  pause_timeout_timer =
      lv_timer_create(pause_timeout_cb, SBX_PAUSE_TIMEOUT_MS, NULL);
  lv_timer_set_repeat_count(pause_timeout_timer, 1);

  sbx_hal_buzzer(SBX_BEEP_ALARM);
  set_status("DOOR !");
  lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFF3030), 0);
  state = SBX_STATE_PAUSED_DOOR;
  sbx_hal_set_system_state((uint8_t)state);
}

static void cycle_tick_cb(lv_timer_t *timer) {
  (void)timer;

  /*SAFETY: door interlock - immediate lamp cut-off + pause*/
  if (sbx_hal_door_is_open()) {
    cycle_pause_door();
    return;
  }

  cycle_elapsed_s++;
  dose_tick();

  uint32_t remaining =
      (cycle_elapsed_s >= cycle_total_s) ? 0 : cycle_total_s - cycle_elapsed_s;
  set_time_display(remaining);
  set_progress((cycle_elapsed_s * 100u) / cycle_total_s);
  chart_update();

  if (cycle_elapsed_s >= cycle_total_s)
    cycle_stop(SBX_STATE_DONE);
}

/* Energise the lamps and start (or continue) counting down.
 * lamps_set(true) is idempotent here - the lamps have been burning
 * throughout the preheat and MUST NOT be interrupted. */
static void cycle_run(void) {
  set_status("STOP");
  lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
  lamps_set(true);
  sbx_hal_buzzer(SBX_BEEP_OK);
  if (!cycle_timer)
    cycle_timer = lv_timer_create(cycle_tick_cb, SBX_CYCLE_TICK_MS, NULL);
  state = SBX_STATE_RUNNING;
  sbx_hal_set_system_state((uint8_t)state);
}

/* 180 s amalgam preheat: lamps ON, dose already accumulating, button
 * shows the countdown as M:SS. */
static void warmup_tick_cb(lv_timer_t *t) {
  (void)t;
  if (sbx_hal_door_is_open()) {
    cycle_pause_door();
    return;
  }

  dose_tick();

  if (warmup_left > 1) {
    warmup_left--;
    char b[12];
    lv_snprintf(b, sizeof(b), "%u:%02u", (unsigned)(warmup_left / 60u),
                (unsigned)(warmup_left % 60u));
    set_status(b);
    chart_update(); /* preheat dose is real dose */
    if (warmup_left <= 5)
      sbx_hal_buzzer(SBX_BEEP_KEY);
  } else {
    if (warmup_timer) {
      lv_timer_del(warmup_timer);
      warmup_timer = NULL;
    }
    cycle_run();
  }
}

/* Start a fresh cycle (fresh=true) or resume after a door pause
 * (fresh=false), always through the 180 s preheat. */
static void cycle_begin(bool fresh) {
  /* Cancel pause timeout if active */
  if (pause_timeout_timer) {
    lv_timer_del(pause_timeout_timer);
    pause_timeout_timer = NULL;
  }

  /*SAFETY: never energise with the door open*/
  if (sbx_hal_door_is_open()) {
    sbx_hal_buzzer(SBX_BEEP_WARN);
    set_status("DOOR !");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFF3030), 0);
    lv_obj_clear_state(ui_BTN_Pause_Top1, LV_STATE_CHECKED);
    return;
  }
  /*Efficacy: both lamps expired -> dose can't be guaranteed*/
  if (lamp_remaining_h(persist.lamp1_seconds) == 0 &&
      lamp_remaining_h(persist.lamp2_seconds) == 0) {
    sbx_hal_buzzer(SBX_BEEP_WARN);
    set_status("LAMPS !");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFF3030), 0);
    lv_obj_clear_state(ui_BTN_Pause_Top1, LV_STATE_CHECKED);
    return;
  }

  if (fresh) {
    cycle_total_s = slider_get_time_s();
    cycle_elapsed_s = 0;
    cycle_eff_s = 0.0f;
    chart_reset();
    set_progress(0);
    set_time_display(cycle_total_s);
  }
  lock_slider(true);

  /* Preheat only for what the amalgam actually lost. A door opened
   * and closed inside the 10 s pause window costs ~8 s here, not 180.
   * A cold lamp still gets the full ramp. Applies to back-to-back
   * cycles too: the second one starts almost hot. */
  uint32_t credit = warm_credit();
  lamp_streak_s = credit;
  uint32_t need = (credit >= SBX_WARMUP_S) ? 0u : (SBX_WARMUP_S - credit);
  warmup_left = (need < SBX_MIN_RESTRIKE_S) ? SBX_MIN_RESTRIKE_S : need;

  state = SBX_STATE_WARMUP;
  sbx_hal_set_system_state((uint8_t)state);
  lamps_set(true);
  lv_obj_add_state(ui_BTN_Pause_Top1, LV_STATE_CHECKED);
  char b[12];
  lv_snprintf(b, sizeof(b), "%u:%02u", (unsigned)(warmup_left / 60u),
              (unsigned)(warmup_left % 60u));
  set_status(b);
  lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x00D2FF), 0);
  sbx_hal_buzzer(SBX_BEEP_KEY);
  if (!warmup_timer)
    warmup_timer = lv_timer_create(warmup_tick_cb, 1000, NULL);
}

/* Poll the door each refresh: pause on open, offer RESUME once closed. */
static void door_monitor(void) {
  bool open = sbx_hal_door_is_open();
  if ((state == SBX_STATE_RUNNING || state == SBX_STATE_WARMUP) && open) {
    cycle_pause_door();
  } else if (state == SBX_STATE_PAUSED_DOOR) {
    if (open) {
      set_status("DOOR !");
      lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFF3030), 0);
    } else {
      set_status("RESUME");
      lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x22DD88), 0);
    }
  } else if (state == SBX_STATE_IDLE) {
    if (open) {
      set_status("DOOR !");
      lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFF3030), 0);
    } else {
      set_status("START");
      lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
    }
  }
}

/*==================================================================
 * Event callbacks added on top of the generated UI
 *=================================================================*/
static void start_btn_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;

  switch (state) {
  case SBX_STATE_RUNNING:
  case SBX_STATE_WARMUP:
    cycle_stop(SBX_STATE_IDLE); /* manual stop */
    break;
  case SBX_STATE_PAUSED_DOOR:
    cycle_begin(false); /* resume, full 180 s re-preheat */
    break;
  case SBX_STATE_DONE:
    /* Inert while "DONE" is held; the 3 s timer arms START */
    break;
  default:             /* IDLE */
    cycle_begin(true); /* fresh cycle */
    break;
  }
}

static void duration_slider_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    return;
  if (state == SBX_STATE_RUNNING)
    return;

  uint32_t sel = slider_get_time_s();
  set_time_display(sel);

  /* Orange readout when the selected run time cannot reach 4 log on
   * the most resistant ENABLED target (preheat dose included). */
  uint32_t need = cycle_time_for_log(4.0f);
  lv_obj_set_style_text_color(
      ui_Label_Time_1,
      (sel < need) ? lv_color_hex(0xFF8800) : lv_color_hex(0xFFFFFF), 0);

  set_status("START");
  lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0xFFFFFF), 0);
  set_progress(0);
  sbx_hal_buzzer(SBX_BEEP_KEY);
}

/*--- Info screen: export / print ---------------------------------*/
static void build_report(char *buf, int len,
                         const char *period /*NULL = none*/) {
  sbx_datetime_t dt;
  sbx_hal_get_datetime(&dt);
  float t = 0, h = 0;
  sbx_hal_read_env(&t, &h);

  uint8_t lamps_active = 0;
  if (lamp_remaining_h(persist.lamp1_seconds) > 0)
    lamps_active++;
  if (lamp_remaining_h(persist.lamp2_seconds) > 0)
    lamps_active++;

  float dose_mj = uvc_dose_from_eff(cycle_eff_s);
  float irr_now = uvc_irradiance_now();
  uint32_t dur_min = cycle_total_s / 60u;
  uint32_t dur_sec = cycle_total_s % 60u;

  /* Per-organism log kill, enabled targets only */
  char kills[400];
  kills[0] = '\0';
  for (int o = 0; o < SBX_ORG_COUNT; o++) {
    if (!organisms[o].enabled)
      continue;
    float lr = log_reduction(dose_mj, organisms[o].d10_x10);
    char line[72];
    snprintf(line, sizeof(line), "  %-16s D10=%4.1f  %.2f log\r\n",
             organisms[o].name, (double)organisms[o].d10_x10 / 10.0,
             (double)lr);
    strncat(kills, line, sizeof(kills) - strlen(kills) - 1);
  }

  snprintf(buf, len,
           "=== SteriBox UV Sterilizer ===\r\n"
           "Date     : %02u/%02u/%04u  %02u:%02u\r\n"
           "%s"
           "------------------------------\r\n"
           "CYCLE\r\n"
           "  Prechauffage : %u s (lampes ON)\r\n"
           "  Duree cycle  : %u min %02u s\r\n"
           "  Lampes       : %u/2\r\n"
           "------------------------------\r\n"
           "DOSE UV-C (point le plus defavorable)\r\n"
           "  Irradiance   : %.2f mW/cm2\r\n"
           "  Expo. equiv. : %.0f s pleine puissance\r\n"
           "  Dose recue   : %.1f mJ/cm2\r\n"
           "------------------------------\r\n"
           "REDUCTION MICROBIENNE (log10)\r\n"
           "%s"
           "------------------------------\r\n"
           "Temperature  : %.1f C\r\n"
           "Humidite     : %.0f %%\r\n"
           "Lampe L1 rest: %u h\r\n"
           "Lampe L2 rest: %u h\r\n"
           "Tps total app: %u h\r\n"
           "Cycles OK    : %u\r\n"
           "Cycles abort.: %u\r\n"
           "------------------------------\r\n"
           "Modele: D10 surface seche @254nm, biphasique.\r\n"
           "Valeurs estimees - a confirmer par indicateur\r\n"
           "biologique et dosimetrie.\r\n"
           "==============================\r\n",
           dt.day, dt.month, dt.year, dt.hour, dt.minute, period ? period : "",
           (unsigned)SBX_WARMUP_S, dur_min, dur_sec, (unsigned)lamps_active,
           (double)irr_now, (double)cycle_eff_s, (double)dose_mj, kills,
           (double)t, (double)h,
           (unsigned)lamp_remaining_h(persist.lamp1_seconds),
           (unsigned)lamp_remaining_h(persist.lamp2_seconds),
           (unsigned)(persist.total_seconds / 3600u),
           (unsigned)persist.cycles_done, (unsigned)persist.cycles_aborted);
}

static void print_btn_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  char report[1024];
  build_report(report, sizeof(report), NULL);
  sbx_hal_buzzer(sbx_hal_usb_print(report) ? SBX_BEEP_OK : SBX_BEEP_WARN);
}

/* Public guarded callbacks registered in ui_screeninfo.c --------------
 * These replace the old unguarded wiring and show an error popup when
 * the required device is not connected.                              */
void sbx_info_export_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  char report[1024];
  build_report(report, sizeof(report), NULL);
  sbx_try_export(ui_screeninfo, "steribox_log.txt", report);
}

void sbx_info_print_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  char report[1024];
  build_report(report, sizeof(report), NULL);
  sbx_try_print(ui_screeninfo, report);
}

/*==================================================================
 * End-of-cycle result popup (Home screen)
 *=================================================================*/
static lv_obj_t *end_overlay;
static lv_obj_t *end_title;
static lv_obj_t *end_meta;
static lv_obj_t *end_dose;
static lv_obj_t *end_logs;

static void end_print_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  char report[1024];
  build_report(report, sizeof(report), NULL);
  sbx_try_print(ui_screenhome, report);
}

static void end_cont_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  lv_obj_add_flag(end_overlay, LV_OBJ_FLAG_HIDDEN);
  done_revert_now(); /* continuing arms START immediately */
  sbx_hal_buzzer(SBX_BEEP_KEY);
}

/* One cyan section-title label */
static lv_obj_t *end_section(lv_obj_t *p, const char *txt, lv_coord_t y) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, txt);
  lv_obj_set_pos(l, 22, y);
  lv_obj_set_style_text_color(l, lv_color_hex(0x00CCFC), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
  return l;
}

static void end_popup_create(void) {
  end_overlay = lv_obj_create(ui_screenhome);
  lv_obj_remove_style_all(end_overlay);
  lv_obj_set_size(end_overlay, 800, 480);
  lv_obj_set_style_bg_color(end_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(end_overlay, 170, 0);
  lv_obj_add_flag(end_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(end_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(end_overlay, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *p = lv_obj_create(end_overlay);
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
  lv_obj_t *btgt = lv_btn_create(p);
  lv_obj_set_size(btgt, 150, 34);
  lv_obj_set_align(btgt, LV_ALIGN_TOP_RIGHT);
  lv_obj_set_pos(btgt, -16, 210);
  lv_obj_set_style_bg_color(btgt, lv_color_hex(0x2A3A52), 0);
  lv_obj_set_style_radius(btgt, 8, 0);
  lv_obj_set_style_shadow_width(btgt, 0, 0);
  lv_obj_add_event_cb(btgt, org_open_cb, LV_EVENT_ALL, NULL);
  lv_obj_t *ltgt = lv_label_create(btgt);
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
  lv_obj_t *bp = lv_btn_create(p);
  lv_obj_set_size(bp, 250, 56);
  lv_obj_set_align(bp, LV_ALIGN_BOTTOM_LEFT);
  lv_obj_set_pos(bp, 16, -14);
  lv_obj_set_style_bg_color(bp, lv_color_hex(0x2A3A52), 0);
  lv_obj_set_style_radius(bp, 10, 0);
  lv_obj_set_style_shadow_width(bp, 0, 0);
  lv_obj_add_event_cb(bp, end_print_cb, LV_EVENT_ALL, NULL);
  lv_obj_t *lp = lv_label_create(bp);
  lv_label_set_text(lp, LV_SYMBOL_SD_CARD "  Print Ticket");
  lv_obj_center(lp);
  lv_obj_set_style_text_color(lp, lv_color_hex(0xEAF2FF), 0);
  lv_obj_set_style_text_font(lp, &lv_font_montserrat_16, 0);

  /* Continue (right) */
  lv_obj_t *bc = lv_btn_create(p);
  lv_obj_set_size(bc, 250, 56);
  lv_obj_set_align(bc, LV_ALIGN_BOTTOM_RIGHT);
  lv_obj_set_pos(bc, -16, -14);
  lv_obj_set_style_bg_color(bc, lv_color_hex(0x00CCFC), 0);
  lv_obj_set_style_radius(bc, 10, 0);
  lv_obj_set_style_shadow_width(bc, 0, 0);
  lv_obj_add_event_cb(bc, end_cont_cb, LV_EVENT_ALL, NULL);
  lv_obj_t *lc = lv_label_create(bc);
  lv_label_set_text(lc, "Continue");
  lv_obj_center(lc);
  lv_obj_set_style_text_color(lc, lv_color_hex(0x06222E), 0);
  lv_obj_set_style_text_font(lc, &lv_font_montserrat_16, 0);
}

static void show_end_popup(bool aborted) {
  if (!end_overlay)
    return;
  char buf[160];

  lv_snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  Sterilisation %s",
              aborted ? "Arretee" : "Terminee");
  lv_label_set_text(end_title, buf);
  lv_obj_set_style_text_color(end_title,
                              lv_color_hex(aborted ? 0xFFB020 : 0x22DD88), 0);

  sbx_datetime_t dt;
  sbx_hal_get_datetime(&dt);
  lv_snprintf(buf, sizeof(buf),
              "Date  : %02u/%02u/%04u   %02u:%02u\n"
              "Duree : %u min %02u s + %u s prechauf.   Cycle N: %u",
              dt.day, dt.month, dt.year, dt.hour, dt.minute,
              (unsigned)(cycle_total_s / 60u), (unsigned)(cycle_total_s % 60u),
              (unsigned)SBX_WARMUP_S, (unsigned)persist.cycles_done);
  lv_label_set_text(end_meta, buf);

  float dose_mj = uvc_dose_from_eff(cycle_eff_s);
  float irr_now = uvc_irradiance_now();
  lv_snprintf(buf, sizeof(buf),
              "Dose : %d.%d mJ/cm2    Irradiance : %d.%02d mW/cm2 (pire point)",
              (int)dose_mj, (int)(dose_mj * 10) % 10, (int)irr_now,
              (int)(irr_now * 100) % 100);
  lv_label_set_text(end_dose, buf);

  /* Log10 reduction for each ENABLED target organism */
  char logs[400];
  logs[0] = '\0';
  for (int o = 0; o < SBX_ORG_COUNT; o++) {
    if (!organisms[o].enabled)
      continue;
    float lr = log_reduction(dose_mj, organisms[o].d10_x10);
    char line[64];
    lv_snprintf(line, sizeof(line), "%s : %d.%02d log10\n", organisms[o].name,
                (int)lr, (int)(lr * 100) % 100);
    strncat(logs, line, sizeof(logs) - strlen(logs) - 1);
  }
  lv_label_set_text(end_logs, logs);

  lv_obj_clear_flag(end_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(end_overlay);
}

/* Info PRINT button: go Home, then show the last result popup so the
 * user confirms Print Ticket / Continue there. */
static void info_print_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  _ui_screen_change(&ui_screenhome, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                    &ui_screenhome_screen_init);
  show_end_popup(state != SBX_STATE_DONE);
}

/*==================================================================
 * Target-organism toggle list (shared "dropdown", on the top layer)
 *=================================================================*/
static lv_obj_t *org_overlay;

static void org_redraw(void) {
  if (cycle_total_s == 0)
    chart_reset();
  else
    chart_update();
}

static void org_toggle_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    return;
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  organisms[idx].enabled =
      lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  org_redraw();

  /* Enabling a resistant target can invalidate the selected duration */
  if (state == SBX_STATE_IDLE) {
    uint32_t need = cycle_time_for_log(4.0f);
    lv_obj_set_style_text_color(ui_Label_Time_1,
                                (slider_get_time_s() < need)
                                    ? lv_color_hex(0xFF8800)
                                    : lv_color_hex(0xFFFFFF),
                                0);
  }

  /* If the result popup is open, refresh its organism list too */
  if (end_overlay && !lv_obj_has_flag(end_overlay, LV_OBJ_FLAG_HIDDEN))
    show_end_popup(state != SBX_STATE_DONE);
}

static void org_close_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  lv_obj_add_flag(org_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void org_open_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  lv_obj_clear_flag(org_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(org_overlay);
}

static void org_list_create(void) {
  org_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(org_overlay);
  lv_obj_set_size(org_overlay, 800, 480);
  lv_obj_set_style_bg_color(org_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(org_overlay, 160, 0);
  lv_obj_add_flag(org_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(org_overlay, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *p = lv_obj_create(org_overlay);
  lv_obj_set_size(p, 470, 440);
  lv_obj_center(p);
  lv_obj_set_style_bg_color(p, lv_color_hex(0x18202E), 0);
  lv_obj_set_style_border_color(p, lv_color_hex(0x00CCFC), 0);
  lv_obj_set_style_border_width(p, 2, 0);
  lv_obj_set_style_radius(p, 14, 0);
  lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(p, 16, 0);
  lv_obj_set_style_pad_row(p, 8, 0);

  lv_obj_t *title = lv_label_create(p);
  lv_label_set_text(title, "Bacteries cibles");
  lv_obj_set_style_text_color(title, lv_color_hex(0x00CCFC), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

  for (int o = 0; o < SBX_ORG_COUNT; o++) {
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 36);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_set_align(dot, LV_ALIGN_LEFT_MID);
    lv_obj_set_style_radius(dot, 8, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(organisms[o].color), 0);
    lv_obj_set_style_bg_opa(dot, 255, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, organisms[o].name);
    lv_obj_set_align(name, LV_ALIGN_LEFT_MID);
    lv_obj_set_pos(name, 28, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);

    /* D10 shown next to the name: makes the resistant targets obvious */
    lv_obj_t *d10 = lv_label_create(row);
    char db[16];
    lv_snprintf(db, sizeof(db), "D10 %u.%u",
                (unsigned)(organisms[o].d10_x10 / 10u),
                (unsigned)(organisms[o].d10_x10 % 10u));
    lv_label_set_text(d10, db);
    lv_obj_set_align(d10, LV_ALIGN_RIGHT_MID);
    lv_obj_set_pos(d10, -84, 0);
    lv_obj_set_style_text_color(d10, lv_color_hex(0x8A97AD), 0);
    lv_obj_set_style_text_font(d10, &lv_font_montserrat_16, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_align(sw, LV_ALIGN_RIGHT_MID);
    if (organisms[o].enabled)
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x00CCFC),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, org_toggle_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)o);
  }

  lv_obj_t *close = lv_btn_create(p);
  lv_obj_set_size(close, lv_pct(100), 48);
  lv_obj_set_style_bg_color(close, lv_color_hex(0x00CCFC), 0);
  lv_obj_set_style_radius(close, 10, 0);
  lv_obj_set_style_shadow_width(close, 0, 0);
  lv_obj_add_event_cb(close, org_close_cb, LV_EVENT_ALL, NULL);
  lv_obj_t *lcl = lv_label_create(close);
  lv_label_set_text(lcl, "Fermer");
  lv_obj_center(lcl);
  lv_obj_set_style_text_color(lcl, lv_color_hex(0x06222E), 0);
  lv_obj_set_style_text_font(lcl, &lv_font_montserrat_16, 0);
}

/*--- Info screen: export date-range panel -------------------------
 * Export no longer writes immediately: a modal panel asks for the
 * From / To dates of the data to export, with Export / Cancel.     */
static lv_obj_t *exp_overlay; /*full-screen modal layer*/
static lv_obj_t *exp_panel;
static lv_obj_t *exp_from_btn, *exp_from_lbl;
static lv_obj_t *exp_to_btn, *exp_to_lbl;
static lv_obj_t *exp_cal;
static lv_obj_t *exp_active_lbl; /*date label the calendar edits*/

static void exp_set_label_date(lv_obj_t *lbl, const sbx_datetime_t *dt) {
  char buf[16];
  lv_snprintf(buf, sizeof(buf), "%02u/%02u/%04u", dt->day, dt->month, dt->year);
  lv_label_set_text(lbl, buf);
}

static void exp_date_btn_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  exp_active_lbl =
      (lv_event_get_target(e) == exp_from_btn) ? exp_from_lbl : exp_to_lbl;
  lv_obj_clear_flag(exp_cal, LV_OBJ_FLAG_HIDDEN);
  sbx_hal_buzzer(SBX_BEEP_KEY);
}

static void exp_cal_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    return;
  lv_calendar_date_t date;
  if (lv_calendar_get_pressed_date(exp_cal, &date) == LV_RES_OK &&
      exp_active_lbl) {
    char buf[16];
    lv_snprintf(buf, sizeof(buf), "%02u/%02u/%04u", (unsigned)date.day,
                (unsigned)date.month, (unsigned)date.year);
    lv_label_set_text(exp_active_lbl, buf);
    lv_obj_add_flag(exp_cal, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
  }
}

static void exp_cancel_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  lv_obj_add_flag(exp_cal, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(exp_overlay, LV_OBJ_FLAG_HIDDEN);
  sbx_hal_buzzer(SBX_BEEP_KEY);
}

static bool exp_parse_date(lv_obj_t *lbl, unsigned *d, unsigned *m,
                           unsigned *y) {
  return sscanf(lv_label_get_text(lbl), "%u/%u/%u", d, m, y) == 3;
}

static void exp_confirm_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;

  unsigned fd, fm, fy, td, tm, ty;
  if (!exp_parse_date(exp_from_lbl, &fd, &fm, &fy) ||
      !exp_parse_date(exp_to_lbl, &td, &tm, &ty)) {
    sbx_hal_buzzer(SBX_BEEP_WARN);
    return;
  }

  /*From must not be after To*/
  uint32_t from_key = fy * 10000u + fm * 100u + fd;
  uint32_t to_key = ty * 10000u + tm * 100u + td;
  if (from_key > to_key) {
    lv_obj_set_style_text_color(exp_from_lbl, lv_color_hex(0xFF5050), 0);
    lv_obj_set_style_text_color(exp_to_lbl, lv_color_hex(0xFF5050), 0);
    sbx_hal_buzzer(SBX_BEEP_WARN);
    return;
  }
  lv_obj_set_style_text_color(exp_from_lbl, lv_color_hex(0xDBE6FF), 0);
  lv_obj_set_style_text_color(exp_to_lbl, lv_color_hex(0xDBE6FF), 0);

  char period[80];
  snprintf(period, sizeof(period),
           "Period           : %02u/%02u/%04u - %02u/%02u/%04u\r\n", fd, fm, fy,
           td, tm, ty);

  char report[1024];
  build_report(report, sizeof(report), period);

  char fname[64];
  snprintf(fname, sizeof(fname), "steribox_%04u%02u%02u-%04u%02u%02u.txt", fy,
           fm, fd, ty, tm, td);

  bool ok = sbx_hal_usb_export(fname, report);
  sbx_hal_buzzer(ok ? SBX_BEEP_OK : SBX_BEEP_WARN);
  if (ok)
    lv_obj_add_flag(exp_overlay, LV_OBJ_FLAG_HIDDEN);
}

/*Row: caption label + clickable date button, returns the button*/
static lv_obj_t *exp_make_date_row(lv_obj_t *parent, lv_coord_t y,
                                   const char *caption, lv_obj_t **out_lbl) {
  lv_obj_t *cap = lv_label_create(parent);
  lv_label_set_text(cap, caption);
  lv_obj_set_pos(cap, 24, y + 14);
  lv_obj_set_style_text_color(cap, lv_color_hex(0x9098AA), 0);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_16, 0);

  lv_obj_t *btn = lv_btn_create(parent);
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

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, "--/--/----");
  lv_obj_set_align(lbl, LV_ALIGN_CENTER);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xDBE6FF), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  *out_lbl = lbl;
  return btn;
}

static void export_panel_create(void) {
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

  lv_obj_t *title = lv_label_create(exp_panel);
  lv_label_set_text(title, LV_SYMBOL_SD_CARD " Export Data Report");
  lv_obj_set_align(title, LV_ALIGN_TOP_MID);
  lv_obj_set_y(title, 6);
  lv_obj_set_style_text_color(title, lv_color_hex(0x00CAFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

  exp_from_btn = exp_make_date_row(exp_panel, 52, "From :", &exp_from_lbl);
  exp_to_btn = exp_make_date_row(exp_panel, 118, "To :", &exp_to_lbl);

  /*Cancel (grey, bottom-left)*/
  lv_obj_t *btn_cancel = lv_btn_create(exp_panel);
  lv_obj_set_size(btn_cancel, 160, 52);
  lv_obj_set_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT);
  lv_obj_set_pos(btn_cancel, 12, -12);
  lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x3A3F4B), 0);
  lv_obj_set_style_bg_opa(btn_cancel, 255, 0);
  lv_obj_set_style_radius(btn_cancel, 10, 0);
  lv_obj_set_style_shadow_width(btn_cancel, 0, 0);
  lv_obj_add_event_cb(btn_cancel, exp_cancel_cb, LV_EVENT_ALL, NULL);
  lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
  lv_label_set_text(lbl_cancel, "Cancel");
  lv_obj_set_align(lbl_cancel, LV_ALIGN_CENTER);
  lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(0xDBE6FF), 0);
  lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_16, 0);

  /*Export (cyan, bottom-right)*/
  lv_obj_t *btn_export = lv_btn_create(exp_panel);
  lv_obj_set_size(btn_export, 160, 52);
  lv_obj_set_align(btn_export, LV_ALIGN_BOTTOM_RIGHT);
  lv_obj_set_pos(btn_export, -12, -12);
  lv_obj_set_style_bg_color(btn_export, lv_color_hex(0x00CAFF), 0);
  lv_obj_set_style_bg_opa(btn_export, 255, 0);
  lv_obj_set_style_radius(btn_export, 10, 0);
  lv_obj_set_style_shadow_width(btn_export, 0, 0);
  lv_obj_add_event_cb(btn_export, exp_confirm_cb, LV_EVENT_ALL, NULL);
  lv_obj_t *lbl_export = lv_label_create(btn_export);
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

static void export_btn_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  /*Pre-fill both dates with today, then open the range picker*/
  sbx_datetime_t dt;
  sbx_hal_get_datetime(&dt);
  exp_set_label_date(exp_from_lbl, &dt);
  exp_set_label_date(exp_to_lbl, &dt);
  lv_obj_set_style_text_color(exp_from_lbl, lv_color_hex(0xDBE6FF), 0);
  lv_obj_set_style_text_color(exp_to_lbl, lv_color_hex(0xDBE6FF), 0);
  lv_calendar_set_today_date(exp_cal, dt.year, dt.month, dt.day);
  lv_calendar_set_showed_date(exp_cal, dt.year, dt.month);
  lv_obj_add_flag(exp_cal, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(exp_overlay, LV_OBJ_FLAG_HIDDEN);
  sbx_hal_buzzer(SBX_BEEP_KEY);
}

/*--- Config screen ------------------------------------------------*/
static void pwd_post_cb(lv_event_t *e) {
  /*Runs AFTER the generated handler that hides the popup:
    re-arm the popup when the password was wrong.*/
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  if (pwd_ok) {
    lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_mainbody, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_popup, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_OK);
  } else {
    lv_obj_add_flag(ui_mainbody, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_popup, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(ui_pwd, "");
    lv_textarea_set_placeholder_text(ui_pwd, "wrong password !");
    sbx_hal_buzzer(SBX_BEEP_WARN);
  }
}

static int lamp_to_reset = 0;

/* Status colour for a lamp given its remaining hours:
   0 -> red (expired), <= warn -> orange, else the lamp's normal colour. */
static uint32_t lamp_status_color(uint32_t remaining_h, uint32_t normal_col) {
  if (remaining_h == 0)
    return 0xFF3030; /*expired  - red   */
  if (remaining_h <= SBX_LAMP_WARN_HOURS)
    return 0xFF8800; /*low      - orange*/
  return normal_col;
}

/* Update one lamp's arc + labels with value, colour and expired state. */
static void refresh_one_lamp(lv_obj_t *arc, lv_obj_t *pct_lbl,
                             lv_obj_t *hrs_lbl, uint32_t lamp_seconds,
                             uint32_t normal_col) {
  char buf[40];
  uint32_t used_h = lamp_seconds / 3600u;
  uint32_t remaining = lamp_remaining_h(lamp_seconds);
  uint32_t pct = (used_h >= SBX_LAMP_LIFE_HOURS)
                     ? 0u
                     : (uint32_t)((SBX_LAMP_LIFE_HOURS - used_h) * 100u /
                                  SBX_LAMP_LIFE_HOURS);
  uint32_t col = lamp_status_color(remaining, normal_col);

  if (arc) {
    lv_arc_set_value(arc, (int16_t)pct);
    lv_obj_set_style_arc_color(arc, lv_color_hex(col),
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
  }
  if (pct_lbl) {
    lv_snprintf(buf, sizeof(buf), "%u%% Remaining", (unsigned)pct);
    lv_label_set_text(pct_lbl, buf);
    lv_obj_set_style_text_color(pct_lbl, lv_color_hex(col), 0);
  }
  /*Remaining hours + current UV-C output, so the operator sees the
    -15% derating that the dose model is already applying.*/
  if (hrs_lbl) {
    if (remaining == 0)
      lv_label_set_text(hrs_lbl, "EXPIRED");
    else {
      uint32_t out_pct = (uint32_t)(lamp_derate(lamp_seconds) * 100.0f + 0.5f);
      lv_snprintf(buf, sizeof(buf), "%u h left  -  UVC %u%%",
                  (unsigned)remaining, (unsigned)out_pct);
      lv_label_set_text(hrs_lbl, buf);
    }
    lv_obj_set_style_text_color(hrs_lbl, lv_color_hex(col), 0);
  }
}

static void refresh_lamp_arcs(void) {
  refresh_one_lamp(ui_arc_lamp1, ui_lamp1_pct_label, ui_lamp1_hours_label,
                   persist.lamp1_seconds, 0x00CCFC); /*Lamp 1 normal: cyan  */
  refresh_one_lamp(ui_arc_lamp2, ui_lamp2_pct_label, ui_lamp2_hours_label,
                   persist.lamp2_seconds, 0xFF7700); /*Lamp 2 normal: amber */
}

static void confirm_yes_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * txt = ui_confirm_ta ? lv_textarea_get_text(ui_confirm_ta) : "0";
    int hours = atoi(txt);
    if(hours < 0) hours = 0;
    if(hours > (int)SBX_LAMP_LIFE_HOURS) hours = (int)SBX_LAMP_LIFE_HOURS;
    uint32_t new_secs = (uint32_t)hours * 3600u;

    if(lamp_to_reset == 1) {
        persist.lamp1_seconds = new_secs;
        sbx_hal_storage_save(&persist);
        sbx_hal_log_event("CFG_LAMP_EDIT", "lamp=1");
        sbx_hal_buzzer(SBX_BEEP_OK);
    }
    else if(lamp_to_reset == 2) {
        persist.lamp2_seconds = new_secs;
        sbx_hal_storage_save(&persist);
        sbx_hal_log_event("CFG_LAMP_EDIT", "lamp=2");
        sbx_hal_buzzer(SBX_BEEP_OK);
    }
    lamp_to_reset = 0;
    if(ui_Keyboard1) _ui_flag_modify(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
    if(ui_confirm_cpanel) lv_obj_set_y(ui_confirm_cpanel, 0);
    lv_obj_add_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    refresh_lamp_arcs();
    info_screen_refresh();
}

static void confirm_no_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lamp_to_reset = 0;
    if(ui_Keyboard1) _ui_flag_modify(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
    if(ui_confirm_cpanel) lv_obj_set_y(ui_confirm_cpanel, 0);
    lv_obj_add_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

static void lamp1_reset_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lamp_to_reset = 1;
    lv_label_set_text(ui_confirm_label, "Edit Lamp 1 Used Hours (0-9000):");
    char buf[16];
    uint32_t h = persist.lamp1_seconds / 3600u;
    lv_snprintf(buf, sizeof(buf), "%u", (unsigned)h);
    if(ui_confirm_ta) lv_textarea_set_text(ui_confirm_ta, buf);
    if(ui_confirm_cpanel) lv_obj_set_y(ui_confirm_cpanel, 0);
    if(ui_Keyboard1) _ui_flag_modify(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
    lv_obj_clear_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

static void lamp2_reset_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lamp_to_reset = 2;
    lv_label_set_text(ui_confirm_label, "Edit Lamp 2 Used Hours (0-9000):");
    char buf[16];
    uint32_t h = persist.lamp2_seconds / 3600u;
    lv_snprintf(buf, sizeof(buf), "%u", (unsigned)h);
    if(ui_confirm_ta) lv_textarea_set_text(ui_confirm_ta, buf);
    if(ui_confirm_cpanel) lv_obj_set_y(ui_confirm_cpanel, 0);
    if(ui_Keyboard1) _ui_flag_modify(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
    lv_obj_clear_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
}

static void calendar_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    return;
  lv_calendar_date_t date;
  if (lv_calendar_get_pressed_date(ui_Calendar2, &date) == LV_RES_OK) {
    char buf[16];
    lv_snprintf(buf, sizeof(buf), "%02u/%02u/%04u", (unsigned)date.day,
                (unsigned)date.month, (unsigned)date.year);
    lv_textarea_set_text(ui_TextArea5, buf);
    lv_obj_add_flag(ui_layer2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Calendar2, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
  }
}

static void calendar_overlay_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  lv_obj_t *target = lv_event_get_target(e);
  if (target == ui_layer2) {
    lv_obj_add_flag(ui_layer2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Calendar2, LV_OBJ_FLAG_HIDDEN);
    sbx_hal_buzzer(SBX_BEEP_KEY);
  }
}

static void config_screen_loaded_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_SCREEN_LOADED)
    return;
  /* Re-arm password gate */
  pwd_ok = false;
  lv_textarea_set_text(ui_pwd, "");
  lv_textarea_set_placeholder_text(ui_pwd, "config_password");
  lv_obj_add_flag(ui_layer2, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_Calendar2, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_mainbody, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(ui_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_confirm_popup, LV_OBJ_FLAG_HIDDEN);

  /* Pre-fill date / time from RTC */
  sbx_datetime_t dt;
  sbx_hal_get_datetime(&dt);
  char buf[32];
  lv_snprintf(buf, sizeof(buf), "%02u/%02u/%04u", dt.day, dt.month, dt.year);
  lv_textarea_set_text(ui_TextArea5, buf);
  /* Roller1 is 12-hour ("12,01..11"): index = hour24 % 12 */
  lv_roller_set_selected(ui_Roller1, dt.hour % 12u, LV_ANIM_OFF);
  lv_roller_set_selected(ui_Roller2, dt.minute, LV_ANIM_OFF);
  /* Roller3 (AM/PM): 0=AM(hour<12), 1=PM */
  lv_roller_set_selected(ui_Roller3, (dt.hour >= 12) ? 1u : 0u, LV_ANIM_OFF);

  /* Refresh arc gauges and labels */
  refresh_lamp_arcs();
}

/*--- Home nav fix: settings icon must open Config, not Info ------*/
static void home_settings_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  _ui_screen_change(&ui_screenconfig, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                    &ui_screenconfig_screen_init);
}

/*==================================================================
 * Backends for the SquareLine named event hooks (see ui_events.c)
 *=================================================================*/
void steribox_ev_confirm_pwd(void) {
  pwd_ok = (strcmp(lv_textarea_get_text(ui_pwd), persist.password) == 0);
  /*pwd_post_cb() (added after the generated handler) applies the result*/
}

void steribox_ev_cancel_pwd(void) {
  /*No password -> leave the config area*/
  _ui_screen_change(&ui_screenhome, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                    &ui_screenhome_screen_init);
}

void steribox_ev_save_config(void) {
  sbx_datetime_t dt;
  sbx_hal_get_datetime(&dt);

  unsigned d, m, y;
  if (sscanf(lv_textarea_get_text(ui_TextArea5), "%u/%u/%u", &d, &m, &y) == 3) {
    dt.day = (uint8_t)d;
    dt.month = (uint8_t)m;
    dt.year = (uint16_t)y;
  }

  char buf[8];
  lv_roller_get_selected_str(ui_Roller1, buf, sizeof(buf));
  uint8_t h = (uint8_t)atoi(buf);
  lv_roller_get_selected_str(ui_Roller2, buf, sizeof(buf));
  dt.minute = (uint8_t)atoi(buf);
  /* Roller3: 0=AM, 1=PM – convert hour to 24h */
  uint16_t ampm = lv_roller_get_selected(ui_Roller3);
  if (ampm == 1 && h < 12)
    dt.hour = (uint8_t)(h + 12u);
  else if (ampm == 0 && h == 12)
    dt.hour = 0u;
  else
    dt.hour = h;

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
static void save_config_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    steribox_ev_save_config();

    /* Check if it's the Apply Changes button to return home */
    lv_obj_t *target = lv_event_get_target(e);
    if (target == ui_BTN_Apply) {
      _ui_screen_change(&ui_screenhome, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                        &ui_screenhome_screen_init);
    }
  }
}

/*==================================================================
 * Periodic UI refresh
 *=================================================================*/
static void refresh_cb(lv_timer_t *timer) {
  (void)timer;
  info_screen_refresh();
  usb_icons_refresh();
  clock_refresh();
  door_monitor();
  sbx_hal_set_system_state((uint8_t)state);
}

/*==================================================================
 * Init
 *=================================================================*/
sbx_state_t steribox_app_get_state(void) { return state; }

void steribox_app_init(void) {
  if (!sbx_hal_storage_load(&persist)) {
    persist_defaults();
    sbx_hal_storage_save(&persist);
  }

  /*--- Home screen ---*/
  /*Progress slider is display-only*/
  lv_obj_clear_flag(ui_Slider_Print_View1, LV_OBJ_FLAG_CLICKABLE);
  /*Settings nav icon: generated code wrongly targets the Info screen*/
  lv_obj_remove_event_cb(ui_BTN_Menu_Move_S1, ui_event_BTN_Menu_Move_S1);
  lv_obj_add_event_cb(ui_BTN_Menu_Move_S1, home_settings_cb, LV_EVENT_ALL,
                      NULL);
  lv_obj_add_event_cb(ui_BTN_Pause_Top1, start_btn_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_event_cb(ui_Slider_Print_Speed2, duration_slider_cb, LV_EVENT_ALL,
                      NULL);

  /*Chart: create one line series per organism, bound to org_data[o]*/
  lv_chart_set_point_count(ui_Chart3, SBX_CHART_POINTS);
  lv_chart_set_range(ui_Chart3, LV_CHART_AXIS_PRIMARY_Y, 0, 600); /* 0..6 log */
  for (int o = 0; o < SBX_ORG_COUNT; o++) {
    organisms[o].ser = lv_chart_add_series(
        ui_Chart3, lv_color_hex(organisms[o].color), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_ext_y_array(ui_Chart3, organisms[o].ser, org_data[o]);
  }
  chart_reset();
  org_list_create();

  /* "Cibles" button on the graph card to open the organism toggle list */
  lv_obj_t *gcard = lv_obj_get_parent(ui_Chart3);
  lv_obj_t *gbtn = lv_btn_create(gcard);
  lv_obj_set_size(gbtn, 150, 34);
  lv_obj_set_align(gbtn, LV_ALIGN_TOP_RIGHT);
  lv_obj_set_pos(gbtn, -12, 12);
  lv_obj_set_style_bg_color(gbtn, lv_color_hex(0x2A3A52), 0);
  lv_obj_set_style_radius(gbtn, 8, 0);
  lv_obj_set_style_shadow_width(gbtn, 0, 0);
  lv_obj_add_event_cb(gbtn, org_open_cb, LV_EVENT_ALL, NULL);
  lv_obj_t *glbl = lv_label_create(gbtn);
  lv_label_set_text(glbl, "Cibles " LV_SYMBOL_DOWN);
  lv_obj_center(glbl);
  lv_obj_set_style_text_color(glbl, lv_color_hex(0xEAF2FF), 0);
  lv_obj_set_style_text_font(glbl, &lv_font_montserrat_16, 0);

  /*Initial time display from the duration slider*/
  set_time_display(slider_get_time_s());
  set_progress(0);
  {
    uint32_t need = cycle_time_for_log(4.0f);
    lv_obj_set_style_text_color(ui_Label_Time_1,
                                (slider_get_time_s() < need)
                                    ? lv_color_hex(0xFF8800)
                                    : lv_color_hex(0xFFFFFF),
                                0);
  }

  /*--- Home end-of-cycle result popup ---*/
  end_popup_create();

  /*--- Info screen ---*/
  export_panel_create();
  lv_obj_add_flag(ui_BTN_Reset2, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(ui_BTN_Reset1, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ui_BTN_Reset2, export_btn_cb, LV_EVENT_ALL,
                      NULL); /*Export*/
  lv_obj_add_event_cb(ui_BTN_Reset1, info_print_cb, LV_EVENT_ALL,
                      NULL); /*PRINT -> Home popup*/

<<<<<<< HEAD
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
    if(ui_confirm_ta) lv_obj_add_event_cb(ui_confirm_ta, confirm_yes_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ui_layer2,       calendar_overlay_cb,   LV_EVENT_ALL, NULL);
    lv_obj_add_flag(ui_layer2, LV_OBJ_FLAG_CLICKABLE);
    /* Apply Changes + Synchronize buttons */
    if(ui_BTN_Apply) lv_obj_add_event_cb(ui_BTN_Apply, save_config_cb, LV_EVENT_ALL, NULL);
=======
  /*--- Config screen ---*/
  lv_obj_add_event_cb(ui_Button2, pwd_post_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(ui_lampe_1, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(ui_lampe_2, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ui_lampe_1, lamp1_reset_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_event_cb(ui_lampe_2, lamp2_reset_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_event_cb(ui_Calendar2, calendar_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_event_cb(ui_screenconfig, config_screen_loaded_cb, LV_EVENT_ALL,
                      NULL);
  lv_obj_add_event_cb(ui_confirm_yes, confirm_yes_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_event_cb(ui_confirm_no, confirm_no_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_event_cb(ui_layer2, calendar_overlay_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(ui_layer2, LV_OBJ_FLAG_CLICKABLE);
  /* Apply Changes + Synchronize buttons */
  if (ui_BTN_Apply)
    lv_obj_add_event_cb(ui_BTN_Apply, save_config_cb, LV_EVENT_ALL, NULL);
>>>>>>> dfeea45e2bc20f827e0b9e9dd96edf40f4175a84

  /*--- Global periodic refresh ---*/
  refresh_timer = lv_timer_create(refresh_cb, SBX_UI_REFRESH_MS, NULL);
  (void)refresh_timer;
  (void)print_btn_cb; /* kept for direct-print wiring if needed */

  info_screen_refresh();
  usb_icons_refresh();

  /*Start on the Home screen (generated ui_init loads Info first)*/
  lv_disp_load_scr(ui_screenhome);

  /* ---- BOOT marker in syslog ---- */
  {
    char detail[160];
    snprintf(detail, sizeof(detail),
             "cycles=%u aborted=%u tot=%uh L1=%uh L2=%uh sd=%s"
             " E=%.2fmW/cm2 rho=%.3f warm=%us",
             (unsigned)persist.cycles_done, (unsigned)persist.cycles_aborted,
             (unsigned)(persist.total_seconds / 3600u),
             (unsigned)lamp_remaining_h(persist.lamp1_seconds),
             (unsigned)lamp_remaining_h(persist.lamp2_seconds),
             sbx_hal_sd_present() ? "ok" : "absent",
             (double)uvc_irradiance_now(), (double)SBX_RHO_EFF,
             (unsigned)SBX_WARMUP_S);
    sbx_hal_log_event("BOOT", detail);
  }
}