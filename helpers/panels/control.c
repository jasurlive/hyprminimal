// Included by helpers/panels.c. Control center (Super+S, the bell, the media capsule).
// Top to bottom: [Wi-Fi | ›] [System], toggles (Bluetooth, Caffeine, Night Light, Silent), Lock / Reboot /
// Shut Down, the current player (playerctld), the notification header and list (opens collapsed: the
// newest card; the header expands it). Keys: 1-7 the tiles in reading order, K play/pause, C clear all.
// Live tile text (network, CPU/RAM, Bluetooth) is read off the UI thread when it opens.

typedef struct { gboolean ok, wifi_on, bt; char *wifi; int cpu, ram; } CcInfo;

static struct {
    GtkWidget *wifi, *wifi_sub, *sys_sub, *t[4];   // t: bluetooth, caffeine, night light, silent
    GtkWidget *media, *art, *title, *artist, *play;
    GtkWidget *head, *head_lbl, *clear, *scroll, *list;
    gboolean expanded, sync;
    guint gen, rebuild;
    char *art_url;
    GDBusProxy *player;
    unsigned long long idle0, total0;
} CC;

static Panel ccp;

static char *msg_bin(void) { return g_build_filename(REPO, "bin", "hyprminimal-msg", NULL); }

static void run_msg(const char *a, const char *b) {   // bin/hyprminimal-msg a [b], detached
    char *m = msg_bin();
    const char *argv[] = {m, a, b, NULL};
    spawn_app(argv);
    g_free(m);
}

static gboolean proc_running(const char *name) {   // a process with this name (comm) exists
    GDir *d = g_dir_open("/proc", 0, NULL);
    gboolean found = FALSE;
    if (!d) return FALSE;
    for (const char *e; !found && (e = g_dir_read_name(d));) {
        if (!g_ascii_isdigit(*e)) continue;
        char p[64], comm[32];
        snprintf(p, sizeof p, "/proc/%s/comm", e);
        found = read_small(p, comm, sizeof comm) && !strcmp(g_strchomp(comm), name);
    }
    g_dir_close(d);
    return found;
}

static gboolean caffeine_on(void) {
    char *f = g_build_filename(NT.run, "caffeine.pid", NULL), b[32];
    gboolean on = read_small(f, b, sizeof b) && kill(atoi(b), 0) == 0;
    g_free(f);
    return on;
}

// ---- tiles
static GtkWidget *tile(const char *title, GtkWidget **sub, const char *key, const char *classes) {
    GtkWidget *b = gtk_button_new(), *row = HBOX(6, NULL), *col = VBOX(0, NULL);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(col), lbl(title, "tile-title", 0.5));
    if (sub) {
        *sub = lbl("", "tile-sub", 0.5);
        gtk_box_append(GTK_BOX(col), *sub);
    }
    gtk_box_append(GTK_BOX(row), col);
    if (key) gtk_box_append(GTK_BOX(row), keycap(key));
    gtk_button_set_child(GTK_BUTTON(b), row);
    add_classes(b, classes);
    return b;
}

static void cc_show_toggles(gboolean wifi, gboolean bt) {
    set_class(CC.wifi, "on", wifi);
    set_class(CC.t[0], "on", bt);
    set_class(CC.t[1], "on", caffeine_on());
    set_class(CC.t[2], "on", proc_running("hyprsunset"));
    set_class(CC.t[3], "on", NT.dnd);
}

static void on_cc_wifi(GtkButton *b, gpointer d) {
    (void)d;
    gboolean on = !gtk_widget_has_css_class(GTK_WIDGET(b), "on");
    set_class(GTK_WIDGET(b), "on", on);
    gtk_label_set_label(GTK_LABEL(CC.wifi_sub), on ? "…" : "Off");
    const char *argv[] = {"timeout", "5", "nmcli", "radio", "wifi", on ? "on" : "off", NULL};
    spawn_detached(argv);
}

static void on_cc_toggle(GtkButton *b, gpointer i) {
    gboolean on = !gtk_widget_has_css_class(GTK_WIDGET(b), "on");
    set_class(GTK_WIDGET(b), "on", on);   // show it at once
    switch (GPOINTER_TO_INT(i)) {
    case 0: run_msg("bluetooth", on ? "on" : "off"); break;
    case 1: run_msg("caffeine", NULL); break;
    case 2: run_msg("nightlight", NULL); break;
    case 3: dnd_set(on); break;
    }
}

