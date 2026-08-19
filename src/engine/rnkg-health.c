/* rnkg-health.c — see rnkg-health.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rnkg-health.h"

#include <math.h>

/* §4.4.1: C = 1 + ceil(-log2(alpha) / H), the smallest integer satisfying
 * alpha >= 2^(-H(C-1)).  Worked example in the spec: alpha = 2^-20 and
 * H = 2.0 give C = 11. */
guint
rnkg_health_rct_cutoff (double h, double alpha_exp)
{
  if (h <= 0.0)
    return G_MAXUINT;  /* no entropy claimed — the test cannot fire */

  return 1 + (guint) ceil (alpha_exp / h);
}

/* Binomial pmf in log space.  Computed via lgamma rather than a product
 * recurrence because for H = 1 the p = 0.5, W = 512 case underflows pmf(0)
 * to zero, and a recurrence seeded with zero stays zero forever. */
static double
log_binom_pmf (guint w, guint k, double p)
{
  const double lc = lgamma (w + 1.0) - lgamma (k + 1.0) - lgamma (w - k + 1.0);

  return lc + k * log (p) + (w - k) * log1p (-p);
}

/* §4.4.2: C is the smallest value with Pr(B >= C) <= alpha, which the spec
 * footnote spells as C = 1 + CRITBINOM(W, 2^-H, 1-alpha).  CRITBINOM is the
 * smallest k whose cumulative probability reaches 1-alpha, so we accumulate
 * the cdf until it crosses that point.  Checked against Table 2. */
guint
rnkg_health_apt_cutoff (guint w, double h, double alpha_exp)
{
  const double alpha  = pow (2.0, -alpha_exp);
  const double p      = pow (2.0, -h);
  const double target = 1.0 - alpha;
  double cdf = 0.0;

  if (h <= 0.0)
    return G_MAXUINT;

  for (guint k = 0; k <= w; k++)
    {
      cdf += exp (log_binom_pmf (w, k, p));
      if (cdf >= target)
        return k + 1;
    }

  return w + 1;  /* unreachable in practice; cdf sums to 1 */
}

void
rnkg_health_init (RnkgHealth *h, double h_assessed, gboolean binary_source)
{
  g_return_if_fail (h != NULL);

  *h = (RnkgHealth) {
    .h_assessed = h_assessed,
    .rct_cutoff = rnkg_health_rct_cutoff (h_assessed, RNKG_HEALTH_ALPHA_EXP),
    .apt_window = binary_source ? 1024 : 512,
    .rct_value  = -1,
    .apt_value  = -1,
    .verdict    = RNKG_HEALTH_OK,
  };

  h->apt_cutoff = rnkg_health_apt_cutoff (h->apt_window, h_assessed,
                                          RNKG_HEALTH_ALPHA_EXP);
}

RnkgHealthVerdict
rnkg_health_push (RnkgHealth *h, guint8 sample)
{
  g_return_val_if_fail (h != NULL, RNKG_HEALTH_FAIL_RCT);

  if (h->verdict != RNKG_HEALTH_OK)
    return h->verdict;  /* latched: a failed source stays failed */

  h->samples++;

  /* Repetition Count (§4.4.1). */
  if ((gint) sample == h->rct_value)
    {
      h->rct_run++;
      if (h->rct_run >= h->rct_cutoff)
        return (h->verdict = RNKG_HEALTH_FAIL_RCT);
    }
  else
    {
      h->rct_value = sample;
      h->rct_run   = 1;
    }

  /* Adaptive Proportion (§4.4.2).  The first sample of each window becomes
   * the value being counted; the remaining W-1 are compared against it. */
  if (h->apt_value < 0)
    {
      h->apt_value = sample;
      h->apt_count = 1;
      h->apt_pos   = 0;
    }
  else
    {
      h->apt_pos++;
      if ((gint) sample == h->apt_value)
        {
          h->apt_count++;
          if (h->apt_count >= h->apt_cutoff)
            return (h->verdict = RNKG_HEALTH_FAIL_APT);
        }
      if (h->apt_pos >= h->apt_window - 1)
        h->apt_value = -1;  /* window done, re-arm on the next sample */
    }

  return RNKG_HEALTH_OK;
}

