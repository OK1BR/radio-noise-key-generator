/* rnkg-source.c — see rnkg-source.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rnkg-source.h"

#include <stdio.h>
#include <string.h>

#ifdef HAVE_LIBRTLSDR
#include <gcrypt.h>
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
  guint32       sample_rate;
  gboolean      wedged;      /* a read timed out; the device is untouchable */
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

/* One collection block of samples discarded right after tuning: the first
 * reads come from a tuner that has not settled yet — a transient, not
 * noise.  Doubles as proof at open time that the device delivers samples
 * at all. */
#define WARMUP_BYTES (256 * 1024)

/* rtlsdr_read_sync() on a wedged device blocks forever (librtlsdr submits
 * its bulk transfer with an infinite timeout), so every read runs in its
 * own thread and the caller waits with a deadline.  The task owns its
 * buffer and is reference counted: if the deadline passes, the caller
 * walks away and the stuck thread keeps only memory it owns, so it can
 * never scribble into a buffer someone else has freed. */
typedef struct {
  rtlsdr_dev_t *dev;
  guint8       *buf;        /* secure memory; gcry_free() wipes it */
  gsize         len;
  GMutex        lock;
  GCond         cond;
  gboolean      finished;
  const char   *fail;       /* static message, NULL on success */
  gint          refs;
} ReadTask;

static void
read_task_unref (ReadTask *t)
{
  if (!g_atomic_int_dec_and_test (&t->refs))
    return;

  gcry_free (t->buf);
  g_mutex_clear (&t->lock);
  g_cond_clear (&t->cond);
  g_free (t);
}

static gpointer
read_task_run (gpointer data)
{
  ReadTask   *t    = data;
  const char *fail = NULL;
  gsize       done = 0;

  while (done < t->len)
    {
      int n_read = 0;
      /* read_sync wants an int length; chunk it so a large request cannot
       * overflow. */
      const int want = (int) MIN (t->len - done, (gsize) (16 * 16384));

      if (rtlsdr_read_sync (t->dev, t->buf + done, want, &n_read) < 0)
        {
          fail = "rtlsdr_read_sync() failed — device removed?";
          break;
        }
      if (n_read <= 0)
        {
          fail = "the device returned no samples";
          break;
        }
      done += n_read;
    }

  g_mutex_lock (&t->lock);
  t->fail     = fail;
  t->finished = TRUE;
  g_cond_signal (&t->cond);
  g_mutex_unlock (&t->lock);

  read_task_unref (t);
  return NULL;
}

static gboolean
watchdog_read (RnkgSource *s, guint8 *buf, gsize len, GError **error)
{
  ReadTask *t;
  GThread  *thread;
  gint64    deadline;
  gboolean  finished;
  const char *fail;

  if (s->wedged)
    {
      g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                           "the device stalled earlier in this run");
      return FALSE;
    }

  t = g_new0 (ReadTask, 1);
  t->dev  = s->dev;
  t->len  = len;
  t->refs = 2;
  g_mutex_init (&t->lock);
  g_cond_init (&t->cond);

  t->buf = gcry_malloc_secure (len);
  if (t->buf == NULL)
    {
      g_mutex_clear (&t->lock);
      g_cond_clear (&t->cond);
      g_free (t);
      g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                           "cannot allocate a secure read buffer");
      return FALSE;
    }

  thread = g_thread_new ("rnkg-read", read_task_run, t);

  /* Four times the stream time for `len` bytes (each complex sample is two
   * of them), plus a flat second for USB latency.  A healthy block takes
   * ~64 ms; only a wedged device gets anywhere near this. */
  deadline = g_get_monotonic_time ()
           + 1000 * (1000 + 4000 * (gint64) len / (2 * s->sample_rate));

  g_mutex_lock (&t->lock);
  while (!t->finished)
    if (!g_cond_wait_until (&t->cond, &t->lock, deadline))
      break;
  finished = t->finished;
  fail     = t->fail;
  g_mutex_unlock (&t->lock);

  if (!finished)
    {
      s->wedged = TRUE;
      g_thread_unref (thread);
      read_task_unref (t);
      g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_FAILED,
                           "the device stopped delivering samples — "
                           "unplug it and start again");
      return FALSE;
    }

  g_thread_join (thread);
  if (fail == NULL)
    memcpy (buf, t->buf, len);
  else
    g_set_error_literal (error, RNKG_ERROR, RNKG_ERROR_FAILED, fail);
  read_task_unref (t);

  return fail == NULL;
}

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
  s->sample_rate = sample_rate;

  /* The watchdog buffers live in secure memory, and the warm-up below is
   * the first read — make sure libgcrypt is up before either. */
  if (!rnkg_crypto_init (error))
    {
      rtlsdr_close (s->dev);
      g_free (s);
      return NULL;
    }

  /* Warm-up: discard the first block outright.  The tuner has not settled
   * this soon after tuning, and what it emits is a transient, not noise. */
  {
    guint8  *scratch = g_malloc (WARMUP_BYTES);
    gboolean ok      = rnkg_source_read (s, scratch, WARMUP_BYTES, error);

    explicit_bzero (scratch, WARMUP_BYTES);
    g_free (scratch);
    if (!ok)
      {
        g_prefix_error (error, "during warm-up: ");
        if (!s->wedged)   /* a wedged device still owns a stuck thread */
          rtlsdr_close (s->dev);
        g_free (s);
        return NULL;
      }
  }

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
  return watchdog_read (s, buf, len, error);
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
  /* A wedged device still has a thread blocked inside rtlsdr_read_sync();
   * closing under it is a use-after-free in the library.  Leak the handle
   * deliberately — the device needs a replug anyway. */
  if (s->kind == SOURCE_RTLSDR && s->dev != NULL && !s->wedged)
    rtlsdr_close (s->dev);
#endif
  if (s->kind == SOURCE_FILE && s->fp != NULL && s->fp != stdin)
    fclose (s->fp);

  g_free (s->description);
  g_free (s);
}
