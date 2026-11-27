// The Settings window (Super+I): one more window of this resident program, so it opens instantly and costs
// little. Closing parks it on a special workspace nothing shows (Hyprland keeps it mapped, sends it no frames):
// the next Super+I only moves it back. hypr/layerrules.lua floats and centres it (class dev.hyprminimal.Settings)
// and closes it on a click outside (bin/hyprminimal-msg settings-close).

#define PARK "special:hyprminimal-settings"
#define SETTINGS_APP_ID "dev.hyprminimal.Settings"

enum { CH_BAR, CH_NOTIFY, CH_THEME, CH_NIGHT, CH_DESKTOP, CH_STARTUP, CH_N };

typedef struct {
    const char *id, *label, *icon, *words, *error_comp;   // error_comp: the part whose failure this page shows
    GtkWidget *(*build)(void);
    GtkWidget *row;
} SPage;

static struct {
    GtkWidget *win, *search, *nav, *stack, *title, *run_banner, *err_banner, *err_lbl, *err_btn;
    GtkWidget *toast, *toast_lbl, *scrim, *dlg;
    guint toast_timer, pending[CH_N];
    char *remembered;
    const char *err_comp;
    gboolean quiet, shown, syncing;
    GtkWidget *hide_bar;                   // Bar → Hide bar switch (Super+M keeps it in sync)
} SW;

static void settings_hide(void);

// ---------------------------------------------------------------- Hyprland IPC (what hyprctl does, no process)
static char *hypr_ctl(const char *cmd) {
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE"), *rt = getenv("XDG_RUNTIME_DIR");
    if (!sig) return NULL;
    char *path = g_strdup_printf("%s/hypr/%s/.socket.sock", rt ? rt : "/tmp", sig);
    GSocketClient *c = g_socket_client_new();
    g_socket_client_set_timeout(c, 1);
    GSocketAddress *a = g_unix_socket_address_new(path);
    GSocketConnection *conn = g_socket_client_connect(c, G_SOCKET_CONNECTABLE(a), NULL, NULL);
    char *out = NULL;
    if (conn) {
        GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        if (g_output_stream_write_all(os, cmd, strlen(cmd), NULL, NULL, NULL)) {
            GString *s = g_string_new(NULL);
            char buf[16384];
            gssize n;
            GInputStream *is = g_io_stream_get_input_stream(G_IO_STREAM(conn));
            while ((n = g_input_stream_read(is, buf, sizeof buf, NULL, NULL)) > 0) g_string_append_len(s, buf, n);
            out = g_string_free(s, FALSE);
        }
        g_object_unref(conn);
    }
    g_object_unref(a);
    g_object_unref(c);
    g_free(path);
    return out;
}

static JsonNode *hypr_json(const char *cmd) {
    char *j = g_strconcat("j/", cmd, NULL), *out = hypr_ctl(j);
    JsonNode *n = out ? json_parse_text(out, NULL) : NULL;
    g_free(j);
    g_free(out);
    return n;
}

static void hypr_dispatch(const char *lua) {
    char *c = g_strconcat("dispatch ", lua, NULL);
    g_free(hypr_ctl(c));
    g_free(c);
}

// our window as Hyprland sees it: address and workspace (FALSE while unmapped)
static gboolean settings_client(char **addr, char **ws_name, gint64 *ws_id) {
    JsonNode *n = hypr_json("clients");
    gboolean found = FALSE;
    if (n && JSON_NODE_HOLDS_ARRAY(n)) {
        JsonArray *a = json_node_get_array(n);
        for (guint i = 0; i < json_array_get_length(a) && !found; i++) {
            JsonObject *o = json_array_get_object_element(a, i);
            if (json_object_get_int_member_with_default(o, "pid", 0) != getpid() ||
                strcmp(json_object_get_string_member_with_default(o, "class", ""), SETTINGS_APP_ID)) continue;
            found = TRUE;
            if (addr) *addr = g_strdup(json_object_get_string_member_with_default(o, "address", ""));
            JsonObject *w = json_object_get_object_member(o, "workspace");
            if (ws_name) *ws_name = g_strdup(w ? json_object_get_string_member_with_default(w, "name", "") : "");
            if (ws_id) *ws_id = w ? json_object_get_int_member_with_default(w, "id", 0) : 0;
        }
    }
    if (n) json_node_unref(n);
    return found;
}

static void active_workspace(char **name, gint64 *id) {
    JsonNode *n = hypr_json("activeworkspace");
    JsonObject *o = n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
    if (name) *name = g_strdup(o ? json_object_get_string_member_with_default(o, "name", "") : "");
    if (id) *id = o ? json_object_get_int_member_with_default(o, "id", G_MININT64) : G_MININT64;
    if (n) json_node_unref(n);
}

