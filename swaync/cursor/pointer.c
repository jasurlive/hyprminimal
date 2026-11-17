// pointer.c: LD_PRELOAD shim for swaync, which has no config for any of these:
//  - GTK4 CSS has no "cursor" property, so every clickable widget gets a hand pointer when mapped
//  - buttons get hover text from $HYPRMINIMAL_SWAYNC_TOOLTIPS (lines: label-substring<TAB>tooltip)
//  - control center: keys 1-9 flip the toggle tiles in reading order, each tile shows its number;
//    K plays/pauses the current player
//  - control center: the "N notifications" header follows swaync's live count (D-Bus SubscribeV2 signal)
//  - control center: live tile labels without a config reload. scripts/control-center.sh writes
//    $XDG_RUNTIME_DIR/hyprminimal/cc/labels (lines: label-substring<TAB>new label, \n escaped);
//    the file "expanded" in that folder grows the notification list, its absence shrinks it
//  - control center: an equalizer in each media card (like the bar's, scripts/media.sh), between the
//    title row and the buttons. Braille text, EQ_FPS frames a second only while the card is shown and
//    its player plays (swaync's play button shows the pause icon); otherwise a flat row, no timer
// Built by bin/hyprminimal on start: gcc -shared -fPIC $(pkg-config --cflags --libs gtk4) -lm
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

#define LIST_COLLAPSED 76    // px of notification list: one card
#define LIST_EXPANDED 560

static GPtrArray *tips;      // pairs: needle, text
static GtkWidget *cc;        // the control-center window, once seen
static char *ccdir;          // $XDG_RUNTIME_DIR/hyprminimal/cc
static guint notif_count = G_MAXUINT;   // live, from swaync's own D-Bus signal; G_MAXUINT = not known yet

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

// the text label of a button: its child, or inside the overlay a number badge put around it
static GtkLabel *button_label(GtkWidget *b) {
  GtkWidget *c = gtk_button_get_child(GTK_BUTTON(b));
  if (c && GTK_IS_OVERLAY(c)) c = gtk_overlay_get_child(GTK_OVERLAY(c));
  return c && GTK_IS_LABEL(c) ? GTK_LABEL(c) : NULL;
}

static void set_tip(GtkWidget *w) {
  if (!tips || gtk_widget_get_tooltip_text(w)) return;
  GtkLabel *l = button_label(w);
  const char *lab = l ? gtk_label_get_text(l) : NULL;
  if (!lab) return;
  for (guint i = 0; i + 1 < tips->len; i += 2)
    if (strstr(lab, tips->pdata[i])) { gtk_widget_set_tooltip_text(w, tips->pdata[i + 1]); return; }
}

// ---------------------------------------------------------------- control center
// swaync's big "No Notifications" picture: the header button already says so
static void cc_apply(void);
static gboolean apply_idle(gpointer d) { (void)d; cc_apply(); return G_SOURCE_REMOVE; }
static void keep_hidden(GtkWidget *w) {
  if (gtk_widget_get_visible(w)) { gtk_widget_set_visible(w, FALSE); g_idle_add(apply_idle, NULL); }
}

static void walk(GtkWidget *w, gboolean in_grid, GPtrArray *buttons, GPtrArray *toggles, GtkWidget **list) {
  if (list && gtk_widget_has_css_class(w, "control-center-list-placeholder")
      && !g_object_get_data(G_OBJECT(w), "hyprminimal-hidden")) {
    g_object_set_data(G_OBJECT(w), "hyprminimal-hidden", GINT_TO_POINTER(1));
    g_signal_connect(w, "notify::visible", G_CALLBACK(keep_hidden), NULL);
    keep_hidden(w);
  }
  if (list && gtk_widget_has_css_class(w, "control-center")) {
    // swaync fixes the panel height; let it follow the content instead (top-anchored, capped by the screen)
    int width;
    gtk_widget_get_size_request(w, &width, NULL);
    gtk_widget_set_size_request(w, width, -1);
    gtk_widget_set_valign(w, GTK_ALIGN_START);
    gtk_widget_set_vexpand(w, FALSE);
    if (GTK_IS_SCROLLED_WINDOW(w)) gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(w), TRUE);
  }
  in_grid = in_grid || gtk_widget_has_css_class(w, "widget-buttons-grid");
  if (in_grid && GTK_IS_BUTTON(w)) {
    if (buttons) g_ptr_array_add(buttons, w);
    if (toggles && GTK_IS_TOGGLE_BUTTON(w)) g_ptr_array_add(toggles, w);
    return;
  }
  if (list && !*list && gtk_widget_has_css_class(w, "control-center-list")) {
    GtkWidget *sw = gtk_widget_get_ancestor(w, GTK_TYPE_SCROLLED_WINDOW);   // the notification list's own
    if (sw) *list = sw;
  }
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
    walk(c, in_grid, buttons, toggles, list);
}

