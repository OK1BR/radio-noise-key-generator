/* settings.c — see settings.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "settings.h"

#include "rnkg-source.h"

static char *
settings_path (void)
{
  return g_build_filename (g_get_user_config_dir (),
                           "radio-noise-key-generator", "settings.ini",
                           NULL);
}

void
rnkg_app_settings_load (RnkgAppSettings *out)
{
  g_autoptr (GKeyFile) kf   = g_key_file_new ();
  g_autofree char     *path = settings_path ();

  g_return_if_fail (out != NULL);

  *out = (RnkgAppSettings) {
    .device    = 0,
    .freq_mhz  = RNKG_DEFAULT_FREQ_HZ / 1e6,
    .rate_msps = RNKG_DEFAULT_SAMPLERATE / 1e6,
    .gain_db   = -1.0,
    .length    = 20,
    .alphabet  = g_strdup ("letters"),
  };

  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
    return;   /* first run: the defaults stand */

  if (g_key_file_has_key (kf, "source", "device", NULL))
    out->device = (guint) g_key_file_get_integer (kf, "source", "device",
                                                  NULL);
  if (g_key_file_has_key (kf, "source", "freq_mhz", NULL))
    out->freq_mhz = g_key_file_get_double (kf, "source", "freq_mhz", NULL);
  if (g_key_file_has_key (kf, "source", "rate_msps", NULL))
    out->rate_msps = g_key_file_get_double (kf, "source", "rate_msps", NULL);
  if (g_key_file_has_key (kf, "source", "gain_db", NULL))
    out->gain_db = g_key_file_get_double (kf, "source", "gain_db", NULL);

  if (g_key_file_has_key (kf, "generate", "length", NULL))
    out->length = (guint) CLAMP (g_key_file_get_integer (kf, "generate",
                                                         "length", NULL),
                                 8, 128);
  if (g_key_file_has_key (kf, "generate", "alphabet", NULL))
    {
      g_free (out->alphabet);
      out->alphabet = g_key_file_get_string (kf, "generate", "alphabet",
                                             NULL);
    }
}

void
rnkg_app_settings_save (const RnkgAppSettings *settings)
{
  g_autoptr (GKeyFile) kf   = g_key_file_new ();
  g_autoptr (GError)   error = NULL;
  g_autofree char     *path = settings_path ();
  g_autofree char     *dir  = g_path_get_dirname (path);

  g_return_if_fail (settings != NULL);

  g_key_file_set_integer (kf, "source", "device", (gint) settings->device);
  g_key_file_set_double  (kf, "source", "freq_mhz", settings->freq_mhz);
  g_key_file_set_double  (kf, "source", "rate_msps", settings->rate_msps);
  g_key_file_set_double  (kf, "source", "gain_db", settings->gain_db);

  g_key_file_set_integer (kf, "generate", "length",
                          (gint) settings->length);
  g_key_file_set_string  (kf, "generate", "alphabet",
                          settings->alphabet != NULL
                            ? settings->alphabet : "letters");

  g_mkdir_with_parents (dir, 0700);
  if (!g_key_file_save_to_file (kf, path, &error))
    g_warning ("cannot save %s: %s", path, error->message);
}

void
rnkg_app_settings_clear (RnkgAppSettings *settings)
{
  if (settings == NULL)
    return;

  g_clear_pointer (&settings->alphabet, g_free);
}