// ---------------------------------------------------------------- toast, banners, dialogs
static gboolean toast_off(gpointer d) {
    (void)d;
    SW.toast_timer = 0;
    gtk_widget_set_visible(SW.toast, FALSE);
    return G_SOURCE_REMOVE;
}

static void settings_toast(const char *text, int seconds) {
    if (!SW.toast) return;
    gtk_label_set_label(GTK_LABEL(SW.toast_lbl), text);
    gtk_widget_set_visible(SW.toast, TRUE);
    if (SW.toast_timer) g_source_remove(SW.toast_timer);
    SW.toast_timer = g_timeout_add_seconds(seconds, toast_off, NULL);
}

static gboolean hyprminimal_active(void) {
    char *p = g_build_filename(RUN_DIR, "active", NULL);
    gboolean on = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return on;
}

static SPage *current_page(void) {
    GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(SW.nav));
    return r ? g_object_get_data(G_OBJECT(r), "hm-page") : NULL;
}

static void update_banners(void) {
    gtk_widget_set_visible(SW.run_banner, !hyprminimal_active());
    SPage *p = current_page();
    const char *comp = p ? p->error_comp : NULL;
    char *msg = comp ? read_error(comp) : g_strdup("");
    if (*msg) {
        const char *name = !strcmp(comp, "waybar") ? "Bar" : !strcmp(comp, "panels") ? "Popups, notifications and OSD" : "Settings";
        char *t = g_strdup_printf("%s failed: %s", name, msg);
        gtk_label_set_label(GTK_LABEL(SW.err_lbl), t);
        g_free(t);
    }
    gtk_button_set_label(GTK_BUTTON(SW.err_btn), comp && !strcmp(comp, "settings") ? "Apply again" : "Retry");
    gtk_widget_set_visible(SW.err_banner, *msg);
    SW.err_comp = comp;
    g_free(msg);
}

static void dialog_close(void) {
    if (!SW.dlg) return;
    gtk_box_remove(GTK_BOX(SW.scrim), SW.dlg);
    SW.dlg = NULL;
    gtk_widget_set_visible(SW.scrim, FALSE);
}

static void dialog_close_clicked(GtkButton *b, gpointer d) { (void)b; (void)d; dialog_close(); }

// a dialog card over the window: header (Cancel · title · ok_label), body returned to fill
static GtkWidget *dialog_new(const char *title, const char *ok_label, const char *ok_class, GtkWidget **ok) {
    dialog_close();
    GtkWidget *card = VBOX(14, "hm-dialog"), *head = HBOX(8, NULL);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    GtkWidget *cancel = text_btn("Cancel", "hm-btn");
    g_signal_connect(cancel, "clicked", G_CALLBACK(dialog_close_clicked), NULL);
    GtkWidget *t = lbl(title, "hm-dialog-title", 0.5);
    gtk_widget_set_hexpand(t, TRUE);
    gtk_box_append(GTK_BOX(head), cancel);
    gtk_box_append(GTK_BOX(head), t);
    if (ok_label) {
        *ok = text_btn(ok_label, ok_class ? ok_class : "hm-btn suggested");
        gtk_box_append(GTK_BOX(head), *ok);
    }
    gtk_box_append(GTK_BOX(card), head);
    GtkWidget *body = VBOX(12, NULL);
    gtk_box_append(GTK_BOX(card), body);
    SW.dlg = card;
    gtk_box_append(GTK_BOX(SW.scrim), card);
    gtk_widget_set_visible(SW.scrim, TRUE);
    return body;
}

typedef struct { void (*fn)(gpointer); gpointer data; GDestroyNotify free; } Confirm;
static void confirm_free(gpointer p) { Confirm *c = p; if (c->free) c->free(c->data); g_free(c); }
static void confirm_ok(GtkButton *b, gpointer d) {
    (void)b;
    Confirm *c = d;
    c->fn(c->data);
    dialog_close();   // frees c (and its data) with the button
}

// "Forget X?" body [Cancel] [verb]: fn(data) on verb; data freed with free_fn afterwards
static void confirm(const char *heading, const char *body, const char *verb, void (*fn)(gpointer), gpointer data,
                    GDestroyNotify free_fn) {
    GtkWidget *ok, *b = dialog_new(heading, verb, "hm-btn destructive", &ok);
    GtkWidget *l = gtk_label_new(body);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 50);
    gtk_widget_add_css_class(l, "hm-row-sub");
    gtk_box_append(GTK_BOX(b), l);
    Confirm *c = g_new0(Confirm, 1);
    c->fn = fn; c->data = data; c->free = free_fn;
    g_object_set_data_full(G_OBJECT(ok), "hm-confirm", c, confirm_free);
    g_signal_connect(ok, "clicked", G_CALLBACK(confirm_ok), c);
}