static void badge(GtkWidget *b, guint n) {
  GtkWidget *c = gtk_button_get_child(GTK_BUTTON(b));
  if (!c || !GTK_IS_LABEL(c)) return;   // already has one (or not a text button)
  g_object_ref(c);
  GtkWidget *ov = gtk_overlay_new();
  gtk_button_set_child(GTK_BUTTON(b), ov);
  gtk_overlay_set_child(GTK_OVERLAY(ov), c);
  g_object_unref(c);
  char num[4];
  g_snprintf(num, sizeof num, "%u", n);
  GtkWidget *k = gtk_label_new(num);
  gtk_widget_add_css_class(k, "key-hint");
  gtk_widget_set_halign(k, GTK_ALIGN_END);
  gtk_widget_set_valign(k, GTK_ALIGN_CENTER);   // a keycap at the middle right
  gtk_widget_set_can_target(k, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(ov), k);
}

// labels file, badges, centred two-line labels, list height: cheap, run on show and on file changes
static void cc_apply(void) {
  if (!cc) return;
  GPtrArray *buttons = g_ptr_array_new(), *toggles = g_ptr_array_new();
  GtkWidget *list = NULL;
  walk(cc, FALSE, buttons, toggles, &list);

  char *path = g_build_filename(ccdir, "labels", NULL), *data = NULL;
  char **lines = g_file_get_contents(path, &data, NULL, NULL) ? g_strsplit(data, "\n", -1) : NULL;
  char *ex = g_build_filename(ccdir, "expanded", NULL);
  gboolean expanded = g_file_test(ex, G_FILE_TEST_EXISTS);

  for (guint i = 0; i < buttons->len; i++) {
    GtkLabel *l = button_label(buttons->pdata[i]);
    if (!l) continue;
    gtk_label_set_justify(l, GTK_JUSTIFY_CENTER);
    for (char **ln = lines; ln && *ln; ln++) {
      char *tab = strchr(*ln, '\t');
      if (!tab || tab == *ln) continue;
      *tab = 0;
      if (strstr(gtk_label_get_text(l), *ln)) {
        char *text = g_strcompress(tab + 1), *arrow;
        if (expanded && (arrow = strstr(text, "󰅀"))) memcpy(arrow, "󰅃", strlen("󰅃"));   // same length
        if (strcmp(text, gtk_label_get_text(l))) gtk_label_set_text(l, text);
        g_free(text);
      }
      *tab = '\t';
    }
    // the "N notifications" header follows the live count (Clear all, dismissals, new arrivals)
    if (notif_count != G_MAXUINT && strstr(gtk_label_get_text(l), "otification")) {
      char *text = notif_count == 0 ? g_strdup("󰂚   No notifications")
        : g_strdup_printf("󰂚   %u notification%s   %s", notif_count, notif_count == 1 ? "" : "s",
                          expanded ? "󰅃" : "󰅀");
      if (strcmp(text, gtk_label_get_text(l))) gtk_label_set_text(l, text);
      g_free(text);
    }
  }
  for (guint i = 0; i < toggles->len && i < 9; i++) badge(toggles->pdata[i], i + 1);
  if (list) {
    // nothing listed: hide the whole notification area (the header says "No notifications")
    GtkWidget *rows = NULL, *area = gtk_widget_get_ancestor(list, GTK_TYPE_STACK);
    area = area ? gtk_widget_get_parent(area) : NULL;
    for (GtkWidget *c = gtk_widget_get_first_child(list); c && !rows; c = gtk_widget_get_first_child(c))
      if (gtk_widget_has_css_class(c, "control-center-list")) rows = gtk_widget_get_first_child(c);
    if (area) gtk_widget_set_visible(area, rows != NULL);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(list), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(list), expanded ? LIST_EXPANDED : LIST_COLLAPSED);
    gtk_widget_set_vexpand(list, FALSE);
  }
  gtk_widget_queue_resize(cc);   // re-measure with the new labels and sizes
  g_strfreev(lines);
  g_free(data); g_free(path); g_free(ex);
  g_ptr_array_free(buttons, TRUE); g_ptr_array_free(toggles, TRUE);
}

