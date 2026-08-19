/* estimate_test.c — MCV against the worked example of SP 800-90B §6.3.1.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <math.h>

#include "rnkg-estimate.h"

static void
test_mcv_spec_example (void)
{
  /* §6.3.1: S = (0,1,1,2,0,1,2,2,0,1,0,1,1,0,2,2,1,0,2,1), L = 20.
   * The most common value is 1 with p_hat = 0.4, giving p_u = 0.6895. */
  static const guint8 s[] = { 0,1,1,2,0,1,2,2,0,1,0,1,1,0,2,2,1,0,2,1 };
  RnkgEstimate e;

  rnkg_estimate_mcv (s, G_N_ELEMENTS (s), &e);

  g_assert_cmpfloat (fabs (e.p_hat - 0.4), <, 1e-12);
  g_assert_cmpfloat (fabs (e.p_upper - 0.6895), <, 5e-5);
  g_assert_cmpfloat (fabs (e.h_min - (-log2 (0.6895))), <, 1e-4);
  g_assert_cmpuint (e.distinct, ==, 3);
}

/* Uniform bytes should land near 8 bits per sample, and the confidence
 * bound means the estimate approaches it from below — never above. */
static void
test_mcv_uniform (void)
{
  const gsize   n = 1u << 20;
  guint8       *buf = g_malloc (n);
  GRand        *r = g_rand_new_with_seed (1312);
  RnkgEstimate  e;

  for (gsize i = 0; i < n; i++)
    buf[i] = (guint8) g_rand_int_range (r, 0, 256);

  rnkg_estimate_mcv (buf, n, &e);

  g_assert_cmpfloat (e.h_min, >, 7.5);
  g_assert_cmpfloat (e.h_min, <=, 8.0);
  g_assert_cmpuint (e.distinct, ==, 256);

  g_rand_free (r);
  g_free (buf);
}

/* A stream stuck on one value must be credited with nothing. */
static void
test_mcv_constant (void)
{
  guint8       *buf = g_malloc0 (4096);
  RnkgEstimate  e;

  rnkg_estimate_mcv (buf, 4096, &e);

  g_assert_cmpfloat (e.p_hat, ==, 1.0);
  g_assert_cmpfloat (e.h_min, ==, 0.0);
  g_assert_cmpfloat (rnkg_estimate_credit (&e), ==, 0.0);

  g_free (buf);
}

/* Min-entropy must never exceed Shannon — if it does, the estimator is
 * wrong in the dangerous direction. */
static void
test_mcv_below_shannon (void)
{
  const gsize   n = 65536;
  guint8       *buf = g_malloc (n);
  GRand        *r = g_rand_new_with_seed (99);
  RnkgEstimate  e;

  /* Skewed: a DC-offset-like distribution clustered around mid-scale. */
  for (gsize i = 0; i < n; i++)
    buf[i] = (guint8) CLAMP (128 + g_rand_int_range (r, -40, 41), 0, 255);

  rnkg_estimate_mcv (buf, n, &e);

  g_assert_cmpfloat (e.h_min, <, e.h_shannon);
  g_assert_cmpfloat (e.h_min, >, 0.0);

  g_rand_free (r);
  g_free (buf);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/estimate/spec-example", test_mcv_spec_example);
  g_test_add_func ("/estimate/uniform", test_mcv_uniform);
  g_test_add_func ("/estimate/constant", test_mcv_constant);
  g_test_add_func ("/estimate/below-shannon", test_mcv_below_shannon);

  return g_test_run ();
}
