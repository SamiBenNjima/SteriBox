/**
 * @file sbx_pdf.h
 * SteriBox - minimal styled PDF writer (portable C, no dependencies).
 *
 * Produces a real PDF 1.4 file: A4 portrait, uncompressed content streams,
 * the two standard Helvetica faces (never embedded, so no font data is
 * needed on the device). Everything is emitted through a caller-supplied
 * sink so the document is streamed straight to an SD file or over the UART
 * tunnel to a USB flash drive.
 *
 * NOTHING is buffered - not even the current page. Each content stream
 * declares its size through an indirect /Length object written after the
 * stream, which is what lets a page of any size be emitted in one forward
 * pass. A page can therefore never silently lose content.
 *
 * Typical use:
 *   static sbx_pdf_t pdf;
 *   sbx_pdf_begin(&pdf, my_sink, ctx);
 *   sbx_pdf_page_begin(&pdf);
 *   sbx_pdf_header_band(&pdf, "TITLE", "subtitle");
 *   sbx_pdf_section(&pdf, "CYCLE");
 *   sbx_pdf_row(&pdf, "Duree", "12 min 30 s");
 *   sbx_pdf_page_end(&pdf);
 *   ok = sbx_pdf_end(&pdf);
 *
 * The row / bar / note helpers break to a new page automatically, so the
 * caller never has to track the vertical cursor.
 */
#ifndef SBX_PDF_H
#define SBX_PDF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*------------------------------------------------------------------
 * Limits
 *-----------------------------------------------------------------*/
#define SBX_PDF_MAX_PAGES 24
/* per page: the /Page, its content stream, and the stream's /Length */
#define SBX_PDF_MAX_OBJ (4 + 3 * SBX_PDF_MAX_PAGES)

/*------------------------------------------------------------------
 * Page geometry (PostScript points, A4 portrait)
 *-----------------------------------------------------------------*/
#define SBX_PDF_PAGE_W 595.0f
#define SBX_PDF_PAGE_H 842.0f
#define SBX_PDF_MARGIN 42.0f
#define SBX_PDF_VALUE_X 250.0f /* left edge of the value column */
#define SBX_PDF_BOTTOM 64.0f   /* below this a new page is started */

/*------------------------------------------------------------------
 * House colours (0xRRGGBB) - kept in sync with the LVGL screens
 *-----------------------------------------------------------------*/
#define SBX_PDF_INK 0x101820     /* body text            */
#define SBX_PDF_MUTED 0x5A6779   /* captions, footer     */
#define SBX_PDF_ACCENT 0x00A5D0  /* section rules, band  */
#define SBX_PDF_BAND 0x18202E    /* header band          */
#define SBX_PDF_ZEBRA 0xF1F5FA   /* alternating row fill */
#define SBX_PDF_OK 0x11A05F      /* pass                 */
#define SBX_PDF_WARN 0xD07000    /* marginal             */
#define SBX_PDF_FAIL 0xC02030    /* fail / aborted       */

/** Sink: return false to abort the document. Called with the raw bytes
 *  to append, in order, exactly once each. */
typedef bool (*sbx_pdf_sink_fn)(void *ctx, const void *data, uint32_t len);

typedef struct {
  sbx_pdf_sink_fn sink;
  void *ctx;

  uint32_t offset;                       /* bytes handed to the sink so far */
  uint32_t obj_off[SBX_PDF_MAX_OBJ + 1]; /* file offset of each object      */
  uint16_t next_obj;                     /* next free object number         */
  uint16_t page_obj[SBX_PDF_MAX_PAGES];  /* object number of each /Page     */
  uint8_t page_count;

  uint16_t cont_obj;    /* content stream object of the open page  */
  uint16_t len_obj;     /* its indirect /Length object             */
  uint32_t stream_start;/* file offset where the stream data began */

  char footer[96]; /* stamped on every page as it is closed */

  float y;         /* drawing cursor, points from the page bottom */
  uint8_t row_idx; /* drives the zebra striping                   */
  bool in_page;
  bool ok;
} sbx_pdf_t;

/*------------------------------------------------------------------
 * Document
 *-----------------------------------------------------------------*/
/** Reset the writer and emit the file header. */
void sbx_pdf_begin(sbx_pdf_t *p, sbx_pdf_sink_fn sink, void *ctx);

/** Finish the document: /Pages, /Catalog, xref table and trailer.
 *  Returns true when every byte reached the sink and the document is
 *  structurally complete. Closes the open page if the caller forgot. */
bool sbx_pdf_end(sbx_pdf_t *p);

/*------------------------------------------------------------------
 * Pages
 *-----------------------------------------------------------------*/
void sbx_pdf_page_begin(sbx_pdf_t *p);
void sbx_pdf_page_end(sbx_pdf_t *p);

/** Page number of the page being drawn (1-based). */
static inline int sbx_pdf_page_no(const sbx_pdf_t *p) {
  return (int)p->page_count;
}

/*------------------------------------------------------------------
 * Content helpers - all break to a new page when they run out of room
 *-----------------------------------------------------------------*/
/** Full-width dark band with a title and a subtitle. Only meaningful as
 *  the first element of a page; resets the cursor underneath it. */
void sbx_pdf_header_band(sbx_pdf_t *p, const char *title, const char *subtitle);

/** Accent-coloured section heading with an underline rule. */
void sbx_pdf_section(sbx_pdf_t *p, const char *title);

/** Zebra-striped "caption .... value" row. */
void sbx_pdf_row(sbx_pdf_t *p, const char *key, const char *value);

/** Same, with the value in bold and a chosen colour (e.g. a verdict). */
void sbx_pdf_row_c(sbx_pdf_t *p, const char *key, const char *value,
                   uint32_t rgb);

/** Free-standing paragraph in the muted colour (no striping). */
void sbx_pdf_note(sbx_pdf_t *p, const char *text);

/** Horizontal bar: label, coloured bar scaled to vmax, value text.
 *  Pass mark >= 0 to draw a dashed target line at that value. */
void sbx_pdf_bar(sbx_pdf_t *p, const char *label, float value, float vmax,
                 uint32_t rgb, const char *value_txt, float mark);

/** Thin horizontal rule across the content width. */
void sbx_pdf_rule(sbx_pdf_t *p);

/** Vertical gap, in points. */
void sbx_pdf_space(sbx_pdf_t *p, float pts);

/** Footer text stamped at the bottom of every page - including pages the
 *  layout helpers create on their own - together with "Page N". Set it
 *  once, before the first page. */
void sbx_pdf_set_footer(sbx_pdf_t *p, const char *left);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SBX_PDF_H*/
