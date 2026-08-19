/* window.c — the main window, M2 (docs/SCOPE.md §2).
 *
 * The dongle is opened and read on a worker thread — opening includes a
 * warm-up read and every block read takes ~64 ms, none of which belongs
 * on the main loop.  Worker and window share a refcounted context, so
 * whichever side stops last frees it and the worker can never touch a
 * dead widget: the window only reads the context from a UI timer, the
 * worker only writes it under the lock.
 *
 * The worker also runs the engine's health tests, structure tests and
 * MCV estimate on every block, so the monitor row shows the same
 * verdicts the CLI would produce.  A failed health test is terminal for
 * that worker (§4.3): the spectrum keeps the last trace, the monitor
 * shows which test failed, and only Retune — a fresh source, fresh
 * health state — starts it again.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "monitor-view.h"
#include "rnkg-collect.h"
#include "rnkg-source.h"
#include "rnkg-spectrum.h"
#include "spectrum-view.h"

#define UI_REFRESH_MS 50   /* 20 Hz; blocks arrive at ~15 Hz */

/* Retune closes the old source on the worker that owns it, so the new
 * worker may find the device still busy for a moment. */
#define OPEN_RETRIES  12
#define OPEN_RETRY_US (250 * 1000)

typedef struct {
  guint   device;
  double  freq_mhz;
  double  rate_msps;
  double  gain_db;     /* < 0 means the tuner's maximum */
} SourceParams;

typedef struct {
  GMutex          lock;
  SourceParams    params;
  RnkgSpectrum    spectrum;
  RnkgMonitorData monitor;
  char           *description;   /* set once the source is open */
  char           *error;         /* set when the source fails */
  gboolean        running;       /* cleared by the window to stop the worker */
  gint            refs;
} SharedState;

