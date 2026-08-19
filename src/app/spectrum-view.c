/* spectrum-view.c — see spectrum-view.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spectrum-view.h"

/* Fixed dB range of the plot.  Empty-channel noise on an 8-bit dongle sits
 * around -70 to -80 dB against a full-scale tone, so this keeps both the
 * noise floor and any carrier on screen without autoscaling jumps. */
#define DB_TOP    0.0
#define DB_BOTTOM -110.0

struct _RnkgSpectrumView {
  GtkDrawingArea parent_instance;

  RnkgSpectrum spectrum;
  double       freq_mhz;
  double       rate_msps;
};

G_DEFINE_FINAL_TYPE (RnkgSpectrumView, rnkg_spectrum_view, GTK_TYPE_DRAWING_AREA)

static double
db_to_y (double db, int height)
{
  const double t = (DB_TOP - db) / (DB_TOP - DB_BOTTOM);

  return CLAMP (t, 0.0, 1.0) * height;
}

static void
draw_trace (cairo_t *cr, const double *db, int width, int height)
{
  for (guint i = 0; i < RNKG_SPECTRUM_BINS; i++)
    {
      const double x = (double) i / (RNKG_SPECTRUM_BINS - 1) * width;
      const double y = db_to_y (db[i], height);

      if (i == 0)
        cairo_move_to (cr, x, y);
      else
        cairo_line_to (cr, x, y);
    }
  cairo_stroke (cr);
}

static void
draw_label (cairo_t *cr, double x, double y, const char *text)
{
  cairo_text_extents_t ext;

  cairo_text_extents (cr, text, &ext);
  cairo_move_to (cr, CLAMP (x - ext.width / 2.0, 2.0, G_MAXDOUBLE), y);
  cairo_show_text (cr, text);
}

static void
draw_func (GtkDrawingArea *area, cairo_t *cr, int width, int height,
           gpointer user_data)
{
  RnkgSpectrumView *self = RNKG_SPECTRUM_VIEW (area);
  g_autofree char *label = NULL;

  (void) user_data;

  /* Family style: pure black, white accents. */
  cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
  cairo_paint (cr);

  cairo_select_font_face (cr, "monospace",
                          CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, 11.0);

  /* dB grid. */
  cairo_set_line_width (cr, 1.0);
  for (double db = DB_TOP; db >= DB_BOTTOM; db -= 20.0)
    {
      const double y = db_to_y (db, height);

      cairo_set_source_rgb (cr, 0.18, 0.18, 0.18);
      cairo_move_to (cr, 0, y);
      cairo_line_to (cr, width, y);
      cairo_stroke (cr);

      cairo_set_source_rgb (cr, 0.45, 0.45, 0.45);
      label = g_strdup_printf ("%.0f", db);
      cairo_move_to (cr, 4, y - 3);
      cairo_show_text (cr, label);
      g_clear_pointer (&label, g_free);
    }

  /* Tuning marker: the centre of the passband, where the tuner sits. */
  cairo_set_source_rgb (cr, 0.55, 0.55, 0.55);
  {
    static const double dashes[] = { 4.0, 4.0 };

    cairo_save (cr);
    cairo_set_dash (cr, dashes, G_N_ELEMENTS (dashes), 0.0);
    cairo_move_to (cr, width / 2.0, 0);
    cairo_line_to (cr, width / 2.0, height);
    cairo_stroke (cr);
    cairo_restore (cr);
  }

  /* Frequency labels: edges and centre. */
  cairo_set_source_rgb (cr, 0.7, 0.7, 0.7);
  label = g_strdup_printf ("%.3f MHz", self->freq_mhz);
  draw_label (cr, width / 2.0, 14, label);
  g_clear_pointer (&label, g_free);

  /* The receiver's own spur sits exactly here; name it so the one peak
   * every RTL-SDR shows at the centre does not read as a signal. */
  cairo_set_source_rgb (cr, 0.45, 0.45, 0.45);
  cairo_move_to (cr, width / 2.0 + 6, 28);
  cairo_show_text (cr, "DC");

  label = g_strdup_printf ("%.3f", self->freq_mhz - self->rate_msps / 2.0);
  cairo_move_to (cr, 2, height - 4);
  cairo_show_text (cr, label);
  g_clear_pointer (&label, g_free);

  label = g_strdup_printf ("%.3f", self->freq_mhz + self->rate_msps / 2.0);
  {
    cairo_text_extents_t ext;

    cairo_text_extents (cr, label, &ext);
    cairo_move_to (cr, width - ext.width - 2, height - 4);
    cairo_show_text (cr, label);
  }
  g_clear_pointer (&label, g_free);

  if (!self->spectrum.has_data)
    return;

  /* Peak hold first, dim, so the live trace paints over it. */
  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgb (cr, 0.35, 0.35, 0.35);
  draw_trace (cr, self->spectrum.peak_db, width, height);

  cairo_set_line_width (cr, 1.5);
  cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
  draw_trace (cr, self->spectrum.psd_db, width, height);
}

static void
rnkg_spectrum_view_class_init (RnkgSpectrumViewClass *klass)
{
  (void) klass;
}

static void
rnkg_spectrum_view_init (RnkgSpectrumView *self)
{
  rnkg_spectrum_init (&self->spectrum);
  self->freq_mhz  = 0.0;
  self->rate_msps = 0.0;

  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self),
                                  draw_func, NULL, NULL);
}

GtkWidget *
rnkg_spectrum_view_new (void)
{
  return g_object_new (RNKG_TYPE_SPECTRUM_VIEW, NULL);
}

void
rnkg_spectrum_view_set_tuning (RnkgSpectrumView *self,
                               double freq_mhz, double rate_msps)
{
  g_return_if_fail (RNKG_IS_SPECTRUM_VIEW (self));

  self->freq_mhz  = freq_mhz;
  self->rate_msps = rate_msps;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
rnkg_spectrum_view_update (RnkgSpectrumView *self, const RnkgSpectrum *spectrum)
{
  g_return_if_fail (RNKG_IS_SPECTRUM_VIEW (self));
  g_return_if_fail (spectrum != NULL);

  self->spectrum = *spectrum;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
