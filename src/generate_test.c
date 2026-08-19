/* generate_test.c — extractor and password generation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <math.h>
#include <string.h>

#include "rnkg-generate.h"

static RnkgExtractor *
ready_extractor (void)
{
  g_autoptr (GError) error = NULL;
  RnkgExtractor *x = rnkg_extractor_new (&error);
  guint8 raw[4096];

  g_assert_no_error (error);
  g_assert_nonnull (x);

  for (gsize i = 0; i < sizeof raw; i++)
    raw[i] = (guint8) (i * 7 + 13);

  g_assert_true (rnkg_extractor_absorb (x, raw, sizeof raw, 4096.0, &error));
  g_assert_no_error (error);
  g_assert_true (rnkg_extractor_finish (x, &error));
  g_assert_no_error (error);

  return x;
}

/* Squeezing must continue the stream, not restart it — otherwise a long key
 * would be a short key repeated, which is the kind of bug that looks fine in
 * a hex dump. */
static void
test_squeeze_continues (void)
{
  g_autoptr (GError) error = NULL;
  RnkgExtractor *x = ready_extractor ();
  guint8 a[64], b[64];

  g_assert_true (rnkg_extractor_squeeze (x, a, sizeof a, &error));
  g_assert_true (rnkg_extractor_squeeze (x, b, sizeof b, &error));
  g_assert_no_error (error);

  g_assert_cmpint (memcmp (a, b, sizeof a), !=, 0);

  rnkg_extractor_free (x);
}

/* The safety property the whole design rests on: absorbing nothing at all
 * still produces output, because the kernel seed is mixed in regardless. */
static void
test_kernel_seed_is_unconditional (void)
{
  g_autoptr (GError) error = NULL;
  RnkgExtractor *x1 = rnkg_extractor_new (&error);
  RnkgExtractor *x2 = rnkg_extractor_new (&error);
  guint8 a[32], b[32];

  g_assert_true (rnkg_extractor_finish (x1, &error));
  g_assert_true (rnkg_extractor_finish (x2, &error));
  g_assert_true (rnkg_extractor_squeeze (x1, a, sizeof a, &error));
  g_assert_true (rnkg_extractor_squeeze (x2, b, sizeof b, &error));
  g_assert_no_error (error);

  /* Two extractors given identical (empty) noise still differ, because each
   * pulled its own getrandom() seed. */
  g_assert_cmpint (memcmp (a, b, sizeof a), !=, 0);

  rnkg_extractor_free (x1);
  rnkg_extractor_free (x2);
}

