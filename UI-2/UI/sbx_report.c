/**
 * @file sbx_report.c
 * SteriBox - report composition. See sbx_report.h.
 */
#include "sbx_report.h"
#include "sbx_pdf.h"

#include <stdio.h>
#include <string.h>

/*==================================================================
 * Formatting helpers
 *=================================================================*/
const char *sbx_report_dur(char *buf, int len, uint32_t seconds) {
  if (seconds >= 3600u)
    snprintf(buf, len, "%u h %02u min %02u s", (unsigned)(seconds / 3600u),
             (unsigned)((seconds % 3600u) / 60u), (unsigned)(seconds % 60u));
  else if (seconds >= 60u)
    snprintf(buf, len, "%u min %02u s", (unsigned)(seconds / 60u),
             (unsigned)(seconds % 60u));
  else
    snprintf(buf, len, "%u s", (unsigned)seconds);
  return buf;
}

static const char *fmt_dt(char *buf, int len, const sbx_datetime_t *dt) {
  snprintf(buf, len, "%02u/%02u/%04u %02u:%02u", (unsigned)dt->day,
           (unsigned)dt->month, (unsigned)dt->year, (unsigned)dt->hour,
           (unsigned)dt->minute);
  return buf;
}

/** Verdict colour for a log reduction against the acceptance threshold. */
static uint32_t verdict_color(float lr, float target) {
  if (lr >= target)
    return SBX_PDF_OK;
  if (lr >= target * 0.75f)
    return SBX_PDF_WARN;
  return SBX_PDF_FAIL;
}

/** True when every enabled target cleared the acceptance threshold. */
static bool all_targets_met(const sbx_report_t *r) {
  if (!r->orgs || r->org_count == 0)
    return false;
  for (uint8_t i = 0; i < r->org_count; i++)
    if (r->orgs[i].log_reduction < r->target_log)
      return false;
  return true;
}

/*==================================================================
 * Plain-text ticket (USB printer, ~44 columns)
 *=================================================================*/
int sbx_report_text(const sbx_report_t *r, char *buf, int len) {
  /* One buffer per conversion: several are consumed by a single
   * snprintf() call, whose argument evaluation order is unspecified. */
  char d1[32], d2[32], d3[32], d4[32], d5[32], d6[32];
  int n = 0;

  n += snprintf(buf + n, len - n,
                "==========================================\r\n"
                "        STERIBOX - STERILISATION\r\n"
                "==========================================\r\n");

  if (r->period)
    n += snprintf(buf + n, len - n, "Periode      : %s\r\n", r->period);

  if (!r->have_cycle) {
    n += snprintf(buf + n, len - n,
                  "Edition      : %s\r\n"
                  "Aucun cycle execute depuis la mise sous\r\n"
                  "tension - rapport d'etat machine.\r\n",
                  fmt_dt(d1, sizeof(d1), &r->end));
  } else {
    n += snprintf(buf + n, len - n,
                  "Cycle N      : %u\r\n"
                  "Resultat     : %s\r\n"
                  "Motif de fin : %s\r\n"
                  "Debut        : %s\r\n"
                  "Fin          : %s\r\n",
                  (unsigned)r->seq, r->result, r->reason,
                  fmt_dt(d1, sizeof(d1), &r->start),
                  fmt_dt(d2, sizeof(d2), &r->end));

    n += snprintf(buf + n, len - n,
                  "------------------------------------------\r\n"
                  "DEROULEMENT\r\n"
                  "  Duree programmee : %s\r\n"
                  "  Duree effectuee  : %s\r\n"
                  "  Prechauffage     : %s\r\n"
                  "  LAMPES ALLUMEES  : %s\r\n"
                  "    dont lampe L1  : %s\r\n"
                  "    dont lampe L2  : %s\r\n"
                  "  Ouvertures porte : %u\r\n"
                  "  Lampes actives   : %u/2\r\n",
                  sbx_report_dur(d1, sizeof(d1), r->planned_s),
                  sbx_report_dur(d2, sizeof(d2), r->elapsed_s),
                  sbx_report_dur(d3, sizeof(d3), r->warmup_s),
                  sbx_report_dur(d4, sizeof(d4), r->lamp_on_s),
                  sbx_report_dur(d5, sizeof(d5), r->lamp1_on_s),
                  sbx_report_dur(d6, sizeof(d6), r->lamp2_on_s),
                  (unsigned)r->door_events, (unsigned)r->lamps_active);

    n += snprintf(buf + n, len - n,
                  "------------------------------------------\r\n"
                  "DOSE UV-C (point le plus defavorable)\r\n"
                  "  Irradiance    : %.2f mW/cm2\r\n"
                  "  Expo. equiv.  : %.0f s pleine puissance\r\n"
                  "  Dose recue    : %.1f mJ/cm2\r\n",
                  (double)r->irradiance, (double)r->eff_s, (double)r->dose_mj);

    n += snprintf(buf + n, len - n,
                  "------------------------------------------\r\n"
                  "REDUCTION MICROBIENNE (log10, cible %.0f)\r\n",
                  (double)r->target_log);
    for (uint8_t i = 0; i < r->org_count; i++)
      n += snprintf(buf + n, len - n, "  %-16s D10=%4.1f %5.2f log %s\r\n",
                    r->orgs[i].name, (double)r->orgs[i].d10,
                    (double)r->orgs[i].log_reduction,
                    (r->orgs[i].log_reduction >= r->target_log) ? "OK" : "--");
  }

  n += snprintf(buf + n, len - n,
                "------------------------------------------\r\n"
                "MACHINE\r\n"
                "  Temperature   : %.1f C\r\n"
                "  Humidite      : %.0f %%\r\n"
                "  Lampe L1 rest.: %u h\r\n"
                "  Lampe L2 rest.: %u h\r\n"
                "  Tps total app.: %u h\r\n"
                "  Cycles OK     : %u\r\n"
                "  Cycles annules: %u\r\n"
                "------------------------------------------\r\n"
                "Modele D10 surface seche @254nm, biphasique.\r\n"
                "Valeurs estimees - a confirmer par indicateur\r\n"
                "biologique et dosimetrie.\r\n"
                "==========================================\r\n\r\n\r\n",
                (double)r->temp_c, (double)r->hum_pct,
                (unsigned)r->lamp1_rem_h, (unsigned)r->lamp2_rem_h,
                (unsigned)r->total_h, (unsigned)r->cycles_done,
                (unsigned)r->cycles_aborted);

  return (n < len) ? n : len - 1;
}

