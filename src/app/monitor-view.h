/* monitor-view.h — live health indicators and the running entropy estimate.
 *
 * One row of verdicts (RCT, APT, serial, I/Q) plus the numbers a person
 * needs to trust the source: measured min-entropy, DC bias, block count.
 * When a test fails it shows *which* and the engine's human message —
 * the failure is terminal until the source is reopened (§4.3).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <gtk/gtk.h>

#include "rnkg-estimate.h"
#include "rnkg-health.h"

G_BEGIN_DECLS

typedef struct {
  RnkgHealthVerdict verdict;    /* latched by the worker */
  RnkgStructure     structure;
  RnkgEstimate      estimate;
  guint64           blocks;
  gboolean          valid;      /* FALSE until the first block lands */
} RnkgMonitorData;

#define RNKG_TYPE_MONITOR_VIEW (rnkg_monitor_view_get_type ())
G_DECLARE_FINAL_TYPE (RnkgMonitorView, rnkg_monitor_view,
                      RNKG, MONITOR_VIEW, GtkBox)

GtkWidget *rnkg_monitor_view_new    (void);

void       rnkg_monitor_view_update (RnkgMonitorView       *self,
                                     const RnkgMonitorData *data);

G_END_DECLS