// ---------------------------------------------------------------- applying changes
static void spawn_quiet(const char *const *argv) { spawn_app(argv); }

static void reload_bar(void) {
    // `hyprminimal respawn` restarts waybar locked (never twice); a crash or a config waybar rejects is
    // caught by its watchdog. Not SIGUSR2: waybar can segfault on in-place reloads.
    if (!hyprminimal_active()) return;
    char *bin = g_build_filename(REPO, "bin", "hyprminimal", NULL);
    const char *argv[] = {bin, "respawn", "waybar", NULL};
    spawn_quiet(argv);
    g_free(bin);
}

static void apply_nightlight(void) {
    char *sh = cfg_bool("nightlight", "enabled")
                   ? g_strdup_printf("pkill -x hyprsunset; sleep 0.2; exec hyprsunset -t %d", (int)cfg_int("nightlight", "temperature"))
                   : g_strdup("pkill -x hyprsunset");
    const char *argv[] = {"sh", "-c", sh, NULL};
    spawn_quiet(argv);
    g_free(sh);
}

static gboolean fire_change(gpointer d) {
    int what = GPOINTER_TO_INT(d);
    SW.pending[what] = 0;
    if (what == CH_BAR) reload_bar();
    else if (what == CH_NIGHT) apply_nightlight();
    else if (what == CH_THEME) {
        // the popups and this window restyle from .generated/panels.css by themselves
        reload_bar();
        gtk_theme();
        const char *k[] = {"pkill", "-USR1", "-x", "kitty", NULL};   // kitty re-reads its colours
        spawn_quiet(k);
        g_free(hypr_ctl("reload"));                                 // window borders
    }
    return G_SOURCE_REMOVE;
}

// save + render now, reload the affected part shortly after (debounced)
static void changed(int what) {
    save_settings();
    GError *err = NULL;
    if (!render(&err)) {   // the running shell keeps its last rendered files
        char *m = g_strdup_printf("Not applied: %s", err ? err->message : "render failed");
        write_error("settings", err ? err->message : "render failed");
        settings_toast(m, 6);
        g_free(m);
        g_clear_error(&err);
        update_banners();
        return;
    }
    write_error("settings", NULL);
    if (SW.pending[what]) g_source_remove(SW.pending[what]);
    SW.pending[what] = g_timeout_add(450, fire_change, GINT_TO_POINTER(what));
}

typedef struct { const char *sect, *key; int what; } Bind;
static Bind *bind_new(const char *sect, const char *key, int what) {
    Bind *b = g_new(Bind, 1);
    b->sect = sect; b->key = key; b->what = what;
    return b;
}

static void on_bool(GtkSwitch *sw, GParamSpec *ps, gpointer d) {
    (void)ps;
    if (SW.syncing) return;
    Bind *b = d;
    cfg_set_bool(b->sect, b->key, gtk_switch_get_active(sw));
    changed(b->what);
}

static GtkWidget *cfg_switch(GtkWidget *list, const char *title, const char *sub, const char *sect, const char *key, int what) {
    GtkWidget *sw = switch_row(list, title, sub, cfg_bool(sect, key));
    g_signal_connect_data(sw, "notify::active", G_CALLBACK(on_bool), bind_new(sect, key, what), (GClosureNotify)(void (*)(void))g_free, 0);
    return sw;
}

static void on_spin(GtkSpinButton *sp, gpointer d) {
    Bind *b = d;
    cfg_set_int(b->sect, b->key, (gint64)gtk_spin_button_get_value(sp));
    changed(b->what);
}

static GtkWidget *cfg_spin(GtkWidget *list, const char *title, const char *sub, const char *sect, const char *key,
                           double lo, double hi, int what) {
    GtkWidget *sp = spin_row(list, title, sub, lo, hi, 1, cfg_int(sect, key));
    g_signal_connect_data(sp, "value-changed", G_CALLBACK(on_spin), bind_new(sect, key, what), (GClosureNotify)(void (*)(void))g_free, 0);
    return sp;
}

typedef struct { Bind b; const char *const *values; gboolean ints; } ComboBind;

static void on_combo(GtkDropDown *dd, GParamSpec *ps, gpointer d) {
    (void)ps;
    ComboBind *c = d;
    guint i = gtk_drop_down_get_selected(dd);
    if (i == GTK_INVALID_LIST_POSITION) return;
    if (c->ints) cfg_set_int(c->b.sect, c->b.key, g_ascii_strtoll(c->values[i], NULL, 10));
    else cfg_set_str(c->b.sect, c->b.key, c->values[i]);
    changed(c->b.what);
}

