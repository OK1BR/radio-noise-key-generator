/* rnkg-estimate.c — see rnkg-estimate.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rnkg-estimate.h"

#include <math.h>

/* Z(1-0.005) from §6.3.1 — the two-sided 99 % confidence bound. */
#define RNKG_MCV_Z 2.576

void
rnkg_estimate_mcv (const guint8 *buf, gsize len, RnkgEstimate *out)
{
  gsize counts[256] = { 0 };
  gsize most = 0;

  g_return_if_fail (out != NULL);
  g_return_if_fail (buf != NULL || len == 0);

  *out = (RnkgEstimate) { 0.0, 1.0, 0.0, 0.0, len, 0 };
  if (len < 2)
    return;

  for (gsize i = 0; i < len; i++)
    counts[buf[i]]++;

  for (guint v = 0; v < 256; v++)
    {
      if (counts[v] == 0)
        continue;

      out->distinct++;
      if (counts[v] > most)
        most = counts[v];

      const double p = (double) counts[v] / len;
      out->h_shannon -= p * log2 (p);
    }

  out->p_hat = (double) most / len;

  /* §6.3.1 step 2: p_u = min(1, p_hat + 2.576 * sqrt(p_hat(1-p_hat)/(L-1))) */
  out->p_upper = out->p_hat
               + RNKG_MCV_Z * sqrt (out->p_hat * (1.0 - out->p_hat)
                                    / (double) (len - 1));
  out->p_upper = MIN (1.0, out->p_upper);

  out->h_min = -log2 (out->p_upper);
}

double
rnkg_estimate_credit (const RnkgEstimate *e)
{
  g_return_val_if_fail (e != NULL, 0.0);

  return e->h_min * (double) e->samples * RNKG_CREDIT_MARGIN;
}
