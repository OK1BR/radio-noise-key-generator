/* rnkg — radio noise key generator, command line front end.
 *
 * Passwords go to stdout, one per line, and nothing else does: diagnostics
 * are on stderr so `rnkg | pass insert` and friends behave.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gcrypt.h>

#include "rnkg-collect.h"
#include "rnkg-generate.h"

static gint     opt_length    = 20;
static gint     opt_count     = 1;
static gint     opt_key_bytes = 0;
static gchar   *opt_alphabet  = NULL;
static gchar   *opt_file      = NULL;
static gint     opt_device    = 0;
static gdouble  opt_freq_mhz  = RNKG_DEFAULT_FREQ_HZ / 1e6;
static gdouble  opt_rate_msps = RNKG_DEFAULT_SAMPLERATE / 1e6;
static gdouble  opt_gain_db   = -1.0;
static gboolean opt_snap      = FALSE;
static gdouble  opt_snap_after = -1.0;
static gboolean opt_verbose   = FALSE;
static gboolean opt_list      = FALSE;

static const GOptionEntry entries[] = {
  { "length", 'n', 0, G_OPTION_ARG_INT, &opt_length,
    "password length in characters (default 20)", "N" },
  { "count", 'c', 0, G_OPTION_ARG_INT, &opt_count,
    "how many passwords to generate (default 1)", "N" },
  { "alphabet", 'a', 0, G_OPTION_ARG_STRING, &opt_alphabet,
    "letters, alnum, all, hex or base64 (default letters)", "NAME" },
  { "key", 'k', 0, G_OPTION_ARG_INT, &opt_key_bytes,
    "emit N bytes of raw key material as hex instead of a password", "N" },
  { "file", 'f', 0, G_OPTION_ARG_FILENAME, &opt_file,
    "read samples from a file instead of the dongle ('-' for stdin)", "PATH" },
  { "device", 'd', 0, G_OPTION_ARG_INT, &opt_device,
    "RTL-SDR device index (default 0)", "N" },
  { "freq", 0, 0, G_OPTION_ARG_DOUBLE, &opt_freq_mhz,
    "tuning frequency in MHz (default 1300)", "MHZ" },
  { "rate", 0, 0, G_OPTION_ARG_DOUBLE, &opt_rate_msps,
    "sample rate in MS/s (default 2.048)", "MSPS" },
  { "gain", 0, 0, G_OPTION_ARG_DOUBLE, &opt_gain_db,
    "tuner gain in dB (default: the tuner's maximum)", "DB" },
  { "snap", 0, 0, G_OPTION_ARG_NONE, &opt_snap,
    "rotate fresh candidates until a newline or EOF on stdin picks one "
    "(a script holds stdin open: sleep 3 | rnkg --snap)", NULL },
  { "snap-after", 0, 0, G_OPTION_ARG_DOUBLE, &opt_snap_after,
    "rotate fresh candidates and snap automatically after SEC seconds", "SEC" },
  { "list", 0, 0, G_OPTION_ARG_NONE, &opt_list,
    "list the RTL-SDR devices present and exit", NULL },
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose,
    "report health tests and entropy estimates on stderr", NULL },
  { NULL, 0, 0, 0, NULL, NULL, NULL },
};

static void
report (const RnkgProgress *p)
{
  g_printerr ("  block %-4" G_GUINT64_FORMAT
              " min-entropy %.2f b/sample (Shannon %.2f)"
              "  serial %+.4f  I/Q %+.4f  DC %+.4f"
              "  credited %.0f/%.0f b%s\n",
              p->blocks, p->last_estimate.h_min, p->last_estimate.h_shannon,
              p->last_structure.serial, p->last_structure.iq,
              p->last_structure.dc_bias,
              p->credited_bits, p->target_bits,
              p->startup_block ? "  (startup test — discarded)" : "");
}

/* --- snap mode -----------------------------------------------------------
 *
 * The CLI face of the GUI's rotation: every stretch of healthy blocks
 * derives a fresh, fully backed candidate (its own extractor, measured
 * credit, the unconditional kernel seed), and the snap picks whichever
 * is current.  The moment of snapping adds nothing and costs nothing —
 * every candidate is equally strong — so a script's timer is as good a
 * finger as a human's.
 *
 * Two triggers: --snap-after SEC is a plain timer; --snap watches stdin
 * and snaps on a newline or EOF, so an agent controls the moment simply
 * by holding the pipe open (sleep 3 | rnkg --snap).
 */

