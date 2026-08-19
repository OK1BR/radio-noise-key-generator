/* rnkg-spectrum.h — averaged power spectrum of a sample block, for display.
 *
 * This is the visual proof the GUI exists for: that the passband is an
 * empty channel and not a carrier.  It is display-only — nothing computed
 * here ever touches the entropy path, the health verdicts or the credit.
 * The FFT is a plain radix-2 written out here so the program keeps zero
 * DSP dependencies; 1024 points a few times a second costs nothing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Display resolution.  Must stay a power of two (radix-2). */
#define RNKG_SPECTRUM_BINS 1024

typedef struct {
  /* Welch-averaged periodogram over the pushed block, Hann windowed,
   * fftshifted so the centre frequency sits at bin RNKG_SPECTRUM_BINS/2.
   * dB relative to a full-scale tone; empty-channel noise sits far down. */
  double psd_db[RNKG_SPECTRUM_BINS];

  /* Peak hold across pushes, same scale, until _reset_peak(). */
  double peak_db[RNKG_SPECTRUM_BINS];

  gboolean has_data;
} RnkgSpectrum;

void rnkg_spectrum_init       (RnkgSpectrum *s);

/* Digest one block of raw interleaved I/Q bytes (as read from the source)
 * into psd_db, and fold it into peak_db.  `len` is in bytes; anything
 * short of one full FFT frame (2 * RNKG_SPECTRUM_BINS bytes) is ignored. */
void rnkg_spectrum_update     (RnkgSpectrum *s, const guint8 *iq, gsize len);

void rnkg_spectrum_reset_peak (RnkgSpectrum *s);

G_END_DECLS
