/* window.h — the main window: spectrum first (docs/SCOPE.md M2).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define RNKG_TYPE_WINDOW (rnkg_window_get_type ())
G_DECLARE_FINAL_TYPE (RnkgWindow, rnkg_window, RNKG, WINDOW,
                      AdwApplicationWindow)

GtkWidget *rnkg_window_new (AdwApplication *app);

G_END_DECLS
