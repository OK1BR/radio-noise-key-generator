/* window.c — the main window, M2 (docs/SPEC.md §8).
 *
 * One window: a spectrum strip (informative, deliberately small — its
 * only job is the visual check that the frequency is empty), one status
 * line, and the generation panel as the main act.  Source settings live
 * behind the hamburger menu, family style; the window itself carries no
 * configuration.
 *
 * The dongle is opened and read on a worker thread — opening includes a
 * warm-up read and every block read takes ~64 ms, none of which belongs
 * on the main loop.  Worker and window share a refcounted context, so
 * whichever side stops last frees it and the worker can never touch a
 * dead widget.  For every healthy block the worker also derives a fresh
 * candidate password — new extractor, this block's measured credit, the
 * unconditional kernel seed — which is what rotates in the generation
 * panel until the user snaps it.  A failed health test is terminal for
 * the worker (§4.3); Reopen or changed settings start a fresh one.
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

#define SPECTRUM_HEIGHT 200   /* informative strip, not the main act */

typedef struct {
  guint  device;
  double freq_mhz;
  double rate_msps;
  double gain_db;     /* < 0 means the tuner's maximum */
} SourceParams;

typedef struct {
  GMutex        lock;
  SourceParams  params;
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
shared_state_new (const SourceParams *params)
{
  SharedState *st = g_new0 (SharedState, 1);

  g_mutex_init (&st->lock);
  st->params = *params;
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

  SourceParams params;
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

  self->state = shared_state_new (&self->params);
  worker = g_thread_new ("rnkg-worker", worker_run, self->state);
  g_thread_unref (worker);

  rnkg_spectrum_view_set_tuning (self->spectrum_view,
                                 self->params.freq_mhz,
                                 self->params.rate_msps);
  gtk_stack_set_visible_child_name (self->stack, "opening");
  gtk_widget_set_visible (GTK_WIDGET (self->reopen), FALSE);
  gtk_label_set_text (self->status_line, "collecting…");
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

/* --- settings ------------------------------------------------------------ */

typedef struct {
  RnkgWindow  *window;   /* strong ref held for the dialog's lifetime */
  AdwComboRow *device;
  AdwSpinRow  *freq;
  AdwSwitchRow *max_gain;
  AdwSpinRow  *gain;
  AdwComboRow *rate;
} SettingsRows;

static const double rate_values[] = { 0.25, 1.024, 2.048, 2.4 };

static void
settings_closed (AdwDialog *dialog, gpointer data)
{
  SettingsRows *rows = data;
  RnkgWindow   *self = rows->window;
  SourceParams  p;

  (void) dialog;

  p.device    = (guint) adw_combo_row_get_selected (rows->device);
  p.freq_mhz  = adw_spin_row_get_value (rows->freq);
  p.rate_msps = rate_values[adw_combo_row_get_selected (rows->rate)];
  p.gain_db   = adw_switch_row_get_active (rows->max_gain)
              ? -1.0
              : adw_spin_row_get_value (rows->gain);

  if (p.device != self->params.device ||
      p.freq_mhz != self->params.freq_mhz ||
      p.rate_msps != self->params.rate_msps ||
      p.gain_db != self->params.gain_db)
    {
      self->params = p;
      window_start_worker (self);
    }

  g_object_unref (self);
  g_free (rows);
}

static void
act_settings (GSimpleAction *action, GVariant *param, gpointer data)
{
  RnkgWindow   *self = data;
  SettingsRows *rows = g_new0 (SettingsRows, 1);
  AdwDialog    *dlg  = adw_preferences_dialog_new ();
  GtkWidget    *page = adw_preferences_page_new ();
  GtkWidget    *group = adw_preferences_group_new ();
  GtkStringList *devices = gtk_string_list_new (NULL);
  const guint    n_devices = rnkg_source_device_count ();

  (void) action;
  (void) param;

  rows->window = g_object_ref (self);

  adw_dialog_set_title (dlg, "Settings");
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), "Source");
  adw_preferences_group_set_description (
      ADW_PREFERENCES_GROUP (group),
      "Applied when the dialog closes. The defaults are chosen for noise, "
      "not for listening; the health tests judge any change.");

  for (guint i = 0; i < n_devices; i++)
    {
      g_autofree char *name = rnkg_source_device_name (i);
      g_autofree char *item =
          g_strdup_printf ("%u: %s", i, name != NULL ? name : "(unnamed)");

      gtk_string_list_append (devices, item);
    }
  if (n_devices == 0)
    gtk_string_list_append (devices, "no device found");

  rows->device = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (rows->device),
                                 "Device");
  adw_combo_row_set_model (rows->device, G_LIST_MODEL (devices));
  g_object_unref (devices);
  adw_combo_row_set_selected (rows->device,
                              MIN (self->params.device,
                                   n_devices > 0 ? n_devices - 1 : 0));

  rows->freq = ADW_SPIN_ROW (
      adw_spin_row_new_with_range (24.0, 1766.0, 0.1));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (rows->freq),
                                 "Frequency (MHz)");
  adw_spin_row_set_digits (rows->freq, 3);
  adw_spin_row_set_value (rows->freq, self->params.freq_mhz);

  rows->max_gain = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (rows->max_gain),
                                 "Maximum gain");
  adw_switch_row_set_active (rows->max_gain, self->params.gain_db < 0.0);

  rows->gain = ADW_SPIN_ROW (adw_spin_row_new_with_range (0.0, 49.6, 0.1));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (rows->gain),
                                 "Gain (dB)");
  adw_spin_row_set_digits (rows->gain, 1);
  adw_spin_row_set_value (rows->gain,
                          self->params.gain_db < 0.0
                            ? 49.6 : self->params.gain_db);
  g_object_bind_property (rows->max_gain, "active",
                          rows->gain, "sensitive",
                          G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);

  {
    const char *rate_names[] = { "0.25", "1.024", "2.048", "2.4", NULL };
    GtkStringList *rates = gtk_string_list_new (rate_names);
    guint selected = 2;

    for (guint i = 0; i < G_N_ELEMENTS (rate_values); i++)
      if (self->params.rate_msps == rate_values[i])
        selected = i;

    rows->rate = ADW_COMBO_ROW (adw_combo_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (rows->rate),
                                   "Sample rate (MS/s)");
    adw_combo_row_set_model (rows->rate, G_LIST_MODEL (rates));
    g_object_unref (rates);
    adw_combo_row_set_selected (rows->rate, selected);
  }

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                             GTK_WIDGET (rows->device));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                             GTK_WIDGET (rows->freq));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                             GTK_WIDGET (rows->max_gain));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                             GTK_WIDGET (rows->gain));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                             GTK_WIDGET (rows->rate));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (group));
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dlg),
                              ADW_PREFERENCES_PAGE (page));

  g_signal_connect (dlg, "closed", G_CALLBACK (settings_closed), rows);
  adw_dialog_present (dlg, GTK_WIDGET (self));
}

