/* collect_test.c — the collection loop: SP 800-90B §4.3 startup behaviour.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "rnkg-collect.h"

/* A temp file holding `blocks` collection blocks of seeded GRand bytes —
 * uncorrelated enough to pass every health and structure test. */
static char *
write_noise_file (guint blocks)
{
  g_autoptr (GError) error = NULL;
  char  *path = NULL;
  gint   fd   = g_file_open_tmp ("rnkg-collect-XXXXXX", &path, &error);
  GRand *rand = g_rand_new_with_seed (0x90b);
  FILE  *fp;

  g_assert_no_error (error);
  fp = fdopen (fd, "wb");
  g_assert_nonnull (fp);

  for (gsize i = 0; i < (gsize) blocks * RNKG_BLOCK_BYTES; i++)
    fputc ((int) g_rand_int_range (rand, 0, 256), fp);

  fclose (fp);
  g_rand_free (rand);
  return path;
}

static char *
write_constant_file (guint blocks, guint8 value)
{
  g_autoptr (GError) error = NULL;
  char *path = NULL;
  gint  fd   = g_file_open_tmp ("rnkg-collect-XXXXXX", &path, &error);
  FILE *fp;

  g_assert_no_error (error);
  fp = fdopen (fd, "wb");
  g_assert_nonnull (fp);

  for (gsize i = 0; i < (gsize) blocks * RNKG_BLOCK_BYTES; i++)
    fputc (value, fp);

  fclose (fp);
  return path;
}

/* The first block feeds the startup test and credits nothing; credit only
 * starts with the second block. */
static void
test_startup_not_credited (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *path = write_noise_file (2);
  RnkgSource    *source    = rnkg_source_file_new (path, &error);
  RnkgCollector *collector = rnkg_collector_new (source, 64.0, &error);
  RnkgProgress   p;

  g_assert_no_error (error);
  g_assert_nonnull (collector);

  g_assert_true (rnkg_collector_step (collector, &error));
  g_assert_no_error (error);
  rnkg_collector_progress (collector, &p);
  g_assert_true (p.startup_block);
  g_assert_cmpfloat (p.credited_bits, ==, 0.0);
  g_assert_false (rnkg_collector_done (collector));

  g_assert_true (rnkg_collector_step (collector, &error));
  g_assert_no_error (error);
  rnkg_collector_progress (collector, &p);
  g_assert_false (p.startup_block);
  g_assert_cmpfloat (p.credited_bits, >, 0.0);

  rnkg_collector_free (collector);
  g_unlink (path);
}

/* A source that fails its startup test never produces anything — §4.3 makes
 * the failure terminal, startup or not. */
static void
test_startup_failure_terminal (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *path = write_constant_file (1, 0x7f);
  RnkgSource    *source    = rnkg_source_file_new (path, &error);
  RnkgCollector *collector = rnkg_collector_new (source, 64.0, &error);

  g_assert_no_error (error);
  g_assert_false (rnkg_collector_step (collector, &error));
  g_assert_error (error, RNKG_ERROR, RNKG_ERROR_HEALTH);

  rnkg_collector_free (collector);
  g_unlink (path);
}

/* Startup samples alone can never satisfy a target: a source that dries up
 * right after startup has credited nothing, and finish() must refuse. */
static void
test_startup_alone_yields_nothing (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *path = write_noise_file (1);
  RnkgSource    *source    = rnkg_source_file_new (path, &error);
  RnkgCollector *collector = rnkg_collector_new (source, 64.0, &error);

  g_assert_no_error (error);
  g_assert_true (rnkg_collector_step (collector, &error));
  g_assert_no_error (error);
  g_assert_false (rnkg_collector_done (collector));

  /* The file is exhausted; the next step is a short read... */
  g_assert_false (rnkg_collector_step (collector, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  /* ...and nothing was credited, so finishing is refused. */
  g_assert_null (rnkg_collector_finish (collector, &error));
  g_assert_error (error, RNKG_ERROR, RNKG_ERROR_INSUFFICIENT_ENTROPY);

  rnkg_collector_free (collector);
  g_unlink (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/collect/startup-not-credited",
                   test_startup_not_credited);
  g_test_add_func ("/collect/startup-failure-terminal",
                   test_startup_failure_terminal);
  g_test_add_func ("/collect/startup-alone-yields-nothing",
                   test_startup_alone_yields_nothing);

  return g_test_run ();
}
