/* rnkg-extract.h — conditioning: raw samples in, key material out.
 *
 * SHAKE-256 is the whole extractor.  It is a sponge with extendable output,
 * so the same primitive both absorbs an arbitrary amount of noise and
 * squeezes an arbitrary amount of key material — which is exactly the shape
 * of this problem ("keys of any length").  No block chaining, no counter
 * mode bolted on top.
 *
 * The kernel is mixed in unconditionally at finish().  That is the safety
 * property this whole program rests on: the output can never be weaker than
 * getrandom() alone, no matter what the dongle hands us — a dead antenna, a
 * driver returning constants, or an attacker jamming the frequency with a
 * signal of their choosing.  The radio can only add.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define RNKG_KERNEL_SEED_BYTES 64

typedef struct _RnkgExtractor RnkgExtractor;

RnkgExtractor *rnkg_extractor_new  (GError **error);
void           rnkg_extractor_free (RnkgExtractor *x);

/* Absorb a block of raw samples, crediting it with `entropy_bits` (from
 * rnkg_estimate_credit()).  Refused once finish() has been called. */
gboolean rnkg_extractor_absorb (RnkgExtractor *x,
                                const guint8  *raw,
                                gsize          len,
                                double         entropy_bits,
                                GError       **error);

double   rnkg_extractor_credited (RnkgExtractor *x);

/* Mix in RNKG_KERNEL_SEED_BYTES from getrandom() and switch to squeezing.
 * After this the extractor accepts no more input. */
gboolean rnkg_extractor_finish  (RnkgExtractor *x, GError **error);

/* Squeeze output.  May be called repeatedly; successive calls continue the
 * stream rather than repeating it. */
gboolean rnkg_extractor_squeeze (RnkgExtractor *x,
                                 guint8        *out,
                                 gsize          len,
                                 GError       **error);

/* Initialise libgcrypt, including secure memory.  Idempotent; call once at
 * program start before anything else in the engine. */
gboolean rnkg_crypto_init (GError **error);

#define RNKG_ERROR (rnkg_error_quark ())
GQuark rnkg_error_quark (void);

typedef enum {
  RNKG_ERROR_FAILED,
  RNKG_ERROR_STATE,
  RNKG_ERROR_NO_DEVICE,
  RNKG_ERROR_HEALTH,
  RNKG_ERROR_INSUFFICIENT_ENTROPY,
} RnkgError;

G_END_DECLS
