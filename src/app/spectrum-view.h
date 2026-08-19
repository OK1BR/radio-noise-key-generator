/* spectrum-view.h — the live spectrum, the reason the GUI exists.
 *
 * A GtkDrawingArea that renders one RnkgSpectrum snapshot: live trace,
 * peak hold, dB grid and the tuning marker.  It owns a copy of whatever
 * it was last given, so the caller's buffer can change freely.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <gtk/gtk.h>

#include "rnkg-spectrum.h"

G_BEGIN_DECLS

#define RNKG_TYPE_SPECTRUM_VIEW (rnkg_spectrum_view_get_type ())
G_DECLARE_FINAL_TYPE (RnkgSpectrumView, rnkg_spectrum_view,
                      RNKG, SPECTRUM_VIEW, GtkDrawingArea)

GtkWidget *rnkg_spectrum_view_new        (void);

/* Centre frequency and span, for the axis labels and the marker. */
void       rnkg_spectrum_view_set_tuning (RnkgSpectrumView *self,
                                          double            freq_mhz,
                                          double            rate_msps);

void       rnkg_spectrum_view_update     (RnkgSpectrumView   *self,
                                          const RnkgSpectrum *spectrum);

G_END_DECLS