/*==================================================================
 * CSV ledger row
 *=================================================================*/
const char *sbx_report_csv_header(void) {
  return "cycle,start,end,result,reason,planned_s,elapsed_s,warmup_s,"
         "lamp_on_s,lamp1_on_s,lamp2_on_s,door_events,lamps_active,"
         "irradiance_mW_cm2,equiv_s,dose_mJ_cm2,temp_C,hum_pct,"
         "lamp1_rem_h,lamp2_rem_h,total_h,cycles_ok,cycles_aborted,"
         "target_log,targets";
}

int sbx_report_csv(const sbx_report_t *r, char *buf, int len) {
  char d1[32], d2[32];
  int n = snprintf(
      buf, len,
      "%u,%s,%s,%s,%s,%u,%u,%u,%u,%u,%u,%u,%u,"
      "%.3f,%.1f,%.2f,%.1f,%.0f,%u,%u,%u,%u,%u,%.1f,",
      (unsigned)r->seq, fmt_dt(d1, sizeof(d1), &r->start),
      fmt_dt(d2, sizeof(d2), &r->end), r->result, r->reason,
      (unsigned)r->planned_s, (unsigned)r->elapsed_s, (unsigned)r->warmup_s,
      (unsigned)r->lamp_on_s, (unsigned)r->lamp1_on_s,
      (unsigned)r->lamp2_on_s, (unsigned)r->door_events,
      (unsigned)r->lamps_active, (double)r->irradiance, (double)r->eff_s,
      (double)r->dose_mj, (double)r->temp_c, (double)r->hum_pct,
      (unsigned)r->lamp1_rem_h, (unsigned)r->lamp2_rem_h,
      (unsigned)r->total_h, (unsigned)r->cycles_done,
      (unsigned)r->cycles_aborted, (double)r->target_log);

  /* Per-target results collapse into the last field as name:log pairs,
   * space separated, so the row width does not depend on how many
   * organisms the operator had enabled. */
  for (uint8_t i = 0; i < r->org_count && n < len - 1; i++)
    n += snprintf(buf + n, len - n, "%s%s:%.2f", i ? " " : "",
                  r->orgs[i].name, (double)r->orgs[i].log_reduction);

  return (n < len) ? n : len - 1;
}

