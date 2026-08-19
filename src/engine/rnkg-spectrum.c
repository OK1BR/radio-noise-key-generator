/* rnkg-spectrum.c — see rnkg-spectrum.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rnkg-spectrum.h"

#include <math.h>

#define N RNKG_SPECTRUM_BINS

/* In-place iterative radix-2 Cooley–Tukey.  The twiddle factor is rotated
 * by recurrence inside each stage; the accumulated error over 1024 points
 * stays around 1e-13, which the gate pins against a naive DFT. */
static void
fft_radix2 (double *re, double *im)
{
  for (guint i = 1, j = 0; i < N; i++)
    {
      guint bit = N >> 1;

      for (; j & bit; bit >>= 1)
        j ^= bit;
      j |= bit;
      if (i < j)
        {
          double t;

          t = re[i]; re[i] = re[j]; re[j] = t;
          t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

  for (guint len = 2; len <= N; len <<= 1)
    {
      const double ang = -2.0 * G_PI / len;
      const double wr  = cos (ang);
      const double wi  = sin (ang);

      for (guint i = 0; i < N; i += len)
        {
          double cr = 1.0, ci = 0.0;

          for (guint k = 0; k < len / 2; k++)
            {
              const guint  a  = i + k;
              const guint  b  = a + len / 2;
              const double tr = re[b] * cr - im[b] * ci;
              const double ti = re[b] * ci + im[b] * cr;
              double       rot;

              re[b] = re[a] - tr;
              im[b] = im[a] - ti;
              re[a] += tr;
              im[a] += ti;

              rot = cr * wr - ci * wi;
              ci  = cr * wi + ci * wr;
              cr  = rot;
            }
        }
    }
}

void
rnkg_spectrum_init (RnkgSpectrum *s)
{
  g_return_if_fail (s != NULL);

  for (guint i = 0; i < N; i++)
    {
      s->psd_db[i]  = -G_MAXDOUBLE;
      s->peak_db[i] = -G_MAXDOUBLE;
    }
  s->has_data = FALSE;
}

void
rnkg_spectrum_update (RnkgSpectrum *s, const guint8 *iq, gsize len)
{
  double power[N] = { 0.0 };
  double window[N];
  double wsum = 0.0;
  gsize  frames;

  g_return_if_fail (s != NULL);
  g_return_if_fail (iq != NULL || len == 0);

  frames = len / (2 * N);
  if (frames == 0)
    return;

  for (guint i = 0; i < N; i++)
    {
      window[i] = 0.5 * (1.0 - cos (2.0 * G_PI * i / N));   /* Hann */
      wsum += window[i];
    }

  /* Subtract the mean of the WHOLE block, not of each frame: the block
   * mean is the receiver's constant DC offset, which would otherwise
   * stand as a spur on the tuned frequency.  Per-frame subtraction
   * would null the centre bin exactly — noise and all — and leave a
   * notch instead; against the block mean each frame keeps its own
   * fluctuation, so the centre shows the same noise as its neighbours.
   * Display hygiene only — the entropy path sees the raw samples,
   * offset included, and MCV charges for it. */
  {
    double mean_re = 0.0, mean_im = 0.0;

    for (gsize n = 0; n < frames * N; n++)
      {
        mean_re += (iq[2 * n]     - 127.5) / 127.5;
        mean_im += (iq[2 * n + 1] - 127.5) / 127.5;
      }
    mean_re /= (double) (frames * N);
    mean_im /= (double) (frames * N);

  for (gsize f = 0; f < frames; f++)
    {
      const guint8 *p = iq + f * 2 * N;
      double re[N], im[N];

      for (guint i = 0; i < N; i++)
        {
          re[i] = ((p[2 * i]     - 127.5) / 127.5 - mean_re) * window[i];
          im[i] = ((p[2 * i + 1] - 127.5) / 127.5 - mean_im) * window[i];
        }

      fft_radix2 (re, im);

      for (guint i = 0; i < N; i++)
        power[i] += re[i] * re[i] + im[i] * im[i];
    }
  }

  /* Normalise so a full-scale complex tone reads 0 dB regardless of the
   * window: such a tone leaves sum(window)^2 in its bin per frame. */
  for (guint i = 0; i < N; i++)
    {
      const guint  shifted = (i + N / 2) % N;   /* DC to the centre */
      const double p       = power[i] / ((double) frames * wsum * wsum);
      const double db      = 10.0 * log10 (p + 1e-20);

      s->psd_db[shifted] = db;
      if (db > s->peak_db[shifted])
        s->peak_db[shifted] = db;
    }
  s->has_data = TRUE;
}

void
rnkg_spectrum_reset_peak (RnkgSpectrum *s)
{
  g_return_if_fail (s != NULL);

  for (guint i = 0; i < N; i++)
    s->peak_db[i] = s->has_data ? s->psd_db[i] : -G_MAXDOUBLE;
}
