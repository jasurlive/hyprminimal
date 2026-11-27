// Included by helpers/panels.c. Notification daemon (org.freedesktop.Notifications): popups in a corner,
// a history the control center lists, and Do not disturb. Replaces swaync.
//   popups: one layer window, a column of cards; click = the app's default action + dismiss. A 3px bar
//           runs out with the timeout (Settings → Notifications: position, seconds). Critical: no timeout.
//   history: newest first, until dismissed ("Clear all", a click, the app closing it). Transient: popup only.
//   bar bell: $XDG_RUNTIME_DIR/hyprminimal/notif.json + SIGRTMIN+8 to waybar, only when its icon changes.
//   DND: hides popups (critical ones too, like swaync); kept across restarts in $XDG_RUNTIME_DIR/hyprminimal/dnd.
#define NOTIF_KEEP 50       // history cap
#define BAR_SIGNAL 8        // waybar custom/notification "signal"

typedef struct {
    guint id;
    char *app, *summary, *body, *icon;  // body: Pango markup (sanitized); icon: theme name or file path
    GdkTexture *image;                  // image-data hint
    char **actions;                     // key, label, key, label …
    int urgency;                        // 0 low, 1 normal, 2 critical
    gboolean transient;
    gint64 time;                        // g_get_real_time()
    GtkWidget *popup;                   // its card in the popup column (NULL: not shown)
    guint timer;
} Notif;

static struct {
    GDBusConnection *bus;
    GPtrArray *list;                    // Notif*, newest first
    guint next_id;
    gboolean dnd;
    GtkWidget *win, *col;
    int timeout_s;
    char pos[24];
    char *run;                          // $XDG_RUNTIME_DIR/hyprminimal
    char bell[24];                      // last alt written for the bar
} NT = {.next_id = 1, .timeout_s = 3, .pos = "top-right"};

static void cc_notifs_changed(void);    // control center (control.c): redraw its list if open

static void notif_free(Notif *n) {
    if (n->timer) g_source_remove(n->timer);
    if (n->popup) gtk_box_remove(GTK_BOX(NT.col), n->popup);
    g_free(n->app); g_free(n->summary); g_free(n->body); g_free(n->icon);
    g_clear_object(&n->image);
    g_strfreev(n->actions);
    g_free(n);
}

static Notif *notif_find(guint id, guint *index) {
    for (guint i = 0; i < NT.list->len; i++) {
        Notif *n = g_ptr_array_index(NT.list, i);
        if (n->id == id) { if (index) *index = i; return n; }
    }
    return NULL;
}

// ---- bar bell
static void signal_waybar(void) {
    GDir *d = g_dir_open("/proc", 0, NULL);
    if (!d) return;
    for (const char *e; (e = g_dir_read_name(d));) {
        if (!g_ascii_isdigit(*e)) continue;
        char p[64], comm[32];
        snprintf(p, sizeof p, "/proc/%s/comm", e);
        if (read_small(p, comm, sizeof comm) && !strcmp(g_strchomp(comm), "waybar")) kill(atoi(e), SIGRTMIN + BAR_SIGNAL);
    }
    g_dir_close(d);
}

static void bell_update(void) {
    const char *alt = NT.list->len ? (NT.dnd ? "dnd-notification" : "notification") : (NT.dnd ? "dnd-none" : "none");
    if (!strcmp(alt, NT.bell)) return;
    snprintf(NT.bell, sizeof NT.bell, "%s", alt);
    char *f = g_build_filename(NT.run, "notif.json", NULL), *js = g_strdup_printf("{\"text\":\"\",\"alt\":\"%s\",\"class\":\"%s\"}\n", alt, alt);
    g_file_set_contents(f, js, -1, NULL);
    g_free(f); g_free(js);
    signal_waybar();
}

static void notifs_changed(void) {
    bell_update();
    cc_notifs_changed();
}

static void emit_closed(guint id, guint reason) {   // 1 expired, 2 dismissed, 3 closed by the app
    if (NT.bus)
        g_dbus_connection_emit_signal(NT.bus, NULL, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                      "NotificationClosed", g_variant_new("(uu)", id, reason), NULL);
}

static void popups_fit(void) {
    if (NT.win) gtk_widget_set_visible(NT.win, gtk_widget_get_first_child(NT.col) != NULL);
}

