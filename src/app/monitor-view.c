/* monitor-view.c — see monitor-view.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "monitor-view.h"

struct _RnkgMonitorView {
  GtkBox parent_instance;

  GtkLabel *rct;
  GtkLabel *apt;
  GtkLabel *serial;
  GtkLabel *iq;
  GtkLabel *dc;
  GtkLabel *entropy;
  GtkLabel *blocks;
  GtkLabel *message;   /* the engine's human message on failure */
};

G_DEFINE_FINAL_TYPE (RnkgMonitorView, rnkg_monitor_view, GTK_TYPE_BOX)

/* An indicator flips between the neutral "dim-label" look and the Adwaita
 * success/error colours; keeping the class sets disjoint makes the flips
 * idempotent. */
static void
set_state (GtkLabel *label, const char *text, const char *state_class)
{
  gtk_label_set_text (label, text);
  gtk_widget_remove_css_class (GTK_WIDGET (label), "success");
  gtk_widget_remove_css_class (GTK_WIDGET (label), "error");
  gtk_widget_remove_css_class (GTK_WIDGET (label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (label), state_class);
}

static GtkLabel *
add_indicator (RnkgMonitorView *self, const char *caption)
{
  GtkWidget *box   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *title = gtk_label_new (caption);
  GtkWidget *value = gtk_label_new ("—");

  gtk_widget_add_css_class (title, "caption");
  gtk_widget_add_css_class (title, "dim-label");
  gtk_widget_add_css_class (value, "numeric");
  gtk_widget_add_css_class (value, "dim-label");

  gtk_box_append (GTK_BOX (box), title);
  gtk_box_append (GTK_BOX (box), value);
  gtk_box_append (GTK_BOX (self), box);

  return GTK_LABEL (value);
}

static void
rnkg_monitor_view_class_init (RnkgMonitorViewClass *klass)
{
  (void) klass;
}

static void
rnkg_monitor_view_init (RnkgMonitorView *self)
{
  GtkWidget *message;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                  GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing (GTK_BOX (self), 18);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 6);

  self->rct     = add_indicator (self, "RCT");
  self->apt     = add_indicator (self, "APT");
  self->serial  = add_indicator (self, "Serial");
  self->iq      = add_indicator (self, "I/Q");
  self->dc      = add_indicator (self, "DC");
  self->entropy = add_indicator (self, "Min-entropy");
  self->blocks  = add_indicator (self, "Blocks");

  message = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (message), TRUE);
  gtk_label_set_xalign (GTK_LABEL (message), 0.0);
  gtk_widget_set_hexpand (message, TRUE);
  gtk_widget_add_css_class (message, "error");
  gtk_widget_set_visible (message, FALSE);
  gtk_box_append (GTK_BOX (self), message);
  self->message = GTK_LABEL (message);
}

GtkWidget *
rnkg_monitor_view_new (void)
{
  return g_object_new (RNKG_TYPE_MONITOR_VIEW, NULL);
}

void
rnkg_monitor_view_update (RnkgMonitorView *self, const RnkgMonitorData *data)
{
  g_autofree char *text = NULL;

  g_return_if_fail (RNKG_IS_MONITOR_VIEW (self));
  g_return_if_fail (data != NULL);

  if (!data->valid)
    return;

  set_state (self->rct,
             data->verdict == RNKG_HEALTH_FAIL_RCT ? "FAIL" : "OK",
             data->verdict == RNKG_HEALTH_FAIL_RCT ? "error" : "success");
  set_state (self->apt,
             data->verdict == RNKG_HEALTH_FAIL_APT ? "FAIL" : "OK",
             data->verdict == RNKG_HEALTH_FAIL_APT ? "error" : "success");

  text = g_strdup_printf ("%+.4f", data->structure.serial);
  set_state (self->serial, text,
             data->verdict == RNKG_HEALTH_FAIL_SERIAL ? "error" : "success");
  g_clear_pointer (&text, g_free);

  text = g_strdup_printf ("%+.4f", data->structure.iq);
  set_state (self->iq, text,
             data->verdict == RNKG_HEALTH_FAIL_IQ ? "error" : "success");
  g_clear_pointer (&text, g_free);

  text = g_strdup_printf ("%+.4f", data->structure.dc_bias);
  set_state (self->dc, text, "dim-label");
  g_clear_pointer (&text, g_free);

  text = g_strdup_printf ("%.2f b/S", data->estimate.h_min);
  set_state (self->entropy, text, "dim-label");
  g_clear_pointer (&text, g_free);

  text = g_strdup_printf ("%" G_GUINT64_FORMAT, data->blocks);
  set_state (self->blocks, text, "dim-label");
  g_clear_pointer (&text, g_free);

  if (data->verdict != RNKG_HEALTH_OK)
    {
      gtk_label_set_text (self->message,
                          rnkg_health_verdict_message (data->verdict));
      gtk_widget_set_visible (GTK_WIDGET (self->message), TRUE);
    }
  else
    gtk_widget_set_visible (GTK_WIDGET (self->message), FALSE);
}