/* Identical noise must not give identical output either — same reason. */
static void
test_same_noise_differs (void)
{
  g_autoptr (GError) error = NULL;
  guint8 raw[1024];
  char *p1, *p2;
  RnkgExtractor *x1 = rnkg_extractor_new (&error);
  RnkgExtractor *x2 = rnkg_extractor_new (&error);

  memset (raw, 0xa5, sizeof raw);

  rnkg_extractor_absorb (x1, raw, sizeof raw, 1024.0, &error);
  rnkg_extractor_absorb (x2, raw, sizeof raw, 1024.0, &error);
  rnkg_extractor_finish (x1, &error);
  rnkg_extractor_finish (x2, &error);

  p1 = rnkg_generate_password (x1, RNKG_ALPHABET_LETTERS, 32, &error);
  p2 = rnkg_generate_password (x2, RNKG_ALPHABET_LETTERS, 32, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (p1, !=, p2);

  rnkg_secure_string_free (p1);
  rnkg_secure_string_free (p2);
  rnkg_extractor_free (x1);
  rnkg_extractor_free (x2);
}

static void
test_absorb_after_finish_refused (void)
{
  g_autoptr (GError) error = NULL;
  RnkgExtractor *x = ready_extractor ();
  guint8 raw[16] = { 0 };

  g_assert_false (rnkg_extractor_absorb (x, raw, sizeof raw, 1.0, &error));
  g_assert_error (error, RNKG_ERROR, RNKG_ERROR_STATE);

  rnkg_extractor_free (x);
}

static void
test_alphabet_membership (void)
{
  static const RnkgAlphabet all[] = {
    RNKG_ALPHABET_LETTERS, RNKG_ALPHABET_ALNUM, RNKG_ALPHABET_ALL,
    RNKG_ALPHABET_HEX, RNKG_ALPHABET_BASE64,
  };

  for (gsize a = 0; a < G_N_ELEMENTS (all); a++)
    {
      g_autoptr (GError) error = NULL;
      RnkgExtractor *x = ready_extractor ();
      const char *chars = rnkg_alphabet_chars (all[a]);
      char *pw = rnkg_generate_password (x, all[a], 200, &error);

      g_assert_no_error (error);
      g_assert_cmpuint (strlen (pw), ==, 200);
      for (gsize i = 0; i < 200; i++)
        g_assert_nonnull (strchr (chars, pw[i]));

      rnkg_secure_string_free (pw);
      rnkg_extractor_free (x);
    }
}

static void
test_alphabet_parse (void)
{
  RnkgAlphabet a;

  g_assert_true (rnkg_alphabet_parse ("hex", &a));
  g_assert_cmpint (a, ==, RNKG_ALPHABET_HEX);
  g_assert_true (rnkg_alphabet_parse ("LETTERS", &a));
  g_assert_cmpint (a, ==, RNKG_ALPHABET_LETTERS);
  g_assert_false (rnkg_alphabet_parse ("klingon", &a));
  g_assert_false (rnkg_alphabet_parse (NULL, &a));
}

/* Rejection sampling, not modulo: 52 does not divide 256, so a modulo
 * mapping would over-represent the first 48 letters by 2/5 of a percent.
 * Over a large sample a chi-squared test sees that; this checks the
 * distribution is flat enough that it cannot be hiding. */
static void
test_no_modulo_bias (void)
{
  g_autoptr (GError) error = NULL;
  RnkgExtractor *x = ready_extractor ();
  const char *chars = rnkg_alphabet_chars (RNKG_ALPHABET_LETTERS);
  const gsize m = strlen (chars);
  const guint batch = 10000;
  const guint rounds = 52;
  const guint n = batch * rounds;
  guint counts[64] = { 0 };
  double chi2 = 0.0;

  for (guint round = 0; round < rounds; round++)
    {
      char *pw = rnkg_generate_password (x, RNKG_ALPHABET_LETTERS,
                                         batch, &error);

      g_assert_no_error (error);
      for (guint i = 0; i < batch; i++)
        counts[strchr (chars, pw[i]) - chars]++;
      rnkg_secure_string_free (pw);
    }

  for (gsize i = 0; i < m; i++)
    {
      const double expected = (double) n / m;
      const double d = counts[i] - expected;

      chi2 += d * d / expected;
    }

  /* 51 degrees of freedom; the 99.9 % critical value is about 87. */
  g_assert_cmpfloat (chi2, <, 87.0);

  rnkg_extractor_free (x);
}

static void
test_strength_bits (void)
{
  /* 20 characters from 52 letters — the CLI default. */
  g_assert_cmpfloat (fabs (rnkg_strength_bits (RNKG_ALPHABET_LETTERS, 20)
                           - 20 * log2 (52.0)), <, 1e-9);
  g_assert_cmpfloat (fabs (rnkg_strength_bits (RNKG_ALPHABET_HEX, 32)
                           - 128.0), <, 1e-9);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/generate/squeeze-continues", test_squeeze_continues);
  g_test_add_func ("/generate/kernel-seed", test_kernel_seed_is_unconditional);
  g_test_add_func ("/generate/same-noise-differs", test_same_noise_differs);
  g_test_add_func ("/generate/absorb-after-finish", test_absorb_after_finish_refused);
  g_test_add_func ("/generate/alphabet-membership", test_alphabet_membership);
  g_test_add_func ("/generate/alphabet-parse", test_alphabet_parse);
  g_test_add_func ("/generate/no-modulo-bias", test_no_modulo_bias);
  g_test_add_func ("/generate/strength-bits", test_strength_bits);

  return g_test_run ();
}
