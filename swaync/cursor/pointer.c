// pointer.c: LD_PRELOAD shim for swaync, which has no config for any of these:
//  - GTK4 CSS has no "cursor" property, so every clickable widget gets a hand pointer when mapped
//  - buttons get hover text from $HYPRMINIMAL_SWAYNC_TOOLTIPS (lines: label-substring<TAB>tooltip)
//  - control center: keys 1-9 press the top-row tiles (Wi-Fi, › network list, System) and the toggle
//    tiles in reading order, each tile shows its number;
//    K plays/pauses the current player, C clears all notifications ("C" keycap after "Clear all")
//  - control center: the "N notifications" header follows swaync's live count (D-Bus SubscribeV2 signal)
//  - control center: live tile labels without a config reload. scripts/control-center.sh writes
//    $XDG_RUNTIME_DIR/hyprminimal/cc/labels (lines: label-substring<TAB>new label, \n escaped);
//    the file "expanded" in that folder grows the notification list, its absence shrinks it
//  - control center: a "K" keycap under each media card's play/pause button
// Built by bin/hyprminimal on start: gcc -shared -fPIC $(pkg-config --cflags --libs gtk4) -lm
#include <gtk/gtk.h>
#include <string.h>
#include <glib/gstdio.h>

#define LIST_COLLAPSED 190   // px of notification list before it is laid out; then: the first entry's height
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
  else if (c && GTK_IS_BOX(c)) c = gtk_widget_get_first_child(c);   // [label][keycap], see key_after
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

// keyed: the tiles keys 1-9 press: toggles, and every button of the top row (buttons-grid#info)
static gboolean keyed(GtkWidget *b) {
  if (GTK_IS_TOGGLE_BUTTON(b)) return TRUE;
  for (GtkWidget *p = gtk_widget_get_parent(b); p; p = gtk_widget_get_parent(p))
    if (gtk_widget_has_css_class(p, "widget-buttons-grid")) return gtk_widget_has_css_class(p, "info");
  return FALSE;
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
    if (toggles && keyed(w)) g_ptr_array_add(toggles, w);
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

// a keycap right after a text button's label: [ Clear all (C) ], for buttons too small for badge()'s overlay
static void key_after(GtkWidget *b, const char *key) {
  GtkWidget *c = gtk_button_get_child(GTK_BUTTON(b));
  if (!c || !GTK_IS_LABEL(c)) return;   // already has one (or not a text button)
  g_object_ref(c);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  gtk_button_set_child(GTK_BUTTON(b), box);
  gtk_box_append(GTK_BOX(box), c);
  g_object_unref(c);
  GtkWidget *k = gtk_label_new(key);
  gtk_widget_add_css_class(k, "key-hint");
  gtk_widget_set_valign(k, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target(k, FALSE);
  gtk_box_append(GTK_BOX(box), k);
}

// collapsed list = the first entry in full: a lone notification, or an app's group (its header and the
// stacked cards swaync shows while the group is collapsed), however many lines it wraps to.
// 0 while nothing is laid out yet
static int first_entry_bottom(GtkWidget *listbox, GtkWidget *row) {
  graphene_rect_t r;
  if (row && gtk_widget_compute_bounds(row, listbox, &r) && r.size.height > 1)
    return (int)(r.origin.y + r.size.height + 0.999f);
  return 0;
}

// An app's group expand button and the list height work together: expanding a group grows the list
// (writes "expanded", like the "N notifications" header), collapsing the last open group shrinks it.
// The header opens/closes everything: expanding it expands every group, shrinking the list (header, or
// the panel opening) collapses them.
static gboolean group_open(GtkWidget *g) { return !gtk_widget_has_css_class(g, "collapsed"); }

static void find_groups(GtkWidget *w, GPtrArray *out) {
  if (gtk_widget_has_css_class(w, "notification-group")) { g_ptr_array_add(out, w); return; }
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) find_groups(c, out);
}

static GtkWidget *find_class(GtkWidget *w, const char *cls) {
  if (gtk_widget_has_css_class(w, cls)) return w;
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *r = find_class(c, cls);
    if (r) return r;
  }
  return NULL;
}

// Expanding keeps the list where it was (swaync scrolls to the end while rows grow): hold the scroll
// position for the length of the expand animation. From the header that is the top.
static GtkWidget *cur_list;   // the notification list's scrolled window, once seen
static double pin_value;
static gint64 pin_until;
static guint pin_id;

static gboolean pin_tick(GtkWidget *sw, GdkFrameClock *clock, gpointer d) {
  (void)d;
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
  if (gtk_adjustment_get_value(adj) != pin_value) gtk_adjustment_set_value(adj, pin_value);
  if (gdk_frame_clock_get_frame_time(clock) < pin_until) return G_SOURCE_CONTINUE;
  pin_id = 0;
  return G_SOURCE_REMOVE;
}

