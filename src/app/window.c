/* window.c — the main window, M2 (docs/SPEC.md §8).
 *
 * One window, three parts: the spectrum (the visual check that the
 * frequency is empty), one status line (the measured numbers while the
 * source is healthy, the engine's message when it is not), and the
 * generation panel — the main act.  No source configuration: the GUI
 * runs on the engine defaults, and whoever needs a different frequency
 * has the CLI.
 *
 * The dongle is opened and read on a worker thread — opening includes a
 * warm-up read and every block read takes ~64 ms, none of which belongs
 * on the main loop.  Worker and window share a refcounted context, so
 * whichever side stops last frees it and the worker can never touch a
 * dead widget.  For every healthy block the worker also derives a fresh
 * candidate password — new extractor, this block's measured credit, the
 * unconditional kernel seed, squeeze — which is what rotates in the
 * generation panel until the user snaps it.  A failed health test is
 * terminal for the worker (§4.3); only Reopen starts a fresh one.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include <string.h>

#include "generation-view.h"
#include "rnkg-collect.h"
#include "rnkg-source.h"
#include "rnkg-spectrum.h"
#include "spectrum-view.h"

#define UI_REFRESH_MS 50   /* 20 Hz; blocks arrive at ~15 Hz */

/* Reopen races the old worker for the device, so the new one retries. */
#define OPEN_RETRIES  12
#define OPEN_RETRY_US (250 * 1000)

typedef struct {
  GMutex        lock;
  RnkgSpectrum  spectrum;
  RnkgEstimate  estimate;
  RnkgStructure structure;
  RnkgHealthVerdict verdict;
  guint64       blocks;
  gboolean      have_block;

  /* What the worker should derive, mirrored from the generation view. */
  RnkgAlphabet  alphabet;
  guint         length;
  gboolean      snapped;

  char         *candidate;     /* latest derived password, if any */
  char         *description;   /* set once the source is open */
  char         *error;         /* set when the source fails */
  gboolean      running;       /* cleared by the window to stop the worker */
  gint          refs;
} SharedState;

static SharedState *
shared_state_new (void)
{
  SharedState *st = g_new0 (SharedState, 1);

  g_mutex_init (&st->lock);
  rnkg_spectrum_init (&st->spectrum);
  st->alphabet = RNKG_ALPHABET_LETTERS;
  st->length   = 20;
  st->running  = TRUE;
  st->refs     = 2;   /* the window and the worker */
  return st;
}

static void
shared_state_unref (SharedState *st)
{
  if (!g_atomic_int_dec_and_test (&st->refs))
    return;

  g_mutex_clear (&st->lock);
  if (st->candidate != NULL)
    {
      explicit_bzero (st->candidate, strlen (st->candidate));
      g_free (st->candidate);
    }
  g_free (st->description);
  g_free (st->error);
  g_free (st);
}

struct _RnkgWindow {
  AdwApplicationWindow parent_instance;

