// Included by helpers/panels.c. Volume / brightness OSD: a small box under the bar,
// centred, that never takes the keyboard or a click and hides 1.5 s after the last change.
//   osd "volume:+5" | "volume:-3" | "volume:mute"   wpctl on the default sink (capped at 100%)
//   osd "brightness:+5" | "brightness:-2"            /sys/class/backlight, set through logind (no root, no fork)
// Changes run one at a time on a worker thread, in the order the keys came.
#define OSD_HIDE_MS 1500

typedef struct { char *what; int pct; gboolean muted, ok, bright; } OsdReq;

static struct { GtkWidget *win, *icon, *bar, *val; guint hide; GThreadPool *pool; } O;

static gboolean osd_hide(gpointer d) { (void)d; O.hide = 0; gtk_widget_set_visible(O.win, FALSE); return G_SOURCE_REMOVE; }

static void on_osd_map(GtkWidget *w, gpointer d);

static void osd_win(void) {
    if (O.win) return;
    O.win = gtk_window_new();
    GtkWindow *w = GTK_WINDOW(O.win);
    gtk_window_set_application(w, app);
    gtk_window_set_decorated(w, FALSE);
    gtk_window_set_resizable(w, FALSE);
    gtk_widget_add_css_class(O.win, "hyprminimal-osd");
    if (LS) {
        gtk_layer_init_for_window(w);
        gtk_layer_set_namespace(w, "hyprminimal-osd");
        gtk_layer_set_layer(w, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(w, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_TOP, TRUE);   // centred under the bar
        gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_TOP, POPUP_TOP);
    }
    GtkWidget *row = HBOX(14, "hm-osd");
    O.icon = lbl("", "osd-icon", 0.5);
    O.bar = gtk_level_bar_new_for_interval(0, 100);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(O.bar), GTK_LEVEL_BAR_OFFSET_LOW);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(O.bar), GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(O.bar), GTK_LEVEL_BAR_OFFSET_FULL);
    gtk_widget_set_size_request(O.bar, 280, -1);
    gtk_widget_set_valign(O.bar, GTK_ALIGN_CENTER);
    O.val = lbl("", "osd-val", 1);
    gtk_label_set_width_chars(GTK_LABEL(O.val), 3);
    gtk_box_append(GTK_BOX(row), O.icon);
    gtk_box_append(GTK_BOX(row), O.bar);
    gtk_box_append(GTK_BOX(row), O.val);
    gtk_window_set_child(w, row);
    gtk_widget_set_can_target(row, FALSE);
    g_signal_connect(O.win, "map", G_CALLBACK(on_osd_map), NULL);
}

static void on_osd_map(GtkWidget *w, gpointer d) {   // clicks go through to what is underneath
    (void)d;
    GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(w));
    cairo_region_t *none = cairo_region_create();
    if (s) gdk_surface_set_input_region(s, none);
    cairo_region_destroy(none);
}

static gboolean osd_show(gpointer d) {
    OsdReq *r = d;
    if (r->ok) {
        osd_win();
        static const char *vol[] = {"\U000f057f", "\U000f0580", "\U000f057e"}, *bri[] = {"\U000f00de", "\U000f00df", "\U000f00e0"};
        int lvl = r->pct < 34 ? 0 : r->pct < 67 ? 1 : 2;
        gtk_label_set_label(GTK_LABEL(O.icon), r->bright ? bri[lvl] : r->muted ? "\U000f075f" : vol[lvl]);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(O.bar), r->pct);
        set_class(O.win, "muted", !r->bright && r->muted);
        char s[8];
        snprintf(s, sizeof s, "%d", r->pct);
        gtk_label_set_label(GTK_LABEL(O.val), s);
        gtk_widget_set_visible(O.win, TRUE);
        if (O.hide) g_source_remove(O.hide);
        O.hide = g_timeout_add(OSD_HIDE_MS, osd_hide, NULL);
    }
    g_free(r->what);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static void osd_volume(OsdReq *r, const char *arg) {
    if (!strcmp(arg, "mute")) {
        char *o = cmd_out(3, "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL);
        g_free(o);
    } else {
        int step = atoi(arg);
        char amt[16];
        snprintf(amt, sizeof amt, "%d%%%c", abs(step), step < 0 ? '-' : '+');
        char *o = cmd_out(3, "wpctl", "set-volume", "-l", "1", "@DEFAULT_AUDIO_SINK@", amt, NULL);
        g_free(o);
        if (step > 0) { o = cmd_out(3, "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "0", NULL); g_free(o); }
    }
    char *g = cmd_out(3, "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL);   // "Volume: 0.45 [MUTED]"
    double v;
    if (g && sscanf(g, "Volume: %lf", &v) == 1) {
        r->pct = (int)lround(v * 100);
        r->muted = strstr(g, "MUTED") != NULL;
        r->ok = TRUE;
    }
    g_free(g);
}

// acpi_video0 and intel_backlight can both exist; rank by type like systemd-backlight does
static char *backlight_dev(void) {   // firmware > platform > raw
    static const char *rank[] = {"firmware", "platform", "raw"};
    GDir *d = g_dir_open("/sys/class/backlight", 0, NULL);
    if (!d) return NULL;
    char *best = NULL;
    int best_r = G_N_ELEMENTS(rank);
    for (const char *e; (e = g_dir_read_name(d));) {
        char p[256], t[16] = "";
        snprintf(p, sizeof p, "/sys/class/backlight/%s/type", e);
        if (!read_small(p, t, sizeof t)) continue;
        g_strchomp(t);
        for (int r = 0; r < best_r; r++)
            if (!strcmp(t, rank[r])) { best_r = r; g_free(best); best = g_strdup(e); break; }
    }
    g_dir_close(d);
    return best;
}

static void osd_brightness(OsdReq *r, const char *arg) {
    char *name = backlight_dev();
    if (!name) return;
    char p[256];
    snprintf(p, sizeof p, "/sys/class/backlight/%s/max_brightness", name);
    long long max = read_ll(p, 0);
    snprintf(p, sizeof p, "/sys/class/backlight/%s/brightness", name);
    long long cur = read_ll(p, -1);
    if (max > 0 && cur >= 0) {
        int pct = CLAMP((int)lround(100.0 * cur / max) + atoi(arg), 0, 100);
        guint32 val = (guint32)llround(pct * (double)max / 100);
        GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
        GVariant *res = sys ? g_dbus_connection_call_sync(sys, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                                                          "org.freedesktop.login1.Session", "SetBrightness",
                                                          g_variant_new("(ssu)", "backlight", name, val), NULL,
                                                          G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL) : NULL;
        if (res) { g_variant_unref(res); r->pct = pct; r->ok = TRUE; r->bright = TRUE; }
        if (sys) g_object_unref(sys);
    }
    g_free(name);
}

static void osd_work(gpointer data, gpointer user) {
    (void)user;
    OsdReq *r = data;
    char *colon = strchr(r->what, ':');
    if (colon) {
        *colon = 0;
        if (!strcmp(r->what, "volume")) osd_volume(r, colon + 1);
        else if (!strcmp(r->what, "brightness")) osd_brightness(r, colon + 1);
    }
    g_idle_add(osd_show, r);
}

static void osd_request(const char *what) {
    if (!O.pool) O.pool = g_thread_pool_new(osd_work, NULL, 1, FALSE, NULL);   // one at a time, in order
    OsdReq *r = g_new0(OsdReq, 1);
    r->what = g_strdup(what);
    g_thread_pool_push(O.pool, r, NULL);
}