/* TRUE once stdin says snap: any newline, or end of file. */
static gboolean
stdin_wants_snap (void)
{
  GPollFD pfd = { .fd = 0, .events = G_IO_IN | G_IO_HUP };

  while (g_poll (&pfd, 1, 0) > 0)
    {
      char    buf[64];
      gssize  n = read (0, buf, sizeof buf);

      if (n <= 0)
        return TRUE;   /* EOF (or a dead pipe): the writer let go */
      if (memchr (buf, '\n', (gsize) n) != NULL)
        return TRUE;
    }
  return FALSE;
}

/* Always returns ordinary heap the caller wipes and g_free()s — the
 * engine's password strings live in secure memory and must go back
 * through rnkg_secure_string_free(), so copy out of them here. */
static char *
snap_candidate_derive (RnkgExtractor *x, RnkgAlphabet alphabet,
                       GError **error)
{
  if (opt_key_bytes <= 0)
    {
      char *secure = rnkg_generate_password (x, alphabet,
                                             (guint) opt_length, error);
      char *copy;

      if (secure == NULL)
        return NULL;
      copy = g_strdup (secure);
      rnkg_secure_string_free (secure);
      return copy;
    }

  {
    g_autofree guint8 *key = g_malloc (opt_key_bytes);
    GString *hex;

    if (!rnkg_generate_key (x, key, opt_key_bytes, error))
      return NULL;
    hex = g_string_new (NULL);
    for (gint b = 0; b < opt_key_bytes; b++)
      g_string_append_printf (hex, "%02x", key[b]);
    explicit_bzero (key, opt_key_bytes);
    return g_string_free (hex, FALSE);
  }
}

