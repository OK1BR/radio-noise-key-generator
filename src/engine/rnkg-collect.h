/* rnkg-collect.h — the loop that ties source, health tests, estimator and
 * extractor together.
 *
 * Both front ends drive this; the CLI runs it to completion, the GTK UI
 * steps it from an idle callback so the window keeps repainting.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib.h>

#include "rnkg-estimate.h"
#include "rnkg-extract.h"
#include "rnkg-health.h"
#include "rnkg-source.h"

G_BEGIN_DECLS

/* One read from the dongle at 2.048 MS/s is a fraction of a second — small
 * enough that the UI stays responsive, large enough that the MCV estimate
 * over it means something (256 possible values need far more than 256
 * samples before a proportion is worth a confidence interval). */
#define RNKG_BLOCK_BYTES (256 * 1024)

/* The min-entropy per sample the health tests are configured against.  Well
 * below what a healthy 8-bit RTL-SDR stream measures, because the cutoffs
 * must not fire on a good source; the estimator, not this constant, decides
 * how much entropy is actually credited. */
#define RNKG_ASSESSED_H 4.0

typedef struct {
  guint64      blocks;
  guint64      bytes;
  double       credited_bits;
  double       target_bits;
  RnkgEstimate last_estimate;
  RnkgStructure last_structure;
} RnkgProgress;

typedef struct _RnkgCollector RnkgCollector;

/* Takes ownership of `source`. */
RnkgCollector *rnkg_collector_new  (RnkgSource *source,
                                    double      target_bits,
                                    GError    **error);
void           rnkg_collector_free (RnkgCollector *c);

/* Read and process one block.  Returns FALSE on a health-test failure or a
 * source error, with `error` set — a failure is terminal for this collector,
 * by design: SP 800-90B §4.3 does not let a source that failed a continuous
 * test go back to producing output. */
gboolean rnkg_collector_step (RnkgCollector *c, GError **error);

gboolean rnkg_collector_done     (RnkgCollector *c);
void     rnkg_collector_progress (RnkgCollector *c, RnkgProgress *out);

/* Seal the collection and hand back the extractor, ready to squeeze.  Fails
 * if the target was not reached.  The collector keeps ownership. */
RnkgExtractor *rnkg_collector_finish (RnkgCollector *c, GError **error);

G_END_DECLS