/* --- about --------------------------------------------------------------- */

static void
act_about (GSimpleAction *action, GVariant *param, gpointer data)
{
  RnkgWindow *self = data;
  AdwDialog  *dlg  = adw_about_dialog_new ();
  const char *renderer = g_getenv ("GSK_RENDERER");
  /* Runtime (not compile-time) library versions — see sdr-for-linux. */
  g_autofree char *dbg = g_strdup_printf (
      "Radio Noise Key Generator %s\n"
      "GTK %u.%u.%u, libadwaita %u.%u.%u, GSK_RENDERER=%s\n"
      "librtlsdr support: %s",
      RNKG_VERSION,
      gtk_get_major_version (), gtk_get_minor_version (),
      gtk_get_micro_version (),
      adw_get_major_version (), adw_get_minor_version (),
      adw_get_micro_version (),
      renderer ? renderer : "default",
      rnkg_source_have_rtlsdr () ? "yes" : "no");

  (void) action;
  (void) param;
  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dlg),
                                         "Radio Noise Key Generator");
  /* Resolves against the installed hicolor icon once M3 lands. */
  adw_about_dialog_set_application_icon (
      ADW_ABOUT_DIALOG (dlg), "cz.ok1br.radio_noise_key_generator");
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dlg), RNKG_VERSION);
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dlg),
                                       "Richard Fakenberg, OK1BR");
  adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (dlg),
      "Passwords and keys from RTL-SDR noise — never weaker than "
      "getrandom(), with the radio's contribution measured, not assumed.");
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dlg),
                                  "© 2026 Richard Fakenberg, OK1BR");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dlg),
                                     GTK_LICENSE_GPL_3_0);
  adw_about_dialog_set_website (
      ADW_ABOUT_DIALOG (dlg),
      "https://github.com/OK1BR/radio-noise-key-generator");
  adw_about_dialog_set_issue_url (
      ADW_ABOUT_DIALOG (dlg),
      "https://github.com/OK1BR/radio-noise-key-generator/issues");
  adw_about_dialog_set_debug_info (ADW_ABOUT_DIALOG (dlg), dbg);
  adw_dialog_present (dlg, GTK_WIDGET (self));
}

