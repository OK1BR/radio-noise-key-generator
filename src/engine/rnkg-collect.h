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

/* The min-entropy per sample the health tests are configured against.  Must
 * sit well below what a healthy stream measures, because the cutoffs must
 * not fire on a good source; the estimator, not this constant, decides how
 * much entropy is actually credited.
 *
 * Measured 2026-08-19 on an RTL-SDR Blog V4 (R828D) at 1300 MHz: the noise
 * spans only ~6 ADC codes around the DC offset and MCV gives 0.63–0.83
 * bits/sample across manual gains — not the >4 bits M0 assumed.  0.3 stays
 * under the worst healthy measurement while a stuck source still trips the
 * repetition count at 1 + ceil(20/0.3) = 68 samples (~33 us of stream). */
#define RNKG_ASSESSED_H 0.3

/* SP 800-90B §4.3: a fixed run of samples must pass the health tests before
 * any output is produced, and those samples are not themselves used.  M0
 * satisfied this only by accident of RNKG_BLOCK_BYTES being the first thing
 * read; it is a named quantity now, so resizing blocks later cannot
 * silently shrink the startup test with it.  One full block — 262144
 * samples — sits far above the standard's floor. */
#define RNKG_STARTUP_SAMPLES (256 * 1024)

typedef struct {
  guint64      blocks;
  guint64      bytes;
  double       credited_bits;
  double       target_bits;
  RnkgEstimate last_estimate;
  RnkgStructure last_structure;
  gboolean     startup_block;  /* the block just read fed the §4.3 startup
                                * test and was discarded, not credited */
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
