/* health_test.c — the cutoffs are checked against the worked examples and
 * Table 2 of NIST SP 800-90B itself, so a refactor that quietly changes the
 * arithmetic cannot pass.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <math.h>

#include "rnkg-health.h"

static void
test_rct_cutoff (void)
{
  /* §4.4.1: "for alpha = 2^-20, an entropy source with H = 2.0 bits per
   * sample would have a repetition count test cutoff value of 1+20/2.0 = 11" */
  g_assert_cmpuint (rnkg_health_rct_cutoff (2.0, 20.0), ==, 11);

  /* Same section, second example: eight bits of min-entropy per sample and
   * "a false-positive rate of approximately once per 10^12 samples" — that
   * rate is alpha = 10^-12, i.e. -log2(alpha) = 39.86, not the 20 above. */
  g_assert_cmpuint (rnkg_health_rct_cutoff (8.0, 39.86), ==, 6);

  /* At the alpha this program actually uses, the same source cuts off at 4. */
  g_assert_cmpuint (rnkg_health_rct_cutoff (8.0, 20.0), ==, 4);

  /* A source claiming no entropy can never trip the test. */
  g_assert_cmpuint (rnkg_health_rct_cutoff (0.0, 20.0), ==, G_MAXUINT);
}

static void
test_apt_cutoff (void)
{
  /* Table 2, non-binary column, W = 512, alpha = 2^-20. */
  g_assert_cmpuint (rnkg_health_apt_cutoff (512, 0.5, 20.0), ==, 410);
  g_assert_cmpuint (rnkg_health_apt_cutoff (512, 1.0, 20.0), ==, 311);
  g_assert_cmpuint (rnkg_health_apt_cutoff (512, 2.0, 20.0), ==, 177);
  g_assert_cmpuint (rnkg_health_apt_cutoff (512, 4.0, 20.0), ==,  62);
  g_assert_cmpuint (rnkg_health_apt_cutoff (512, 8.0, 20.0), ==,  13);

  /* Table 2, binary column, W = 1024. */
  g_assert_cmpuint (rnkg_health_apt_cutoff (1024, 0.2, 20.0), ==, 941);
  g_assert_cmpuint (rnkg_health_apt_cutoff (1024, 0.4, 20.0), ==, 840);
  g_assert_cmpuint (rnkg_health_apt_cutoff (1024, 0.6, 20.0), ==, 748);
  g_assert_cmpuint (rnkg_health_apt_cutoff (1024, 0.8, 20.0), ==, 664);
  g_assert_cmpuint (rnkg_health_apt_cutoff (1024, 1.0, 20.0), ==, 589);
}

/* A dongle that has been unplugged mid-read, or a driver handing back a
 * zeroed buffer, is the failure this test exists for. */
static void
test_rct_catches_stuck_source (void)
{
  RnkgHealth h;

  rnkg_health_init (&h, 4.0, FALSE);
  g_assert_cmpuint (h.rct_cutoff, ==, 6);  /* 1 + ceil(20/4) */

  for (guint i = 0; i < h.rct_cutoff - 1; i++)
    g_assert_cmpint (rnkg_health_push (&h, 0x42), ==, RNKG_HEALTH_OK);

  g_assert_cmpint (rnkg_health_push (&h, 0x42), ==, RNKG_HEALTH_FAIL_RCT);

  /* Latched: a source that failed does not recover by sending good data. */
  g_assert_cmpint (rnkg_health_push (&h, 0x11), ==, RNKG_HEALTH_FAIL_RCT);
}

static void
test_apt_catches_dominant_value (void)
{
  RnkgHealth h;
  RnkgHealthVerdict v = RNKG_HEALTH_OK;

  rnkg_health_init (&h, 4.0, FALSE);

  /* Alternate between two values: neither repeats consecutively, so the RCT
   * stays quiet, but one of them occupies half the window — far above the
   * cutoff of 62 for H = 4. */
  for (guint i = 0; i < 512 && v == RNKG_HEALTH_OK; i++)
    v = rnkg_health_push (&h, (i % 2) ? 0x00 : 0xff);

  g_assert_cmpint (v, ==, RNKG_HEALTH_FAIL_APT);
}

