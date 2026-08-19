/* rnkg-generate.c — see rnkg-generate.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rnkg-generate.h"

#include <gcrypt.h>
#include <math.h>
#include <string.h>

#define LETTERS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
#define DIGITS  "0123456789"
#define PUNCT   "!#%+,-./:=?@_"

const char *
rnkg_alphabet_chars (RnkgAlphabet a)
{
  switch (a)
    {
    case RNKG_ALPHABET_LETTERS: return LETTERS;
    case RNKG_ALPHABET_ALNUM:   return LETTERS DIGITS;
    case RNKG_ALPHABET_ALL:     return LETTERS DIGITS PUNCT;
    case RNKG_ALPHABET_HEX:     return "0123456789abcdef";
    case RNKG_ALPHABET_BASE64:  return LETTERS DIGITS "+/";
    default:                    return LETTERS;
    }
}

const char *
rnkg_alphabet_name (RnkgAlphabet a)
{
  switch (a)
    {
    case RNKG_ALPHABET_LETTERS: return "letters";
    case RNKG_ALPHABET_ALNUM:   return "alnum";
    case RNKG_ALPHABET_ALL:     return "all";
    case RNKG_ALPHABET_HEX:     return "hex";
    case RNKG_ALPHABET_BASE64:  return "base64";
    default:                    return "letters";
    }
}

gboolean
rnkg_alphabet_parse (const char *name, RnkgAlphabet *out)
{
  static const RnkgAlphabet all[] = {
    RNKG_ALPHABET_LETTERS, RNKG_ALPHABET_ALNUM, RNKG_ALPHABET_ALL,
    RNKG_ALPHABET_HEX, RNKG_ALPHABET_BASE64,
  };

  g_return_val_if_fail (out != NULL, FALSE);

  if (name == NULL)
    return FALSE;

  for (gsize i = 0; i < G_N_ELEMENTS (all); i++)
    {
      if (g_ascii_strcasecmp (name, rnkg_alphabet_name (all[i])) == 0)
        {
          *out = all[i];
          return TRUE;
        }
    }

  return FALSE;
}

double
rnkg_strength_bits (RnkgAlphabet a, guint length)
{
  return length * log2 ((double) strlen (rnkg_alphabet_chars (a)));
}

char *
rnkg_generate_password (RnkgExtractor *x,
                        RnkgAlphabet   alphabet,
                        guint          length,
                        GError       **error)
{
  const char  *chars = rnkg_alphabet_chars (alphabet);
  const gsize  m     = strlen (chars);
  /* Rejection threshold, kept in a guint: for alphabets whose size divides
   * 256 (hex, base64) the value *is* 256, which a guint8 would wrap to 0 and
   * reject every byte forever. */
  const guint  limit = 256 - (256 % m);
  char        *out;
  guint        filled = 0;

  g_return_val_if_fail (x != NULL, NULL);
  g_return_val_if_fail (length > 0, NULL);

  out = gcry_malloc_secure (length + 1);
  if (out == NULL)
    {
      g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                           "cannot allocate secure memory for the password");
      return NULL;
    }

  while (filled < length)
    {
      guint8 block[64];

      if (!rnkg_extractor_squeeze (x, block, sizeof block, error))
        {
          rnkg_secure_string_free (out);
          return NULL;
        }

      for (gsize i = 0; i < sizeof block && filled < length; i++)
        {
          if ((guint) block[i] < limit)
            out[filled++] = chars[block[i] % m];
        }

      explicit_bzero (block, sizeof block);
    }

  out[length] = '\0';
  return out;
}

gboolean
rnkg_generate_key (RnkgExtractor *x, guint8 *out, gsize len, GError **error)
{
  return rnkg_extractor_squeeze (x, out, len, error);
}

void
rnkg_secure_string_free (char *s)
{
  if (s == NULL)
    return;

  explicit_bzero (s, strlen (s));
  gcry_free (s);
}
