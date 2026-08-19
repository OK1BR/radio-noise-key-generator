/* rnkg-collect.c — see rnkg-collect.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rnkg-collect.h"

#include <gcrypt.h>

struct _RnkgCollector {
  RnkgSource    *source;
  RnkgExtractor *extractor;
  RnkgHealth     health;
  guint8        *block;
  double         target_bits;
  guint64        blocks;
  guint64        bytes;
  RnkgEstimate   last_estimate;
  RnkgStructure  last_structure;
  gboolean       sealed;
};

RnkgCollector *
rnkg_collector_new (RnkgSource *source, double target_bits, GError **error)
{
  RnkgCollector *c;

  g_return_val_if_fail (source != NULL, NULL);

  c = g_new0 (RnkgCollector, 1);
  c->source      = source;
  c->target_bits = target_bits;

  c->extractor = rnkg_extractor_new (error);
  if (c->extractor == NULL)
    {
      g_free (c);
      return NULL;
    }

  /* Raw I/Q bytes take 256 values, so the source is not binary. */
  rnkg_health_init (&c->health, RNKG_ASSESSED_H, FALSE);

  c->block = gcry_malloc_secure (RNKG_BLOCK_BYTES);
  if (c->block == NULL)
    {
      g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                           "cannot allocate a secure sample buffer");
      rnkg_extractor_free (c->extractor);
      g_free (c);
      return NULL;
    }

  return c;
}

void
rnkg_collector_free (RnkgCollector *c)
{
  if (c == NULL)
    return;

  if (c->block != NULL)
    {
      explicit_bzero (c->block, RNKG_BLOCK_BYTES);
      gcry_free (c->block);
    }
  rnkg_extractor_free (c->extractor);
  rnkg_source_free (c->source);
  g_free (c);
}

gboolean
rnkg_collector_step (RnkgCollector *c, GError **error)
{
  RnkgHealthVerdict v;

  g_return_val_if_fail (c != NULL, FALSE);

  if (c->sealed)
    {
      g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_STATE,
                           "collection is already sealed");
      return FALSE;
    }

  if (!rnkg_source_read (c->source, c->block, RNKG_BLOCK_BYTES, error))
    return FALSE;

  /* Approved continuous tests first — they are the ones that catch a dead
   * or stuck source, and there is no point estimating entropy of garbage. */
  v = rnkg_health_push_block (&c->health, c->block, RNKG_BLOCK_BYTES);
  if (v != RNKG_HEALTH_OK)
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_HEALTH,
                   "%s", rnkg_health_verdict_message (v));
      return FALSE;
    }

  /* Then the structure tests, which are what catch a carrier. */
  rnkg_structure_analyse (c->block, RNKG_BLOCK_BYTES, &c->last_structure);
  v = rnkg_structure_verdict (&c->last_structure);
  if (v != RNKG_HEALTH_OK)
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_HEALTH,
                   "%s (serial %.4f, I/Q %.4f)",
                   rnkg_health_verdict_message (v),
                   c->last_structure.serial, c->last_structure.iq);
      return FALSE;
    }

  rnkg_estimate_mcv (c->block, RNKG_BLOCK_BYTES, &c->last_estimate);

  if (!rnkg_extractor_absorb (c->extractor, c->block, RNKG_BLOCK_BYTES,
                              rnkg_estimate_credit (&c->last_estimate), error))
    return FALSE;

  c->blocks++;
  c->bytes += RNKG_BLOCK_BYTES;

  return TRUE;
}

gboolean
rnkg_collector_done (RnkgCollector *c)
{
  g_return_val_if_fail (c != NULL, FALSE);

  return rnkg_extractor_credited (c->extractor) >= c->target_bits;
}

void
rnkg_collector_progress (RnkgCollector *c, RnkgProgress *out)
{
  g_return_if_fail (c != NULL);
  g_return_if_fail (out != NULL);

  *out = (RnkgProgress) {
    .blocks         = c->blocks,
    .bytes          = c->bytes,
    .credited_bits  = rnkg_extractor_credited (c->extractor),
    .target_bits    = c->target_bits,
    .last_estimate  = c->last_estimate,
    .last_structure = c->last_structure,
  };
}

RnkgExtractor *
rnkg_collector_finish (RnkgCollector *c, GError **error)
{
  g_return_val_if_fail (c != NULL, NULL);

  if (!rnkg_collector_done (c))
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_INSUFFICIENT_ENTROPY,
                   "collected %.0f bits of the %.0f required",
                   rnkg_extractor_credited (c->extractor), c->target_bits);
      return NULL;
    }

  if (!c->sealed)
    {
      if (!rnkg_extractor_finish (c->extractor, error))
        return NULL;
      c->sealed = TRUE;
    }

  return c->extractor;
}