static SharedState *
shared_state_new (const SourceParams *params)
{
  SharedState *st = g_new0 (SharedState, 1);

  g_mutex_init (&st->lock);
  st->params = *params;
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
  RnkgMonitorView  *monitor_view;
  AdwStatusPage    *status_page;
  GtkStack         *stack;
  AdwWindowTitle   *title;

  GtkDropDown   *device_drop;
  GtkSpinButton *freq_spin;
  GtkSpinButton *gain_spin;
  GtkDropDown   *rate_drop;

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

static gboolean
worker_keep_going (SharedState *st)
{
  gboolean keep;

  g_mutex_lock (&st->lock);
  keep = st->running;
  g_mutex_unlock (&st->lock);
  return keep;
}

static gpointer
worker_run (gpointer data)
{
  SharedState *st     = data;
  GError      *error  = NULL;
  RnkgSource  *source = NULL;
  guint8      *block  = NULL;
  RnkgHealth   health;
  SourceParams p;

  g_mutex_lock (&st->lock);
  p = st->params;
  g_mutex_unlock (&st->lock);

  for (guint attempt = 0; source == NULL; attempt++)
    {
      source = rnkg_source_rtlsdr_new (p.device,
                                       (guint32) (p.freq_mhz * 1e6),
                                       (guint32) (p.rate_msps * 1e6),
                                       p.gain_db < 0.0
                                         ? RNKG_DEFAULT_GAIN_AUTO
                                         : (gint) (p.gain_db * 10.0),
                                       &error);
      if (source != NULL)
        break;
      if (attempt >= OPEN_RETRIES || !worker_keep_going (st))
        {
          worker_set_error (st, g_steal_pointer (&error));
          shared_state_unref (st);
          return NULL;
        }
      g_clear_error (&error);
      g_usleep (OPEN_RETRY_US);
    }

  g_mutex_lock (&st->lock);
  st->description = g_strdup (rnkg_source_describe (source));
  g_mutex_unlock (&st->lock);

  /* Fresh source, fresh health state — a reopen is a restart in the
   * §4.3 sense, nothing latched carries over. */
  rnkg_health_init (&health, RNKG_ASSESSED_H, FALSE);

  block = g_malloc (RNKG_BLOCK_BYTES);

  while (worker_keep_going (st))
    {
      RnkgHealthVerdict verdict;
      RnkgStructure     structure;
      RnkgEstimate      estimate;

      if (!rnkg_source_read (source, block, RNKG_BLOCK_BYTES, &error))
        {
          worker_set_error (st, g_steal_pointer (&error));
          break;
        }

      verdict = rnkg_health_push_block (&health, block, RNKG_BLOCK_BYTES);
      rnkg_structure_analyse (block, RNKG_BLOCK_BYTES, &structure);
      if (verdict == RNKG_HEALTH_OK)
        verdict = rnkg_structure_verdict (&structure);
      rnkg_estimate_mcv (block, RNKG_BLOCK_BYTES, &estimate);

      g_mutex_lock (&st->lock);
      rnkg_spectrum_update (&st->spectrum, block, RNKG_BLOCK_BYTES);
      st->monitor.verdict   = verdict;
      st->monitor.structure = structure;
      st->monitor.estimate  = estimate;
      st->monitor.blocks++;
      st->monitor.valid     = TRUE;
      g_mutex_unlock (&st->lock);

      /* Terminal: keep the evidence on screen, stop feeding it. */
      if (verdict != RNKG_HEALTH_OK)
        break;
    }

  g_free (block);
  rnkg_source_free (source);
  shared_state_unref (st);
  return NULL;
}

/* --- window -------------------------------------------------------------- */

static gboolean on_refresh (gpointer data);

static void
window_start_worker (RnkgWindow *self)
{
  static const double rates[] = { 0.25, 1.024, 2.048, 2.4 };
  SourceParams p;
  GThread     *worker;

  p.device    = (guint) gtk_drop_down_get_selected (self->device_drop);
  p.freq_mhz  = gtk_spin_button_get_value (self->freq_spin);
  p.rate_msps = rates[gtk_drop_down_get_selected (self->rate_drop)];
  p.gain_db   = gtk_spin_button_get_value (self->gain_spin);

  if (self->state != NULL)
    {
      g_mutex_lock (&self->state->lock);
      self->state->running = FALSE;
      g_mutex_unlock (&self->state->lock);
      g_clear_pointer (&self->state, shared_state_unref);
    }

  self->state = shared_state_new (&p);
  worker = g_thread_new ("rnkg-monitor", worker_run, self->state);
  g_thread_unref (worker);

  rnkg_spectrum_view_set_tuning (self->spectrum_view, p.freq_mhz, p.rate_msps);
  gtk_stack_set_visible_child_name (self->stack, "opening");
  adw_window_title_set_subtitle (self->title, NULL);

  if (self->refresh_id == 0)
    self->refresh_id = g_timeout_add (UI_REFRESH_MS, on_refresh, self);
}

static gboolean
on_refresh (gpointer data)
{
  RnkgWindow  *self = RNKG_WINDOW (data);
  SharedState *st   = self->state;
  RnkgSpectrum    spectrum;
  RnkgMonitorData monitor;
  g_autofree char *description = NULL;
  g_autofree char *error = NULL;

  g_mutex_lock (&st->lock);
  spectrum    = st->spectrum;
  monitor     = st->monitor;
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

  if (spectrum.has_data)
    {
      gtk_stack_set_visible_child_name (self->stack, "spectrum");
      rnkg_spectrum_view_update (self->spectrum_view, &spectrum);
      rnkg_monitor_view_update (self->monitor_view, &monitor);
    }

  return G_SOURCE_CONTINUE;
}

static void
on_retune (GtkButton *button, gpointer data)
{
  (void) button;

  window_start_worker (RNKG_WINDOW (data));
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

static GtkWidget *
labelled (const char *caption, GtkWidget *control)
{
  GtkWidget *box   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *title = gtk_label_new (caption);

  gtk_widget_add_css_class (title, "dim-label");
  gtk_box_append (GTK_BOX (box), title);
  gtk_box_append (GTK_BOX (box), control);
  return box;
}

static void
build_source_bar (RnkgWindow *self, GtkWidget *bar)
{
  GtkWidget  *retune;
  GtkStringList *devices = gtk_string_list_new (NULL);
  const guint n = rnkg_source_device_count ();

  for (guint i = 0; i < n; i++)
    {
      g_autofree char *name = rnkg_source_device_name (i);
      g_autofree char *item =
          g_strdup_printf ("%u: %s", i, name != NULL ? name : "(unnamed)");

      gtk_string_list_append (devices, item);
    }
  if (n == 0)
    gtk_string_list_append (devices, "no device");

  self->device_drop = GTK_DROP_DOWN (
      gtk_drop_down_new (G_LIST_MODEL (devices), NULL));

  self->freq_spin = GTK_SPIN_BUTTON (
      gtk_spin_button_new_with_range (24.0, 1766.0, 0.1));
  gtk_spin_button_set_digits (self->freq_spin, 3);
  gtk_spin_button_set_value (self->freq_spin, RNKG_DEFAULT_FREQ_HZ / 1e6);

  self->gain_spin = GTK_SPIN_BUTTON (
      gtk_spin_button_new_with_range (0.0, 49.6, 0.1));
  gtk_spin_button_set_digits (self->gain_spin, 1);
  gtk_spin_button_set_value (self->gain_spin, 49.6);

  {
    const char *rates[] = { "0.25", "1.024", "2.048", "2.4", NULL };

    self->rate_drop = GTK_DROP_DOWN (gtk_drop_down_new_from_strings (rates));
    gtk_drop_down_set_selected (self->rate_drop, 2);   /* 2.048 MS/s */
  }

  retune = gtk_button_new_with_label ("Retune");
  gtk_widget_add_css_class (retune, "suggested-action");
  g_signal_connect (retune, "clicked", G_CALLBACK (on_retune), self);

  gtk_box_append (GTK_BOX (bar),
                  labelled ("Device", GTK_WIDGET (self->device_drop)));
  gtk_box_append (GTK_BOX (bar),
                  labelled ("MHz", GTK_WIDGET (self->freq_spin)));
  gtk_box_append (GTK_BOX (bar),
                  labelled ("Gain dB", GTK_WIDGET (self->gain_spin)));
  gtk_box_append (GTK_BOX (bar),
                  labelled ("MS/s", GTK_WIDGET (self->rate_drop)));
  gtk_box_append (GTK_BOX (bar), retune);
}

static void
rnkg_window_init (RnkgWindow *self)
{
  GtkWidget *toolbar_view;
  GtkWidget *header;
  GtkWidget *spinner_page;
  GtkWidget *source_bar;
  GtkWidget *bottom;

  gtk_window_set_title (GTK_WINDOW (self), "Radio Noise Key Generator");
  gtk_window_set_default_size (GTK_WINDOW (self), 960, 600);

  self->title = ADW_WINDOW_TITLE (
      adw_window_title_new ("Radio Noise Key Generator", NULL));
  header = adw_header_bar_new ();
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->title));

  self->spectrum_view = RNKG_SPECTRUM_VIEW (rnkg_spectrum_view_new ());
  self->monitor_view  = RNKG_MONITOR_VIEW (rnkg_monitor_view_new ());

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

  source_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (source_bar, 12);
  gtk_widget_set_margin_end (source_bar, 12);
  gtk_widget_set_margin_top (source_bar, 6);
  gtk_widget_set_margin_bottom (source_bar, 6);
  build_source_bar (self, source_bar);

  bottom = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (bottom),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (bottom), GTK_WIDGET (self->monitor_view));
  gtk_box_append (GTK_BOX (bottom),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (bottom), source_bar);

  toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar_view), bottom);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view),
                                GTK_WIDGET (self->stack));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      toolbar_view);

  window_start_worker (self);
}

GtkWidget *
rnkg_window_new (AdwApplication *app)
{
  return g_object_new (RNKG_TYPE_WINDOW, "application", app, NULL);
}