static void on_cc_open(GtkButton *b, gpointer kind) { (void)b; manager_toggle(kind); }   // wifi list, system

static void on_cc_session(GtkButton *b, gpointer i) {
    (void)b;
    manager_close();
    if (GPOINTER_TO_INT(i) == 0) { run_msg("lock", NULL); return; }
    const char *argv[] = {"systemctl", GPOINTER_TO_INT(i) == 1 ? "reboot" : "poweroff", NULL};
    spawn_app(argv);
}

// ---- live info (worker thread)
static void cc_info_free(gpointer d) { CcInfo *i = d; g_free(i->wifi); g_free(i); }

static void cc_info_work(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    CcInfo *i = g_new0(CcInfo, 1);
    gint64 t0 = g_get_monotonic_time();
    char *radio = cmd_out(4, "nmcli", "radio", "wifi", NULL);
    i->wifi_on = radio && !strcmp(radio, "enabled");
    g_free(radio);
    if (i->wifi_on) {
        char *l = cmd_out(4, "nmcli", "-t", "-f", "ACTIVE,SIGNAL,SSID", "dev", "wifi", "list", "--rescan", "no", NULL);
        char **lines = g_strsplit(l ? l : "", "\n", -1);
        for (char **x = lines; *x && !i->wifi; x++) {
            char **f = split_terse(*x);
            if (g_strv_length(f) >= 3 && !strcmp(f[0], "yes")) {
                char *head = g_utf8_strlen(f[2], -1) > 12 ? g_utf8_substring(f[2], 0, 11) : NULL;
                i->wifi = g_strdup_printf("%s%s · %s%%", head ? head : f[2], head ? "…" : "", f[1]);
                g_free(head);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(l);
        if (!i->wifi) i->wifi = g_strdup("Not connected");
    } else i->wifi = g_strdup("Off");
    char *bt = cmd_out(2, "systemctl", "is-active", "bluetooth", NULL);
    if (bt && !strcmp(bt, "active")) {
        char *show = cmd_out(2, "bluetoothctl", "show", NULL);
        i->bt = show && strstr(show, "Powered: yes");
        g_free(show);
    }
    g_free(bt);
    gint64 left = 250000 - (g_get_monotonic_time() - t0);   // CPU: at least a quarter second between samples
    if (left > 0) g_usleep(left);
    unsigned long long idle, total;
    cpu_times(&idle, &total);
    i->cpu = total > CC.total0 ? (int)(100 * (1 - (double)(idle - CC.idle0) / (total - CC.total0))) : 0;
    long long mt = 0, ma = 0;
    FILE *f = fopen("/proc/meminfo", "re");
    if (f) {
        char k[64];
        long long v;
        while (fscanf(f, "%63s %lld %*s", k, &v) == 2) {
            if (!strcmp(k, "MemTotal:")) mt = v;
            else if (!strcmp(k, "MemAvailable:")) { ma = v; break; }
        }
        fclose(f);
    }
    i->ram = mt ? (int)(100 * (mt - ma) / mt) : 0;
    i->ok = TRUE;
    g_task_return_pointer(t, i, cc_info_free);
}

static void cc_info_done(GObject *src, GAsyncResult *res, gpointer gen) {
    (void)src;
    CcInfo *i = g_task_propagate_pointer(G_TASK(res), NULL);
    if (i && GPOINTER_TO_UINT(gen) == CC.gen) {
        gtk_label_set_label(GTK_LABEL(CC.wifi_sub), i->wifi);
        char s[64];
        snprintf(s, sizeof s, "CPU %d%% · RAM %d%%", i->cpu, i->ram);
        gtk_label_set_label(GTK_LABEL(CC.sys_sub), s);
        cc_show_toggles(i->wifi_on, i->bt);
    }
    if (i) cc_info_free(i);
}

// ---- media (playerctld: the current player)
static void cc_media_update(void) {
    if (!CC.player) { gtk_widget_set_visible(CC.media, FALSE); return; }
    GVariant *st = g_dbus_proxy_get_cached_property(CC.player, "PlaybackStatus");
    GVariant *md = g_dbus_proxy_get_cached_property(CC.player, "Metadata");
    const char *s = st ? g_variant_get_string(st, NULL) : "";
    gboolean playing = !strcmp(s, "Playing"), show = playing || !strcmp(s, "Paused");
    gtk_widget_set_visible(CC.media, show);
    if (show) {
        const char *title = NULL, *art = NULL;
        const char **artists = NULL;
        if (md) {
            g_variant_lookup(md, "xesam:title", "&s", &title);
            g_variant_lookup(md, "xesam:artist", "^a&s", &artists);
            g_variant_lookup(md, "mpris:artUrl", "&s", &art);
        }
        gtk_label_set_label(GTK_LABEL(CC.title), title && *title ? title : "Unknown title");
        char *who = artists && artists[0] ? g_strjoinv(", ", (char **)artists) : g_strdup("");
        gtk_label_set_label(GTK_LABEL(CC.artist), who);
        gtk_widget_set_visible(CC.artist, *who);
        g_free(who);
        g_free(artists);
        gtk_button_set_label(GTK_BUTTON(CC.play), playing ? "\U000f03e4" : "\U000f040a");   // pause / play
        if (g_strcmp0(art, CC.art_url) != 0) {   // album art: local files only (players cache it there)
            g_free(CC.art_url);
            CC.art_url = g_strdup(art);
            char *path = art && g_str_has_prefix(art, "file://") ? g_filename_from_uri(art, NULL, NULL) : NULL;
            GdkTexture *tex = path ? gdk_texture_new_from_filename(path, NULL) : NULL;
            gtk_image_set_from_paintable(GTK_IMAGE(CC.art), tex ? GDK_PAINTABLE(tex) : NULL);
            gtk_widget_set_visible(CC.art, tex != NULL);
            if (tex) g_object_unref(tex);
            g_free(path);
        }
    }
    if (st) g_variant_unref(st);
    if (md) g_variant_unref(md);
}

static void player_changed(GDBusProxy *p, GVariant *ch, GStrv inv, gpointer d) {
    (void)p; (void)ch; (void)inv; (void)d;
    if (current == &ccp) cc_media_update();
}

static void player_owner(GObject *o, GParamSpec *ps, gpointer d) { (void)o; (void)ps; (void)d; if (current == &ccp) cc_media_update(); }

static void player_ready(GObject *src, GAsyncResult *res, gpointer d) {
    (void)src; (void)d;
    CC.player = g_dbus_proxy_new_for_bus_finish(res, NULL);
    if (!CC.player) return;
    g_signal_connect(CC.player, "g-properties-changed", G_CALLBACK(player_changed), NULL);
    g_signal_connect(CC.player, "notify::g-name-owner", G_CALLBACK(player_owner), NULL);
    if (current == &ccp) cc_media_update();
}

static void player_call(const char *method) {
    if (CC.player) g_dbus_proxy_call(CC.player, method, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL, NULL);
}

static void on_media(GtkButton *b, gpointer m) { (void)b; player_call(m); }

// ---- notifications
static void cc_list_build(void) {
    CC.rebuild = 0;
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(CC.list))) gtk_box_remove(GTK_BOX(CC.list), c);
    guint n = NT.list->len;
    char s[64];
    if (n) snprintf(s, sizeof s, "\U000f009a   %u notification%s   %s", n, n == 1 ? "" : "s",
                    n > 1 ? (CC.expanded ? "\U000f0143" : "\U000f0140") : "");
    else snprintf(s, sizeof s, "\U000f009c   No notifications");
    gtk_label_set_label(GTK_LABEL(CC.head_lbl), s);
    gtk_widget_set_sensitive(CC.clear, n > 0);
    gtk_widget_set_visible(CC.scroll, n > 0);
    for (guint i = 0; i < n && (CC.expanded || i == 0); i++)
        gtk_box_append(GTK_BOX(CC.list), notif_card(g_ptr_array_index(NT.list, i), TRUE));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(CC.scroll), GTK_POLICY_NEVER,
                                   CC.expanded ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(CC.scroll), CC.expanded ? 560 : 400);
}

