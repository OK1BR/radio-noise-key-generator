/* window.c — the main window, M2 spectrum-first (docs/SCOPE.md §2).
 *
 * The dongle is opened and read on a worker thread — opening includes a
 * warm-up read and every block read takes ~64 ms, none of which belongs
 * on the main loop.  Worker and window share a refcounted context, so
 * whichever side stops last frees it and the worker can never touch a
 * dead widget: the window only reads the context from a UI timer, the
 * worker only writes it under the lock.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "rnkg-collect.h"
#include "rnkg-source.h"
#include "rnkg-spectrum.h"
#include "spectrum-view.h"

#define UI_REFRESH_MS 50   /* 20 Hz; blocks arrive at ~15 Hz */

typedef struct {
  GMutex       lock;
  RnkgSpectrum spectrum;
  char        *description;   /* set once the source is open */
  char        *error;         /* set when the source fails */
  gboolean     running;       /* cleared by the window to stop the worker */
  guint64      blocks;
  gint         refs;
} SharedState;

static SharedState *
shared_state_new (void)
{
  SharedState *st = g_new0 (SharedState, 1);

  g_mutex_init (&st->lock);
  rnkg_spectrum_init (&st->spectrum);
  st->running = TRUE;
  st->refs    = 2;   /* the window and the worker */
  return st;
}

static void
shared_state_unref (SharedState *st)
{
  if (!g_atomic_int_dec_and_test (&st->refs))
    return;

  g_mutex_clear (&st->lock);
  g_free (st->description);
  g_free (st->error);
  g_free (st);
}

struct _RnkgWindow {
  AdwApplicationWindow parent_instance;

  RnkgSpectrumView *spectrum_view;
  AdwStatusPage    *status_page;
  GtkStack         *stack;
  AdwWindowTitle   *title;

  SharedState *state;
  guint        refresh_id;
};

G_DEFINE_FINAL_TYPE (RnkgWindow, rnkg_window, ADW_TYPE_APPLICATION_WINDOW)

/* --- worker -------------------------------------------------------------- */

static void
worker_set_error (SharedState *st, GError *error)
{
  g_mutex_lock (&st->lock);
  if (st->error == NULL)
    st->error = g_strdup (error != NULL ? error->message : "unknown failure");
  g_mutex_unlock (&st->lock);
  g_clear_error (&error);
}

static gpointer
worker_run (gpointer data)
{
  SharedState *st     = data;
  GError      *error  = NULL;
  RnkgSource  *source = NULL;
  guint8      *block  = NULL;

  source = rnkg_source_rtlsdr_new (0,
                                   RNKG_DEFAULT_FREQ_HZ,
                                   RNKG_DEFAULT_SAMPLERATE,
                                   RNKG_DEFAULT_GAIN_AUTO,
                                   &error);
  if (source == NULL)
    {
      worker_set_error (st, g_steal_pointer (&error));
      shared_state_unref (st);
      return NULL;
    }

  g_mutex_lock (&st->lock);
  st->description = g_strdup (rnkg_source_describe (source));
  g_mutex_unlock (&st->lock);

  block = g_malloc (RNKG_BLOCK_BYTES);

  while (TRUE)
    {
      gboolean keep_going;

      g_mutex_lock (&st->lock);
      keep_going = st->running;
      g_mutex_unlock (&st->lock);
      if (!keep_going)
        break;

      if (!rnkg_source_read (source, block, RNKG_BLOCK_BYTES, &error))
        {
          worker_set_error (st, g_steal_pointer (&error));
          break;
        }

      g_mutex_lock (&st->lock);
      rnkg_spectrum_update (&st->spectrum, block, RNKG_BLOCK_BYTES);
      st->blocks++;
      g_mutex_unlock (&st->lock);
    }

  g_free (block);
  rnkg_source_free (source);
  shared_state_unref (st);
  return NULL;
}

/* --- window -------------------------------------------------------------- */

static gboolean
on_refresh (gpointer data)
{
  RnkgWindow  *self = RNKG_WINDOW (data);
  SharedState *st   = self->state;
  RnkgSpectrum snapshot;
  g_autofree char *description = NULL;
  g_autofree char *error = NULL;
  gboolean has_data;

  g_mutex_lock (&st->lock);
  snapshot    = st->spectrum;
  has_data    = st->spectrum.has_data;
  description = g_strdup (st->description);
  error       = g_strdup (st->error);
  g_mutex_unlock (&st->lock);

  if (error != NULL)
    {
      adw_status_page_set_description (self->status_page, error);
      gtk_stack_set_visible_child_name (self->stack, "status");
      self->refresh_id = 0;
      return G_SOURCE_REMOVE;
    }

  if (description != NULL)
    adw_window_title_set_subtitle (self->title, description);

  if (has_data)
    {
      gtk_stack_set_visible_child_name (self->stack, "spectrum");
      rnkg_spectrum_view_update (self->spectrum_view, &snapshot);
    }

  return G_SOURCE_CONTINUE;
}

static void
rnkg_window_dispose (GObject *obj)
{
  RnkgWindow *self = RNKG_WINDOW (obj);

  g_clear_handle_id (&self->refresh_id, g_source_remove);

  if (self->state != NULL)
    {
      g_mutex_lock (&self->state->lock);
      self->state->running = FALSE;
      g_mutex_unlock (&self->state->lock);
      g_clear_pointer (&self->state, shared_state_unref);
    }

  G_OBJECT_CLASS (rnkg_window_parent_class)->dispose (obj);
}

static void
rnkg_window_class_init (RnkgWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = rnkg_window_dispose;
}

static void
rnkg_window_init (RnkgWindow *self)
{
  GtkWidget *toolbar_view;
  GtkWidget *header;
  GtkWidget *spinner_page;
  GThread   *worker;

  gtk_window_set_title (GTK_WINDOW (self), "Radio Noise Key Generator");
  gtk_window_set_default_size (GTK_WINDOW (self), 900, 520);

  self->title = ADW_WINDOW_TITLE (
      adw_window_title_new ("Radio Noise Key Generator", NULL));
  header = adw_header_bar_new ();
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->title));

  self->spectrum_view = RNKG_SPECTRUM_VIEW (rnkg_spectrum_view_new ());
  rnkg_spectrum_view_set_tuning (self->spectrum_view,
                                 RNKG_DEFAULT_FREQ_HZ / 1e6,
                                 RNKG_DEFAULT_SAMPLERATE / 1e6);

  self->status_page = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (self->status_page, "dialog-error-symbolic");
  adw_status_page_set_title (self->status_page, "No noise source");

  spinner_page = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (spinner_page),
                             "Opening the dongle…");
  adw_status_page_set_description (ADW_STATUS_PAGE (spinner_page),
                                   "Tuning and discarding the warm-up "
                                   "transient");

  self->stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_add_named (self->stack, spinner_page, "opening");
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->spectrum_view),
                       "spectrum");
  gtk_stack_add_named (self->stack, GTK_WIDGET (self->status_page), "status");
  gtk_stack_set_visible_child_name (self->stack, "opening");

  toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view),
                                GTK_WIDGET (self->stack));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      toolbar_view);

  self->state = shared_state_new ();
  worker = g_thread_new ("rnkg-spectrum", worker_run, self->state);
  g_thread_unref (worker);

  self->refresh_id = g_timeout_add (UI_REFRESH_MS, on_refresh, self);
}

GtkWidget *
rnkg_window_new (AdwApplication *app)
{
  return g_object_new (RNKG_TYPE_WINDOW, "application", app, NULL);
}
