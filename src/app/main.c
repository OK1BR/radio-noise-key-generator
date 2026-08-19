/* main.c — radio-noise-key-generator GTK4/libadwaita front-end bootstrap.
 *
 * The engine lives in src/engine/ and stays GLib-only (no GTK) so it is
 * testable headless; the window itself is window.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <adwaita.h>

#include "window.h"

static void
on_activate (AdwApplication *app, gpointer user_data)
{
  GtkWindow *win;

  (void) user_data;

  win = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (win == NULL)
    win = GTK_WINDOW (rnkg_window_new (app));
  gtk_window_present (win);

  /* Verification hook: RNKG_AUTOCLOSE_MS=N closes the window after N ms,
   * so the app can be exercised from a script without a hand on it. */
  {
    const char *auto_ms = g_getenv ("RNKG_AUTOCLOSE_MS");

    if (auto_ms != NULL)
      g_timeout_add_once ((guint) g_ascii_strtoull (auto_ms, NULL, 10),
                          (GSourceOnceFunc) gtk_window_close, win);
  }
}

int
main (int argc, char **argv)
{
  AdwApplication *app;
  int status;

  app = adw_application_new ("cz.ok1br.radio_noise_key_generator",
                             G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