static void hold_scroll(gboolean top) {   // top: from the header; else where the list is now
  if (!cur_list) return;
  if (top) pin_value = 0;
  else if (!pin_id) pin_value = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(cur_list)));
  pin_until = g_get_monotonic_time() + 600 * 1000;
  if (!pin_id) pin_id = gtk_widget_add_tick_callback(cur_list, pin_tick, NULL, NULL);
}

static gboolean by_group;   // the "expanded" change came from a group's own button: leave other groups be

static void set_expanded(gboolean on) {
  char *ex = g_build_filename(ccdir, "expanded", NULL);
  gboolean now = g_file_test(ex, G_FILE_TEST_EXISTS);
  if (on && !now) { by_group = TRUE; g_file_set_contents(ex, "", 0, NULL); }
  else if (!on && now) { by_group = TRUE; g_unlink(ex); }
  g_free(ex);
}

static void on_group_classes(GtkWidget *g, GParamSpec *p, gpointer d) {
  (void)p; (void)d;
  gboolean open = group_open(g);
  if (open == GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g), "hyprminimal-open"))) return;   // other class
  g_object_set_data(G_OBJECT(g), "hyprminimal-open", GINT_TO_POINTER(open));
  if (open) { hold_scroll(FALSE); set_expanded(TRUE); return; }
  GPtrArray *groups = g_ptr_array_new();   // collapsed: shrink the list once no group is open
  if (cc) find_groups(cc, groups);
  gboolean any = FALSE;
  for (guint i = 0; i < groups->len; i++) any = any || group_open(groups->pdata[i]);
  g_ptr_array_free(groups, TRUE);
  if (!any) set_expanded(FALSE);
}

// a collapsed group hides its header buttons: expand it the way a click on its stack does, through the
// group's own click gesture (on a box inside it; swaync's toggle function isn't exported). Never on the
// cards themselves (SwayNotificationCenterExpandableGroup): a click there opens/dismisses a notification.
static gboolean click_gesture(GtkWidget *w) {
  GListModel *ctl = gtk_widget_observe_controllers(w);
  gboolean done = FALSE;
  for (guint i = 0, n = g_list_model_get_n_items(ctl); i < n && !done; i++) {
    GObject *c = g_list_model_get_item(ctl, i);
    if (GTK_IS_GESTURE_CLICK(c)) {
      double x = gtk_widget_get_width(w) / 2.0, y = gtk_widget_get_height(w) / 2.0;
      g_signal_emit_by_name(c, "pressed", 1, x, y);
      g_signal_emit_by_name(c, "released", 1, x, y);
      done = TRUE;
    }
    g_object_unref(c);
  }
  g_object_unref(ctl);
  return done;
}

static gboolean click_group(GtkWidget *w, int depth) {
  if (depth > 4 || !strcmp(G_OBJECT_TYPE_NAME(w), "SwayNotificationCenterExpandableGroup")) return FALSE;
  if (click_gesture(w)) return TRUE;
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
    if (click_group(c, depth + 1)) return TRUE;
  return FALSE;
}

static void sync_groups(GtkWidget *list, gboolean expanded) {
  static int was = -1;   // list state last time: act on the header's changes only, once
  gboolean header = was != (int)expanded && !by_group;
  if (header && expanded) hold_scroll(TRUE);
  was = expanded;
  by_group = FALSE;
  GPtrArray *groups = g_ptr_array_new();
  find_groups(list, groups);
  for (guint i = 0; i < groups->len; i++) {
    GtkWidget *g = groups->pdata[i];
    if (!g_object_get_data(G_OBJECT(g), "hyprminimal-watched")) {
      g_object_set_data(G_OBJECT(g), "hyprminimal-watched", GINT_TO_POINTER(1));
      g_object_set_data(G_OBJECT(g), "hyprminimal-open", GINT_TO_POINTER(group_open(g)));
      g_signal_connect(g, "notify::css-classes", G_CALLBACK(on_group_classes), NULL);
    }
    if (!expanded && group_open(g)) {
      GtkWidget *b = find_class(g, "notification-group-collapse-button");
      if (b) gtk_widget_activate(b);
    } else if (expanded && header && !group_open(g)) click_group(g, 0);
  }
  g_ptr_array_free(groups, TRUE);
}

// swaync animates groups collapsing/expanding and new arrivals: follow the first entry's height every
// frame for a moment after each change, so the collapsed list ends exactly at its bottom
static gint64 follow_until;
static guint follow_id;
static GtkWidget *follow_list;