// values[i] is stored for labels[i] (ints: stored as a number)
static GtkWidget *cfg_combo(GtkWidget *list, const char *title, const char *sect, const char *key,
                            const char *const *values, const char *const *labels, gboolean ints, int what) {
    int sel = -1;
    char cur[32];
    if (ints) snprintf(cur, sizeof cur, "%" G_GINT64_FORMAT, cfg_int(sect, key));
    for (int i = 0; values[i]; i++)
        if (!strcmp(values[i], ints ? cur : cfg_str(sect, key))) sel = i;
    GtkWidget *dd = combo_row(list, title, NULL, labels, sel);
    ComboBind *c = g_new(ComboBind, 1);
    c->b = (Bind){sect, key, what};
    c->values = values;
    c->ints = ints;
    g_signal_connect_data(dd, "notify::selected", G_CALLBACK(on_combo), c, (GClosureNotify)(void (*)(void))g_free, 0);
    return dd;
}

// ---------------------------------------------------------------- General
static void theme_toggled(GtkCheckButton *c, gpointer id) {
    if (!gtk_check_button_get_active(c) || !strcmp(cfg_str("general", "theme"), id)) return;
    cfg_set_str("general", "theme", id);
    changed(CH_THEME);
}

static void row_check(GtkWidget *row, gpointer chk) { (void)row; gtk_check_button_set_active(chk, TRUE); }

static void swatch_paint(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer col) {
    (void)a;
    int c[3];
    hex_rgb(col, c);
    cairo_set_source_rgb(cr, c[0] / 255.0, c[1] / 255.0, c[2] / 255.0);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
}

static GtkWidget *swatches(const char *id) {
    Roles r;
    roles(id, &r);
    const char *cols[4] = {r.t->bg, r.c[R_SURFACE_VAR], r.c[R_PRIMARY], r.c[R_ON_SURFACE_VAR]};
    GtkWidget *b = HBOX(0, "hm-swatches");
    gtk_widget_set_overflow(b, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    for (int i = 0; i < 4; i++) {
        GtkWidget *da = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), 20);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), 22);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), swatch_paint, g_strdup(cols[i]), g_free);
        gtk_box_append(GTK_BOX(b), da);
    }
    return b;
}

static void desktop_name_apply(GtkWidget *e, gpointer d) {
    (void)d;
    cfg_set_str("desktop", "name", gtk_editable_get_text(GTK_EDITABLE(e)));
    changed(CH_DESKTOP);
    settings_toast("Name under the logo saved", 3);
}

static void font_chosen(GtkFontDialogButton *b, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    PangoFontDescription *fd = gtk_font_dialog_button_get_font_desc(b);
    const char *fam = fd ? pango_font_description_get_family(fd) : NULL;
    if (!fam || !strcmp(fam, cfg_str("general", "font"))) return;
    cfg_set_str("general", "font", fam);
    changed(CH_BAR);
    gtk_theme();   // other apps' UI font
}

static GtkWidget *page_general(void) {
    GtkWidget *p, *sc = page_new(&p);
    GtkWidget *g = group_new(p, "Theme", "Bar, panels, notifications, OSD, launcher, Settings, terminal, lock screen "
                                         "and window borders all follow it.");
    const char *cur = theme_get(cfg_str("general", "theme"))->id;
    GtkWidget *first = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(THEMES); i++) {
        GtkWidget *row = row_add(g, THEMES[i].label, THEMES[i].desc), *chk = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), !strcmp(THEMES[i].id, cur));
        if (first) gtk_check_button_set_group(GTK_CHECK_BUTTON(chk), GTK_CHECK_BUTTON(first));
        else first = chk;
        gtk_widget_set_cursor_from_name(chk, "pointer");
        row_prefix(row, chk);
        row_suffix(row, swatches(THEMES[i].id));
        row_on_activate(row, row_check, chk);
        g_signal_connect(chk, "toggled", G_CALLBACK(theme_toggled), (gpointer)THEMES[i].id);
    }
    GtkWidget *gd = group_new(p, "Desktop", "The background behind your windows: theme colour, Arch logo and a name. "
                                            "Logo off and an empty name: a plain colour, nothing else.");
    entry_row(gd, "Name under the logo (empty: none)", cfg_str("desktop", "name"), desktop_name_apply, NULL);
    cfg_switch(gd, "Logo", "Arch Linux logo in the middle of the desktop", "desktop", "logo", CH_DESKTOP);
    GtkWidget *gf = group_new(p, "Font", NULL);
    GtkWidget *fr = row_add(gf, "Interface font", "Used by the bar, panels, Settings, lock screen and other apps");
    GtkWidget *fb = gtk_font_dialog_button_new(gtk_font_dialog_new());
    gtk_font_dialog_button_set_level(GTK_FONT_DIALOG_BUTTON(fb), GTK_FONT_LEVEL_FAMILY);
    PangoFontDescription *fd = pango_font_description_from_string(cfg_str("general", "font"));
    gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(fb), fd);
    pango_font_description_free(fd);
    gtk_widget_set_valign(fb, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(fb, "pointer");
    g_signal_connect(fb, "notify::font-desc", G_CALLBACK(font_chosen), NULL);
    row_suffix(fr, fb);
    return sc;
}

