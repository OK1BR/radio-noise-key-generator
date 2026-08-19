/* generation-view.h — the main act: live candidates and the Snap button.
 *
 * Every healthy block derives a fresh candidate password and the label
 * rotates through them; Snap freezes whichever is on screen at the
 * moment the user chooses.  Copy puts the frozen code on the clipboard
 * and clears it again after a timer, the same manners as pass-for-linux.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <gtk/gtk.h>

#include "rnkg-generate.h"

G_BEGIN_DECLS

#define RNKG_TYPE_GENERATION_VIEW (rnkg_generation_view_get_type ())
G_DECLARE_FINAL_TYPE (RnkgGenerationView, rnkg_generation_view,
                      RNKG, GENERATION_VIEW, GtkBox)

GtkWidget *rnkg_generation_view_new (void);

/* The rotating display.  Ignored while the view is snapped, so the
 * caller can push candidates unconditionally. */
void rnkg_generation_view_set_candidate (RnkgGenerationView *self,
                                         const char         *text);

gboolean rnkg_generation_view_is_snapped (RnkgGenerationView *self);

/* What the worker should derive: alphabet and length, as currently set. */
void rnkg_generation_view_get_params (RnkgGenerationView *self,
                                      RnkgAlphabet       *alphabet,
                                      guint              *length);

G_END_DECLS
