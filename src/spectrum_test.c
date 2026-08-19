/* spectrum_test.c — the display FFT: correctness against a naive DFT and
 * the properties the spectrum view relies on.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <math.h>

#include "rnkg-spectrum.h"

#define N RNKG_SPECTRUM_BINS

/* One synthetic I/Q frame carrying a full-scale complex tone at `bin`
 * cycles per frame (negative for the lower half of the passband). */
static void
fill_tone (guint8 *iq, gsize frames, gint bin, double amplitude)
{
  for (gsize n = 0; n < frames * N; n++)
    {
      const double ph = 2.0 * G_PI * bin * (double) n / N;

      iq[2 * n]     = (guint8) lround (127.5 + amplitude * cos (ph));
      iq[2 * n + 1] = (guint8) lround (127.5 + amplitude * sin (ph));
    }
}

/* The radix-2 must agree with the O(N^2) definition of the DFT.  Feed one
 * frame of seeded GRand noise through rnkg_spectrum_update() and rebuild
 * the same periodogram by hand. */
static void
test_fft_matches_dft (void)
{
  GRand *rand = g_rand_new_with_seed (0xff7);
  guint8 iq[2 * N];
  double re[N], im[N], window[N], wsum = 0.0;
  RnkgSpectrum s;

  for (guint i = 0; i < 2 * N; i++)
    iq[i] = (guint8) g_rand_int_range (rand, 0, 256);
  g_rand_free (rand);

  rnkg_spectrum_init (&s);
  rnkg_spectrum_update (&s, iq, sizeof iq);
  g_assert_true (s.has_data);

  for (guint i = 0; i < N; i++)
    {
      window[i] = 0.5 * (1.0 - cos (2.0 * G_PI * i / N));
      wsum += window[i];
    }

  for (guint k = 0; k < N; k++)
    {
      double sr = 0.0, si = 0.0;

      for (guint n = 0; n < N; n++)
        {
          const double xr = (iq[2 * n]     - 127.5) / 127.5 * window[n];
          const double xi = (iq[2 * n + 1] - 127.5) / 127.5 * window[n];
          const double c  = cos (-2.0 * G_PI * k * (double) n / N);
          const double sn = sin (-2.0 * G_PI * k * (double) n / N);

          sr += xr * c - xi * sn;
          si += xr * sn + xi * c;
        }

      re[k] = sr;
      im[k] = si;
    }

  for (guint k = 0; k < N; k++)
    {
      const double p  = (re[k] * re[k] + im[k] * im[k]) / (wsum * wsum);
      const double db = 10.0 * log10 (p + 1e-20);

      g_assert_cmpfloat (fabs (db - s.psd_db[(k + N / 2) % N]), <, 1e-6);
    }
}

/* A full-scale tone at +100 cycles/frame must land in centre+100, read
 * about 0 dB there, and tower over everything outside its leakage skirt. */
static void
test_tone_lands_in_bin (void)
{
  g_autofree guint8 *iq = g_malloc (8 * 2 * N);
  RnkgSpectrum s;
  guint peak_at = 0;

  fill_tone (iq, 8, 100, 127.0);
  rnkg_spectrum_init (&s);
  rnkg_spectrum_update (&s, iq, 8 * 2 * N);

  for (guint i = 1; i < N; i++)
    if (s.psd_db[i] > s.psd_db[peak_at])
      peak_at = i;

  g_assert_cmpuint (peak_at, ==, N / 2 + 100);
  g_assert_cmpfloat (s.psd_db[peak_at], >, -1.0);
  g_assert_cmpfloat (s.psd_db[peak_at], <, 1.0);

  /* 30 dB, not more: quantising the tone to 8 bits leaves deterministic
   * harmonics a few tens of dB down, and they are real, not FFT error. */
  for (guint i = 0; i < N; i++)
    if (i + 8 < peak_at || i > peak_at + 8)
      g_assert_cmpfloat (s.psd_db[i], <, s.psd_db[peak_at] - 30.0);
}

/* Pure DC (constant bytes) is the RTL2832U's centre spur: it must show in
 * the middle bin and nowhere else. */
static void
test_dc_is_centred (void)
{
  g_autofree guint8 *iq = g_malloc (2 * 2 * N);
  RnkgSpectrum s;

  for (guint i = 0; i < 2 * 2 * N; i++)
    iq[i] = 200;
  rnkg_spectrum_init (&s);
  rnkg_spectrum_update (&s, iq, 2 * 2 * N);

  /* The Hann window leaks into the two neighbouring bins (about -6 dB);
   * everything beyond them must be far down. */
  for (guint i = 0; i < N; i++)
    if (i + 1 < N / 2 || i > N / 2 + 1)
      g_assert_cmpfloat (s.psd_db[i], <, s.psd_db[N / 2] - 40.0);
}

/* Peak hold keeps the maximum across updates and forgets it on reset. */
static void
test_peak_hold (void)
{
  g_autofree guint8 *iq = g_malloc (2 * 2 * N);
  RnkgSpectrum s;

  rnkg_spectrum_init (&s);

  fill_tone (iq, 2, 100, 127.0);
  rnkg_spectrum_update (&s, iq, 2 * 2 * N);
  fill_tone (iq, 2, -200, 127.0);
  rnkg_spectrum_update (&s, iq, 2 * 2 * N);

  /* The old tone is gone from the live trace but held in the peak. */
  g_assert_cmpfloat (s.psd_db[N / 2 + 100], <, -40.0);
  g_assert_cmpfloat (s.peak_db[N / 2 + 100], >, -1.0);
  g_assert_cmpfloat (s.peak_db[N / 2 - 200], >, -1.0);

  rnkg_spectrum_reset_peak (&s);
  g_assert_cmpfloat (s.peak_db[N / 2 + 100], <, -40.0);
  g_assert_cmpfloat (s.peak_db[N / 2 - 200], >, -1.0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/spectrum/fft-matches-dft", test_fft_matches_dft);
  g_test_add_func ("/spectrum/tone-lands-in-bin", test_tone_lands_in_bin);
  g_test_add_func ("/spectrum/dc-is-centred", test_dc_is_centred);
  g_test_add_func ("/spectrum/peak-hold", test_peak_hold);

  return g_test_run ();
}