static void notif_remove(guint id, guint reason) {
    guint i;
    Notif *n = notif_find(id, &i);
    if (!n) return;
    g_ptr_array_remove_index(NT.list, i);
    notif_free(n);
    popups_fit();
    emit_closed(id, reason);
    notifs_changed();
}

static void notif_clear_all(void) {
    while (NT.list->len) {
        Notif *n = g_ptr_array_index(NT.list, NT.list->len - 1);
        guint id = n->id;
        g_ptr_array_remove_index(NT.list, NT.list->len - 1);
        notif_free(n);
        emit_closed(id, 2);
    }
    popups_fit();
    notifs_changed();
}

static void notif_invoke(guint id, const char *key) {   // the app gets its action; then dismissed
    if (NT.bus && key)
        g_dbus_connection_emit_signal(NT.bus, NULL, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                      "ActionInvoked", g_variant_new("(us)", id, key), NULL);
    notif_remove(id, 2);
}

static const char *notif_default_action(Notif *n) {
    for (char **a = n->actions; a && a[0] && a[1]; a += 2)
        if (!strcmp(a[0], "default")) return a[0];
    return NULL;
}

// ---- cards (popups and the control center's list)
// A click runs after the event: the card is destroyed by it, never from inside its own handler.
typedef struct { guint id; char *key; gboolean close_cc; } Pending;

static gboolean pending_run(gpointer d) {
    Pending *p = d;
    notif_invoke(p->id, p->key);
    if (p->close_cc && current && !strcmp(current->kind, "control")) manager_close();   // the app is coming up
    g_free(p->key);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void card_activate(guint id, const char *key) {
    Pending *p = g_new0(Pending, 1);
    p->id = id;
    p->key = g_strdup(key);
    p->close_cc = key != NULL;   // a plain dismiss keeps the control center open
    g_idle_add(pending_run, p);
}

static void on_card_click(GtkGestureClick *g, int np, double x, double y, gpointer id) {
    (void)np; (void)x; (void)y;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    Notif *n = notif_find(GPOINTER_TO_UINT(id), NULL);
    if (n) card_activate(n->id, notif_default_action(n));
}

static void on_card_action(GtkButton *b, gpointer id) {
    card_activate(GPOINTER_TO_UINT(id), g_object_get_data(G_OBJECT(b), "key"));
}

static char *ago(gint64 t) {
    gint64 s = (g_get_real_time() - t) / G_USEC_PER_SEC;
    if (s < 60) return g_strdup("Now");
    if (s < 3600) return g_strdup_printf("%d min", (int)(s / 60));
    if (s < 86400) return g_strdup_printf("%d h", (int)(s / 3600));
    GDateTime *d = g_date_time_new_from_unix_local(t / G_USEC_PER_SEC);
    char *r = g_date_time_format(d, "%d.%m");
    g_date_time_unref(d);
    return r;
}

static GtkWidget *notif_card(Notif *n, gboolean in_cc) {
    GtkWidget *card = VBOX(0, "notif");
    if (n->urgency == 2) gtk_widget_add_css_class(card, "critical");
    GtkWidget *row = HBOX(10, "notif-content"), *col = VBOX(2, NULL);
    GtkWidget *img = NULL;
    if (n->image) img = gtk_image_new_from_paintable(GDK_PAINTABLE(n->image));
    else if (n->icon && *n->icon) {
        if (n->icon[0] == '/') img = gtk_image_new_from_file(n->icon);
        else if (gtk_icon_theme_has_icon(gtk_icon_theme_get_for_display(gdk_display_get_default()), n->icon))
            img = gtk_image_new_from_icon_name(n->icon);
    }
    if (img) {
        gtk_image_set_pixel_size(GTK_IMAGE(img), in_cc ? 36 : 44);
        gtk_widget_set_valign(img, GTK_ALIGN_START);
        gtk_widget_add_css_class(img, "notif-image");
        gtk_box_append(GTK_BOX(row), img);
    }
    gtk_widget_set_hexpand(col, TRUE);
    GtkWidget *top = HBOX(8, NULL), *sum = lbl(n->summary && *n->summary ? n->summary : n->app, "summary", 0);
    gtk_widget_set_hexpand(sum, TRUE);
    gtk_box_append(GTK_BOX(top), sum);
    if (in_cc) {
        char *t = ago(n->time);
        gtk_box_append(GTK_BOX(top), lbl(t, "time", 1));
        g_free(t);
    }
    gtk_box_append(GTK_BOX(col), top);
    if (n->body && *n->body) {
        GtkWidget *b = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(b), n->body);
        gtk_label_set_xalign(GTK_LABEL(b), 0);
        gtk_label_set_wrap(GTK_LABEL(b), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(b), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_lines(GTK_LABEL(b), in_cc ? 3 : 5);
        gtk_label_set_ellipsize(GTK_LABEL(b), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(b), 10);   // wrap at the card's width, not the text's
        gtk_widget_add_css_class(b, "body");
        gtk_box_append(GTK_BOX(col), b);
    }
    gtk_box_append(GTK_BOX(row), col);
    gtk_box_append(GTK_BOX(card), row);
    GtkWidget *acts = NULL;
    for (char **a = n->actions; a && a[0] && a[1]; a += 2) {
        if (!strcmp(a[0], "default")) continue;
        if (!acts) {
            acts = HBOX(6, "notif-actions");
            gtk_box_set_homogeneous(GTK_BOX(acts), TRUE);
            gtk_box_append(GTK_BOX(card), acts);
        }
        GtkWidget *bt = gtk_button_new_with_label(*a[1] ? a[1] : a[0]);
        gtk_widget_add_css_class(bt, "notif-action");
        g_object_set_data_full(G_OBJECT(bt), "key", g_strdup(a[0]), g_free);
        g_signal_connect(bt, "clicked", G_CALLBACK(on_card_action), GUINT_TO_POINTER(n->id));
        gtk_widget_set_cursor_from_name(bt, "pointer");
        gtk_box_append(GTK_BOX(acts), bt);
    }
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "released", G_CALLBACK(on_card_click), GUINT_TO_POINTER(n->id));
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    gtk_widget_set_cursor_from_name(row, "pointer");
    return card;
}

