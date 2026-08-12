/**
 * @file sbx_pdf.c
 * SteriBox - minimal styled PDF writer. See sbx_pdf.h for the API.
 *
 * Structure of the produced file
 * ------------------------------
 *   %PDF-1.4
 *   3 0 obj  /Font Helvetica
 *   4 0 obj  /Font Helvetica-Bold
 *   5 0 obj  /Page 1        6 0 obj  content stream 1
 *   7 0 obj  /Page 2        8 0 obj  content stream 2   ...
 *   2 0 obj  /Pages   (written last: it needs every /Kid)
 *   1 0 obj  /Catalog
 *   xref / trailer / %%EOF
 *
 * Objects may appear in any order in a PDF - only the xref offsets matter -
 * which is what lets the whole document be streamed in a single pass.
 */
#include "sbx_pdf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Object numbers 1..4 are fixed; pages take 5, 6, 7, 8, ... */
#define OBJ_CATALOG 1
#define OBJ_PAGES 2
#define OBJ_FONT 3
#define OBJ_FONT_BOLD 4
#define OBJ_FIRST_PAGE 5

/*==================================================================
 * Raw output
 *=================================================================*/
static void out(sbx_pdf_t *p, const void *data, uint32_t len) {
  if (!p->ok || len == 0)
    return;
  if (!p->sink(p->ctx, data, len)) {
    p->ok = false;
    return;
  }
  p->offset += len;
}

static void outs(sbx_pdf_t *p, const char *s) {
  out(p, s, (uint32_t)strlen(s));
}

static void outf(sbx_pdf_t *p, const char *fmt, ...) {
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n > 0)
    out(p, tmp, (uint32_t)((n < (int)sizeof(tmp)) ? n : (int)sizeof(tmp) - 1));
}

/** Record where object `num` starts, then open it. */
static void obj_begin(sbx_pdf_t *p, uint16_t num) {
  if (num <= SBX_PDF_MAX_OBJ)
    p->obj_off[num] = p->offset;
  outf(p, "%u 0 obj\n", (unsigned)num);
}

static void obj_end(sbx_pdf_t *p) { outs(p, "endobj\n"); }

/*==================================================================
 * Page content stream
 *
 * Operators go straight to the sink. The stream's byte count is only
 * known once the page is finished, so it is declared through an indirect
 * /Length object emitted just after "endstream" - which is exactly what
 * removes any per-page size limit.
 *=================================================================*/
static void cs(sbx_pdf_t *p, const char *fmt, ...) {
  if (!p->in_page)
    return;
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  out(p, tmp, (uint32_t)((n < (int)sizeof(tmp)) ? n : (int)sizeof(tmp) - 1));
}

/** Append a PDF literal string with the three characters that must be
 *  escaped - ( ) \ - protected. Non-ASCII bytes are replaced so the
 *  document stays inside WinAnsiEncoding and never carries a NUL. */
static void cs_str(sbx_pdf_t *p, const char *s) {
  char esc[256];
  uint32_t n = 0;
  if (!s)
    s = "";
  for (; *s && n < sizeof(esc) - 2; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '(' || c == ')' || c == '\\') {
      esc[n++] = '\\';
      esc[n++] = (char)c;
    } else if (c < 0x20 || c > 0x7E) {
      esc[n++] = '?';
    } else {
      esc[n++] = (char)c;
    }
  }
  esc[n] = '\0';
  cs(p, "%s", esc);
}

/*==================================================================
 * Drawing primitives (operate on the current page's content stream)
 *=================================================================*/
static void set_fill(sbx_pdf_t *p, uint32_t rgb) {
  cs(p, "%.3f %.3f %.3f rg\n", (double)((rgb >> 16) & 0xFF) / 255.0,
     (double)((rgb >> 8) & 0xFF) / 255.0, (double)(rgb & 0xFF) / 255.0);
}

static void set_stroke(sbx_pdf_t *p, uint32_t rgb) {
  cs(p, "%.3f %.3f %.3f RG\n", (double)((rgb >> 16) & 0xFF) / 255.0,
     (double)((rgb >> 8) & 0xFF) / 255.0, (double)(rgb & 0xFF) / 255.0);
}