/*==================================================================
 * Styled PDF
 *=================================================================*/
static sbx_pdf_t s_pdf; /* ~10 kB - static on purpose, never on a stack */

static bool pdf_sink(void *ctx, const void *data, uint32_t len) {
  (void)ctx;
  return sbx_hal_file_write(data, len);
}

bool sbx_report_pdf(const sbx_report_t *r, sbx_dest_t dest,
                    const char *filename) {
  char a[48], b[48], c[48];

  if (!sbx_hal_file_open(dest, filename))
    return false;

  sbx_pdf_begin(&s_pdf, pdf_sink, NULL);

  snprintf(a, sizeof(a), "SteriBox - genere le %s",
           fmt_dt(b, sizeof(b), &r->end));
  sbx_pdf_set_footer(&s_pdf, a);

  sbx_pdf_page_begin(&s_pdf);

  /*--- title band --------------------------------------------------*/
  if (r->have_cycle)
    snprintf(a, sizeof(a), "Cycle N %u  -  %s", (unsigned)r->seq, r->result);
  else
    snprintf(a, sizeof(a), "Rapport d'etat machine");
  sbx_pdf_header_band(&s_pdf, "RAPPORT DE STERILISATION UV-C", a);

  if (r->period) {
    snprintf(a, sizeof(a), "Periode exportee : %s", r->period);
    sbx_pdf_note(&s_pdf, a);
    sbx_pdf_space(&s_pdf, 6.0f);
  }

  /*--- verdict -----------------------------------------------------*/
  if (r->have_cycle) {
    sbx_pdf_section(&s_pdf, "RESULTAT");
    sbx_pdf_row_c(&s_pdf, "Issue du cycle", r->result,
                  r->completed ? SBX_PDF_OK : SBX_PDF_FAIL);
    sbx_pdf_row(&s_pdf, "Motif de fin", r->reason);
    sbx_pdf_row(&s_pdf, "Debut", fmt_dt(a, sizeof(a), &r->start));
    sbx_pdf_row(&s_pdf, "Fin", fmt_dt(a, sizeof(a), &r->end));
    if (r->completed) {
      bool met = all_targets_met(r);
      snprintf(a, sizeof(a), "%s (cible %.0f log)",
               met ? "Cibles atteintes" : "Cibles NON atteintes",
               (double)r->target_log);
      sbx_pdf_row_c(&s_pdf, "Conformite dose", a,
                    met ? SBX_PDF_OK : SBX_PDF_WARN);
    } else {
      sbx_pdf_row_c(&s_pdf, "Conformite dose",
                    "Non applicable - cycle interrompu", SBX_PDF_FAIL);
    }
    sbx_pdf_space(&s_pdf, 10.0f);

    /*--- timing ----------------------------------------------------*/
    sbx_pdf_section(&s_pdf, "DEROULEMENT");
    sbx_pdf_row(&s_pdf, "Duree programmee",
                sbx_report_dur(a, sizeof(a), r->planned_s));
    sbx_pdf_row(&s_pdf, "Duree effectuee",
                sbx_report_dur(a, sizeof(a), r->elapsed_s));
    sbx_pdf_row(&s_pdf, "Prechauffage amalgame",
                sbx_report_dur(a, sizeof(a), r->warmup_s));
    sbx_pdf_row_c(&s_pdf, "Temps lampes allumees",
                  sbx_report_dur(a, sizeof(a), r->lamp_on_s), SBX_PDF_ACCENT);
    snprintf(a, sizeof(a), "L1 %s   /   L2 %s",
             sbx_report_dur(b, sizeof(b), r->lamp1_on_s),
             sbx_report_dur(c, sizeof(c), r->lamp2_on_s));
    sbx_pdf_row(&s_pdf, "  detail par tube", a);
    snprintf(a, sizeof(a), "%u", (unsigned)r->door_events);
    sbx_pdf_row(&s_pdf, "Ouvertures de porte", a);
    snprintf(a, sizeof(a), "%u / 2", (unsigned)r->lamps_active);
    sbx_pdf_row(&s_pdf, "Lampes operationnelles", a);
    sbx_pdf_note(&s_pdf,
                 "Le temps lampes allumees inclut le prechauffage, pendant "
                 "lequel la sortie UV-C monte de 25% a 100%.");
    sbx_pdf_space(&s_pdf, 8.0f);

    /*--- dose ------------------------------------------------------*/
    sbx_pdf_section(&s_pdf, "DOSE UV-C AU POINT LE PLUS DEFAVORABLE");
    snprintf(a, sizeof(a), "%.2f mW/cm2", (double)r->irradiance);
    sbx_pdf_row(&s_pdf, "Irradiance (vieillissement inclus)", a);
    snprintf(a, sizeof(a), "%.0f s a pleine puissance", (double)r->eff_s);
    sbx_pdf_row(&s_pdf, "Exposition equivalente", a);
    snprintf(a, sizeof(a), "%.1f mJ/cm2", (double)r->dose_mj);
    sbx_pdf_row_c(&s_pdf, "Dose recue", a, SBX_PDF_ACCENT);
    sbx_pdf_space(&s_pdf, 10.0f);

    /*--- log reduction chart ---------------------------------------*/
    sbx_pdf_section(&s_pdf, "REDUCTION MICROBIENNE ESTIMEE");
    float vmax = r->target_log > 0.0f ? r->target_log * 1.5f : 6.0f;
    for (uint8_t i = 0; i < r->org_count; i++) {
      float lr = r->orgs[i].log_reduction;
      if (lr > vmax)
        vmax = lr;
    }
    for (uint8_t i = 0; i < r->org_count; i++) {
      float lr = r->orgs[i].log_reduction;
      snprintf(a, sizeof(a), "%.2f log", (double)lr);
      snprintf(b, sizeof(b), "%s  (D10 %.1f)", r->orgs[i].name,
               (double)r->orgs[i].d10);
      sbx_pdf_bar(&s_pdf, b, lr, vmax, verdict_color(lr, r->target_log), a,
                  r->target_log);
    }
    snprintf(a, sizeof(a),
             "Trait vertical = seuil d'acceptation (%.0f log10).",
             (double)r->target_log);
    sbx_pdf_note(&s_pdf, a);
    sbx_pdf_space(&s_pdf, 8.0f);
  }

  /*--- machine -----------------------------------------------------*/
  sbx_pdf_section(&s_pdf, "ETAT MACHINE");
  snprintf(a, sizeof(a), "%.1f C", (double)r->temp_c);
  sbx_pdf_row(&s_pdf, "Temperature chambre", a);
  snprintf(a, sizeof(a), "%.0f %%", (double)r->hum_pct);
  sbx_pdf_row(&s_pdf, "Humidite relative", a);
  snprintf(a, sizeof(a), "%u h", (unsigned)r->lamp1_rem_h);
  sbx_pdf_row_c(&s_pdf, "Lampe L1 - duree restante", a,
                r->lamp1_rem_h ? SBX_PDF_INK : SBX_PDF_FAIL);
  snprintf(a, sizeof(a), "%u h", (unsigned)r->lamp2_rem_h);
  sbx_pdf_row_c(&s_pdf, "Lampe L2 - duree restante", a,
                r->lamp2_rem_h ? SBX_PDF_INK : SBX_PDF_FAIL);
  snprintf(a, sizeof(a), "%u h", (unsigned)r->total_h);
  sbx_pdf_row(&s_pdf, "Temps de fonctionnement total", a);
  snprintf(a, sizeof(a), "%u", (unsigned)r->cycles_done);
  sbx_pdf_row(&s_pdf, "Cycles termines", a);
  snprintf(a, sizeof(a), "%u", (unsigned)r->cycles_aborted);
  sbx_pdf_row(&s_pdf, "Cycles annules", a);

  sbx_pdf_space(&s_pdf, 12.0f);
  sbx_pdf_rule(&s_pdf);
  sbx_pdf_note(&s_pdf,
               "Modele: reduction log10 a partir des valeurs D10 sur surface "
               "seche a 254 nm, cinetique biphasique.");
  sbx_pdf_note(&s_pdf,
               "Les doses indiquees sont calculees au point le plus "
               "defavorable de la chambre et restent des estimations:");
  sbx_pdf_note(&s_pdf,
               "elles doivent etre confirmees par indicateur biologique et "
               "par dosimetrie 254 nm avant usage clinique.");

  sbx_pdf_page_end(&s_pdf);
  bool built = sbx_pdf_end(&s_pdf);

  /* Only commit a document that was composed in full; close() is what
   * confirms the bytes actually reached the medium. */
  return sbx_hal_file_close(built) && built;
}