// ---- popups
static void popup_place(void) {
    if (!NT.win || !LS) return;
    GtkWindow *w = GTK_WINDOW(NT.win);
    gboolean bottom = g_str_has_prefix(NT.pos, "bottom");
    gboolean left = g_str_has_suffix(NT.pos, "left"), right = g_str_has_suffix(NT.pos, "right");
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_TOP, !bottom);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_BOTTOM, bottom);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_LEFT, left);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_RIGHT, right);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_TOP, 2);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_BOTTOM, 2);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_LEFT, 0);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_RIGHT, 0);
    gtk_box_set_spacing(GTK_BOX(NT.col), 0);
}

static void popup_win(void) {
    if (NT.win) return;
    NT.win = gtk_window_new();
    GtkWindow *w = GTK_WINDOW(NT.win);
    gtk_window_set_application(w, app);
    gtk_window_set_decorated(w, FALSE);
    gtk_window_set_resizable(w, FALSE);
    gtk_widget_add_css_class(NT.win, "hyprminimal-notifications");
    if (LS) {
        gtk_layer_init_for_window(w);
        gtk_layer_set_namespace(w, "hyprminimal-notifications");
        gtk_layer_set_layer(w, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(w, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);   // never takes the keyboard
    }
    NT.col = VBOX(0, "notif-popups");
    gtk_widget_set_size_request(NT.col, 480, -1);
    gtk_window_set_child(w, NT.col);
    popup_place();
}

static gboolean popup_expire(gpointer id) {
    Notif *n = notif_find(GPOINTER_TO_UINT(id), NULL);
    if (!n) return G_SOURCE_REMOVE;
    n->timer = 0;
    if (n->transient) { notif_remove(n->id, 1); return G_SOURCE_REMOVE; }
    if (n->popup) { gtk_box_remove(GTK_BOX(NT.col), n->popup); n->popup = NULL; }
    popups_fit();
    return G_SOURCE_REMOVE;
}

static void popup_show(Notif *n) {
    if (NT.dnd) {
        if (n->transient) notif_remove(n->id, 1);
        return;
    }
    popup_win();
    GtkWidget *card = notif_card(n, FALSE);
    if (n->urgency != 2) gtk_widget_add_css_class(card, "countdown");
    if (n->popup) {   // replaced: same place in the column
        gtk_box_insert_child_after(GTK_BOX(NT.col), card, n->popup);
        gtk_box_remove(GTK_BOX(NT.col), n->popup);
    } else if (g_str_has_prefix(NT.pos, "bottom")) gtk_box_append(GTK_BOX(NT.col), card);   // newest nearest the edge
    else gtk_box_prepend(GTK_BOX(NT.col), card);
    n->popup = card;
    if (n->timer) { g_source_remove(n->timer); n->timer = 0; }
    if (n->urgency != 2) n->timer = g_timeout_add_seconds(NT.timeout_s, popup_expire, GUINT_TO_POINTER(n->id));
    gtk_widget_set_visible(NT.win, TRUE);
}

// ---- D-Bus
static char *sanitize_body(const char *body) {
    if (!body || !*body) return g_strdup("");
    // the spec's <a href> and <img> aren't Pango: keep the link text, drop images. A bare & (common in
    // plain-text bodies) becomes &amp; so the rest of the markup still works
    static GRegex *tags, *amp;
    if (!tags) tags = g_regex_new("<a\\b[^>]*>|</a>|<img\\b[^>]*/?>", G_REGEX_CASELESS, 0, NULL);
    if (!amp) amp = g_regex_new("&(?!#[0-9]+;|#x[0-9a-fA-F]+;|[a-zA-Z]+;)", 0, 0, NULL);
    char *t = g_regex_replace_literal(tags, body, -1, 0, "", 0, NULL);
    char *s = t ? g_regex_replace_literal(amp, t, -1, 0, "&amp;", 0, NULL) : NULL;
    g_free(t);
    if (!s || !pango_parse_markup(s, -1, 0, NULL, NULL, NULL, NULL)) {
        g_free(s);
        return g_markup_escape_text(body, -1);
    }
    return s;
}

static GdkTexture *hint_image(GVariant *v) {   // (iiibiiay): width, height, rowstride, alpha, bits, channels, data
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE("(iiibiiay)"))) return NULL;
    int w, h, stride, bits, ch;
    gboolean alpha;
    GVariant *data;
    g_variant_get(v, "(iiibii@ay)", &w, &h, &stride, &alpha, &bits, &ch, &data);
    GdkTexture *t = NULL;
    gsize len;
    const guchar *px = g_variant_get_fixed_array(data, &len, 1);
    if (w > 0 && h > 0 && bits == 8 && (ch == 3 || ch == 4) && len >= (gsize)stride * (h - 1) + (gsize)w * ch) {
        GBytes *b = g_bytes_new(px, len);
        t = gdk_memory_texture_new(w, h, ch == 4 ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, b, stride);
        g_bytes_unref(b);
    }
    g_variant_unref(data);
    return t;
}