static void rect(sbx_pdf_t *p, float x, float y, float w, float h,
                 uint32_t rgb) {
  set_fill(p, rgb);
  cs(p, "%.2f %.2f %.2f %.2f re f\n", (double)x, (double)y, (double)w,
     (double)h);
}

static void line(sbx_pdf_t *p, float x1, float y1, float x2, float y2,
                 float w, uint32_t rgb) {
  set_stroke(p, rgb);
  cs(p, "%.2f w %.2f %.2f m %.2f %.2f l S\n", (double)w, (double)x1,
     (double)y1, (double)x2, (double)y2);
}

static void text(sbx_pdf_t *p, float x, float y, float size, bool bold,
                 uint32_t rgb, const char *s) {
  set_fill(p, rgb);
  cs(p, "BT /%s %.1f Tf %.2f %.2f Td (", bold ? "FB" : "FR", (double)size,
     (double)x, (double)y);
  cs_str(p, s);
  cs(p, ") Tj ET\n");
}

/*==================================================================
 * Document
 *=================================================================*/
void sbx_pdf_begin(sbx_pdf_t *p, sbx_pdf_sink_fn sink, void *ctx) {
  memset(p, 0, sizeof(*p));
  p->sink = sink;
  p->ctx = ctx;
  p->ok = (sink != NULL);
  p->next_obj = OBJ_FIRST_PAGE;

  outs(p, "%PDF-1.4\n");

  /* The two standard faces. Never embedded: every PDF reader ships them. */
  obj_begin(p, OBJ_FONT);
  outs(p, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
          "/Encoding /WinAnsiEncoding >>\n");
  obj_end(p);

  obj_begin(p, OBJ_FONT_BOLD);
  outs(p, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold "
          "/Encoding /WinAnsiEncoding >>\n");
  obj_end(p);
}

void sbx_pdf_page_begin(sbx_pdf_t *p) {
  if (!p->ok || p->in_page)
    return;
  if (p->page_count >= SBX_PDF_MAX_PAGES) {
    p->ok = false;
    return;
  }

  uint16_t page_obj = p->next_obj++;
  p->cont_obj = p->next_obj++;
  p->len_obj = p->next_obj++;
  p->page_obj[p->page_count++] = page_obj;

  obj_begin(p, page_obj);
  outf(p,
       "<< /Type /Page /Parent %u 0 R /MediaBox [0 0 %.0f %.0f] "
       "/Resources << /Font << /FR %u 0 R /FB %u 0 R >> >> "
       "/Contents %u 0 R >>\n",
       (unsigned)OBJ_PAGES, (double)SBX_PDF_PAGE_W, (double)SBX_PDF_PAGE_H,
       (unsigned)OBJ_FONT, (unsigned)OBJ_FONT_BOLD, (unsigned)p->cont_obj);
  obj_end(p);

  obj_begin(p, p->cont_obj);
  outf(p, "<< /Length %u 0 R >>\nstream\n", (unsigned)p->len_obj);

  p->stream_start = p->offset;
  p->row_idx = 0;
  p->in_page = true;
  p->y = SBX_PDF_PAGE_H - SBX_PDF_MARGIN;
}

/** Stamp the footer rule, the caller's text and "Page N" at the bottom of
 *  the page that is about to be closed. */
static void stamp_footer(sbx_pdf_t *p) {
  char pg[24];
  snprintf(pg, sizeof(pg), "Page %d", sbx_pdf_page_no(p));

  line(p, SBX_PDF_MARGIN, 48.0f, SBX_PDF_PAGE_W - SBX_PDF_MARGIN, 48.0f, 0.5f,
       0xC7D2E0);
  if (p->footer[0])
    text(p, SBX_PDF_MARGIN, 34.0f, 8.0f, false, SBX_PDF_MUTED, p->footer);
  text(p, SBX_PDF_PAGE_W - SBX_PDF_MARGIN - 44.0f, 34.0f, 8.0f, false,
       SBX_PDF_MUTED, pg);
}