static int
run_snap (RnkgSource *source, RnkgAlphabet alphabet, double target_bits)
{
  g_autoptr (GError) error = NULL;
  RnkgHealth     health;
  RnkgExtractor *x = NULL;
  guint8        *block;
  char          *current = NULL;
  const gint64   deadline = opt_snap_after >= 0.0
    ? g_get_monotonic_time () + (gint64) (opt_snap_after * G_USEC_PER_SEC)
    : 0;
  const gboolean show = isatty (2) && !opt_verbose;
  double         credited = 0.0;
  int            ret = 1;

  if (!rnkg_crypto_init (&error))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }

  rnkg_health_init (&health, RNKG_ASSESSED_H, FALSE);
  block = gcry_malloc_secure (RNKG_BLOCK_BYTES);
  if (block == NULL)
    {
      g_printerr ("cannot allocate a secure sample buffer\n");
      return 1;
    }

  for (;;)
    {
      RnkgHealthVerdict verdict;
      RnkgStructure     structure;
      RnkgEstimate      estimate;

      if (!rnkg_source_read (source, block, RNKG_BLOCK_BYTES, &error))
        {
          g_printerr ("%s\n", error->message);
          goto out;
        }

      verdict = rnkg_health_push_block (&health, block, RNKG_BLOCK_BYTES);
      rnkg_structure_analyse (block, RNKG_BLOCK_BYTES, &structure);
      if (verdict == RNKG_HEALTH_OK)
        verdict = rnkg_structure_verdict (&structure);
      if (verdict != RNKG_HEALTH_OK)
        {
          if (show)
            g_printerr ("\r\033[K");
          if (verdict == RNKG_HEALTH_FAIL_SERIAL ||
              verdict == RNKG_HEALTH_FAIL_IQ)
            g_printerr ("%s (serial %.4f, I/Q %.4f)\n",
                        rnkg_health_verdict_message (verdict),
                        structure.serial, structure.iq);
          else
            g_printerr ("%s\n", rnkg_health_verdict_message (verdict));
          goto out;
        }

      rnkg_estimate_mcv (block, RNKG_BLOCK_BYTES, &estimate);

      if (x == NULL)
        {
          x = rnkg_extractor_new (&error);
          if (x == NULL)
            {
              g_printerr ("%s\n", error->message);
              goto out;
            }
          credited = 0.0;
        }

      if (!rnkg_extractor_absorb (x, block, RNKG_BLOCK_BYTES,
                                  rnkg_estimate_credit (&estimate), &error))
        {
          g_printerr ("%s\n", error->message);
          goto out;
        }
      credited += rnkg_estimate_credit (&estimate);

      /* Enough measured entropy for one candidate: derive it and start
       * accumulating for the next.  At the defaults a single block covers
       * any sane target, so this rotates at the block rate. */
      if (credited >= target_bits)
        {
          char *fresh;

          if (!rnkg_extractor_finish (x, &error) ||
              (fresh = snap_candidate_derive (x, alphabet, &error)) == NULL)
            {
              g_printerr ("%s\n", error->message);
              goto out;
            }
          if (current != NULL)
            {
              explicit_bzero (current, strlen (current));
              g_free (current);
            }
          current = fresh;
          g_clear_pointer (&x, rnkg_extractor_free);

          if (show)
            g_printerr ("\r\033[K%s", current);
        }

      if (current == NULL)
        continue;   /* nothing to snap yet */

      if (opt_snap_after >= 0.0)
        {
          if (g_get_monotonic_time () >= deadline)
            break;
        }
      else if (stdin_wants_snap ())
        break;
    }

  if (show)
    g_printerr ("\r\033[K");
  g_print ("%s\n", current);
  ret = 0;

out:
  if (current != NULL)
    {
      explicit_bzero (current, strlen (current));
      g_free (current);
    }
  if (x != NULL)
    rnkg_extractor_free (x);
  gcry_free (block);
  return ret;
}

static int
list_devices (void)
{
  const guint n = rnkg_source_device_count ();

  if (!rnkg_source_have_rtlsdr ())
    {
      g_printerr ("This build has no librtlsdr support.\n");
      return 1;
    }
  if (n == 0)
    {
      g_printerr ("No RTL-SDR device found.\n");
      return 1;
    }

  for (guint i = 0; i < n; i++)
    {
      char *name = rnkg_source_device_name (i);

      g_print ("%u: %s\n", i, name != NULL ? name : "(unnamed)");
      g_free (name);
    }

  return 0;
}