static guint do_notify(GVariant *p) {
    const char *app, *icon, *summary, *body;
    guint replaces;
    const char **actions;
    GVariant *hints;
    int expire;
    g_variant_get(p, "(&su&s&s&s^a&s@a{sv}i)", &app, &replaces, &icon, &summary, &body, &actions, &hints, &expire);
    (void)expire;   // ours: Settings → Notifications → Show for (critical: until dismissed)
    Notif *n = replaces ? notif_find(replaces, NULL) : NULL;
    if (n) {
        g_free(n->app); g_free(n->summary); g_free(n->body); g_free(n->icon);
        g_clear_object(&n->image);
        g_strfreev(n->actions);
    } else {
        n = g_new0(Notif, 1);
        n->id = NT.next_id++;
        if (!NT.next_id) NT.next_id = 1;
        g_ptr_array_insert(NT.list, 0, n);
    }
    n->app = g_strdup(app);
    n->summary = g_strdup(summary);
    n->body = sanitize_body(body);
    n->icon = g_str_has_prefix(icon, "file://") ? g_filename_from_uri(icon, NULL, NULL) : g_strdup(icon);
    n->actions = g_strdupv((char **)actions);
    g_free(actions);
    n->time = g_get_real_time();
    n->urgency = 1;
    n->transient = FALSE;
    guchar u;
    if (g_variant_lookup(hints, "urgency", "y", &u)) n->urgency = u;
    gboolean tr;
    if (g_variant_lookup(hints, "transient", "b", &tr)) n->transient = tr;
    static const char *img_keys[] = {"image-data", "image_data", "icon_data"};
    for (int k = 0; k < 3 && !n->image; k++) {
        GVariant *v = g_variant_lookup_value(hints, img_keys[k], NULL);
        n->image = hint_image(v);
        if (v) g_variant_unref(v);
    }
    const char *ip;
    if (!n->image && (g_variant_lookup(hints, "image-path", "&s", &ip) || g_variant_lookup(hints, "image_path", "&s", &ip)) && *ip) {
        g_free(n->icon);
        n->icon = g_str_has_prefix(ip, "file://") ? g_filename_from_uri(ip, NULL, NULL) : g_strdup(ip);
    }
    g_variant_unref(hints);
    while (NT.list->len > NOTIF_KEEP) {
        Notif *old = g_ptr_array_index(NT.list, NT.list->len - 1);
        g_ptr_array_remove_index(NT.list, NT.list->len - 1);
        notif_free(old);
    }
    popup_show(n);
    notifs_changed();
    return n->id;
}

