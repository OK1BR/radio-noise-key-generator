/* rnkg-source.c — see rnkg-source.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rnkg-source.h"

#include <stdio.h>

#ifdef HAVE_LIBRTLSDR
#include <rtl-sdr.h>
#endif

typedef enum {
  SOURCE_RTLSDR,
  SOURCE_FILE,
} SourceKind;

struct _RnkgSource {
  SourceKind kind;
  char      *description;
#ifdef HAVE_LIBRTLSDR
  rtlsdr_dev_t *dev;
#endif
  FILE *fp;
};

gboolean
rnkg_source_have_rtlsdr (void)
{
#ifdef HAVE_LIBRTLSDR
  return TRUE;
#else
  return FALSE;
#endif
}

guint
rnkg_source_device_count (void)
{
#ifdef HAVE_LIBRTLSDR
  return rtlsdr_get_device_count ();
#else
  return 0;
#endif
}

char *
rnkg_source_device_name (guint index)
{
#ifdef HAVE_LIBRTLSDR
  const char *name = rtlsdr_get_device_name (index);

  return name != NULL ? g_strdup (name) : NULL;
#else
  (void) index;
  return NULL;
#endif
}

#ifdef HAVE_LIBRTLSDR
/* Highest gain the tuner offers, in tenths of a dB.  We want the front end
 * wide open: the noise we are after is the receiver's own thermal floor, and
 * turning the gain down just replaces it with quantisation steps. */
static gint
highest_gain (rtlsdr_dev_t *dev)
{
  gint  count = rtlsdr_get_tuner_gains (dev, NULL);
  gint *gains;
  gint  best = 0;

  if (count <= 0)
    return 0;

  gains = g_new0 (gint, count);
  if (rtlsdr_get_tuner_gains (dev, gains) == count)
    best = gains[count - 1];  /* librtlsdr returns them in ascending order */
  g_free (gains);

  return best;
}
#endif

RnkgSource *
rnkg_source_rtlsdr_new (guint    device_index,
                        guint32  freq_hz,
                        guint32  sample_rate,
                        gint     gain_tenth_db,
                        GError **error)
{
#ifdef HAVE_LIBRTLSDR
  RnkgSource *s;
  int         rc;

  if (rtlsdr_get_device_count () == 0)
    {
      g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_NO_DEVICE,
                           "no RTL-SDR device found");
      return NULL;
    }

  s = g_new0 (RnkgSource, 1);
  s->kind = SOURCE_RTLSDR;

  rc = rtlsdr_open (&s->dev, device_index);
  if (rc < 0)
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_NO_DEVICE,
                   "cannot open RTL-SDR device %u (error %d) — check that no "
                   "other program holds it and that the udev rules are in place",
                   device_index, rc);
      g_free (s);
      return NULL;
    }

  if (rtlsdr_set_sample_rate (s->dev, sample_rate) < 0 ||
      rtlsdr_set_center_freq (s->dev, freq_hz) < 0)
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                   "cannot tune the device to %u Hz at %u S/s",
                   freq_hz, sample_rate);
      rtlsdr_close (s->dev);
      g_free (s);
      return NULL;
    }

  /* Manual gain, wide open.  Automatic gain control would track whatever
   * signal wanders into the passband — the opposite of what we want. */
  rtlsdr_set_tuner_gain_mode (s->dev, 1);
  rtlsdr_set_agc_mode (s->dev, 0);
  if (gain_tenth_db == RNKG_DEFAULT_GAIN_AUTO)
    gain_tenth_db = highest_gain (s->dev);
  rtlsdr_set_tuner_gain (s->dev, gain_tenth_db);

  rtlsdr_reset_buffer (s->dev);

  s->description = g_strdup_printf ("RTL-SDR #%u at %.3f MHz, %.3f MS/s, "
                                    "gain %.1f dB",
                                    device_index, freq_hz / 1e6,
                                    sample_rate / 1e6, gain_tenth_db / 10.0);
  return s;
#else
  (void) device_index; (void) freq_hz; (void) sample_rate; (void) gain_tenth_db;
  g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_NO_DEVICE,
                       "this build has no librtlsdr support");
  return NULL;
#endif
}

RnkgSource *
rnkg_source_file_new (const char *path, GError **error)
{
  RnkgSource *s;
  FILE       *fp;

  g_return_val_if_fail (path != NULL, NULL);

  fp = g_strcmp0 (path, "-") == 0 ? stdin : fopen (path, "rb");
  if (fp == NULL)
    {
      g_set_error (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                   "cannot open %s", path);
      return NULL;
    }

  s = g_new0 (RnkgSource, 1);
  s->kind        = SOURCE_FILE;
  s->fp          = fp;
  s->description = g_strdup_printf ("file %s", path);

  return s;
}

gboolean
rnkg_source_read (RnkgSource *s, guint8 *buf, gsize len, GError **error)
{
  g_return_val_if_fail (s != NULL, FALSE);
  g_return_val_if_fail (buf != NULL || len == 0, FALSE);

  if (s->kind == SOURCE_FILE)
    {
      if (fread (buf, 1, len, s->fp) != len)
        {
          g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                               "short read from the sample file");
          return FALSE;
        }
      return TRUE;
    }

#ifdef HAVE_LIBRTLSDR
  {
    gsize done = 0;

    while (done < len)
      {
        int n_read = 0;
        /* read_sync wants an int length; chunk it so a large request cannot
         * overflow, and so a stalled device fails on its own chunk. */
        const int want = (int) MIN (len - done, (gsize) (16 * 16384));

        if (rtlsdr_read_sync (s->dev, buf + done, want, &n_read) < 0)
          {
            g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                                 "rtlsdr_read_sync() failed — device removed?");
            return FALSE;
          }
        if (n_read <= 0)
          {
            g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                                 "the device returned no samples");
            return FALSE;
          }
        done += n_read;
      }

    return TRUE;
  }
#else
  g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_NO_DEVICE,
                       "this build has no librtlsdr support");
  return FALSE;
#endif
}

const char *
rnkg_source_describe (RnkgSource *s)
{
  g_return_val_if_fail (s != NULL, "");

  return s->description;
}

void
rnkg_source_free (RnkgSource *s)
{
  if (s == NULL)
    return;

#ifdef HAVE_LIBRTLSDR
  if (s->kind == SOURCE_RTLSDR && s->dev != NULL)
    rtlsdr_close (s->dev);
#endif
  if (s->kind == SOURCE_FILE && s->fp != NULL && s->fp != stdin)
    fclose (s->fp);

  g_free (s->description);
  g_free (s);
}