static void
test_health_accepts_random_data (void)
{
  RnkgHealth h;
  GRand     *r = g_rand_new_with_seed (20260819);

  rnkg_health_init (&h, 4.0, FALSE);

  for (guint i = 0; i < 1000000; i++)
    {
      if (rnkg_health_push (&h, (guint8) g_rand_int_range (r, 0, 256))
          != RNKG_HEALTH_OK)
        g_error ("health tests fired on uniform random data at sample %u", i);
    }

  g_rand_free (r);
}

/* The point of the structure tests, and why the two approved tests are not
 * enough on their own: this stream passes RCT and APT — no value repeats,
 * no value dominates a window — yet its samples are half-correlated with
 * their predecessor.  That is what band-limited noise looks like, which is
 * what you get when the dongle is pointed at a signal rather than at an
 * empty channel.  Only the correlation test sees it. */
static void
test_structure_catches_correlated_noise (void)
{
  const gsize    n = 262144;
  guint8        *buf = g_malloc (n);
  GRand         *r = g_rand_new_with_seed (4242);
  RnkgHealth     h;
  RnkgStructure  s;
  gint           prev = g_rand_int_range (r, 0, 256);

  for (gsize i = 0; i < n; i++)
    {
      const gint cur = g_rand_int_range (r, 0, 256);

      buf[i] = (guint8) ((cur + prev) / 2);   /* lag-1 correlation ~0.5 */
      prev = cur;
    }

  rnkg_health_init (&h, 4.0, FALSE);
  g_assert_cmpint (rnkg_health_push_block (&h, buf, n), ==, RNKG_HEALTH_OK);

  rnkg_structure_analyse (buf, n, &s);
  g_assert_cmpint (rnkg_structure_verdict (&s), ==, RNKG_HEALTH_FAIL_SERIAL);

  g_rand_free (r);
  g_free (buf);
}

/* A plain unmodulated carrier is the easy case — it is so degenerate that
 * the approved tests catch it too.  Checked so the easy case cannot regress
 * while attention is on the hard one above. */
static void
test_structure_catches_a_carrier (void)
{
  const gsize    n = 65536;
  guint8        *buf = g_malloc (n);
  RnkgHealth     h;
  RnkgStructure  s;

  for (gsize i = 0; i < n; i++)
    buf[i] = (guint8) (127.5 + 100.0 * sin (2.0 * G_PI * i / 16.0));

  rnkg_health_init (&h, 4.0, FALSE);
  g_assert_cmpint (rnkg_health_push_block (&h, buf, n), !=, RNKG_HEALTH_OK);

  rnkg_structure_analyse (buf, n, &s);
  g_assert_cmpint (rnkg_structure_verdict (&s), !=, RNKG_HEALTH_OK);

  g_free (buf);
}

static void
test_structure_passes_noise (void)
{
  const gsize    n = 262144;
  guint8        *buf = g_malloc (n);
  GRand         *r = g_rand_new_with_seed (7);
  RnkgStructure  s;

  for (gsize i = 0; i < n; i++)
    buf[i] = (guint8) g_rand_int_range (r, 0, 256);

  rnkg_structure_analyse (buf, n, &s);
  g_assert_cmpint (rnkg_structure_verdict (&s), ==, RNKG_HEALTH_OK);
  g_assert_cmpfloat (fabs (s.dc_bias), <, 0.01);

  g_rand_free (r);
  g_free (buf);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/health/rct-cutoff", test_rct_cutoff);
  g_test_add_func ("/health/apt-cutoff", test_apt_cutoff);
  g_test_add_func ("/health/rct-stuck", test_rct_catches_stuck_source);
  g_test_add_func ("/health/apt-dominant", test_apt_catches_dominant_value);
  g_test_add_func ("/health/accepts-random", test_health_accepts_random_data);
  g_test_add_func ("/health/structure-correlated", test_structure_catches_correlated_noise);
  g_test_add_func ("/health/structure-carrier", test_structure_catches_a_carrier);
  g_test_add_func ("/health/structure-noise", test_structure_passes_noise);

  return g_test_run ();
}