// ---------------------------------------------------------------- Bar
static int tray_item_count(void) {   // apps showing a tray icon (StatusNotifierItem) right now
    GDBusConnection *bus = g_application_get_dbus_connection(G_APPLICATION(app));
    if (!bus) return 0;
    GVariant *r = g_dbus_connection_call_sync(bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                                              "org.freedesktop.DBus.Properties", "Get",
                                              g_variant_new("(ss)", "org.kde.StatusNotifierWatcher", "RegisteredStatusNotifierItems"),
                                              G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    if (!r) return 0;
    GVariant *v;
    g_variant_get(r, "(v)", &v);
    int n = g_variant_is_of_type(v, G_VARIANT_TYPE("as")) ? (int)g_variant_n_children(v) : 0;
    g_variant_unref(v);
    g_variant_unref(r);
    return n;
}

static GtkWidget *page_bar(void) {
    GtkWidget *p, *sc = page_new(&p);
    GtkWidget *gv = group_new(p, "Visibility", NULL);
    SW.hide_bar = cfg_switch(gv, "Hide bar", "Windows use the full screen; widgets and their popups are gone until turned "
                                             "back on. Super+M toggles it.", "bar", "hidden", CH_BAR);
    GtkWidget *gt = group_new(p, "Text and icons", "One scale for the whole bar: every label uses the text size, every icon "
                                                   "the icon size, all with the same weight.");
    cfg_spin(gt, "Text size", "pixels", "bar", "font_size", 11, 24, CH_BAR);
    cfg_spin(gt, "Icon size", "pixels", "bar", "icon_size", 12, 32, CH_BAR);
    cfg_spin(gt, "Clock size", "pixels", "bar", "clock_size", 11, 32, CH_BAR);
    static const char *wv[] = {"400", "500", "600", "700", NULL}, *wl[] = {"Regular", "Medium", "Semibold", "Bold", NULL};
    cfg_combo(gt, "Weight", "bar", "font_weight", wv, wl, TRUE, CH_BAR);
    GtkWidget *gc = group_new(p, "Clock", NULL);
    cfg_switch(gc, "24-hour clock", NULL, "bar", "clock_24h", CH_BAR);
    static const char *sides[][2] = {{"left", "Widgets: Left"}, {"center", "Widgets: Center"}, {"right", "Widgets: Right"}};
    for (int s = 0; s < 3; s++) {
        GtkWidget *gw = group_new(p, sides[s][1], NULL);
        for (size_t i = 0; i < G_N_ELEMENTS(WIDGETS); i++) {
            if (strcmp(WIDGETS[i].side, sides[s][0])) continue;
            char *sub = NULL;
            if (!strcmp(WIDGETS[i].key, "tray")) {
                int n = tray_item_count();
                sub = n ? g_strdup_printf("Icons of apps that offer one (Telegram, Discord, Steam, AnyDesk…). %d app%s showing an icon now.",
                                          n, n != 1 ? "s" : "")
                        : g_strdup("Icons of apps that offer one (Telegram, Discord, Steam, AnyDesk…). No running app offers one "
                                   "right now, so it stays hidden.");
            }
            cfg_switch(gw, WIDGETS[i].label, sub, "bar.widgets", WIDGETS[i].key, CH_BAR);
            g_free(sub);
        }
    }
    return sc;
}

// Super+M (bin/hyprminimal-msg bar) just showed/hid the running bar: save that, without restarting waybar
static void settings_bar_toggled(void) {
    if (!CFG) load_settings();
    cfg_set_bool("bar", "hidden", !cfg_bool("bar", "hidden"));
    save_settings();
    GError *err = NULL;
    if (!render(&err)) { write_error("settings", err ? err->message : "render failed"); g_clear_error(&err); return; }
    if (SW.hide_bar && gtk_switch_get_active(GTK_SWITCH(SW.hide_bar)) != cfg_bool("bar", "hidden")) {
        SW.syncing = TRUE;   // no handler: no bar restart
        gtk_switch_set_active(GTK_SWITCH(SW.hide_bar), cfg_bool("bar", "hidden"));
        SW.syncing = FALSE;
    }
}

// ---------------------------------------------------------------- Notifications
static void dnd_switched(GtkSwitch *sw, GParamSpec *ps, gpointer d) { (void)ps; (void)d; dnd_set(gtk_switch_get_active(sw)); }
static void test_notification(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    const char *argv[] = {"notify-send", "-a", "hyprminimal", "Test notification", "This is how it looks", NULL};
    spawn_quiet(argv);
}

static GtkWidget *page_notifications(void) {
    GtkWidget *p, *sc = page_new(&p);
    GtkWidget *g = group_new(p, "Popups", NULL);
    static const char *pv[] = {"top-right", "top-center", "top-left", "bottom-right", "bottom-center", "bottom-left", NULL};
    static const char *pl[] = {"Top right", "Top center", "Top left", "Bottom right", "Bottom center", "Bottom left", NULL};
    cfg_combo(g, "Position", "notifications", "position", pv, pl, FALSE, CH_NOTIFY);
    cfg_spin(g, "Show for", "seconds", "notifications", "timeout", 1, 60, CH_NOTIFY);
    GtkWidget *g2 = group_new(p, "Do not disturb", NULL);
    GtkWidget *sw = switch_row(g2, "Do not disturb", "Hide popups; notifications still go to the control center", NT.dnd);
    g_signal_connect(sw, "notify::active", G_CALLBACK(dnd_switched), NULL);
    GtkWidget *r = row_add(g2, "Send a test notification", NULL), *b = text_btn("Test", "hm-btn pill");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(test_notification), NULL);
    row_suffix(r, b);
    row_on_activate(r, test_notification, NULL);
    return sc;
}