  RnkgSpectrumView   *spectrum_view;
  RnkgGenerationView *generation_view;
  AdwStatusPage      *status_page;
  GtkStack           *stack;
  AdwWindowTitle     *title;
  GtkLabel           *status_line;
  GtkButton          *reopen;

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

/* One fresh candidate from one healthy block: its own extractor, this
 * block's measured credit, the unconditional kernel seed at finish. */
static char *
derive_candidate (const guint8 *block, const RnkgEstimate *estimate,
                  RnkgAlphabet alphabet, guint length)
{
  g_autoptr (GError) error = NULL;
  RnkgExtractor *x;
  char          *password = NULL;

  if (rnkg_estimate_credit (estimate) <
      rnkg_strength_bits (alphabet, length))
    return NULL;   /* cannot happen at sane settings, but never lie */

  x = rnkg_extractor_new (&error);
  if (x == NULL)
    return NULL;

  if (rnkg_extractor_absorb (x, block, RNKG_BLOCK_BYTES,
                             rnkg_estimate_credit (estimate), &error) &&
      rnkg_extractor_finish (x, &error))
    password = rnkg_generate_password (x, alphabet, length, &error);

  rnkg_extractor_free (x);
  return password;
}

static gpointer
worker_run (gpointer data)
{
  SharedState *st     = data;
  GError      *error  = NULL;
  RnkgSource  *source = NULL;
  guint8      *block  = NULL;
  RnkgHealth   health;

  for (guint attempt = 0; source == NULL; attempt++)
    {
      source = rnkg_source_rtlsdr_new (0,
                                       RNKG_DEFAULT_FREQ_HZ,
                                       RNKG_DEFAULT_SAMPLERATE,
                                       RNKG_DEFAULT_GAIN_AUTO,
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
      RnkgAlphabet      alphabet;
      guint             length;
      gboolean          snapped;
      char             *candidate = NULL;

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
      alphabet = st->alphabet;
      length   = st->length;
      snapped  = st->snapped;
      g_mutex_unlock (&st->lock);

      if (verdict == RNKG_HEALTH_OK && !snapped)
        candidate = derive_candidate (block, &estimate, alphabet, length);

      g_mutex_lock (&st->lock);
      rnkg_spectrum_update (&st->spectrum, block, RNKG_BLOCK_BYTES);
      st->estimate   = estimate;
      st->structure  = structure;
      st->verdict    = verdict;
      st->blocks++;
      st->have_block = TRUE;
      if (candidate != NULL)
        {
          if (st->candidate != NULL)
            {
              explicit_bzero (st->candidate, strlen (st->candidate));
              g_free (st->candidate);
            }
          st->candidate = g_strdup (candidate);
        }
      g_mutex_unlock (&st->lock);

      if (candidate != NULL)
        rnkg_secure_string_free (candidate);

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
  GThread *worker;

  if (self->state != NULL)
    {
      g_mutex_lock (&self->state->lock);
      self->state->running = FALSE;
      g_mutex_unlock (&self->state->lock);
      g_clear_pointer (&self->state, shared_state_unref);
    }

  self->state = shared_state_new ();
  worker = g_thread_new ("rnkg-worker", worker_run, self->state);
  g_thread_unref (worker);

  rnkg_spectrum_view_set_tuning (self->spectrum_view,
                                 RNKG_DEFAULT_FREQ_HZ / 1e6,
                                 RNKG_DEFAULT_SAMPLERATE / 1e6);
  gtk_stack_set_visible_child_name (self->stack, "opening");
  gtk_widget_set_visible (GTK_WIDGET (self->reopen), FALSE);
  adw_window_title_set_subtitle (self->title, NULL);

  if (self->refresh_id == 0)
    self->refresh_id = g_timeout_add (UI_REFRESH_MS, on_refresh, self);
}

static gboolean
on_refresh (gpointer data)
{
  RnkgWindow  *self = RNKG_WINDOW (data);
  SharedState *st   = self->state;
  RnkgSpectrum      spectrum;
  RnkgEstimate      estimate;
  RnkgStructure     structure;
  RnkgHealthVerdict verdict;
  guint64           blocks;
  gboolean          have_block;
  g_autofree char *candidate = NULL;
  g_autofree char *description = NULL;
  g_autofree char *error = NULL;
  RnkgAlphabet alphabet;
  guint        length;

  /* Push the panel's settings in, pull the results out. */
  rnkg_generation_view_get_params (self->generation_view, &alphabet, &length);

  g_mutex_lock (&st->lock);
  st->alphabet = alphabet;
  st->length   = length;
  st->snapped  = rnkg_generation_view_is_snapped (self->generation_view);
  spectrum    = st->spectrum;
  estimate    = st->estimate;
  structure   = st->structure;
  verdict     = st->verdict;
  blocks      = st->blocks;
  have_block  = st->have_block;
  candidate   = g_strdup (st->candidate);
  description = g_strdup (st->description);
  error       = g_strdup (st->error);
  g_mutex_unlock (&st->lock);

  if (error != NULL)
    {
      adw_status_page_set_description (self->status_page, error);
      gtk_stack_set_visible_child_name (self->stack, "status");
      gtk_widget_set_visible (GTK_WIDGET (self->reopen), TRUE);
      self->refresh_id = 0;
      return G_SOURCE_REMOVE;
    }

  if (description != NULL)
    adw_window_title_set_subtitle (self->title, description);

  if (!have_block)
    return G_SOURCE_CONTINUE;

  gtk_stack_set_visible_child_name (self->stack, "spectrum");
  rnkg_spectrum_view_update (self->spectrum_view, &spectrum);

  if (verdict == RNKG_HEALTH_OK)
    {
      g_autofree char *line =
          g_strdup_printf ("%.2f b/sample · serial %+.4f · I/Q %+.4f · "
                           "DC %+.4f · block %" G_GUINT64_FORMAT,
                           estimate.h_min, structure.serial, structure.iq,
                           structure.dc_bias, blocks);

      gtk_label_set_text (self->status_line, line);
      gtk_widget_remove_css_class (GTK_WIDGET (self->status_line), "error");
      gtk_widget_add_css_class (GTK_WIDGET (self->status_line), "dim-label");
    }
  else
    {
      gtk_label_set_text (self->status_line,
                          rnkg_health_verdict_message (verdict));
      gtk_widget_remove_css_class (GTK_WIDGET (self->status_line),
                                   "dim-label");
      gtk_widget_add_css_class (GTK_WIDGET (self->status_line), "error");
      gtk_widget_set_visible (GTK_WIDGET (self->reopen), TRUE);
      self->refresh_id = 0;
      return G_SOURCE_REMOVE;
    }

  if (candidate != NULL)
    {
      rnkg_generation_view_set_candidate (self->generation_view, candidate);
      explicit_bzero (candidate, strlen (candidate));
    }

  return G_SOURCE_CONTINUE;
}

static void
on_reopen (GtkButton *button, gpointer data)
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

static void
rnkg_window_init (RnkgWindow *self)
{
  GtkWidget *toolbar_view;
  GtkWidget *header;
  GtkWidget *spinner_page;
  GtkWidget *status_row;
  GtkWidget *bottom;

  gtk_window_set_title (GTK_WINDOW (self), "Radio Noise Key Generator");
  gtk_window_set_default_size (GTK_WINDOW (self), 900, 560);

  self->title = ADW_WINDOW_TITLE (
      adw_window_title_new ("Radio Noise Key Generator", NULL));
  header = adw_header_bar_new ();
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->title));

  self->spectrum_view   = RNKG_SPECTRUM_VIEW (rnkg_spectrum_view_new ());
  self->generation_view =
      RNKG_GENERATION_VIEW (rnkg_generation_view_new ());

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

  self->status_line = GTK_LABEL (gtk_label_new ("collecting…"));
  gtk_widget_add_css_class (GTK_WIDGET (self->status_line), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->status_line), "numeric");
  gtk_label_set_wrap (self->status_line, TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->status_line), TRUE);

  self->reopen = GTK_BUTTON (gtk_button_new_with_label ("Reopen"));
  gtk_widget_set_visible (GTK_WIDGET (self->reopen), FALSE);
  g_signal_connect (self->reopen, "clicked", G_CALLBACK (on_reopen), self);

  status_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (status_row, 12);
  gtk_widget_set_margin_end (status_row, 12);
  gtk_widget_set_margin_top (status_row, 6);
  gtk_widget_set_margin_bottom (status_row, 6);
  gtk_box_append (GTK_BOX (status_row), GTK_WIDGET (self->status_line));
  gtk_box_append (GTK_BOX (status_row), GTK_WIDGET (self->reopen));

  bottom = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (bottom),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (bottom), status_row);
  gtk_box_append (GTK_BOX (bottom),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (bottom), GTK_WIDGET (self->generation_view));

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