static void on_dir_change(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent e, gpointer d) {
  (void)m; (void)f; (void)o; (void)d;
  if (e == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || e == G_FILE_MONITOR_EVENT_CREATED
      || e == G_FILE_MONITOR_EVENT_DELETED || e == G_FILE_MONITOR_EVENT_MOVED_IN
      || e == G_FILE_MONITOR_EVENT_RENAMED)
    cc_apply();
}

static gboolean on_key(GtkEventControllerKey *k, guint keyval, guint code, GdkModifierType mods, gpointer win) {
  (void)k; (void)code;
  if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return FALSE;
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(win));
  if (focus && GTK_IS_EDITABLE(focus)) return FALSE;   // typing an inline reply
  if (keyval == GDK_KEY_k || keyval == GDK_KEY_K) {   // K: play/pause, like YouTube; the same player as the bar
    g_spawn_command_line_async("playerctl play-pause", NULL);
    return TRUE;
  }
  guint n = keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9 ? keyval - GDK_KEY_1
          : keyval >= GDK_KEY_KP_1 && keyval <= GDK_KEY_KP_9 ? keyval - GDK_KEY_KP_1 : G_MAXUINT;
  if (n == G_MAXUINT) return FALSE;
  GPtrArray *t = g_ptr_array_new();
  walk(GTK_WIDGET(win), FALSE, NULL, t, NULL);
  gboolean hit = n < t->len;
  if (hit) gtk_widget_activate(t->pdata[n]);
  g_ptr_array_free(t, TRUE);
  return hit;
}

// notification count: swaync emits SubscribeV2(count, dnd, cc_open, inhibited) on every change
// (older versions: Subscribe(count, dnd, cc_open)); the bar's counter uses it too; we hear our own process's signal through the bus. NotificationCount seeds the value.
#define SWAYNC_NAME "org.erikreider.swaync.cc"
#define SWAYNC_PATH "/org/erikreider/swaync/cc"
static void on_count_signal(GDBusConnection *c, const char *s, const char *p, const char *i,
                            const char *sig, GVariant *params, gpointer d) {
  (void)c; (void)s; (void)p; (void)i; (void)sig; (void)d;
  if (!g_str_has_prefix(g_variant_get_type_string(params), "(u")) return;
  g_variant_get_child(params, 0, "u", &notif_count);
  g_idle_add(apply_idle, NULL);   // after swaync has finished removing/adding the rows
}
static void on_count_reply(GObject *bus, GAsyncResult *res, gpointer d) {
  (void)d;
  GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), res, NULL);
  if (!v) return;
  g_variant_get(v, "(u)", &notif_count);
  g_variant_unref(v);
  cc_apply();
}
static void count_watch(void) {
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus) return;
  g_dbus_connection_signal_subscribe(bus, NULL, SWAYNC_NAME, "SubscribeV2", SWAYNC_PATH, NULL,
                                     G_DBUS_SIGNAL_FLAGS_NONE, on_count_signal, NULL, NULL);
  g_dbus_connection_signal_subscribe(bus, NULL, SWAYNC_NAME, "Subscribe", SWAYNC_PATH, NULL,
                                     G_DBUS_SIGNAL_FLAGS_NONE, on_count_signal, NULL, NULL);
  g_dbus_connection_call(bus, SWAYNC_NAME, SWAYNC_PATH, SWAYNC_NAME, "NotificationCount", NULL,
                         G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL,
                         on_count_reply, NULL);
  g_object_unref(bus);   // the shared session bus stays alive inside swaync
}