// ---------------------------------------------------------------- Startup
static struct { GtkWidget *add, *list; } SU;
static void su_fill(void);

static void su_add_cmd(const char *cmd) {
    char *c = g_strstrip(g_strdup(cmd));
    if (*c) {
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "command", c);
        json_object_set_boolean_member(o, "enabled", TRUE);
        json_array_add_object_element(cfg_startup(), o);
        changed(CH_STARTUP);
        entry_set_text(SU.add, "");
        su_fill();
        settings_toast("Added: runs at your next login", 3);
    }
    g_free(c);
}

static void su_add_apply(GtkWidget *e, gpointer d) { (void)d; su_add_cmd(gtk_editable_get_text(GTK_EDITABLE(e))); }
static void su_add_clicked(GtkButton *b, gpointer d) { (void)b; (void)d; su_add_cmd(gtk_editable_get_text(GTK_EDITABLE(SU.add))); }

static void su_picked(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    char *path = g_file_get_path(f), *q = g_shell_quote(path);
    // not executable: run it through sh rather than fail silently at login
    char *cmd = access(path, X_OK) == 0 ? g_strdup(q) : g_strdup_printf("sh %s", q);
    su_add_cmd(cmd);
    g_free(cmd); g_free(q); g_free(path);
    g_object_unref(f);
}

static void su_pick(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Script to run at login");
    GFile *home = g_file_new_for_path(g_get_home_dir());
    gtk_file_dialog_set_initial_folder(dlg, home);
    g_object_unref(home);
    gtk_file_dialog_open(dlg, GTK_WINDOW(SW.win), NULL, su_picked, NULL);
    g_object_unref(dlg);
}

static JsonObject *su_item(GtkWidget *w) {   // the startup entry a row's widget edits
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "hm-index"));
    JsonArray *a = cfg_startup();
    return i < json_array_get_length(a) ? json_array_get_object_element(a, i) : NULL;
}

static void su_edit(GtkWidget *e, gpointer d) {
    (void)d;
    JsonObject *o = su_item(e);
    char *c = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(e))));
    if (o && *c) {
        json_object_set_string_member(o, "command", c);
        changed(CH_STARTUP);
        settings_toast("Startup command saved", 3);
    }
    g_free(c);
}

static void su_switched(GtkSwitch *sw, GParamSpec *ps, gpointer row) {
    (void)ps;
    JsonObject *o = su_item(GTK_WIDGET(sw));
    if (!o) return;
    json_object_set_boolean_member(o, "enabled", gtk_switch_get_active(sw));
    row_set_title(row, gtk_switch_get_active(sw) ? "Runs at login" : "Off");
    changed(CH_STARTUP);
}

