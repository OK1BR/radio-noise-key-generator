/* rnkg-source.h — where the raw samples come from.
 *
 * Two implementations behind one interface: the RTL-SDR dongle, and a plain
 * file.  The file source is not a toy — it is how the health tests and the
 * estimator get exercised without hardware, and how a captured block can be
 * replayed against the NIST SP800-90B_EntropyAssessment tool.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib.h>

#include "rnkg-extract.h"

G_BEGIN_DECLS

/* Defaults chosen for noise, not for listening:
 *
 * - 1300 MHz sits in the quiet stretch between the GSM/LTE bands and the
 *   aeronautical L-band, above anything a bare dongle picks up indoors.
 *   Nothing is transmitted there in region 1 at any useful level.
 * - Maximum tuner gain, so the thermal noise of the front end dominates the
 *   ADC's least significant bits rather than the quantisation floor.
 * - 2.048 MS/s: comfortably inside the 2.56 MS/s the USB link sustains
 *   without dropping samples.
 */
#define RNKG_DEFAULT_FREQ_HZ    1300000000u
#define RNKG_DEFAULT_SAMPLERATE 2048000u
#define RNKG_DEFAULT_GAIN_AUTO  G_MININT   /* let the tuner pick the maximum */

typedef struct _RnkgSource RnkgSource;

/* TRUE if the build has librtlsdr; the CLI and UI use it to explain the
 * absence of the dongle option rather than silently omitting it. */
gboolean rnkg_source_have_rtlsdr (void);

/* Number of RTL-SDR devices present.  0 without librtlsdr. */
guint rnkg_source_device_count (void);

/* Human-readable name of device `index`, or NULL. */
char *rnkg_source_device_name (guint index);

RnkgSource *rnkg_source_rtlsdr_new (guint     device_index,
                                    guint32   freq_hz,
                                    guint32   sample_rate,
                                    gint      gain_tenth_db,
                                    GError  **error);

RnkgSource *rnkg_source_file_new   (const char *path, GError **error);

/* Read exactly `len` bytes.  Returns FALSE and sets `error` on short read,
 * device failure, or end of file. */
gboolean rnkg_source_read (RnkgSource *s,
                           guint8     *buf,
                           gsize       len,
                           GError    **error);

const char *rnkg_source_describe (RnkgSource *s);

void rnkg_source_free (RnkgSource *s);

G_END_DECLS