static void on_method(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method,
                      GVariant *p, GDBusMethodInvocation *inv, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface; (void)d;
    if (!strcmp(method, "Notify")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", do_notify(p)));
    } else if (!strcmp(method, "CloseNotification")) {
        guint id;
        g_variant_get(p, "(u)", &id);
        notif_remove(id, 3);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "GetCapabilities")) {
        const char *caps[] = {"body", "body-markup", "actions", "icon-static", "persistence"};
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@as)", g_variant_new_strv(caps, G_N_ELEMENTS(caps))));
    } else if (!strcmp(method, "GetServerInformation")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ssss)", "hyprminimal", "hyprminimal", "1.0", "1.2"));
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
    }
}

static const char NOTIFY_XML[] =
    "<node><interface name='org.freedesktop.Notifications'>"
    "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/>"
    "<arg type='a{sv}' direction='in'/><arg type='i' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
    "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
    "<method name='GetServerInformation'><arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "<arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
    "<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
    "<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
    "</interface></node>";

static void on_bus(GDBusConnection *c, const char *name, gpointer d) {
    (void)name; (void)d;
    static const GDBusInterfaceVTable vt = {on_method, NULL, NULL, {0}};
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(NOTIFY_XML, NULL);
    NT.bus = c;
    g_dbus_connection_register_object(c, "/org/freedesktop/Notifications", info->interfaces[0], &vt, NULL, NULL, NULL);
    g_dbus_node_info_unref(info);
}

static void on_name_lost(GDBusConnection *c, const char *name, gpointer d) {
    (void)c; (void)d;
    g_printerr("panels: lost the bus name %s (another notification daemon is running?)\n", name);
}

// ---- settings and DND
static void notify_conf(void) {   // .generated/notify.conf: position=…, timeout=… (Settings → Notifications)
    char *f = g_build_filename(GEN, "notify.conf", NULL), *t = NULL;
    if (g_file_get_contents(f, &t, NULL, NULL)) {
        char **l = g_strsplit(t, "\n", -1);
        for (char **x = l; *x; x++) {
            if (g_str_has_prefix(*x, "position=")) snprintf(NT.pos, sizeof NT.pos, "%s", *x + 9);
            else if (g_str_has_prefix(*x, "timeout=")) NT.timeout_s = CLAMP(atoi(*x + 8), 1, 600);
        }
        g_strfreev(l);
    }
    g_free(t);
    g_free(f);
    popup_place();
}

static void dnd_set(gboolean on) {
    NT.dnd = on;
    char *f = g_build_filename(NT.run, "dnd", NULL);
    if (on) g_file_set_contents(f, "on\n", -1, NULL);
    else g_unlink(f);
    g_free(f);
    if (on) {   // what is on screen goes away now
        for (guint i = 0; i < NT.list->len; i++) {
            Notif *n = g_ptr_array_index(NT.list, i);
            if (n->popup && n->urgency != 2) { if (n->timer) g_source_remove(n->timer); n->timer = 0; popup_expire(GUINT_TO_POINTER(n->id)); }
        }
    }
    notifs_changed();
}

static void notify_init(void) {
    NT.list = g_ptr_array_new();
    NT.run = g_build_filename(g_get_user_runtime_dir(), "hyprminimal", NULL);
    g_mkdir_with_parents(NT.run, 0700);
    char *f = g_build_filename(NT.run, "dnd", NULL);
    NT.dnd = g_file_test(f, G_FILE_TEST_EXISTS);
    g_free(f);
    notify_conf();
    bell_update();
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.Notifications",
                   G_BUS_NAME_OWNER_FLAGS_REPLACE | G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT, on_bus, NULL, on_name_lost,
                   NULL, NULL);
}
