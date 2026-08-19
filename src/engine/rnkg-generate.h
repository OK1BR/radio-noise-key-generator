/* rnkg-generate.h — turning squeezed bytes into passwords and keys.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib.h>

#include "rnkg-extract.h"

G_BEGIN_DECLS

/* Built-in alphabets.  "letters" is the default for the same reason it is in
 * sumheslo(1): a password made only of letters survives being typed on a
 * keyboard whose layout is not the one it was generated on — which is the
 * situation every LUKS passphrase prompt puts you in. */
typedef enum {
  RNKG_ALPHABET_LETTERS,
  RNKG_ALPHABET_ALNUM,
  RNKG_ALPHABET_ALL,
  RNKG_ALPHABET_HEX,
  RNKG_ALPHABET_BASE64,
} RnkgAlphabet;

const char *rnkg_alphabet_chars (RnkgAlphabet a);
const char *rnkg_alphabet_name  (RnkgAlphabet a);
gboolean    rnkg_alphabet_parse (const char *name, RnkgAlphabet *out);

/* Bits of strength in a password of `length` characters drawn uniformly from
 * `alphabet` — length * log2(|alphabet|). */
double rnkg_strength_bits (RnkgAlphabet a, guint length);

/* Draw `length` characters uniformly from the alphabet.
 *
 * Rejection sampling, never modulo: mapping 256 byte values onto an alphabet
 * whose size does not divide 256 biases the low residues, and the bias is
 * invisible in the output.  Bytes at or above the largest multiple of the
 * alphabet size are discarded and redrawn.
 *
 * Returns a NUL-terminated string in secure memory; free with
 * rnkg_secure_string_free(). */
char *rnkg_generate_password (RnkgExtractor *x,
                              RnkgAlphabet   alphabet,
                              guint          length,
                              GError       **error);

/* Raw key material of any length, as bytes. */
gboolean rnkg_generate_key (RnkgExtractor *x,
                            guint8        *out,
                            gsize          len,
                            GError       **error);

void rnkg_secure_string_free (char *s);

G_END_DECLS