int
main (int argc, char **argv)
{
  g_autoptr (GOptionContext) ctx = NULL;
  g_autoptr (GError) error = NULL;
  RnkgAlphabet   alphabet = RNKG_ALPHABET_LETTERS;
  RnkgSource    *source;
  RnkgCollector *collector;
  RnkgExtractor *extractor;
  double         target_bits;

  ctx = g_option_context_new ("- generate passwords and keys from radio noise");
  g_option_context_add_main_entries (ctx, entries, NULL);
  if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      return 2;
    }

  if (opt_list)
    return list_devices ();

  if (opt_alphabet != NULL && !rnkg_alphabet_parse (opt_alphabet, &alphabet))
    {
      g_printerr ("unknown alphabet '%s'\n", opt_alphabet);
      return 2;
    }

  if (opt_length < 1 || opt_count < 1 || opt_key_bytes < 0)
    {
      g_printerr ("length, count and key size must be positive\n");
      return 2;
    }

  if ((opt_snap || opt_snap_after >= 0.0) && opt_count != 1)
    {
      g_printerr ("a snap picks exactly one candidate; --count does not "
                  "apply\n");
      return 2;
    }
  if (opt_snap && opt_snap_after >= 0.0)
    {
      g_printerr ("--snap and --snap-after are two triggers for the same "
                  "thing; pick one\n");
      return 2;
    }
  if (opt_snap && g_strcmp0 (opt_file, "-") == 0)
    {
      g_printerr ("--snap watches stdin, which '--file -' is already "
                  "reading; use --snap-after\n");
      return 2;
    }

  /* Collect enough for everything asked for at once, so a run of passwords
   * is not several independently seeded runs. */
  target_bits = opt_key_bytes > 0
              ? opt_key_bytes * 8.0 * opt_count
              : rnkg_strength_bits (alphabet, opt_length) * opt_count;

  if (opt_file != NULL)
    source = rnkg_source_file_new (opt_file, &error);
  else
    source = rnkg_source_rtlsdr_new (opt_device,
                                     (guint32) (opt_freq_mhz * 1e6),
                                     (guint32) (opt_rate_msps * 1e6),
                                     opt_gain_db < 0.0
                                       ? RNKG_DEFAULT_GAIN_AUTO
                                       : (gint) (opt_gain_db * 10.0),
                                     &error);
  if (source == NULL)
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }

  if (opt_verbose)
    g_printerr ("Source: %s\n", rnkg_source_describe (source));

  if (opt_snap || opt_snap_after >= 0.0)
    {
      const double one_target = opt_key_bytes > 0
        ? opt_key_bytes * 8.0
        : rnkg_strength_bits (alphabet, opt_length);
      int ret = run_snap (source, alphabet, one_target);

      rnkg_source_free (source);
      g_free (opt_alphabet);
      g_free (opt_file);
      return ret;
    }

  collector = rnkg_collector_new (source, target_bits, &error);
  if (collector == NULL)
    {
      g_printerr ("%s\n", error->message);
      rnkg_source_free (source);
      return 1;
    }

  while (!rnkg_collector_done (collector))
    {
      RnkgProgress p;

      if (!rnkg_collector_step (collector, &error))
        {
          g_printerr ("%s\n", error->message);
          rnkg_collector_free (collector);
          return 1;
        }

      rnkg_collector_progress (collector, &p);
      if (opt_verbose)
        report (&p);
    }

  extractor = rnkg_collector_finish (collector, &error);
  if (extractor == NULL)
    {
      g_printerr ("%s\n", error->message);
      rnkg_collector_free (collector);
      return 1;
    }

  for (gint i = 0; i < opt_count; i++)
    {
      if (opt_key_bytes > 0)
        {
          g_autofree guint8 *key = g_malloc (opt_key_bytes);
          g_autoptr (GString) hex = g_string_new (NULL);

          if (!rnkg_generate_key (extractor, key, opt_key_bytes, &error))
            {
              g_printerr ("%s\n", error->message);
              rnkg_collector_free (collector);
              return 1;
            }
          for (gint b = 0; b < opt_key_bytes; b++)
            g_string_append_printf (hex, "%02x", key[b]);
          g_print ("%s\n", hex->str);
          explicit_bzero (key, opt_key_bytes);
        }
      else
        {
          char *pw = rnkg_generate_password (extractor, alphabet,
                                             opt_length, &error);

          if (pw == NULL)
            {
              g_printerr ("%s\n", error->message);
              rnkg_collector_free (collector);
              return 1;
            }
          g_print ("%s\n", pw);
          rnkg_secure_string_free (pw);
        }
    }

  if (opt_verbose && opt_key_bytes == 0)
    g_printerr ("Each password: %u characters from %zu, %.0f bits\n",
                opt_length, strlen (rnkg_alphabet_chars (alphabet)),
                rnkg_strength_bits (alphabet, opt_length));

  rnkg_collector_free (collector);
  g_free (opt_alphabet);
  g_free (opt_file);

  return 0;
}