void sbx_pdf_page_end(sbx_pdf_t *p) {
  if (!p->ok || !p->in_page)
    return;
  stamp_footer(p);

  /* Exact stream length: the EOL that follows is the delimiter before
   * "endstream" and is not part of the stream data. */
  uint32_t stream_len = p->offset - p->stream_start;
  p->in_page = false;

  outs(p, "\nendstream\n");
  obj_end(p);

  obj_begin(p, p->len_obj);
  outf(p, "%u\n", (unsigned)stream_len);
  obj_end(p);
}

bool sbx_pdf_end(sbx_pdf_t *p) {
  if (p->in_page)
    sbx_pdf_page_end(p);
  if (!p->ok || p->page_count == 0)
    return false;

  /* /Pages - now that every page object number is known */
  obj_begin(p, OBJ_PAGES);
  outf(p, "<< /Type /Pages /Count %u /Kids [", (unsigned)p->page_count);
  for (uint8_t i = 0; i < p->page_count; i++)
    outf(p, "%s%u 0 R", i ? " " : "", (unsigned)p->page_obj[i]);
  outs(p, "] >>\n");
  obj_end(p);

  obj_begin(p, OBJ_CATALOG);
  outf(p, "<< /Type /Catalog /Pages %u 0 R >>\n", (unsigned)OBJ_PAGES);
  obj_end(p);

  /* xref: entries are exactly 20 bytes each, object 0 is the free head */
  uint32_t xref_at = p->offset;
  uint32_t count = p->next_obj; /* objects 0 .. next_obj-1 */
  outf(p, "xref\n0 %u\n", (unsigned)count);
  outs(p, "0000000000 65535 f \n");
  for (uint32_t n = 1; n < count; n++)
    outf(p, "%010u 00000 n \n",
         (unsigned)((n <= SBX_PDF_MAX_OBJ) ? p->obj_off[n] : 0u));

  outf(p,
       "trailer\n<< /Size %u /Root %u 0 R >>\nstartxref\n%u\n%%%%EOF\n",
       (unsigned)count, (unsigned)OBJ_CATALOG, (unsigned)xref_at);

  return p->ok;
}

/*==================================================================
 * Layout helpers
 *=================================================================*/
static float content_w(void) { return SBX_PDF_PAGE_W - 2.0f * SBX_PDF_MARGIN; }

/** Guarantee `need` points of vertical room, breaking the page if not. */
static void ensure(sbx_pdf_t *p, float need) {
  if (!p->in_page) {
    sbx_pdf_page_begin(p);
    return;
  }
  if (p->y - need < SBX_PDF_BOTTOM) {
    sbx_pdf_page_end(p);
    sbx_pdf_page_begin(p);
    p->y -= 8.0f; /* small top inset on continuation pages */
  }
}

void sbx_pdf_space(sbx_pdf_t *p, float pts) {
  ensure(p, pts);
  p->y -= pts;
}

void sbx_pdf_header_band(sbx_pdf_t *p, const char *title,
                         const char *subtitle) {
  if (!p->in_page)
    sbx_pdf_page_begin(p);
  const float band_h = 86.0f;
  const float top = SBX_PDF_PAGE_H;

  rect(p, 0.0f, top - band_h, SBX_PDF_PAGE_W, band_h, SBX_PDF_BAND);
  /* accent rule closing the band */
  rect(p, 0.0f, top - band_h, SBX_PDF_PAGE_W, 3.0f, SBX_PDF_ACCENT);

  text(p, SBX_PDF_MARGIN, top - 40.0f, 21.0f, true, 0xFFFFFF, title);
  if (subtitle && *subtitle)
    text(p, SBX_PDF_MARGIN, top - 62.0f, 10.0f, false, 0x9FB2CC, subtitle);

  p->y = top - band_h - 26.0f;
  p->row_idx = 0;
}