RnkgHealthVerdict
rnkg_health_push_block (RnkgHealth *h, const guint8 *buf, gsize len)
{
  g_return_val_if_fail (h != NULL, RNKG_HEALTH_FAIL_RCT);
  g_return_val_if_fail (buf != NULL || len == 0, RNKG_HEALTH_FAIL_RCT);

  for (gsize i = 0; i < len; i++)
    {
      if (rnkg_health_push (h, buf[i]) != RNKG_HEALTH_OK)
        return h->verdict;
    }

  return RNKG_HEALTH_OK;
}

/* Pearson correlation between two strided views of the same buffer. */
static double
correlate (const guint8 *a, const guint8 *b, gsize n, gsize stride)
{
  double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;

  if (n < 2)
    return 0.0;

  for (gsize i = 0; i < n; i++)
    {
      const double x = a[i * stride];
      const double y = b[i * stride];

      sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
    }

  const double cov = sab / n - (sa / n) * (sb / n);
  const double va  = saa / n - (sa / n) * (sa / n);
  const double vb  = sbb / n - (sb / n) * (sb / n);

  if (va <= 0.0 || vb <= 0.0)
    return 1.0;  /* a constant branch is maximally "correlated" — a failure */

  return cov / sqrt (va * vb);
}

void
rnkg_structure_analyse (const guint8 *buf, gsize len, RnkgStructure *out)
{
  double sum = 0.0;

  g_return_if_fail (out != NULL);
  g_return_if_fail (buf != NULL || len == 0);

  *out = (RnkgStructure) { 0.0, 0.0, 0.0 };
  if (len < 4)
    return;

  /* Lag-1: a carrier makes consecutive samples ride the same waveform. */
  out->serial = correlate (buf, buf + 1, len - 1, 1);

  /* I against Q: the dongle interleaves them, so they are the even and odd
   * bytes.  Correlated branches mean the tuner is seeing structure, or the
   * ADC is coupling the two. */
  out->iq = correlate (buf, buf + 1, len / 2, 2);

  for (gsize i = 0; i < len; i++)
    sum += buf[i];
  out->dc_bias = (sum / len - 127.5) / 255.0;
}

RnkgHealthVerdict
rnkg_structure_verdict (const RnkgStructure *s)
{
  g_return_val_if_fail (s != NULL, RNKG_HEALTH_FAIL_SERIAL);

  if (fabs (s->serial) > RNKG_STRUCTURE_MAX_SERIAL)
    return RNKG_HEALTH_FAIL_SERIAL;
  if (fabs (s->iq) > RNKG_STRUCTURE_MAX_IQ)
    return RNKG_HEALTH_FAIL_IQ;

  return RNKG_HEALTH_OK;
}

const char *
rnkg_health_verdict_message (RnkgHealthVerdict v)
{
  switch (v)
    {
    case RNKG_HEALTH_OK:
      return "ok";
    case RNKG_HEALTH_FAIL_RCT:
      return "repetition count test failed — the source is stuck on one value "
             "(dongle unplugged, or the driver is handing back a constant)";
    case RNKG_HEALTH_FAIL_APT:
      return "adaptive proportion test failed — one sample value dominates "
             "(gain far too low, or the input is saturated)";
    case RNKG_HEALTH_FAIL_SERIAL:
      return "consecutive samples are correlated — this looks like a signal, "
             "not noise; retune to an empty frequency";
    case RNKG_HEALTH_FAIL_IQ:
      return "the I and Q branches are correlated — this looks like a signal, "
             "not noise; retune to an empty frequency";
    default:
      return "unknown verdict";
    }
}
