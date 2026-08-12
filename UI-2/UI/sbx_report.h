/**
 * @file sbx_report.h
 * SteriBox - report composition.
 *
 * One input struct, three renderings:
 *   sbx_report_text()  -> the plain-text ticket sent to the USB printer
 *   sbx_report_pdf()   -> the styled A4 PDF written to a USB drive or SD
 *   sbx_report_csv()   -> one machine-readable row for the cycle ledger
 *
 * Keeping them behind a single struct is what guarantees the ticket, the
 * PDF and the CSV can never disagree about what a cycle delivered.
 *
 * The caller fills sbx_report_t from its own state; this module owns no
 * device state and does no I/O other than through the HAL file writer.
 */
#ifndef SBX_REPORT_H
#define SBX_REPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "steribox_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One target organism as it appeared in this cycle. */
typedef struct {
  const char *name;
  uint32_t color;      /* 0xRRGGBB, matches the on-screen chart series */
  float d10;           /* mJ/cm2 per log10 reduction                   */
  float log_reduction; /* achieved log10 reduction for this cycle      */
} sbx_report_org_t;

/** Everything a report can say about one cycle plus the device around it. */
typedef struct {
  /*--- identity -------------------------------------------------*/
  uint32_t seq;        /* device-wide cycle number, aborted included */
  bool completed;      /* false for a stopped / timed-out cycle      */
  const char *result;  /* short verdict, e.g. "TERMINE"              */
  const char *reason;  /* why it ended, human readable               */
  bool have_cycle;     /* false = device status report, no cycle yet */
  sbx_datetime_t start;
  sbx_datetime_t end;

  /*--- timing (seconds) -----------------------------------------*/
  uint32_t planned_s; /* run time selected by the operator      */
  uint32_t elapsed_s; /* run seconds actually completed         */
  uint32_t warmup_s;  /* preheat seconds actually performed     */
  uint32_t lamp_on_s; /* LAMP ACTIVE time, preheat included     */
  uint32_t lamp1_on_s;
  uint32_t lamp2_on_s;
  uint32_t door_events;

  /*--- UV-C dose ------------------------------------------------*/
  float eff_s;      /* full-power-equivalent lamp seconds */
  float dose_mj;    /* worst-point dose, mJ/cm2           */
  float irradiance; /* worst-point irradiance, mW/cm2     */
  uint8_t lamps_active;

  /*--- environment & device counters ----------------------------*/
  float temp_c;
  float hum_pct;
  uint32_t lamp1_rem_h;
  uint32_t lamp2_rem_h;
  uint32_t total_h;
  uint32_t cycles_done;
  uint32_t cycles_aborted;

  /*--- targets --------------------------------------------------*/
  const sbx_report_org_t *orgs;
  uint8_t org_count;
  float target_log; /* acceptance threshold drawn on the chart */

  /*--- optional --------------------------------------------------*/
  const char *period; /* "01/02/2026 - 28/02/2026", NULL if none */
} sbx_report_t;

/*------------------------------------------------------------------
 * Renderings
 *-----------------------------------------------------------------*/
/** Render the printer ticket into `buf`. Returns the length written. */
int sbx_report_text(const sbx_report_t *r, char *buf, int len);

/** Render one CSV row (no trailing newline) for the cycle ledger. */
int sbx_report_csv(const sbx_report_t *r, char *buf, int len);

/** Column header matching sbx_report_csv(), written when the ledger file
 *  is first created. */
const char *sbx_report_csv_header(void);

/** Compose the styled PDF and stream it to `dest` as `filename`.
 *  Returns false if the destination is unavailable or the write failed. */
bool sbx_report_pdf(const sbx_report_t *r, sbx_dest_t dest,
                    const char *filename);

/** "1 min 30 s" / "2 h 05 min" style duration, into a caller buffer. */
const char *sbx_report_dur(char *buf, int len, uint32_t seconds);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SBX_REPORT_H*/