static gboolean cc_list_later(gpointer d) { (void)d; cc_list_build(); return G_SOURCE_REMOVE; }

static void cc_notifs_changed(void) {
    if (current == &ccp && !CC.rebuild) CC.rebuild = g_idle_add(cc_list_later, NULL);
}

static void on_cc_head(GtkButton *b, gpointer d) { (void)b; (void)d; CC.expanded = !CC.expanded; cc_list_build(); }
static void on_cc_clear(GtkButton *b, gpointer d) { (void)b; (void)d; notif_clear_all(); }

// ---- panel
static gboolean cc_key(Panel *p, guint kv) {
    (void)p;
    GtkWidget *keyed[] = {CC.wifi, NULL, NULL, CC.t[0], CC.t[1], CC.t[2], CC.t[3]};
    if (kv >= GDK_KEY_1 && kv <= GDK_KEY_7) {
        int i = kv - GDK_KEY_1;
        if (i == 1) manager_toggle("wifi");
        else if (i == 2) manager_toggle("system");
        else g_signal_emit_by_name(keyed[i], "clicked");
        return TRUE;
    }
    if (kv == GDK_KEY_k) { player_call("PlayPause"); return TRUE; }
    if (kv == GDK_KEY_c) { notif_clear_all(); return TRUE; }
    return FALSE;
}

