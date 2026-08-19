/* generation-view.c — see generation-view.h.
 *
 * Secrets in the UI, decided (docs/SPEC.md §8): showing candidates on
 * screen is the point of the rotation, so the label holds them in
 * ordinary heap and that is accepted — nothing reaches disk, our own
 * copies are wiped, and the clipboard clears itself.  What GTK and Pango
 * copy internally cannot be wiped; pass-for-linux made the same call.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "generation-view.h"

#include <string.h>

#define CLIPBOARD_CLEAR_S 45

/* Order matches rnkg_alphabet_parse() names. */
static const char *const alphabet_names[] = {
  "letters", "alnum", "all", "hex", "base64", NULL,
};

struct _RnkgGenerationView {
  GtkBox parent_instance;

  GtkLabel    *candidate;
  GtkButton   *snap;
  GtkButton   *copy;
  GtkScale    *length;
  GtkDropDown *alphabet;

  gboolean snapped;
  guint    clear_id;
};

G_DEFINE_FINAL_TYPE (RnkgGenerationView, rnkg_generation_view, GTK_TYPE_BOX)

/* --- clipboard ----------------------------------------------------------- */

static void
clear_check_done (GObject *source, GAsyncResult *result, gpointer data)
{
  GdkClipboard    *clipboard = GDK_CLIPBOARD (source);
  g_autofree char *ours      = data;
  g_autofree char *text      = NULL;

  text = gdk_clipboard_read_text_finish (clipboard, result, NULL);
  /* Only wipe what is still ours — the user may have copied something
   * else meanwhile, and that is not ours to destroy. */
  if (text != NULL && g_strcmp0 (text, ours) == 0)
    gdk_clipboard_set_text (clipboard, "");
  if (text != NULL)
    explicit_bzero (text, strlen (text));
  explicit_bzero (ours, strlen (ours));
}

typedef struct {
  RnkgGenerationView *view;        /* strong; keeps clear_id valid */
  GdkClipboard       *clipboard;   /* strong; outlives the widget tree */
  char               *text;
} ClearJob;

static void
clear_job_free (gpointer data)
{
  ClearJob *job = data;

  if (job->text != NULL)
    {
      explicit_bzero (job->text, strlen (job->text));
      g_free (job->text);
    }
  g_object_unref (job->clipboard);
  g_object_unref (job->view);
  g_free (job);
}

static gboolean
clear_job_fire (gpointer data)
{
  ClearJob *job = data;

  job->view->clear_id = 0;
  gdk_clipboard_read_text_async (job->clipboard, NULL, clear_check_done,
                                 g_steal_pointer (&job->text));
  return G_SOURCE_REMOVE;   /* the destroy notify frees the job */
}

static void
on_copy (GtkButton *button, gpointer data)
{
  RnkgGenerationView *self = RNKG_GENERATION_VIEW (data);
  const char *text = gtk_label_get_text (self->candidate);
  ClearJob   *job;

  (void) button;

  if (text == NULL || *text == '\0')
    return;

  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), text);

  /* One pending clear at a time; a re-copy restarts the clock. */
  g_clear_handle_id (&self->clear_id, g_source_remove);
  job = g_new0 (ClearJob, 1);
  job->view      = g_object_ref (self);
  job->clipboard = g_object_ref (gtk_widget_get_clipboard (GTK_WIDGET (self)));
  job->text      = g_strdup (text);
  self->clear_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                               CLIPBOARD_CLEAR_S,
                                               clear_job_fire, job,
                                               clear_job_free);
}

/* --- snap ---------------------------------------------------------------- */

static void
on_snap (GtkButton *button, gpointer data)
{
  RnkgGenerationView *self = RNKG_GENERATION_VIEW (data);

  self->snapped = !self->snapped;
  gtk_button_set_label (button, self->snapped ? "Rotate" : "Snap");
  gtk_widget_set_visible (GTK_WIDGET (self->copy), self->snapped);
  if (self->snapped)
    gtk_widget_remove_css_class (GTK_WIDGET (button), "suggested-action");
  else
    gtk_widget_add_css_class (GTK_WIDGET (button), "suggested-action");
}

/* --- view ---------------------------------------------------------------- */

static void
rnkg_generation_view_dispose (GObject *obj)
{
  RnkgGenerationView *self = RNKG_GENERATION_VIEW (obj);

  g_clear_handle_id (&self->clear_id, g_source_remove);

  G_OBJECT_CLASS (rnkg_generation_view_parent_class)->dispose (obj);
}

