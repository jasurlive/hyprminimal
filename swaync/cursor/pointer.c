// pointer.c: LD_PRELOAD shim for swaync, which has no config for either of these:
//  - GTK4 CSS has no "cursor" property, so every clickable widget gets a hand pointer when mapped
//  - buttons get hover text from $HYPRMINIMAL_SWAYNC_TOOLTIPS (lines: label-substring<TAB>tooltip)
// Built by bin/hyprminimal on start: gcc -shared -fPIC $(pkg-config --cflags --libs gtk4)
#include <gtk/gtk.h>
#include <string.h>

static GPtrArray *tips;   // pairs: needle, text

static void load_tips(void) {
  const char *path = g_getenv("HYPRMINIMAL_SWAYNC_TOOLTIPS");
  char *data = NULL;
  if (!path || !g_file_get_contents(path, &data, NULL, NULL)) return;
  tips = g_ptr_array_new();
  char **lines = g_strsplit(data, "\n", -1);
  for (char **l = lines; *l; l++) {
    char *tab = strchr(*l, '\t');
    if (**l == '#' || !tab || tab == *l) continue;
    g_ptr_array_add(tips, g_strndup(*l, tab - *l));
    g_ptr_array_add(tips, g_strdup(g_strstrip(tab + 1)));
  }
  g_strfreev(lines);
  g_free(data);
}

static void set_tip(GtkWidget *w) {
  if (!tips || gtk_widget_get_tooltip_text(w)) return;
  const char *lab = gtk_button_get_label(GTK_BUTTON(w));
  if (!lab) {   // label may be a child widget instead of the button's own label
    GtkWidget *c = gtk_button_get_child(GTK_BUTTON(w));
    if (c && GTK_IS_LABEL(c)) lab = gtk_label_get_text(GTK_LABEL(c));
  }
  if (!lab) return;
  for (guint i = 0; i + 1 < tips->len; i += 2)
    if (strstr(lab, tips->pdata[i])) { gtk_widget_set_tooltip_text(w, tips->pdata[i + 1]); return; }
}

static gboolean on_map(GSignalInvocationHint *hint, guint n, const GValue *params, gpointer data) {
  (void)hint; (void)n; (void)data;
  GtkWidget *w = g_value_get_object(&params[0]);
  if (g_getenv("HYPRMINIMAL_POINTER_DEBUG")) g_message("map %s", G_OBJECT_TYPE_NAME(w));
  if (GTK_IS_BUTTON(w)) set_tip(w);
  if (GTK_IS_BUTTON(w) || GTK_IS_SWITCH(w) || GTK_IS_SCALE(w) || GTK_IS_CHECK_BUTTON(w)
      || GTK_IS_DROP_DOWN(w))
    gtk_widget_set_cursor_from_name(w, "pointer");
  return TRUE;  // keep the hook
}

__attribute__((constructor)) static void hyprminimal_pointer_init(void) {
  load_tips();
  // we're loaded now: commands swaync runs (bluetoothctl, GTK3 apps…) must not get this GTK4 shim
  g_unsetenv("LD_PRELOAD");
  // signals exist only once the class is initialised; without this the lookup returns 0
  g_type_class_ref(GTK_TYPE_WIDGET);
  guint id = g_signal_lookup("map", GTK_TYPE_WIDGET);
  if (id) g_signal_add_emission_hook(id, 0, on_map, NULL, NULL);
  else g_warning("hyprminimal pointer shim: no map signal, cursor unchanged");
}
