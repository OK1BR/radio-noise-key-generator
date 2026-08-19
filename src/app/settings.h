/* settings.h — GKeyFile persistence for the app's few preferences.
 *
 * Family pattern (log-for-linux): a plain ini file under
 * ~/.config/radio-noise-key-generator/settings.ini.  Nothing secret is
 * ever stored here — tuning parameters and generation defaults only.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  /* [source] */
  guint  device;
  double freq_mhz;
  double rate_msps;
  double gain_db;      /* < 0 means the tuner's maximum */

  /* [generate] */
  guint  length;
  char  *alphabet;     /* name as rnkg_alphabet_parse() takes it */
} RnkgAppSettings;

/* Fills `out` with the stored values, or the defaults where the file or
 * a key is missing.  `out->alphabet` is owned by the caller. */
void rnkg_app_settings_load (RnkgAppSettings *out);

void rnkg_app_settings_save (const RnkgAppSettings *settings);

void rnkg_app_settings_clear (RnkgAppSettings *settings);

G_END_DECLS