static void
rnkg_generation_view_class_init (RnkgGenerationViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = rnkg_generation_view_dispose;
}

static void
rnkg_generation_view_init (RnkgGenerationView *self)
{
  GtkWidget *controls;
  GtkWidget *label;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 6);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 12);

  /* The rotating code, front and centre. */
  label = gtk_label_new ("collecting…");
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_CHAR);
  gtk_widget_add_css_class (label, "title-2");
  gtk_widget_add_css_class (label, "monospace");
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (self), label);
  self->candidate = GTK_LABEL (label);

  controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (controls, GTK_ALIGN_CENTER);

  /* A slider, not a spin button — a much longer string should be one
   * drag, not forty clicks. */
  self->length = GTK_SCALE (
      gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                8.0, 128.0, 1.0));
  gtk_range_set_value (GTK_RANGE (self->length), 20.0);
  gtk_scale_set_digits (self->length, 0);
  gtk_scale_set_draw_value (self->length, TRUE);
  gtk_scale_set_value_pos (self->length, GTK_POS_LEFT);
  gtk_widget_set_size_request (GTK_WIDGET (self->length), 240, -1);
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->length));

  self->alphabet = GTK_DROP_DOWN (
      gtk_drop_down_new_from_strings (alphabet_names));
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->alphabet));

  self->snap = GTK_BUTTON (gtk_button_new_with_label ("Snap"));
  gtk_widget_add_css_class (GTK_WIDGET (self->snap), "suggested-action");
  g_signal_connect (self->snap, "clicked", G_CALLBACK (on_snap), self);
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->snap));

  self->copy = GTK_BUTTON (gtk_button_new_with_label ("Copy"));
  gtk_widget_set_visible (GTK_WIDGET (self->copy), FALSE);
  g_signal_connect (self->copy, "clicked", G_CALLBACK (on_copy), self);
  gtk_box_append (GTK_BOX (controls), GTK_WIDGET (self->copy));

  gtk_box_append (GTK_BOX (self), controls);
}

GtkWidget *
rnkg_generation_view_new (void)
{
  return g_object_new (RNKG_TYPE_GENERATION_VIEW, NULL);
}

void
rnkg_generation_view_set_candidate (RnkgGenerationView *self, const char *text)
{
  g_return_if_fail (RNKG_IS_GENERATION_VIEW (self));

  if (self->snapped || text == NULL)
    return;

  gtk_label_set_text (self->candidate, text);
}

gboolean
rnkg_generation_view_is_snapped (RnkgGenerationView *self)
{
  g_return_val_if_fail (RNKG_IS_GENERATION_VIEW (self), FALSE);

  return self->snapped;
}

void
rnkg_generation_view_get_params (RnkgGenerationView *self,
                                 RnkgAlphabet *alphabet, guint *length)
{
  g_return_if_fail (RNKG_IS_GENERATION_VIEW (self));

  if (alphabet != NULL)
    {
      guint selected = gtk_drop_down_get_selected (self->alphabet);

      if (selected >= G_N_ELEMENTS (alphabet_names) - 1 ||
          !rnkg_alphabet_parse (alphabet_names[selected], alphabet))
        *alphabet = RNKG_ALPHABET_LETTERS;
    }
  if (length != NULL)
    *length = (guint) gtk_range_get_value (GTK_RANGE (self->length));
}

void
rnkg_generation_view_set_params (RnkgGenerationView *self,
                                 const char *alphabet_name, guint length)
{
  g_return_if_fail (RNKG_IS_GENERATION_VIEW (self));

  for (guint i = 0; alphabet_names[i] != NULL; i++)
    if (g_strcmp0 (alphabet_names[i], alphabet_name) == 0)
      {
        gtk_drop_down_set_selected (self->alphabet, i);
        break;
      }
  gtk_range_set_value (GTK_RANGE (self->length), CLAMP (length, 8, 128));
}

const char *
rnkg_generation_view_alphabet_name (RnkgGenerationView *self)
{
  guint selected;

  g_return_val_if_fail (RNKG_IS_GENERATION_VIEW (self), "letters");

  selected = gtk_drop_down_get_selected (self->alphabet);
  return selected < G_N_ELEMENTS (alphabet_names) - 1
       ? alphabet_names[selected] : "letters";
}