static gboolean follow_tick(GtkWidget *listbox, GdkFrameClock *clock, gpointer d) {
  (void)d;
  int b = first_entry_bottom(listbox, gtk_widget_get_first_child(listbox));
  if (b > 0 && follow_list) gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(follow_list), MIN(b, LIST_EXPANDED));
  if (gdk_frame_clock_get_frame_time(clock) < follow_until) return G_SOURCE_CONTINUE;
  follow_id = 0;
  return G_SOURCE_REMOVE;
}

static void follow_first_entry(GtkWidget *list, GtkWidget *listbox) {
  follow_list = list;
  follow_until = g_get_monotonic_time() + 800 * 1000;
  if (!follow_id) follow_id = gtk_widget_add_tick_callback(listbox, follow_tick, NULL, NULL);
}

static void stop_following(GtkWidget *listbox) {
  if (follow_id && listbox) gtk_widget_remove_tick_callback(listbox, follow_id);
  follow_id = 0;
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
    if (!strcmp(gtk_label_get_text(l), "Clear all")) key_after(buttons->pdata[i], "C");
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
  for (guint i = 0; i < toggles->len && i < 9; i++) {
    GtkLabel *l = button_label(toggles->pdata[i]);
    char num[4];
    g_snprintf(num, sizeof num, "%u", i + 1);
    // the › beside Wi-Fi is too narrow for an overlay: keycap after the arrow instead
    if (l && !strcmp(gtk_label_get_text(l), "󰅂")) key_after(toggles->pdata[i], num);
    else badge(toggles->pdata[i], i + 1);
  }
  if (list) {
    // nothing listed: hide the whole notification area (the header says "No notifications")
    GtkWidget *rows = NULL, *listbox = NULL, *area = gtk_widget_get_ancestor(list, GTK_TYPE_STACK);
    area = area ? gtk_widget_get_parent(area) : NULL;
    for (GtkWidget *c = gtk_widget_get_first_child(list); c && !rows; c = gtk_widget_get_first_child(c))
      if (gtk_widget_has_css_class(c, "control-center-list")) { listbox = c; rows = gtk_widget_get_first_child(c); }
    if (area) gtk_widget_set_visible(area, rows != NULL);
    cur_list = list;
    sync_groups(list, expanded);
    int h = LIST_EXPANDED;
    if (!expanded) {
      int b = listbox ? first_entry_bottom(listbox, rows) : 0;
      h = b > 0 ? MIN(b, LIST_EXPANDED) : LIST_COLLAPSED;
      if (listbox && rows) follow_first_entry(list, listbox);   // then every frame until it settles
    } else stop_following(listbox);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(list), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(list), h);
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
  if (keyval == GDK_KEY_c || keyval == GDK_KEY_C) {   // C: Clear all, like the button in the header
    g_spawn_command_line_async("swaync-client -C -sw", NULL);
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

// ---------------------------------------------------------------- media card
// swaync's play/pause button gets a "K" keycap below it (like the number keycaps on the tiles). Button and
// keycap go in a little column; every control in the row hangs from the top so the icons stay in one line.
// No animation here: swaync's own play/pause icon already shows the state.
static void media_attach(GtkWidget *overlay) {
  if (!GTK_IS_BOX(overlay) || g_object_get_data(G_OBJECT(overlay), "hyprminimal-media")) return;
  GtkWidget *head = gtk_widget_get_first_child(overlay);
  GtkWidget *row = head ? gtk_widget_get_next_sibling(head) : NULL, *play = NULL;
  if (!row) return;
  g_object_set_data(G_OBJECT(overlay), "hyprminimal-media", GINT_TO_POINTER(1));
  for (GtkWidget *b = gtk_widget_get_first_child(row); b; b = gtk_widget_get_next_sibling(b)) {
    const char *icon = GTK_IS_BUTTON(b) ? gtk_button_get_icon_name(GTK_BUTTON(b)) : NULL;
    if (icon && strstr(icon, "media-playback")) play = b;
    gtk_widget_set_valign(b, GTK_ALIGN_START);
  }
  if (!play) return;
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

static gboolean on_map(GSignalInvocationHint *hint, guint n, const GValue *params, gpointer data) {
  (void)hint; (void)n; (void)data;
  GtkWidget *w = g_value_get_object(&params[0]);
  if (gtk_widget_has_css_class(w, "control-center")) {
    GtkRoot *r = gtk_widget_get_root(w);
    if (r) { cc_found(GTK_WIDGET(r)); cc_apply(); }
  }
  if (gtk_widget_has_css_class(w, "mpris-overlay")) media_attach(w);
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
