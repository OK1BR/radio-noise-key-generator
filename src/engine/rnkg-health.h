/* rnkg-health.h — continuous health tests for the noise source.
 *
 * Implements the two approved tests of NIST SP 800-90B §4.4 (Repetition
 * Count, Adaptive Proportion) plus the developer-defined structure tests
 * §4.5 permits on top of them.  The structure tests are what catch an
 * RTL-SDR tuned to a carrier: a modulated signal passes RCT and APT
 * happily, because its byte histogram is flat — it is the *correlation*
 * between neighbouring samples that gives it away.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Both approved tests use this false-positive target: alpha = 2^-20,
 * expressed as -log2(alpha) because that is how both cutoffs consume it. */
#define RNKG_HEALTH_ALPHA_EXP 20.0

typedef enum {
  RNKG_HEALTH_OK = 0,
  RNKG_HEALTH_FAIL_RCT,          /* source stuck on one value (§4.4.1) */
  RNKG_HEALTH_FAIL_APT,          /* one value far too frequent (§4.4.2) */
  RNKG_HEALTH_FAIL_SERIAL,       /* neighbouring samples correlated */
  RNKG_HEALTH_FAIL_IQ,           /* I and Q branches correlated */
} RnkgHealthVerdict;

typedef struct {
  /* Configuration, fixed at init from the assessed min-entropy. */
  double h_assessed;             /* bits of min-entropy per sample */
  guint  rct_cutoff;             /* C of §4.4.1 */
  guint  apt_window;             /* W of §4.4.2: 1024 binary, 512 otherwise */
  guint  apt_cutoff;             /* C of §4.4.2 */

  /* Repetition Count state. */
  gint   rct_value;              /* A; -1 before the first sample */
  guint  rct_run;                /* B */

  /* Adaptive Proportion state. */
  gint   apt_value;              /* A */
  guint  apt_count;              /* B */
  guint  apt_pos;                /* i, within the current window */

  guint64           samples;
  RnkgHealthVerdict verdict;
} RnkgHealth;

/* Cutoffs, exposed so the tests can check them against the worked
 * examples in SP 800-90B (§4.4.1 text and Table 2). */
guint  rnkg_health_rct_cutoff (double h, double alpha_exp);
guint  rnkg_health_apt_cutoff (guint w, double h, double alpha_exp);

void   rnkg_health_init       (RnkgHealth *h,
                               double      h_assessed,
                               gboolean    binary_source);

/* Feed samples.  Returns the verdict; once a test fails the health object
 * latches that verdict and every further call reports it, because a source
 * that has failed a continuous test may not be trusted again without a
 * restart (§4.3). */
RnkgHealthVerdict rnkg_health_push       (RnkgHealth   *h,
                                          guint8        sample);
RnkgHealthVerdict rnkg_health_push_block (RnkgHealth   *h,
                                          const guint8 *buf,
                                          gsize         len);

/* Structure tests (§4.5, developer-defined).  Run over a whole block
 * rather than sample by sample. */
typedef struct {
  double serial;     /* lag-1 Pearson correlation of the byte stream */
  double iq;         /* correlation between even (I) and odd (Q) bytes */
  double dc_bias;    /* mean offset from mid-scale, as a fraction of range */
} RnkgStructure;

/* Thresholds picked well above what thermal noise produces over the block
 * sizes we use, so a pass is meaningful and a fail is not a coin flip. */
#define RNKG_STRUCTURE_MAX_SERIAL 0.05
#define RNKG_STRUCTURE_MAX_IQ     0.05

void              rnkg_structure_analyse (const guint8  *buf,
                                          gsize          len,
                                          RnkgStructure *out);
RnkgHealthVerdict rnkg_structure_verdict (const RnkgStructure *s);

const char *rnkg_health_verdict_message (RnkgHealthVerdict v);

G_END_DECLS