static void cc_found(GtkWidget *win) {
  if (cc == win) return;
  cc = win;
  g_object_add_weak_pointer(G_OBJECT(cc), (gpointer *)&cc);
  GtkEventController *k = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(k, GTK_PHASE_CAPTURE);
  g_signal_connect(k, "key-pressed", G_CALLBACK(on_key), win);
  gtk_widget_add_controller(win, k);
  static GFileMonitor *mon;
  if (!mon) {
    g_mkdir_with_parents(ccdir, 0700);
    GFile *dir = g_file_new_for_path(ccdir);
    mon = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (mon) g_signal_connect(mon, "changed", G_CALLBACK(on_dir_change), NULL);
    g_object_unref(dir);
    count_watch();
  }
}

// ---------------------------------------------------------------- media equalizer
// EQ_CHARS braille characters, each 2 columns of 4 dots: 2*EQ_CHARS slim dotted bars. No audio data:
// each bar jumps to a new random height every frame, the middle ones may reach higher than the edges.
#define EQ_CHARS 15
#define EQ_BARS (2 * EQ_CHARS)
#define EQ_FPS 4
static const guint EQ_L[5] = {0, 0x40, 0x44, 0x46, 0x47};   // left column, 0-4 dots from the bottom
static const guint EQ_R[5] = {0, 0x80, 0xA0, 0xB0, 0xB8};   // right column
static GPtrArray *eqs;       // the equalizer labels (weak: removed when swaync drops a player card)
static guint eq_tick;

static void eq_render(GtkWidget *eq, const int *h) {
  GString *s = g_string_new("<span font_family='DejaVu Sans' letter_spacing='-6000'>");
  for (int i = 0; i < EQ_BARS; i += 2) {
    char buf[8];
    buf[g_unichar_to_utf8(0x2800 + EQ_L[h[i]] + EQ_R[h[i + 1]], buf)] = 0;
    g_string_append(s, buf);
  }
  g_string_append(s, "</span>");
  if (g_strcmp0(s->str, g_object_get_data(G_OBJECT(eq), "hyprminimal-markup"))) {
    gtk_label_set_markup(GTK_LABEL(eq), s->str);
    g_object_set_data_full(G_OBJECT(eq), "hyprminimal-markup", g_strdup(s->str), g_free);
  }
  g_string_free(s, TRUE);
}

// playing = swaync's play/pause button shows the pause icon
static gboolean eq_playing(GtkWidget *eq) {
  GtkWidget *b = g_object_get_data(G_OBJECT(eq), "hyprminimal-play");
  const char *icon = b ? gtk_button_get_icon_name(GTK_BUTTON(b)) : NULL;
  return icon && strstr(icon, "playback-pause");
}