static void su_run(GtkButton *b, gpointer e) {
    (void)b;
    char *cmd = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(e))));
    if (*cmd) {
        g_mkdir_with_parents(RUN_DIR, 0755);
        char *log = g_build_filename(RUN_DIR, "startup.log", NULL), *ql = g_shell_quote(log);
        char *sh = g_strdup_printf("cd ~ && exec sh -c %s >>%s 2>&1 </dev/null", "\"$0\"", ql);
        const char *argv[] = {"sh", "-c", sh, cmd, NULL};
        spawn_quiet(argv);
        char *t = g_strdup_printf("Started: %s", cmd);
        settings_toast(t, 3);
        g_free(t); g_free(sh); g_free(ql); g_free(log);
    }
    g_free(cmd);
}

static gboolean su_fill_idle(gpointer d) { (void)d; su_fill(); return G_SOURCE_REMOVE; }

static void su_remove(GtkButton *b, gpointer d) {
    (void)d;
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "hm-index"));
    JsonArray *a = cfg_startup();
    if (i < json_array_get_length(a)) json_array_remove_element(a, i);
    changed(CH_STARTUP);
    g_idle_add(su_fill_idle, NULL);   // not while the clicked button is being destroyed
}

static void su_fill(void) {
    for (GtkWidget *c; (c = gtk_widget_get_first_child(SU.list));) gtk_list_box_remove(GTK_LIST_BOX(SU.list), c);
    JsonArray *a = cfg_startup();
    guint n = json_array_get_length(a);
    if (!n) dim_row(SU.list, "Nothing yet", "Add a command above");
    for (guint i = 0; i < n; i++) {
        JsonNode *node = json_array_get_element(a, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *o = json_node_get_object(node);
        gboolean on = !json_object_has_member(o, "enabled") || json_object_get_boolean_member(o, "enabled");
        GtkWidget *row, *e = entry_row_full(SU.list, on ? "Runs at login" : "Off",
                                            json_object_get_string_member_with_default(o, "command", ""), FALSE, su_edit, NULL, &row);
        GtkWidget *sw = gtk_switch_new(), *run = icon_btn("media-playback-start-symbolic", "Run now");
        GtkWidget *rm = icon_btn("user-trash-symbolic", "Remove");
        gtk_switch_set_active(GTK_SWITCH(sw), on);
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(sw, "Run at login");
        gtk_widget_set_cursor_from_name(sw, "pointer");
        for (GtkWidget *const *w = (GtkWidget *const[]){e, sw, rm, NULL}; *w; w++)
            g_object_set_data(G_OBJECT(*w), "hm-index", GUINT_TO_POINTER(i));
        g_signal_connect(sw, "notify::active", G_CALLBACK(su_switched), row);
        g_signal_connect(run, "clicked", G_CALLBACK(su_run), e);
        g_signal_connect(rm, "clicked", G_CALLBACK(su_remove), NULL);
        row_prefix(row, sw);
        row_suffix(row, run);
        row_suffix(row, rm);
    }
}

static GtkWidget *page_startup(void) {
    GtkWidget *p, *sc = page_new(&p);
    GtkWidget *ga = group_new(p, "Add", "Anything you would type in a terminal: an app (telegram-desktop), a script "
                                        "(~/bin/sync.sh) or a shell line (sleep 5 &amp;&amp; discord).");
    GtkWidget *row;
    SU.add = entry_row_full(ga, "Command or script", "", FALSE, su_add_apply, NULL, &row);
    GtkWidget *ok = g_object_get_data(G_OBJECT(SU.add), "hm-ok");
    gtk_widget_unparent(ok);   // the + button adds instead
    g_object_set_data(G_OBJECT(SU.add), "hm-ok", NULL);
    GtkWidget *pick = icon_btn("document-open-symbolic", "Choose a script…"), *add = icon_btn("list-add-symbolic", "Add");
    g_signal_connect(pick, "clicked", G_CALLBACK(su_pick), NULL);
    g_signal_connect(add, "clicked", G_CALLBACK(su_add_clicked), NULL);
    row_suffix(row, pick);
    row_suffix(row, add);
    char *log = g_build_filename(RUN_DIR, "startup.log", NULL), *el = g_markup_escape_text(log, -1);
    char *desc = g_strdup_printf("Started once per login, after the bar is up (not again on hyprminimal restart). "
                                 "Output goes to %s.", el);
    SU.list = group_new(p, "Run at login", desc);
    g_free(desc); g_free(el); g_free(log);
    su_fill();
    return sc;
}

// ---------------------------------------------------------------- Night light
static char *kelvin(GtkScale *s, double v, gpointer d) { (void)s; (void)d; return g_strdup_printf("%.0f K", v); }
static void temp_changed(GtkRange *r, gpointer d) {
    (void)d;
    cfg_set_int("nightlight", "temperature", (gint64)gtk_range_get_value(r));
    changed(CH_NIGHT);
}

static GtkWidget *page_nightlight(void) {
    GtkWidget *p, *sc = page_new(&p);
    GtkWidget *g = group_new(p, "Night light", "Warmer colours to reduce eye strain (hyprsunset).");
    cfg_switch(g, "Enabled", NULL, "nightlight", "enabled", CH_NIGHT);
    GtkWidget *r = row_add(g, "Temperature", NULL);
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 2500, 6500, 100);
    gtk_range_set_value(GTK_RANGE(s), cfg_int("nightlight", "temperature"));
    gtk_widget_set_size_request(s, 280, -1);
    gtk_scale_set_draw_value(GTK_SCALE(s), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_LEFT);
    gtk_scale_set_format_value_func(GTK_SCALE(s), kelvin, NULL, NULL);
    gtk_widget_set_cursor_from_name(s, "pointer");
    g_signal_connect(s, "value-changed", G_CALLBACK(temp_changed), NULL);
    row_suffix(r, s);
    return sc;
}