static void cc_build(Panel *p, GtkWidget *body) {
    (void)p;
    gtk_widget_add_css_class(body, "cc");
    gtk_box_set_spacing(GTK_BOX(body), 8);
    // [ Wi-Fi | › ] [ System ]
    GtkWidget *top = HBOX(6, NULL), *wgrp = HBOX(0, "joined");
    gtk_box_set_homogeneous(GTK_BOX(top), TRUE);
    CC.wifi = tile("\U000f05a9   Wi-Fi", &CC.wifi_sub, "1", "tile cc-tile left");
    gtk_widget_set_hexpand(CC.wifi, TRUE);
    g_signal_connect(CC.wifi, "clicked", G_CALLBACK(on_cc_wifi), NULL);
    GtkWidget *more = tile("\U000f0142", NULL, "2", "tile cc-tile right more");
    g_signal_connect(more, "clicked", G_CALLBACK(on_cc_open), "wifi");
    gtk_box_append(GTK_BOX(wgrp), CC.wifi);
    gtk_box_append(GTK_BOX(wgrp), more);
    GtkWidget *sys = tile("\U000f04c5   System", &CC.sys_sub, "3", "tile cc-tile");
    g_signal_connect(sys, "clicked", G_CALLBACK(on_cc_open), "system");
    gtk_box_append(GTK_BOX(top), wgrp);
    gtk_box_append(GTK_BOX(top), sys);
    gtk_box_append(GTK_BOX(body), top);
    // toggles, 2 x 2
    static const char *names[] = {"\U000f00af   Bluetooth", "\U000f0176   Caffeine", "\U000f0594   Night Light",
                                  "\U000f009b   Silent"}, *keys[] = {"4", "5", "6", "7"};
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    for (int i = 0; i < 4; i++) {
        CC.t[i] = tile(names[i], NULL, keys[i], "tile cc-tile toggle");
        g_signal_connect(CC.t[i], "clicked", G_CALLBACK(on_cc_toggle), GINT_TO_POINTER(i));
        gtk_grid_attach(GTK_GRID(grid), CC.t[i], i % 2, i / 2, 1, 1);
    }
    gtk_box_append(GTK_BOX(body), grid);
    // session
    static const char *sess[] = {"\U000f033e  Lock", "\U000f0453  Reboot", "\U000f0425  Shut Down"};
    GtkWidget *srow = HBOX(6, NULL);
    gtk_box_set_homogeneous(GTK_BOX(srow), TRUE);
    for (int i = 0; i < 3; i++) {
        GtkWidget *b = gtk_button_new_with_label(sess[i]);
        add_classes(b, "pill");
        g_signal_connect(b, "clicked", G_CALLBACK(on_cc_session), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(srow), b);
    }
    gtk_box_append(GTK_BOX(body), srow);
    // media
    CC.media = VBOX(6, "card media");
    GtkWidget *mrow = HBOX(12, NULL), *mcol = VBOX(2, NULL);
    CC.art = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(CC.art), 48);
    gtk_widget_add_css_class(CC.art, "art");
    gtk_widget_set_valign(mcol, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(mcol, TRUE);
    CC.title = lbl("", "m-title", 0);
    CC.artist = lbl("", "m-artist", 0);
    gtk_box_append(GTK_BOX(mcol), CC.title);
    gtk_box_append(GTK_BOX(mcol), CC.artist);
    gtk_box_append(GTK_BOX(mrow), CC.art);
    gtk_box_append(GTK_BOX(mrow), mcol);
    gtk_box_append(GTK_BOX(CC.media), mrow);
    GtkWidget *ctl = HBOX(14, NULL), *pcol = VBOX(2, NULL);
    gtk_widget_set_halign(ctl, GTK_ALIGN_CENTER);
    GtkWidget *prev = gtk_button_new_with_label("\U000f04ae"), *next = gtk_button_new_with_label("\U000f04ad");
    CC.play = gtk_button_new_with_label("\U000f040a");
    GtkWidget *mb[] = {prev, CC.play, next};
    const char *mm[] = {"Previous", "PlayPause", "Next"};
    for (int i = 0; i < 3; i++) {
        add_classes(mb[i], "round flat-icon");
        gtk_widget_set_valign(mb[i], GTK_ALIGN_START);
        g_signal_connect(mb[i], "clicked", G_CALLBACK(on_media), (gpointer)mm[i]);
    }
    gtk_widget_set_halign(pcol, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(pcol), CC.play);
    GtkWidget *kk = keycap("K");
    gtk_widget_set_halign(kk, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(pcol), kk);
    gtk_box_append(GTK_BOX(ctl), prev);
    gtk_box_append(GTK_BOX(ctl), pcol);
    gtk_box_append(GTK_BOX(ctl), next);
    gtk_box_append(GTK_BOX(CC.media), ctl);
    gtk_widget_set_visible(CC.media, FALSE);
    gtk_box_append(GTK_BOX(body), CC.media);
    // notifications: [ 󰂚 N notifications ⌄ ] [ Clear all (C) ]
    CC.head = HBOX(6, "notif-head");
    GtkWidget *hb = gtk_button_new();
    CC.head_lbl = lbl("", NULL, 0.5);
    gtk_button_set_child(GTK_BUTTON(hb), CC.head_lbl);
    gtk_widget_set_hexpand(hb, TRUE);
    add_classes(hb, "flat-row");
    g_signal_connect(hb, "clicked", G_CALLBACK(on_cc_head), NULL);
    CC.clear = gtk_button_new();
    GtkWidget *crow = HBOX(6, NULL);
    gtk_box_append(GTK_BOX(crow), lbl("Clear all", NULL, 0));
    gtk_box_append(GTK_BOX(crow), keycap("C"));
    gtk_button_set_child(GTK_BUTTON(CC.clear), crow);
    add_classes(CC.clear, "flat-row outlined");
    g_signal_connect(CC.clear, "clicked", G_CALLBACK(on_cc_clear), NULL);
    gtk_box_append(GTK_BOX(CC.head), hb);
    gtk_box_append(GTK_BOX(CC.head), CC.clear);
    gtk_box_append(GTK_BOX(body), CC.head);
    CC.list = VBOX(8, "cc-list");
    CC.scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(CC.scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(CC.scroll), CC.list);
    gtk_box_append(GTK_BOX(body), CC.scroll);
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL,
                             "org.mpris.MediaPlayer2.playerctld", "/org/mpris/MediaPlayer2",
                             "org.mpris.MediaPlayer2.Player", NULL, player_ready, NULL);
}

static void cc_opened(Panel *p) {
    (void)p;
    CC.gen++;
    CC.expanded = FALSE;   // opens collapsed every time
    cpu_times(&CC.idle0, &CC.total0);
    cc_show_toggles(gtk_widget_has_css_class(CC.wifi, "on"), gtk_widget_has_css_class(CC.t[0], "on"));
    cc_media_update();
    cc_list_build();
    GTask *t = g_task_new(NULL, NULL, cc_info_done, GUINT_TO_POINTER(CC.gen));
    g_task_run_in_thread(t, cc_info_work);
    g_object_unref(t);
}

static void cc_closed(Panel *p) {
    (void)p;
    CC.gen++;
    if (CC.rebuild) { g_source_remove(CC.rebuild); CC.rebuild = 0; }
}

static Panel ccp = {.kind = "control", .width = 420, .build = cc_build, .opened = cc_opened, .closed = cc_closed,
                    .key = cc_key};