static gboolean eq_on_tick(gpointer d) {
  (void)d;
  gboolean any = FALSE;
  for (guint i = 0; eqs && i < eqs->len; i++) {
    GtkWidget *eq = eqs->pdata[i];
    int *h = g_object_get_data(G_OBJECT(eq), "hyprminimal-h");
    if (!gtk_widget_get_mapped(eq) || !eq_playing(eq)) {   // rest: one flat row of dots
      for (int j = 0; j < EQ_BARS; j++) h[j] = 1;
      eq_render(eq, h);
      continue;
    }
    any = TRUE;
    for (int j = 0; j < EQ_BARS; j++) {   // bell-shaped ceiling: edges reach 2 dots, the middle 4
      int top = 2 + (int)lround(2 * pow(sin(G_PI * (j + 0.5) / EQ_BARS), 1.5));
      h[j] = (h[j] + g_random_int_range(0, top - 1)) % top + 1;   // never the same height twice
    }
    eq_render(eq, h);
  }
  if (!any) eq_tick = 0;
  return any ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void eq_wake(void) {
  if (!eq_tick) eq_tick = g_timeout_add(1000 / EQ_FPS, eq_on_tick, NULL);
}
static void eq_on_icon(GObject *o, GParamSpec *ps, gpointer d) { (void)o; (void)ps; (void)d; eq_wake(); }
static void eq_gone(gpointer d, GObject *dead) { (void)d; if (eqs) g_ptr_array_remove(eqs, dead); }

// a media card was shown: give it an equalizer once, and (re)start the animation
static void eq_attach(GtkWidget *overlay) {
  GtkWidget *eq = g_object_get_data(G_OBJECT(overlay), "hyprminimal-eq");
  if (!eq) {
    GtkWidget *head = gtk_widget_get_first_child(overlay);
    if (!GTK_IS_BOX(overlay) || !head) return;
    eq = gtk_label_new(NULL);
    gtk_widget_add_css_class(eq, "hyprminimal-eq");
    gtk_widget_set_halign(eq, GTK_ALIGN_CENTER);
    gtk_widget_set_overflow(eq, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_can_target(eq, FALSE);
    int *h = g_new(int, EQ_BARS);
    for (int j = 0; j < EQ_BARS; j++) h[j] = 1;
    g_object_set_data_full(G_OBJECT(eq), "hyprminimal-h", h, g_free);
    eq_render(eq, h);
    gtk_box_insert_child_after(GTK_BOX(overlay), eq, head);
    g_object_set_data(G_OBJECT(overlay), "hyprminimal-eq", eq);
    if (!eqs) eqs = g_ptr_array_new();
    g_ptr_array_add(eqs, eq);
    g_object_weak_ref(G_OBJECT(eq), eq_gone, NULL);
    // the play/pause button: wakes the timer when playback starts while the panel is open, and gets a
    // "K" keycap below it (like the number keycaps on the tiles). Button and keycap go in a little column;
    // every control in the row hangs from the top so the icons stay in one line.
    GtkWidget *row = gtk_widget_get_next_sibling(eq), *play = NULL;
    for (GtkWidget *b = row ? gtk_widget_get_first_child(row) : NULL; b; b = gtk_widget_get_next_sibling(b)) {
      const char *icon = GTK_IS_BUTTON(b) ? gtk_button_get_icon_name(GTK_BUTTON(b)) : NULL;
      if (icon && strstr(icon, "media-playback")) play = b;
      gtk_widget_set_valign(b, GTK_ALIGN_START);
    }
    if (play) {
      g_object_set_data(G_OBJECT(eq), "hyprminimal-play", play);
      g_signal_connect(play, "notify::icon-name", G_CALLBACK(eq_on_icon), NULL);
      gtk_widget_set_tooltip_text(play, "Play / pause (K)");
      GtkWidget *before = gtk_widget_get_prev_sibling(play);
      GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_add_css_class(col, "hyprminimal-play");
      gtk_widget_set_valign(col, GTK_ALIGN_START);
      g_object_ref(play);
      gtk_box_remove(GTK_BOX(row), play);
      gtk_box_append(GTK_BOX(col), play);
      g_object_unref(play);
      GtkWidget *k = gtk_label_new("K");
      gtk_widget_add_css_class(k, "key-hint");
      gtk_widget_set_halign(k, GTK_ALIGN_CENTER);
      gtk_widget_set_can_target(k, FALSE);
      gtk_box_append(GTK_BOX(col), k);
      gtk_box_insert_child_after(GTK_BOX(row), col, before);
    }
  }
  eq_wake();
}

static gboolean on_map(GSignalInvocationHint *hint, guint n, const GValue *params, gpointer data) {
  (void)hint; (void)n; (void)data;
  GtkWidget *w = g_value_get_object(&params[0]);
  if (gtk_widget_has_css_class(w, "control-center")) {
    GtkRoot *r = gtk_widget_get_root(w);
    if (r) { cc_found(GTK_WIDGET(r)); cc_apply(); }
  }
  if (gtk_widget_has_css_class(w, "mpris-overlay")) eq_attach(w);
  if (GTK_IS_BUTTON(w)) set_tip(w);
  if (GTK_IS_BUTTON(w) || GTK_IS_SWITCH(w) || GTK_IS_SCALE(w) || GTK_IS_CHECK_BUTTON(w)
      || GTK_IS_DROP_DOWN(w))
    gtk_widget_set_cursor_from_name(w, "pointer");
  return TRUE;  // keep the hook
}

__attribute__((constructor)) static void hyprminimal_pointer_init(void) {
  load_tips();
  ccdir = g_build_filename(g_get_user_runtime_dir(), "hyprminimal", "cc", NULL);
  // we're loaded now: commands swaync runs (bluetoothctl, GTK3 apps…) must not get this GTK4 shim
  g_unsetenv("LD_PRELOAD");
  // signals exist only once the class is initialised; without this the lookup returns 0
  g_type_class_ref(GTK_TYPE_WIDGET);
  guint id = g_signal_lookup("map", GTK_TYPE_WIDGET);
  if (id) g_signal_add_emission_hook(id, 0, on_map, NULL, NULL);
  else g_warning("hyprminimal pointer shim: no map signal, cursor unchanged");
}
