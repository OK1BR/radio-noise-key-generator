/* rnkg-extract.c — see rnkg-extract.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rnkg-extract.h"

#include <gcrypt.h>
#include <sys/random.h>
#include <string.h>

/* Domain separator, so this construction can be revised later without a new
 * version silently producing the old version's output for the same noise. */
static const char RNKG_DOMAIN[] = "cz.ok1br.rnkg/shake256/v1";

struct _RnkgExtractor {
  gcry_md_hd_t md;
  double       credited;
  gboolean     finished;
};

G_DEFINE_QUARK (rnkg-error-quark, rnkg_error)

gboolean
rnkg_crypto_init (GError **error)
{
  static gboolean done = FALSE;

  if (done)
    return TRUE;

  if (!gcry_check_version (GCRYPT_VERSION))
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                   "libgcrypt is older than the version built against (%s)",
                   GCRYPT_VERSION);
      return FALSE;
    }

  /* Secure memory: mlocked, wiped on free.  Key material never leaves it.
   * Sized to hold a collection block (RNKG_BLOCK_BYTES, 256 KiB) plus the
   * sponge state and a generated secret at the same time, with room to
   * spare — and well under the usual 8 MiB RLIMIT_MEMLOCK. */
  gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
  gcry_control (GCRYCTL_INIT_SECMEM, 2 * 1024 * 1024, 0);
  gcry_control (GCRYCTL_RESUME_SECMEM_WARN);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

  done = TRUE;
  return TRUE;
}

RnkgExtractor *
rnkg_extractor_new (GError **error)
{
  RnkgExtractor *x;
  gcry_error_t   err;

  if (!rnkg_crypto_init (error))
    return NULL;

  x = g_new0 (RnkgExtractor, 1);

  err = gcry_md_open (&x->md, GCRY_MD_SHAKE256, GCRY_MD_FLAG_SECURE);
  if (err)
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                   "cannot open SHAKE-256: %s", gcry_strerror (err));
      g_free (x);
      return NULL;
    }

  gcry_md_write (x->md, RNKG_DOMAIN, sizeof RNKG_DOMAIN - 1);

  return x;
}

void
rnkg_extractor_free (RnkgExtractor *x)
{
  if (x == NULL)
    return;

  gcry_md_close (x->md);
  g_free (x);
}

gboolean
rnkg_extractor_absorb (RnkgExtractor *x,
                       const guint8  *raw,
                       gsize          len,
                       double         entropy_bits,
                       GError       **error)
{
  g_return_val_if_fail (x != NULL, FALSE);
  g_return_val_if_fail (raw != NULL || len == 0, FALSE);

  if (x->finished)
    {
      g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_STATE,
                           "extractor is already squeezing; cannot absorb");
      return FALSE;
    }

  gcry_md_write (x->md, raw, len);
  x->credited += entropy_bits;

  return TRUE;
}

double
rnkg_extractor_credited (RnkgExtractor *x)
{
  g_return_val_if_fail (x != NULL, 0.0);

  return x->credited;
}

gboolean
rnkg_extractor_finish (RnkgExtractor *x, GError **error)
{
  guint8  seed[RNKG_KERNEL_SEED_BYTES];
  ssize_t got;

  g_return_val_if_fail (x != NULL, FALSE);

  if (x->finished)
    return TRUE;

  /* getrandom() without GRND_RANDOM: on a modern kernel the pool is already
   * seeded and this never blocks after boot. */
  got = getrandom (seed, sizeof seed, 0);
  if (got != (ssize_t) sizeof seed)
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                   "getrandom() returned %zd of %zu bytes",
                   got, sizeof seed);
      return FALSE;
    }

  gcry_md_write (x->md, seed, sizeof seed);
  explicit_bzero (seed, sizeof seed);

  x->finished = TRUE;
  return TRUE;
}

gboolean
rnkg_extractor_squeeze (RnkgExtractor *x,
                        guint8        *out,
                        gsize          len,
                        GError       **error)
{
  gcry_error_t err;

  g_return_val_if_fail (x != NULL, FALSE);
  g_return_val_if_fail (out != NULL || len == 0, FALSE);

  if (!x->finished)
    {
      g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_STATE,
                           "call rnkg_extractor_finish() before squeezing");
      return FALSE;
    }

  err = gcry_md_extract (x->md, GCRY_MD_SHAKE256, out, len);
  if (err)
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                   "SHAKE-256 extract failed: %s", gcry_strerror (err));
      return FALSE;
    }

  return TRUE;
}