// ---------------------------------------------------------------- System, About
static void shell_cmd(const char *action) {
    char *bin = g_build_filename(REPO, "bin", "hyprminimal", NULL);
    const char *argv[] = {bin, action, NULL};
    spawn_quiet(argv);   // its own session: survives this program being restarted by it
    g_free(bin);
    settings_toast(!strcmp(action, "start") ? "Starting hyprminimal…" : "Restarting hyprminimal…", 3);
}

static void restart_shell(GtkWidget *w, gpointer d) { (void)w; (void)d; shell_cmd(hyprminimal_active() ? "restart" : "start"); }

static void open_tool(GtkWidget *w, gpointer cmd) {
    (void)w;
    char *p = g_find_program_in_path(cmd);
    if (p) { const char *argv[] = {cmd, NULL}; spawn_quiet(argv); }
    else { char *t = g_strdup_printf("%s is not installed", (char *)cmd); settings_toast(t, 3); g_free(t); }
    g_free(p);
}

static GtkWidget *action_row(GtkWidget *list, const char *title, const char *sub, const char *label, RowFn fn, gpointer d) {
    GtkWidget *r = row_add(list, title, sub), *b = text_btn(label, "hm-btn pill");
    gtk_widget_set_can_target(b, FALSE);   // the whole row is the button
    row_suffix(r, b);
    row_on_activate(r, fn, d);
    return r;
}

static GtkWidget *page_system(void) {
    GtkWidget *p, *sc = page_new(&p);
    GtkWidget *g = group_new(p, "Shell", NULL);
    action_row(g, "Restart hyprminimal", "Reload bar, popups, notifications and OSD", "Restart", restart_shell, NULL);
    GtkWidget *g2 = group_new(p, "System settings", NULL);
    action_row(g2, "Audio", "Output, input and app volumes", "Open", open_tool, "pavucontrol");
    action_row(g2, "Displays", "Resolution, scale, arrangement", "Open", open_tool, "nwg-displays");
    action_row(g2, "Bluetooth", "Devices", "Open", open_tool, "blueman-manager");
    return sc;
}

static GtkWidget *page_about(void) {
    GtkWidget *p, *sc = page_new(&p);
    GtkWidget *g = group_new(p, "hyprminimal", "A minimal shell for Hyprland, official Arch packages only.");
    static const char *pkgs[] = {"hyprland", "waybar", "hyprlock", NULL};
    char *out = NULL;
    g_spawn_sync(NULL, (char *[]){"pacman", "-Q", "hyprland", "waybar", "hyprlock", NULL}, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL);
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (int i = 0; pkgs[i]; i++) {
        const char *ver = "not installed";
        for (char **l = lines; *l; l++) {
            char **f = g_strsplit(*l, " ", 3);
            if (f[0] && f[1] && !strcmp(f[0], pkgs[i])) { row_add(g, pkgs[i], f[1]); ver = NULL; }
            g_strfreev(f);
            if (!ver) break;
        }
        if (ver) row_add(g, pkgs[i], ver);
    }
    g_strfreev(lines);
    g_free(out);
    GtkWidget *g2 = group_new(p, "Files", NULL);
    row_sub_selectable(row_add(g2, "Settings", SETTINGS_PATH));
    char *kb = g_build_filename(g_get_home_dir(), ".config", "hypr", "keybind.lua", NULL);
    row_sub_selectable(row_add(g2, "Keybinds", kb));
    g_free(kb);
    return sc;
}
