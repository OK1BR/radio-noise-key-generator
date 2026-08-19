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
