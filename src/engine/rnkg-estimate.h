/* rnkg-estimate.h — min-entropy estimation for the noise source.
 *
 * Min-entropy, not Shannon.  Shannon entropy answers "how well does this
 * compress"; min-entropy answers "how well can an attacker guess the most
 * likely sample", which is the only one of the two that bounds what a key
 * derived from this data is worth.  For an 8-bit RTL-SDR stream with a DC
 * offset the two differ substantially, and Shannon flatters the source.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  double p_hat;      /* observed proportion of the most common value */
  double p_upper;    /* upper bound of its 99 % confidence interval */
  double h_min;      /* -log2(p_upper): bits of min-entropy per sample */
  double h_shannon;  /* reported for contrast only; never used for credit */
  gsize  samples;
  guint  distinct;   /* how many of the 256 values ever occurred */
} RnkgEstimate;

/* Most Common Value estimate, SP 800-90B §6.3.1.  The spec's worked example
 * (L = 20, p_hat = 0.4) yields p_upper = 0.6895; the unit test pins it. */
void rnkg_estimate_mcv (const guint8 *buf, gsize len, RnkgEstimate *out);

/* Bits of entropy this block may be credited with.  The MCV estimate is per
 * sample, so the block total is h_min * samples — but we hand back only a
 * fraction of it.  The margin is deliberate: MCV is an i.i.d. estimator and
 * an RTL-SDR stream is not i.i.d. (USB framing, DC offset, tuner spurs all
 * leave structure), so the true min-entropy sits below what MCV reports. */
#define RNKG_CREDIT_MARGIN 0.5

double rnkg_estimate_credit (const RnkgEstimate *e);

G_END_DECLS