static const GActionEntry win_actions[] = {
  { .name = "settings", .activate = act_settings },
  { .name = "about", .activate = act_about },
};

/* --- construction -------------------------------------------------------- */

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
  GtkWidget *menu_btn;
  GMenu     *menu;
  GtkWidget *spinner_page;
  GtkWidget *status_row;
  GtkWidget *content;

  self->params = (SourceParams) {
    .device    = 0,
    .freq_mhz  = RNKG_DEFAULT_FREQ_HZ / 1e6,
    .rate_msps = RNKG_DEFAULT_SAMPLERATE / 1e6,
    .gain_db   = -1.0,   /* the tuner's maximum */
  };

  gtk_window_set_title (GTK_WINDOW (self), "Radio Noise Key Generator");
  gtk_window_set_default_size (GTK_WINDOW (self), 820, 560);

  g_action_map_add_action_entries (G_ACTION_MAP (self), win_actions,
                                   G_N_ELEMENTS (win_actions), self);

  self->title = ADW_WINDOW_TITLE (
      adw_window_title_new ("Radio Noise Key Generator", NULL));
  header = adw_header_bar_new ();
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->title));

  menu = g_menu_new ();
  g_menu_append (menu, "_Settings…", "win.settings");
  g_menu_append (menu, "_About Radio Noise Key Generator", "win.about");
  menu_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn),
                                 "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_btn),
                                  G_MENU_MODEL (menu));
  g_object_unref (menu);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_btn);

  self->spectrum_view   = RNKG_SPECTRUM_VIEW (rnkg_spectrum_view_new ());
  self->generation_view =
      RNKG_GENERATION_VIEW (rnkg_generation_view_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->generation_view), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (self->generation_view),
                         GTK_ALIGN_CENTER);

  self->status_page = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (self->status_page, "dialog-error-symbolic");
  adw_status_page_set_title (self->status_page, "No noise source");

  spinner_page = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (spinner_page),
                             "Opening the dongle…");

  self->stack = GTK_STACK (gtk_stack_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->stack), -1,
                               SPECTRUM_HEIGHT);
  gtk_widget_set_vexpand (GTK_WIDGET (self->stack), FALSE);
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

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->stack));
  gtk_box_append (GTK_BOX (content),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (content), status_row);
  gtk_box_append (GTK_BOX (content),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->generation_view));

  toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), content);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      toolbar_view);

  window_start_worker (self);
}

GtkWidget *
rnkg_window_new (AdwApplication *app)
{
  return g_object_new (RNKG_TYPE_WINDOW, "application", app, NULL);
}