void sbx_pdf_section(sbx_pdf_t *p, const char *title) {
  ensure(p, 34.0f);
  text(p, SBX_PDF_MARGIN, p->y - 11.0f, 11.0f, true, SBX_PDF_ACCENT, title);
  line(p, SBX_PDF_MARGIN, p->y - 17.0f, SBX_PDF_PAGE_W - SBX_PDF_MARGIN,
       p->y - 17.0f, 0.7f, SBX_PDF_ACCENT);
  p->y -= 28.0f;
  p->row_idx = 0;
}

void sbx_pdf_rule(sbx_pdf_t *p) {
  ensure(p, 10.0f);
  line(p, SBX_PDF_MARGIN, p->y - 4.0f, SBX_PDF_PAGE_W - SBX_PDF_MARGIN, p->y - 4.0f,
       0.5f, 0xC7D2E0);
  p->y -= 10.0f;
}

/* One table row. rgb is the value colour; bold picks the bold face. */
static void row_impl(sbx_pdf_t *p, const char *key, const char *value,
                     uint32_t rgb, bool bold) {
  const float row_h = 19.0f;
  ensure(p, row_h);

  if ((p->row_idx & 1u) == 0u)
    rect(p, SBX_PDF_MARGIN, p->y - row_h + 4.0f, content_w(), row_h,
         SBX_PDF_ZEBRA);

  text(p, SBX_PDF_MARGIN + 8.0f, p->y - row_h + 10.0f, 10.0f, false,
       SBX_PDF_MUTED, key);
  text(p, SBX_PDF_VALUE_X, p->y - row_h + 10.0f, 10.0f, bold, rgb, value);

  p->y -= row_h;
  p->row_idx++;
}

void sbx_pdf_row(sbx_pdf_t *p, const char *key, const char *value) {
  row_impl(p, key, value, SBX_PDF_INK, false);
}

void sbx_pdf_row_c(sbx_pdf_t *p, const char *key, const char *value,
                   uint32_t rgb) {
  row_impl(p, key, value, rgb, true);
}

void sbx_pdf_note(sbx_pdf_t *p, const char *text_) {
  ensure(p, 15.0f);
  text(p, SBX_PDF_MARGIN, p->y - 10.0f, 9.0f, false, SBX_PDF_MUTED, text_);
  p->y -= 14.0f;
}

void sbx_pdf_bar(sbx_pdf_t *p, const char *label, float value, float vmax,
                 uint32_t rgb, const char *value_txt, float mark) {
  const float row_h = 22.0f;
  /* Wide enough for the longest label ("M. tuberculosis (D10 43.0)") at
   * 10 pt Helvetica, so a name never runs under its own bar. */
  const float bar_x = 210.0f;
  const float bar_w = SBX_PDF_PAGE_W - SBX_PDF_MARGIN - 66.0f - bar_x;
  const float bar_h = 9.0f;
  ensure(p, row_h);

  if (vmax <= 0.0f)
    vmax = 1.0f;
  float frac = value / vmax;
  if (frac < 0.0f)
    frac = 0.0f;
  if (frac > 1.0f)
    frac = 1.0f;

  float by = p->y - row_h + 8.0f;

  text(p, SBX_PDF_MARGIN + 8.0f, by + 0.5f, 10.0f, false, SBX_PDF_INK, label);
  rect(p, bar_x, by, bar_w, bar_h, 0xE3E9F2);          /* track */
  rect(p, bar_x, by, bar_w * frac, bar_h, rgb);        /* value */

  /* Target marker (e.g. the 4 log acceptance line) */
  if (mark >= 0.0f && mark <= vmax) {
    float mx = bar_x + bar_w * (mark / vmax);
    line(p, mx, by - 3.0f, mx, by + bar_h + 3.0f, 1.0f, 0x334155);
  }

  if (value_txt && *value_txt)
    text(p, bar_x + bar_w + 10.0f, by + 0.5f, 10.0f, true, rgb, value_txt);

  p->y -= row_h;
}

void sbx_pdf_set_footer(sbx_pdf_t *p, const char *left) {
  snprintf(p->footer, sizeof(p->footer), "%s", left ? left : "");
}
