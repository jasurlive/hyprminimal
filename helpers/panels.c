// pkg-config: gtk4-layer-shell-0 gtk4
// hyprminimal bar popups: calendar (clock), battery, network, system. One small resident GTK4 program,
// built by bin/hyprminimal into .generated/bin/panels and started hidden by `hyprminimal start`.
// Replaces the Python panels that kept the whole Settings app (~110 MB) resident: this costs a few MB,
// reads /proc and /sys directly, and only shells out for nmcli (network panel) and the hotspot
// (settings/nm.py), off the UI thread.
//
// Also owns the desktop background (theme colour + Arch logo, replaces a wallpaper program), the clock
// click strip on the bar (.generated/clock_zone.json) and the click-outside catcher for rofi popups. Theme: .generated/panels.css (written by `hyprminimal-settings --apply`), reloaded live.
// D-Bus (org.gtk.Actions on dev.hyprminimal.Panels, /dev/hyprminimal/Panels), used by bin/hyprminimal-msg:
//   panel s       toggle a popup: calendar | battery | network | system | launcher | session
//   panel-close   close the open popup (Hyprland, on a click outside it)
//   catch s       a rofi popup is open (its process name; "" = it closed): outside clicks close it
//   quit
// The launcher (Super+R) and session menu live here too: shown, not started, so they open in one frame
// (rofi took ~100 ms per key press). rofi stays the fallback when this program isn't running.
// The Wi-Fi network list (control center ›) and the Settings window stay in Python (settings/), on demand.
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#define POPUP_TOP 8      // gap under the bar (its exclusive zone already reserves bar + margin)
#define POPUP_RIGHT 4    // right edge in line with the bar (bar margin is 4)

static GtkApplication *app;
static char *REPO, *GEN;         // repo, repo/.generated
static gboolean LS;              // layer shell available (Wayland compositor supports it)

// --------------------------------------------------------------------------- small helpers
static gboolean read_small(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "re");
    if (!f) return FALSE;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return r > 0;
}

static long long read_ll(const char *path, long long def) {
    char b[64];
    if (!read_small(path, b, sizeof b)) return def;
    char *e;
    long long v = strtoll(b, &e, 10);
    return e == b ? def : v;
}

static void add_classes(GtkWidget *w, const char *classes) {
    if (!classes) return;
    char **v = g_strsplit(classes, " ", -1);
    for (char **c = v; *c; c++)
        if (**c) gtk_widget_add_css_class(w, *c);
    g_strfreev(v);
}

// one-line label that may ellipsize, so changing text never resizes the popup
static GtkWidget *lbl(const char *text, const char *classes, float xalign) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    add_classes(l, classes);
    return l;
}

static GtkWidget *box(GtkOrientation o, int spacing, const char *classes) {
    GtkWidget *b = gtk_box_new(o, spacing);
    add_classes(b, classes);
    return b;
}
#define VBOX(s, c) box(GTK_ORIENTATION_VERTICAL, s, c)
#define HBOX(s, c) box(GTK_ORIENTATION_HORIZONTAL, s, c)

static void set_class(GtkWidget *w, const char *cls, gboolean on) {
    if (on) gtk_widget_add_css_class(w, cls);
    else gtk_widget_remove_css_class(w, cls);
}

// GTK4 has no CSS cursor: hand pointer on everything clickable
static void pointerize(GtkWidget *w) {
    if (GTK_IS_BUTTON(w) || GTK_IS_SWITCH(w) || GTK_IS_CALENDAR(w)) gtk_widget_set_cursor_from_name(w, "pointer");
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) pointerize(c);
}

// key/value grid: "IP address ........ 192.168.1.4"
static GtkWidget *kv_grid(const char *const *keys, GtkWidget **vals, int n) {
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 18);
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    for (int i = 0; i < n; i++) {
        gtk_grid_attach(GTK_GRID(g), lbl(keys[i], "kv-key", 0), 0, i, 1, 1);
        vals[i] = lbl("…", "kv-val", 1);
        gtk_widget_set_hexpand(vals[i], TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(vals[i]), 26);
        gtk_grid_attach(GTK_GRID(g), vals[i], 1, i, 1, 1);
    }
    return g;
}

static void human_bytes(double n, char *out, size_t sz) {
    static const char *u[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int i = 0;
    while (n >= 1024 && i < 5) { n /= 1024; i++; }
    if (i == 0) snprintf(out, sz, "%.0f B", n);
    else snprintf(out, sz, "%.1f %s", n, u[i]);
}

static void human_rate(double bps, char *out, size_t sz) {
    static const char *u[] = {"B/s", "kB/s", "MB/s", "GB/s", "TB/s"};
    int i = 0;
    while (bps >= 1000 && i < 4) { bps /= 1000; i++; }
    if (i == 0) snprintf(out, sz, "%.0f B/s", bps);
    else snprintf(out, sz, "%.1f %s", bps, u[i]);
}

// run a command, wait, return stdout without the trailing newline (NULL on failure). Worker threads only.
static char *cmd_out(int timeout_s, const char *first, ...) {
    const char *argv[24];
    char tbuf[8];
    int n = 0;
    snprintf(tbuf, sizeof tbuf, "%d", timeout_s);
    argv[n++] = "timeout";
    argv[n++] = tbuf;
    va_list ap;
    va_start(ap, first);
    for (const char *a = first; a && n < 23; a = va_arg(ap, const char *)) argv[n++] = a;
    va_end(ap);
    argv[n] = NULL;
    char *out = NULL;
    int status = 0;
    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                      &out, NULL, &status, NULL))
        return NULL;
    if (out) g_strchomp(out);
    return out;
}

// fire-and-forget D-Bus call that never starts a service (the target may simply not be running)
static void dbus_poke(GBusType bus, const char *dest, const char *path, const char *iface, const char *method,
                      GVariant *params) {
    GDBusConnection *c = g_bus_get_sync(bus, NULL, NULL);
    if (!c) { if (params) g_variant_unref(g_variant_ref_sink(params)); return; }
    g_dbus_connection_call(c, dest, path, iface, method, params, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000,
                           NULL, NULL, NULL);
    g_object_unref(c);
}

static GVariant *gtk_action(const char *name, GVariant *param) {
    GVariantBuilder av;
    g_variant_builder_init(&av, G_VARIANT_TYPE("av"));
    if (param) g_variant_builder_add(&av, "v", param);
    return g_variant_new("(sava{sv})", name, &av, NULL);
}

static void hide_control_center(void) {   // one popup at a time: never on top of the Super+S control center
    dbus_poke(G_BUS_TYPE_SESSION, "org.erikreider.swaync.cc", "/org/erikreider/swaync/cc",
              "org.erikreider.swaync.cc", "SetVisibility", g_variant_new("(b)", FALSE));
}

static void close_python_panel(void) {    // nor on top of the Wi-Fi list (settings/panels.py)
    dbus_poke(G_BUS_TYPE_SESSION, "dev.hyprminimal.Settings", "/dev/hyprminimal/Settings", "org.gtk.Actions",
              "Activate", gtk_action("panel-close", NULL));
}

static void spawn_detached(const char *const *argv) {
    g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
}

// --------------------------------------------------------------------------- panel base
typedef struct Panel Panel;
struct Panel {
    const char *kind;
    int width;
    gboolean bottom, center;
    gboolean middle;                 // centred on screen, no anchors (launcher)
    guint interval_ms;               // refresh period while open (0 = never)
    void (*build)(Panel *, GtkWidget *body);
    void (*refresh)(Panel *);
    void (*opened)(Panel *);         // before the first refresh
    void (*closed)(Panel *);
    gboolean (*key)(Panel *, guint keyval);
    GtkWidget *win, *body;
    guint tick;
};

static Panel *current;
static void manager_close(void);

static gboolean on_key(GtkEventControllerKey *c, guint kv, guint code, GdkModifierType m, gpointer data) {
    (void)c; (void)code; (void)m;
    Panel *p = data;
    if (kv == GDK_KEY_Escape) { manager_close(); return TRUE; }
    return p->key ? p->key(p, gdk_keyval_to_lower(kv)) : FALSE;
}

static gboolean panel_tick(gpointer data) {
    Panel *p = data;
    p->refresh(p);
    return G_SOURCE_CONTINUE;
}

static void panel_create(Panel *p) {
    p->win = gtk_window_new();
    GtkWindow *w = GTK_WINDOW(p->win);
    gtk_window_set_application(w, app);
    gtk_window_set_decorated(w, FALSE);
    gtk_window_set_resizable(w, FALSE);
    gtk_widget_add_css_class(p->win, "hyprminimal-panel");
    gtk_window_set_default_size(w, p->width, -1);
    if (LS) {
        gtk_layer_init_for_window(w);
        gtk_layer_set_namespace(w, "hyprminimal-panel");
        gtk_layer_set_layer(w, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(w, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    }
    if (LS && !p->middle) {   // anchored to no edge = centred by the compositor
        GtkLayerShellEdge edge = p->bottom ? GTK_LAYER_SHELL_EDGE_BOTTOM : GTK_LAYER_SHELL_EDGE_TOP;
        gtk_layer_set_anchor(w, edge, TRUE);
        gtk_layer_set_margin(w, edge, POPUP_TOP);
        if (!p->center) {   // popups sit on the right, the calendar centred
            gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
            gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_RIGHT, POPUP_RIGHT);
        }
    }
    p->body = VBOX(14, "panel");
    gtk_widget_set_size_request(p->body, p->width, -1);
    // fixed width: the scroller caps the natural width (never scrolls), so text can't resize the popup
    GtkWidget *clamp = gtk_scrolled_window_new();
    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(clamp);
    gtk_scrolled_window_set_policy(sw, GTK_POLICY_NEVER, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_width(sw, TRUE);
    gtk_scrolled_window_set_propagate_natural_height(sw, TRUE);
    gtk_scrolled_window_set_min_content_width(sw, p->width);
    gtk_scrolled_window_set_max_content_width(sw, p->width);
    gtk_scrolled_window_set_child(sw, p->body);
    gtk_window_set_child(w, clamp);
    p->build(p, p->body);
    pointerize(p->body);
    // capture phase: Esc closes even when a focused child (switch, button) would take it
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), p);
    gtk_widget_add_controller(p->win, keys);
}

static void panel_stop_tick(Panel *p) {
    if (p->tick) { g_source_remove(p->tick); p->tick = 0; }
}

static void panel_opened(Panel *p) {
    panel_stop_tick(p);                  // never stack timers (two opens in a row)
    if (p->opened) p->opened(p);
    if (p->refresh) p->refresh(p);
    if (p->interval_ms && p->refresh) p->tick = g_timeout_add(p->interval_ms, panel_tick, p);
}

static void panel_closed(Panel *p) {
    panel_stop_tick(p);
    if (p->closed) p->closed(p);
}

// --------------------------------------------------------------------------- calendar
static struct { GtkWidget *time, *date, *cal; } C;

static void cal_build(Panel *p, GtkWidget *body) {
    (void)p;
    C.time = lbl("", "big", 0.5);
    C.date = lbl("", "muted", 0.5);
    C.cal = gtk_calendar_new();
    gtk_calendar_set_show_week_numbers(GTK_CALENDAR(C.cal), FALSE);
    gtk_box_append(GTK_BOX(body), C.time);
    gtk_box_append(GTK_BOX(body), C.date);
    gtk_box_append(GTK_BOX(body), C.cal);
}

static void cal_refresh(Panel *p) {
    (void)p;
    GDateTime *now = g_date_time_new_now_local();
    char *t = g_date_time_format(now, "%H:%M:%S"), *d = g_date_time_format(now, "%A, %d %B %Y");
    gtk_label_set_label(GTK_LABEL(C.time), t);
    gtk_label_set_label(GTK_LABEL(C.date), d);
    g_free(t);
    g_free(d);
    g_date_time_unref(now);
}

// tick exactly on the second boundary (a plain 1 s timer drifts and the seconds skip or stutter)
static gboolean cal_tick(gpointer data) {
    Panel *p = data;
    cal_refresh(p);
    gint64 ms = g_get_real_time() / 1000 % 1000;
    p->tick = g_timeout_add(1000 - ms + 2, cal_tick, p);
    return G_SOURCE_REMOVE;
}

static void cal_opened(Panel *p) {
    GDateTime *now = g_date_time_new_now_local();
#if GTK_CHECK_VERSION(4, 20, 0)
    gtk_calendar_set_date(GTK_CALENDAR(C.cal), now);
#else
    gtk_calendar_select_day(GTK_CALENDAR(C.cal), now);
#endif
    g_date_time_unref(now);
    cal_refresh(p);
    gint64 ms = g_get_real_time() / 1000 % 1000;
    p->tick = g_timeout_add(1000 - ms + 2, cal_tick, p);
}

static Panel calendar = {.kind = "calendar", .width = 340, .center = TRUE, .build = cal_build,
                         .opened = cal_opened};

// --------------------------------------------------------------------------- battery
static struct {
    GtkWidget *icon, *pct, *state, *level, *v[4], *mode_btn[3];
    char bat[128];
    GDBusProxy *ppd;            // power-profiles-daemon (system bus): no python powerprofilesctl per tick
} B;
static const char *MODES[] = {"power-saver", "balanced", "performance"};

static void bat_show_profile(void) {
    const char *cur = "";
    GVariant *v = B.ppd ? g_dbus_proxy_get_cached_property(B.ppd, "ActiveProfile") : NULL;
    if (v) cur = g_variant_get_string(v, NULL);
    for (int i = 0; i < 3; i++) set_class(B.mode_btn[i], "on", strcmp(cur, MODES[i]) == 0);
    if (v) g_variant_unref(v);
}

static void bat_set_mode(int i) {
    for (int j = 0; j < 3; j++) set_class(B.mode_btn[j], "on", i == j);   // show it at once
    if (!B.ppd) return;
    g_dbus_proxy_call(B.ppd, "org.freedesktop.DBus.Properties.Set",
                      g_variant_new("(ssv)", g_dbus_proxy_get_interface_name(B.ppd), "ActiveProfile",
                                    g_variant_new_string(MODES[i])),
                      G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);   // PropertiesChanged confirms it
}

static void bat_mode_clicked(GtkButton *b, gpointer i) { (void)b; bat_set_mode(GPOINTER_TO_INT(i)); }

static gboolean bat_key(Panel *p, guint kv) {
    (void)p;
    int i = kv == GDK_KEY_s ? 0 : kv == GDK_KEY_b ? 1 : kv == GDK_KEY_p ? 2 : -1;
    if (i >= 0) bat_set_mode(i);
    return i >= 0;
}

static void ppd_changed(GDBusProxy *px, GVariant *ch, GStrv inv, gpointer d) {
    (void)px; (void)ch; (void)inv; (void)d;
    bat_show_profile();
}

static void ppd_ready(GObject *src, GAsyncResult *res, gpointer legacy) {
    (void)src;
    GDBusProxy *px = g_dbus_proxy_new_for_bus_finish(res, NULL);
    char *owner = px ? g_dbus_proxy_get_name_owner(px) : NULL;
    if (!owner) {                       // not running under the new name: try the old one once
        if (px) g_object_unref(px);
        if (!legacy)
            g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "net.hadess.PowerProfiles",
                                     "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles", NULL, ppd_ready,
                                     GINT_TO_POINTER(1));
        return;
    }
    g_free(owner);
    B.ppd = px;
    g_signal_connect(px, "g-properties-changed", G_CALLBACK(ppd_changed), NULL);
    bat_show_profile();
}

static void bat_build(Panel *p, GtkWidget *body) {
    (void)p;
    GtkWidget *top = HBOX(14, NULL), *col = VBOX(0, NULL);
    B.icon = lbl("", "icon", 0);
    B.pct = lbl("", "big", 0);
    B.state = lbl("", "muted", 0);
    gtk_box_append(GTK_BOX(top), B.icon);
    gtk_box_append(GTK_BOX(col), B.pct);
    gtk_box_append(GTK_BOX(col), B.state);
    gtk_box_append(GTK_BOX(top), col);
    gtk_box_append(GTK_BOX(body), top);
    B.level = gtk_level_bar_new_for_interval(0, 100);
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(B.level), "low", 20);
    gtk_box_append(GTK_BOX(body), B.level);
    GtkWidget *card = VBOX(0, "card");
    static const char *keys[] = {"Time left", "Power draw", "Health", "Charge cycles"};
    gtk_box_append(GTK_BOX(card), kv_grid(keys, B.v, 4));
    gtk_box_append(GTK_BOX(body), card);
    GtkWidget *hdr = lbl("POWER MODE", "pink", 0);
    gtk_widget_set_margin_top(hdr, 4);
    gtk_box_append(GTK_BOX(body), hdr);
    // one full-width tile per mode, keycap at the middle right like the Super+S toggles: S / B / P
    static const char *names[] = {"Power Saver", "Balanced", "Performance"}, *caps[] = {"S", "B", "P"};
    GtkWidget *modes = VBOX(6, NULL);
    for (int i = 0; i < 3; i++) {
        GtkWidget *row = HBOX(8, NULL), *name = lbl(names[i], NULL, 0), *cap = lbl(caps[i], "key-hint", 0.5);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_widget_set_valign(cap, GTK_ALIGN_CENTER);
        gtk_widget_set_can_target(cap, FALSE);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), cap);
        B.mode_btn[i] = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(B.mode_btn[i]), row);
        add_classes(B.mode_btn[i], "tile mode");
        g_signal_connect(B.mode_btn[i], "clicked", G_CALLBACK(bat_mode_clicked), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(modes), B.mode_btn[i]);
    }
    gtk_box_append(GTK_BOX(body), modes);
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.UPower.PowerProfiles",
                             "/org/freedesktop/UPower/PowerProfiles", "org.freedesktop.UPower.PowerProfiles", NULL,
                             ppd_ready, NULL);
}

static const char *uev(char **lines, const char *key) {   // POWER_SUPPLY_<key>=value
    size_t k = strlen(key);
    for (char **l = lines; *l; l++)
        if (strncmp(*l, "POWER_SUPPLY_", 13) == 0 && strncmp(*l + 13, key, k) == 0 && (*l)[13 + k] == '=')
            return *l + 14 + k;
    return NULL;
}

static double uevd(char **lines, const char *a, const char *b) {
    const char *v = uev(lines, a);
    if (!v && b) v = uev(lines, b);
    return v ? g_ascii_strtod(v, NULL) : 0;
}

static void bat_find(void) {
    B.bat[0] = 0;
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!d) return;
    char best[64] = "";
    for (const char *n; (n = g_dir_read_name(d));)
        if (strncmp(n, "BAT", 3) == 0 && (!best[0] || strcmp(n, best) < 0)) snprintf(best, sizeof best, "%s", n);
    g_dir_close(d);
    if (best[0]) snprintf(B.bat, sizeof B.bat, "/sys/class/power_supply/%s", best);
}

static void bat_opened(Panel *p) {
    (void)p;
    bat_find();
    bat_show_profile();
}

static void bat_refresh(Panel *p) {
    (void)p;
    if (!B.bat[0]) { gtk_label_set_label(GTK_LABEL(B.pct), "No battery"); return; }
    char path[192], buf[4096], s[64];
    snprintf(path, sizeof path, "%s/uevent", B.bat);
    if (!read_small(path, buf, sizeof buf)) return;
    char **l = g_strsplit(buf, "\n", -1);
    int cap = (int)uevd(l, "CAPACITY", NULL);
    const char *status = uev(l, "STATUS");
    if (!status) status = "Unknown";
    snprintf(s, sizeof s, "%d%%", cap);
    gtk_label_set_label(GTK_LABEL(B.pct), s);
    gtk_label_set_label(GTK_LABEL(B.state), status);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(B.level), cap);
    static const char *icons[] = {"", "", "", "", ""};
    gtk_label_set_label(GTK_LABEL(B.icon), strcmp(status, "Charging") == 0 ? "\U000f0084" : icons[MIN(4, cap / 21)]);
    double now = uevd(l, "CHARGE_NOW", "ENERGY_NOW"), full = uevd(l, "CHARGE_FULL", "ENERGY_FULL");
    double design = uevd(l, "CHARGE_FULL_DESIGN", "ENERGY_FULL_DESIGN");
    gboolean amps = uev(l, "CURRENT_NOW") != NULL;
    double rate = uevd(l, "CURRENT_NOW", "POWER_NOW"), volts = uevd(l, "VOLTAGE_NOW", NULL) / 1e6;
    double watts = amps ? rate * volts / 1e6 : rate / 1e6, hours = 0;
    if (rate > 0 && strcmp(status, "Discharging") == 0) hours = now / rate;
    else if (rate > 0 && strcmp(status, "Charging") == 0) hours = (full - now) / rate;
    if (hours > 0) snprintf(s, sizeof s, "%d h %02d min", (int)hours, (int)(hours * 60) % 60);
    gtk_label_set_label(GTK_LABEL(B.v[0]), hours > 0 ? s : "—");
    snprintf(s, sizeof s, "%.1f W", watts);
    gtk_label_set_label(GTK_LABEL(B.v[1]), watts > 0 ? s : "—");
    snprintf(s, sizeof s, "%.0f%%", design > 0 ? 100 * full / design : 0);
    gtk_label_set_label(GTK_LABEL(B.v[2]), design > 0 ? s : "—");
    const char *cyc = uev(l, "CYCLE_COUNT");
    gtk_label_set_label(GTK_LABEL(B.v[3]), cyc ? cyc : "—");
    g_strfreev(l);
}

static Panel battery = {.kind = "battery", .width = 360, .interval_ms = 5000, .build = bat_build,
                        .opened = bat_opened, .refresh = bat_refresh, .key = bat_key};

// --------------------------------------------------------------------------- network
typedef struct {
    char *dev, *type, *conn, *ip, *gw, *psk, *extra;
    gboolean radio;
} NetInfo;

typedef struct {
    gboolean ok, supported, on, together, station;
    int clients;
    char *ssid, *err;
} HsInfo;

static struct {
    GtkWidget *icon, *ssid, *sub, *sw, *down, *up, *pw_card, *pw, *eye, *copy, *v[2];
    GtkWidget *hs_card, *hs_sub, *hs_sw;
    gboolean sw_sync, hs_sync, hs_busy, shown, info_busy, hs_running;
    int hs_ticks;
    char *dev, *type, *extra, *psk;
    gboolean have_last;
    gint64 last_t;
    unsigned long long last_rx, last_tx;
    guint first_speed;
    guint gen;                         // bumped on open/close: stale worker results are dropped
} N;

static void net_free_info(gpointer d) {
    NetInfo *i = d;
    if (i->psk) explicit_bzero(i->psk, strlen(i->psk));
    g_free(i->dev); g_free(i->type); g_free(i->conn); g_free(i->ip); g_free(i->gw); g_free(i->psk); g_free(i->extra);
    g_free(i);
}

static void hs_free(gpointer d) {
    HsInfo *h = d;
    g_free(h->ssid); g_free(h->err);
    g_free(h);
}

static void forget_psk(void) {
    if (N.psk) { explicit_bzero(N.psk, strlen(N.psk)); g_free(N.psk); N.psk = NULL; }
}

static void show_pw(void) {
    if (!N.psk || !*N.psk) gtk_label_set_label(GTK_LABEL(N.pw), "—");
    else if (N.shown) gtk_label_set_label(GTK_LABEL(N.pw), N.psk);
    else {
        char dots[64] = "";
        size_t n = MIN(g_utf8_strlen(N.psk, -1), 14);
        for (size_t i = 0; i < n; i++) strcat(dots, "•");
        gtk_label_set_label(GTK_LABEL(N.pw), dots);
    }
    gtk_button_set_label(GTK_BUTTON(N.eye), N.shown ? "\U000f0209" : "\U000f0208");
    gtk_widget_set_tooltip_text(N.eye, N.shown ? "Hide password" : "Show password");
}

static void on_eye(GtkButton *b, gpointer d) { (void)b; (void)d; N.shown = !N.shown; show_pw(); }

static gboolean copy_reset(gpointer d) { (void)d; gtk_button_set_label(GTK_BUTTON(N.copy), "\U000f018f"); return G_SOURCE_REMOVE; }

static void on_copy(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    if (!N.psk || !*N.psk) return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(N.copy), N.psk);
    gtk_button_set_label(GTK_BUTTON(N.copy), "\U000f012c");   // check mark
    g_timeout_add(1200, copy_reset, NULL);
}

// signal from the kernel, instant (nmcli can stall seconds while it rescans)
static gboolean wifi_signal(const char *dev, int *pct) {
    FILE *f = fopen("/proc/net/wireless", "re");
    if (!f) return FALSE;
    char line[256];
    gboolean ok = FALSE;
    for (int i = 0; fgets(line, sizeof line, f); i++) {
        if (i < 2) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        if (strcmp(g_strstrip(line), dev) != 0) continue;
        unsigned st;
        double link;
        if (sscanf(colon + 1, "%x %lf", &st, &link) == 2) {
            *pct = MIN(100, (int)lround(link * 100 / 70));
            ok = TRUE;
        }
        break;
    }
    fclose(f);
    return ok;
}

static void show_signal(void) {
    static const char *icons[] = {"\U000f092f", "\U000f091f", "\U000f0922", "\U000f0925", "\U000f0928"};
    int pct;
    if (N.dev && wifi_signal(N.dev, &pct)) {
        const char *w = pct >= 80 ? "Excellent" : pct >= 60 ? "Good" : pct >= 40 ? "Fair" : "Weak";
        char s[160];
        snprintf(s, sizeof s, "%s %d%%%s%s", w, pct, N.extra && *N.extra ? " · " : "", N.extra ? N.extra : "");
        gtk_label_set_label(GTK_LABEL(N.sub), s);
        gtk_label_set_label(GTK_LABEL(N.icon), icons[MIN(4, pct / 20)]);
    } else {
        gtk_label_set_label(GTK_LABEL(N.sub), N.extra && *N.extra ? N.extra : "Wi-Fi");
    }
}

static void net_speed(void) {
    if (!N.dev) return;
    char p[160];
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/rx_bytes", N.dev);
    long long rx = read_ll(p, -1);
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/tx_bytes", N.dev);
    long long tx = read_ll(p, -1);
    if (rx < 0 || tx < 0) return;
    gint64 t = g_get_monotonic_time();
    if (N.have_last) {
        double dt = MAX(t - N.last_t, 1000) / 1e6;
        char s[32];
        human_rate((rx - (long long)N.last_rx) / dt, s, sizeof s);
        gtk_label_set_label(GTK_LABEL(N.down), s);
        human_rate((tx - (long long)N.last_tx) / dt, s, sizeof s);
        gtk_label_set_label(GTK_LABEL(N.up), s);
    } else {
        gtk_label_set_label(GTK_LABEL(N.down), "…");
        gtk_label_set_label(GTK_LABEL(N.up), "…");
    }
    N.have_last = TRUE;
    N.last_t = t;
    N.last_rx = rx;
    N.last_tx = tx;
}

static gboolean first_speed(gpointer d) { (void)d; N.first_speed = 0; net_speed(); return G_SOURCE_REMOVE; }

static void schedule_first_speed(void) {   // baseline now, first real speed 0.4 s later
    N.have_last = FALSE;
    net_speed();
    if (N.first_speed) g_source_remove(N.first_speed);
    N.first_speed = g_timeout_add(400, first_speed, NULL);
}

// worker thread: interface from the routing table, the rest from NetworkManager
static void net_info_work(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    NetInfo *i = g_new0(NetInfo, 1);
    FILE *f = fopen("/proc/net/route", "re");
    if (f) {
        char line[256], ifc[64], dst[16];
        if (fgets(line, sizeof line, f))
            while (fgets(line, sizeof line, f))
                if (sscanf(line, "%63s %15s", ifc, dst) == 2 && strcmp(dst, "00000000") == 0) { i->dev = g_strdup(ifc); break; }
        fclose(f);
    }
    if (i->dev) {
        char p[160];
        snprintf(p, sizeof p, "/sys/class/net/%s/wireless", i->dev);
        i->type = g_strdup(g_file_test(p, G_FILE_TEST_EXISTS) ? "wifi" : "ethernet");
    }
    char *radio = cmd_out(6, "nmcli", "radio", "wifi", NULL);
    i->radio = !radio || strcmp(radio, "disabled") != 0;
    g_free(radio);
    if (i->dev) {
        char *show = cmd_out(6, "nmcli", "-t", "-f", "GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY", "dev", "show",
                             i->dev, NULL);
        char **l = g_strsplit(show ? show : "", "\n", -1);
        for (char **x = l; *x; x++) {
            char *v = strchr(*x, ':');
            if (!v) continue;
            *v++ = 0;
            if (strcmp(*x, "GENERAL.CONNECTION") == 0 && *v && strcmp(v, "--") != 0) i->conn = g_strdup(v);
            else if (g_str_has_prefix(*x, "IP4.ADDRESS") && !i->ip) i->ip = g_strndup(v, strcspn(v, "/"));
            else if (strcmp(*x, "IP4.GATEWAY") == 0 && *v && strcmp(v, "--") != 0) i->gw = g_strdup(v);
        }
        g_strfreev(l);
        g_free(show);
    }
    if (i->type && strcmp(i->type, "wifi") == 0 && i->conn) {
        i->psk = cmd_out(4, "nmcli", "-s", "-g", "802-11-wireless-security.psk", "connection", "show", "id", i->conn,
                         NULL);
        // cached scan only (--rescan no): never stalls
        char *list = cmd_out(4, "nmcli", "-t", "-f", "IN-USE,FREQ,SECURITY", "dev", "wifi", "list", "--rescan", "no", NULL);
        char **l = g_strsplit(list ? list : "", "\n", -1);
        for (char **x = l; *x; x++) {
            char **fld = g_strsplit(*x, ":", 3);
            if (g_strv_length(fld) == 3 && strcmp(fld[0], "*") == 0) {
                int mhz = atoi(fld[1]);
                const char *band = mhz >= 5925 ? "6 GHz" : mhz >= 4900 ? "5 GHz" : mhz ? "2.4 GHz" : "";
                char *sec = *fld[2] ? g_strdelimit(g_strdup(fld[2]), " ", '/') : g_strdup("Open");
                i->extra = *band ? g_strdup_printf("%s · %s", band, sec) : g_strdup(sec);
                g_free(sec);
                g_strfreev(fld);
                break;
            }
            g_strfreev(fld);
        }
        g_strfreev(l);
        g_free(list);
    }
    g_task_return_pointer(t, i, net_free_info);
}

static void net_info_done(GObject *src, GAsyncResult *res, gpointer gen) {
    (void)src;
    N.info_busy = FALSE;
    NetInfo *i = g_task_propagate_pointer(G_TASK(res), NULL);
    if (!i) return;
    if (GPOINTER_TO_UINT(gen) != N.gen || !current || strcmp(current->kind, "network") != 0) { net_free_info(i); return; }
    gboolean first = !N.dev || g_strcmp0(N.dev, i->dev) != 0;
    g_free(N.dev); N.dev = g_strdup(i->dev);
    g_free(N.type); N.type = g_strdup(i->type);
    g_free(N.extra); N.extra = g_strdup(i->extra ? i->extra : "");
    forget_psk();
    N.psk = g_strdup(i->psk ? i->psk : "");
    if (first && N.dev) schedule_first_speed();
    N.sw_sync = TRUE;
    gtk_switch_set_active(GTK_SWITCH(N.sw), i->radio);
    N.sw_sync = FALSE;
    gboolean wifi = g_strcmp0(N.type, "wifi") == 0;
    gtk_widget_set_visible(N.pw_card, wifi && *N.psk);
    show_pw();
    if (!N.dev) {
        gtk_label_set_label(GTK_LABEL(N.ssid), i->radio ? "Disconnected" : "Wi-Fi off");
        gtk_label_set_label(GTK_LABEL(N.sub), "");
        gtk_label_set_label(GTK_LABEL(N.icon), "\U000f092e");
        for (int k = 0; k < 2; k++) gtk_label_set_label(GTK_LABEL(N.v[k]), "—");
        gtk_label_set_label(GTK_LABEL(N.down), "—");
        gtk_label_set_label(GTK_LABEL(N.up), "—");
    } else {
        gtk_label_set_label(GTK_LABEL(N.ssid), i->conn ? i->conn : i->dev);
        if (wifi) show_signal();
        else {
            gtk_label_set_label(GTK_LABEL(N.sub), "Ethernet");
            gtk_label_set_label(GTK_LABEL(N.icon), "\U000f0200");
        }
        gtk_label_set_label(GTK_LABEL(N.v[0]), i->ip ? i->ip : "—");
        gtk_label_set_label(GTK_LABEL(N.v[1]), i->gw ? i->gw : "—");
    }
    net_free_info(i);
}

static void net_info(void) {
    if (N.info_busy) return;                   // single-flight: a slow NetworkManager never piles up nmcli
    N.info_busy = TRUE;
    GTask *t = g_task_new(NULL, NULL, net_info_done, GUINT_TO_POINTER(N.gen));
    g_task_run_in_thread(t, net_info_work);
    g_object_unref(t);
}

static gboolean net_info_later(gpointer d) { (void)d; net_info(); return G_SOURCE_REMOVE; }

static gboolean on_wifi_switch(GtkSwitch *sw, gboolean on, gpointer d) {
    (void)sw; (void)d;
    if (N.sw_sync) return FALSE;
    const char *argv[] = {"timeout", "5", "nmcli", "radio", "wifi", on ? "on" : "off", NULL};
    spawn_detached(argv);
    g_timeout_add(1500, net_info_later, NULL);
    return FALSE;
}

// hotspot: settings/nm.py holds the logic (NetworkManager profile or the side-by-side hostapd helper)
static void hs_status_work(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)c;
    const char *nm = d;
    HsInfo *h = g_new0(HsInfo, 1);
    char *out = cmd_out(20, "python3", "-I", nm, "hotspot-status", NULL);
    char **l = g_strsplit(out ? out : "", "\n", -1);
    for (char **x = l; *x; x++) {
        char *v = strchr(*x, '=');
        if (!v) continue;
        *v++ = 0;
        h->ok = TRUE;
        if (!strcmp(*x, "supported")) h->supported = atoi(v);
        else if (!strcmp(*x, "on")) h->on = atoi(v);
        else if (!strcmp(*x, "together")) h->together = atoi(v);
        else if (!strcmp(*x, "station")) h->station = atoi(v);
        else if (!strcmp(*x, "clients")) h->clients = atoi(v);
        else if (!strcmp(*x, "ssid")) h->ssid = g_strdup(v);
    }
    g_strfreev(l);
    g_free(out);
    g_task_return_pointer(t, h, hs_free);
}

static void hs_show(HsInfo *h) {
    gtk_widget_set_visible(N.hs_card, h->supported);
    N.hs_sync = TRUE;
    gtk_switch_set_active(GTK_SWITCH(N.hs_sw), h->on);
    N.hs_sync = FALSE;
    set_class(N.hs_card, "on", h->on);
    gtk_widget_remove_css_class(N.hs_sub, "hs-error");
    char s[200];
    if (h->on) {
        char n[48];
        if (h->clients) snprintf(n, sizeof n, "%d device%s", h->clients, h->clients != 1 ? "s" : "");
        else snprintf(n, sizeof n, "no devices yet");
        snprintf(s, sizeof s, "“%s” · %s", h->ssid ? h->ssid : "", n);
    } else if (h->station) {
        snprintf(s, sizeof s, "%s", h->together ? "Off · shares this Wi-Fi too" : "Off · Wi-Fi disconnects while on");
    } else {
        snprintf(s, sizeof s, "Off");
    }
    gtk_label_set_label(GTK_LABEL(N.hs_sub), s);
}

static void hs_status_done(GObject *src, GAsyncResult *res, gpointer gen) {
    (void)src;
    N.hs_running = FALSE;
    HsInfo *h = g_task_propagate_pointer(G_TASK(res), NULL);
    if (h && h->ok && !N.hs_busy && GPOINTER_TO_UINT(gen) == N.gen) hs_show(h);
    if (h) hs_free(h);
}

static char *nm_py(void) { return g_build_filename(REPO, "settings", "nm.py", NULL); }

static void hs_info(void) {
    if (N.hs_running) return;
    N.hs_running = TRUE;
    GTask *t = g_task_new(NULL, NULL, hs_status_done, GUINT_TO_POINTER(N.gen));
    g_task_set_task_data(t, nm_py(), g_free);
    g_task_run_in_thread(t, hs_status_work);
    g_object_unref(t);
}

static void hs_set_work(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)d; (void)c;
    char **argv = g_task_get_task_data(t);
    char *err = NULL;
    int status = 0;
    HsInfo *h = g_new0(HsInfo, 1);
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, &err,
                     &status, NULL))
        h->ok = g_spawn_check_wait_status(status, NULL);
    h->err = err ? g_strstrip(err) : g_strdup("unknown error");
    g_task_return_pointer(t, h, hs_free);
}

static void hs_set_done(GObject *src, GAsyncResult *res, gpointer on) {
    (void)src;
    HsInfo *h = g_task_propagate_pointer(G_TASK(res), NULL);
    N.hs_busy = FALSE;
    gtk_widget_set_sensitive(N.hs_sw, TRUE);
    if (h && h->ok) {
        hs_info();
        g_timeout_add(1500, net_info_later, NULL);   // Wi-Fi may have changed too
    } else {
        N.hs_sync = TRUE;
        gtk_switch_set_active(GTK_SWITCH(N.hs_sw), !GPOINTER_TO_INT(on));
        N.hs_sync = FALSE;
        gtk_widget_add_css_class(N.hs_sub, "hs-error");
        const char *e = h && h->err && *h->err ? h->err : "unknown error";
        char *s = g_strdup_printf("Failed: %s", e);
        gtk_label_set_label(GTK_LABEL(N.hs_sub), s);
        gtk_widget_set_tooltip_text(N.hs_sub, e);
        g_free(s);
    }
    if (h) hs_free(h);
}

static gboolean on_hs_switch(GtkSwitch *sw, gboolean on, gpointer d) {
    (void)sw; (void)d;
    if (N.hs_sync || N.hs_busy) return FALSE;
    N.hs_busy = TRUE;
    gtk_widget_set_sensitive(N.hs_sw, FALSE);
    gtk_widget_remove_css_class(N.hs_sub, "hs-error");
    gtk_label_set_label(GTK_LABEL(N.hs_sub), on ? "Starting…" : "Stopping…");
    char *nm = nm_py();
    char **argv = g_new0(char *, 6);
    argv[0] = g_strdup("python3"); argv[1] = g_strdup("-I"); argv[2] = nm;
    argv[3] = g_strdup("hotspot"); argv[4] = g_strdup(on ? "on" : "off");
    GTask *t = g_task_new(NULL, NULL, hs_set_done, GINT_TO_POINTER(on));
    g_task_set_task_data(t, argv, (GDestroyNotify)g_strfreev);
    g_task_run_in_thread(t, hs_set_work);
    g_object_unref(t);
    return FALSE;
}

static void on_net_settings(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    manager_close();
    char *bin = g_build_filename(REPO, "bin", "hyprminimal-settings", NULL);
    const char *argv[] = {bin, "--page", "network", NULL};
    spawn_detached(argv);
    g_free(bin);
}

static void net_build(Panel *p, GtkWidget *body) {
    (void)p;
    gtk_widget_add_css_class(body, "compact");
    GtkWidget *top = HBOX(12, NULL), *col = VBOX(0, NULL);
    N.icon = lbl("\U000f0928", "icon", 0);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
    N.ssid = lbl("", "mid", 0);
    gtk_label_set_max_width_chars(GTK_LABEL(N.ssid), 18);
    N.sub = lbl("", "muted caption", 0);
    gtk_box_append(GTK_BOX(col), N.ssid);
    gtk_box_append(GTK_BOX(col), N.sub);
    N.sw = gtk_switch_new();
    gtk_widget_set_valign(N.sw, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(N.sw, "Wi-Fi on/off");
    g_signal_connect(N.sw, "state-set", G_CALLBACK(on_wifi_switch), NULL);
    gtk_box_append(GTK_BOX(top), N.icon);
    gtk_box_append(GTK_BOX(top), col);
    gtk_box_append(GTK_BOX(top), N.sw);
    gtk_box_append(GTK_BOX(body), top);

    // live speed: two small tiles, fixed-width digits so they don't wobble
    GtkWidget *speeds = HBOX(8, NULL);
    gtk_box_set_homogeneous(GTK_BOX(speeds), TRUE);
    N.down = lbl("…", "speed accent", 0);
    N.up = lbl("…", "speed pink", 0);
    GtkWidget *lbs[] = {N.down, N.up};
    const char *glyph[] = {"\U000f0045", "\U000f005d"}, *title[] = {"Download", "Upload"},
               *cls[] = {"icon-sm accent", "icon-sm pink"};
    for (int i = 0; i < 2; i++) {
        GtkWidget *tile = HBOX(10, "card speed-tile"), *g = lbl(glyph[i], cls[i], 0), *c2 = VBOX(0, NULL);
        gtk_widget_set_valign(g, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(c2), lbl(title[i], "muted caption", 0));
        gtk_box_append(GTK_BOX(c2), lbs[i]);
        gtk_box_append(GTK_BOX(tile), g);
        gtk_box_append(GTK_BOX(tile), c2);
        gtk_box_append(GTK_BOX(speeds), tile);
    }
    gtk_box_append(GTK_BOX(body), speeds);

    // password: hidden until the eye is clicked; copy works either way
    N.pw_card = HBOX(6, "card tight");
    GtkWidget *pcol = VBOX(0, NULL);
    gtk_widget_set_hexpand(pcol, TRUE);
    gtk_widget_set_valign(pcol, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(pcol), lbl("Password", "kv-key caption", 0));
    N.pw = lbl("", "kv-val pw", 0);
    gtk_box_append(GTK_BOX(pcol), N.pw);
    gtk_box_append(GTK_BOX(N.pw_card), pcol);
    N.eye = gtk_button_new_with_label("\U000f0208");
    add_classes(N.eye, "round flat-icon");
    gtk_widget_set_valign(N.eye, GTK_ALIGN_CENTER);
    g_signal_connect(N.eye, "clicked", G_CALLBACK(on_eye), NULL);
    N.copy = gtk_button_new_with_label("\U000f018f");
    add_classes(N.copy, "round flat-icon");
    gtk_widget_set_valign(N.copy, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(N.copy, "Copy password");
    g_signal_connect(N.copy, "clicked", G_CALLBACK(on_copy), NULL);
    gtk_box_append(GTK_BOX(N.pw_card), N.eye);
    gtk_box_append(GTK_BOX(N.pw_card), N.copy);
    gtk_box_append(GTK_BOX(body), N.pw_card);

    // hotspot: share this connection over Wi-Fi (shown when the adapter allows it)
    N.hs_card = HBOX(12, "card hotspot");
    gtk_widget_set_visible(N.hs_card, FALSE);
    GtkWidget *hi = lbl("\U000f0003", "hs-icon", 0), *hcol = VBOX(0, NULL);
    gtk_widget_set_valign(hi, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(hcol, TRUE);
    gtk_widget_set_valign(hcol, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hcol), lbl("Hotspot", "net-name", 0));
    N.hs_sub = lbl("", "muted caption", 0);
    gtk_box_append(GTK_BOX(hcol), N.hs_sub);
    N.hs_sw = gtk_switch_new();
    gtk_widget_set_valign(N.hs_sw, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(N.hs_sw, "Share this computer's internet over Wi-Fi");
    g_signal_connect(N.hs_sw, "state-set", G_CALLBACK(on_hs_switch), NULL);
    gtk_box_append(GTK_BOX(N.hs_card), hi);
    gtk_box_append(GTK_BOX(N.hs_card), hcol);
    gtk_box_append(GTK_BOX(N.hs_card), N.hs_sw);
    gtk_box_append(GTK_BOX(body), N.hs_card);

    static const char *keys[] = {"IP address", "Gateway"};
    gtk_box_append(GTK_BOX(body), kv_grid(keys, N.v, 2));
    GtkWidget *s = gtk_button_new_with_label("Network settings…");
    gtk_widget_add_css_class(s, "link-btn");
    g_signal_connect(s, "clicked", G_CALLBACK(on_net_settings), NULL);
    gtk_box_append(GTK_BOX(body), s);
}

static void net_opened(Panel *p) {
    (void)p;
    N.gen++;
    N.have_last = FALSE;
    N.shown = FALSE;
    N.hs_ticks = 0;
    net_info();
    hs_info();
    if (N.dev) schedule_first_speed();
}

static void net_closed(Panel *p) {
    (void)p;
    N.gen++;
    N.shown = FALSE;
    forget_psk();                       // don't keep the password around while hidden
    if (N.first_speed) { g_source_remove(N.first_speed); N.first_speed = 0; }
}

static void net_refresh(Panel *p) {
    (void)p;
    if (++N.hs_ticks % 5 == 0) hs_info();   // connected devices come and go
    if (!N.dev) return;
    if (g_strcmp0(N.type, "wifi") == 0) show_signal();   // live, every second
    if (!N.first_speed) net_speed();
}

static Panel network = {.kind = "network", .width = 360, .interval_ms = 1000, .build = net_build,
                        .opened = net_opened, .closed = net_closed, .refresh = net_refresh};

// --------------------------------------------------------------------------- system
static GtkWidget *system_panel_win(void);
enum { S_CPU, S_TEMP, S_MEM, S_DISK, S_BAT, S_N };
static struct {
    GtkWidget *host, *up, *val[S_N], *bar[S_N], *card[S_N];
    double shown[S_N], goal[S_N];
    gboolean have[S_N];
    guint anim, first;
    unsigned long long idle, total;
    char **temps;                       // sensor files found on open
    char bat[128];
} S;

static void cpu_times(unsigned long long *idle, unsigned long long *total) {
    char b[512];
    unsigned long long v[8] = {0};
    *idle = *total = 0;
    if (!read_small("/proc/stat", b, sizeof b)) return;
    // cpu  user nice system idle iowait irq softirq steal (guest is already inside user)
    if (sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
               &v[7]) < 4)
        return;
    for (int i = 0; i < 8; i++) *total += v[i];
    *idle = v[3] + v[4];
}

static gboolean glide(GtkWidget *w, GdkFrameClock *fc, gpointer d) {
    (void)w; (void)fc; (void)d;
    gboolean moving = FALSE;
    for (int k = 0; k < S_N; k++) {
        if (!S.have[k]) continue;
        double cur = S.shown[k], nxt = cur + (S.goal[k] - cur) * 0.18;
        if (fabs(S.goal[k] - nxt) < 0.002) nxt = S.goal[k];
        else moving = TRUE;
        if (nxt != cur) {
            S.shown[k] = nxt;
            gtk_level_bar_set_value(GTK_LEVEL_BAR(S.bar[k]), nxt);
        }
    }
    if (!moving) { S.anim = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void sys_set(int k, double frac, const char *text) {
    frac = CLAMP(frac, 0.0, 1.0);
    gtk_label_set_label(GTK_LABEL(S.val[k]), text);
    set_class(S.bar[k], "high", frac > 0.85);
    S.goal[k] = frac;
    if (!S.have[k] || !gtk_widget_get_mapped(system_panel_win())) {    // first value: no glide from zero
        S.have[k] = TRUE;
        S.shown[k] = frac;
        gtk_level_bar_set_value(GTK_LEVEL_BAR(S.bar[k]), frac);
    } else if (!S.anim) {
        S.anim = gtk_widget_add_tick_callback(S.bar[S_CPU], glide, NULL, NULL);
    }
}

static void read_cpu(void) {
    unsigned long long idle, total;
    cpu_times(&idle, &total);
    unsigned long long li = S.idle, lt = S.total;
    S.idle = idle;
    S.total = total;
    if (total <= lt) return;            // no jiffies passed yet: keep the old value
    double used = 1.0 - (double)(idle - li) / (double)(total - lt);
    char s[32];
    snprintf(s, sizeof s, "%.0f%% busy", CLAMP(used, 0.0, 1.0) * 100);
    sys_set(S_CPU, used, s);
}

static gboolean first_cpu(gpointer d) { (void)d; S.first = 0; read_cpu(); return G_SOURCE_REMOVE; }

static void sys_build(Panel *p, GtkWidget *body) {
    (void)p;
    GtkWidget *top = HBOX(14, NULL), *col = VBOX(0, NULL);
    gtk_box_append(GTK_BOX(top), lbl("\U000f0379", "icon", 0));
    S.host = lbl("", "mid", 0);
    S.up = lbl("", "muted", 0);
    gtk_box_append(GTK_BOX(col), S.host);
    gtk_box_append(GTK_BOX(col), S.up);
    gtk_box_append(GTK_BOX(top), col);
    gtk_box_append(GTK_BOX(body), top);
    static const char *titles[] = {"Processor", "Temperature", "Memory", "Storage", "Battery"};
    for (int k = 0; k < S_N; k++) {
        S.card[k] = VBOX(6, "card");
        GtkWidget *head = HBOX(0, NULL);
        gtk_box_append(GTK_BOX(head), lbl(titles[k], "kv-key", 0));
        S.val[k] = lbl("…", "kv-val", 1);
        gtk_widget_set_hexpand(S.val[k], TRUE);
        gtk_box_append(GTK_BOX(head), S.val[k]);
        gtk_box_append(GTK_BOX(S.card[k]), head);
        S.bar[k] = gtk_level_bar_new_for_interval(0, 1);
        gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(S.bar[k]), "low", 0.0);
        gtk_box_append(GTK_BOX(S.card[k]), S.bar[k]);
        gtk_box_append(GTK_BOX(body), S.card[k]);
    }
}

static gboolean is_cpu_sensor(const char *name) {
    static const char *cpu[] = {"k10temp", "coretemp", "zenpower", "cpu_thermal", "soc_thermal", NULL};
    for (const char **c = cpu; *c; c++)
        if (strcmp(name, *c) == 0) return TRUE;
    return FALSE;
}

// find the sensor files once per open instead of scanning /sys every tick
static void sys_probe(void) {
    g_strfreev(S.temps);
    GPtrArray *cpu = g_ptr_array_new(), *other = g_ptr_array_new();
    GDir *d = g_dir_open("/sys/class/hwmon", 0, NULL);
    for (const char *n; d && (n = g_dir_read_name(d));) {
        char path[256], name[64];
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", n);
        if (!read_small(path, name, sizeof name)) continue;
        g_strstrip(name);
        GPtrArray *into = is_cpu_sensor(name) ? cpu : other;
        snprintf(path, sizeof path, "/sys/class/hwmon/%s", n);
        GDir *h = g_dir_open(path, 0, NULL);
        for (const char *f; h && (f = g_dir_read_name(h));)
            if (g_str_has_prefix(f, "temp") && g_str_has_suffix(f, "_input"))
                g_ptr_array_add(into, g_build_filename(path, f, NULL));
        if (h) g_dir_close(h);
    }
    if (d) g_dir_close(d);
    if (!cpu->len) {
        GDir *t = g_dir_open("/sys/class/thermal", 0, NULL);
        for (const char *n; t && (n = g_dir_read_name(t));)
            if (g_str_has_prefix(n, "thermal_zone")) g_ptr_array_add(other, g_strdup_printf("/sys/class/thermal/%s/temp", n));
        if (t) g_dir_close(t);
    }
    GPtrArray *use = cpu->len ? cpu : other;
    g_ptr_array_free(use == cpu ? other : cpu, TRUE);   // the unused list's strings
    g_ptr_array_add(use, NULL);
    S.temps = (char **)g_ptr_array_free(use, FALSE);
    bat_find();
    snprintf(S.bat, sizeof S.bat, "%s", B.bat);
    gtk_widget_set_visible(S.card[S_BAT], S.bat[0] != 0);
}

static void sys_refresh(Panel *p);

static void sys_opened(Panel *p) {
    (void)p;
    struct utsname u;
    if (uname(&u) == 0) gtk_label_set_label(GTK_LABEL(S.host), u.nodename);
    sys_probe();
    // a CPU reading needs two samples some time apart: a short first window gives a real number
    // within a blink, instead of a bogus 100% from a zero-length interval
    cpu_times(&S.idle, &S.total);
    gtk_label_set_label(GTK_LABEL(S.val[S_CPU]), "…");
    if (S.first) g_source_remove(S.first);
    S.first = g_timeout_add(250, first_cpu, NULL);
}

static void sys_closed(Panel *p) {
    (void)p;
    if (S.first) { g_source_remove(S.first); S.first = 0; }
    if (S.anim) { gtk_widget_remove_tick_callback(S.bar[S_CPU], S.anim); S.anim = 0; }
}

static void sys_refresh(Panel *p) {
    (void)p;
    char b[256], s[96], x[32], y[32];
    // uptime
    if (read_small("/proc/uptime", b, sizeof b)) {
        long secs = (long)g_ascii_strtod(b, NULL);
        long d = secs / 86400, h = secs / 3600 % 24, m = secs / 60 % 60;
        GString *t = g_string_new("On for ");
        if (d) g_string_append_printf(t, "%ld day%s, ", d, d != 1 ? "s" : "");
        if (h || d) g_string_append_printf(t, "%ld hour%s ", h, h != 1 ? "s" : "");
        g_string_append_printf(t, "%ld min", m);
        gtk_label_set_label(GTK_LABEL(S.up), t->str);
        g_string_free(t, TRUE);
    }
    if (!S.first) read_cpu();           // the very first reading comes from first_cpu
    // temperature: hottest CPU sensor (any sensor if the CPU has none)
    double best = -1;
    for (char **f = S.temps; f && *f; f++) {
        long long v = read_ll(*f, -1);
        double t = v / 1000.0;
        if (v >= 0 && t > 10 && t < 120 && t > best) best = t;
    }
    gtk_widget_set_visible(S.card[S_TEMP], best > 0);
    if (best > 0) {
        snprintf(s, sizeof s, "%.0f °C, %s", best, best < 60 ? "cool" : best < 80 ? "warm" : "hot");
        sys_set(S_TEMP, MIN(1.0, best / 100), s);
    }
    // memory
    FILE *f = fopen("/proc/meminfo", "re");
    if (f) {
        char line[128];
        unsigned long long tot = 0, avail = 0, v;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu", &v) == 1) tot = v * 1024;
            else if (sscanf(line, "MemAvailable: %llu", &v) == 1) { avail = v * 1024; break; }
        }
        fclose(f);
        if (tot) {
            human_bytes(tot - avail, x, sizeof x);
            human_bytes(tot, y, sizeof y);
            snprintf(s, sizeof s, "%s of %s used", x, y);
            sys_set(S_MEM, (double)(tot - avail) / tot, s);
        }
    }
    // disk (/)
    struct statvfs st;
    if (statvfs("/", &st) == 0 && st.f_blocks) {
        double dt = (double)st.f_blocks * st.f_frsize, df = (double)st.f_bavail * st.f_frsize;
        human_bytes(df, x, sizeof x);
        human_bytes(dt, y, sizeof y);
        snprintf(s, sizeof s, "%s free of %s", x, y);
        sys_set(S_DISK, (dt - df) / dt, s);
    }
    // battery
    if (S.bat[0]) {
        char path[192], st2[32];
        snprintf(path, sizeof path, "%s/capacity", S.bat);
        long long cap = read_ll(path, -1);
        snprintf(path, sizeof path, "%s/status", S.bat);
        if (cap >= 0 && read_small(path, st2, sizeof st2)) {
            g_strstrip(st2);
            char *low = g_ascii_strdown(st2, -1);
            snprintf(s, sizeof s, "%lld%%, %s", cap, low);
            g_free(low);
            sys_set(S_BAT, cap / 100.0, s);
        }
    }
}

static Panel sysp = {.kind = "system", .width = 380, .interval_ms = 1000, .build = sys_build,
                     .opened = sys_opened, .closed = sys_closed, .refresh = sys_refresh};
static GtkWidget *system_panel_win(void) { return sysp.win; }

// --------------------------------------------------------------------------- launcher (Super+R)
// Numbered app list: apps are numbered A-Z (numbers stay the same between runs), the 5 launched last are
// pinned on top, newest first. Type to search, a number + Enter launches that app, "rm NAME" / "rm NUMBER"
// + Enter uninstalls it in a terminal that asks first. The list is scripts/launcher.py's cache, shared with
// the rofi fallback (helpers/launcher.c), and rebuilt when an applications folder changes (inotify).
#define KEEP_RECENT 5
#define LAUNCHER_LINES 8

static gboolean OWN_GSK;   // we set GSK_RENDERER=cairo for ourselves: what we launch must not inherit it

static void new_session(gpointer d) { (void)d; setsid(); }

// an app or command that outlives us: its own session, no stdio, without our GSK_RENDERER
static void spawn_app(const char *const *argv) {
    char **env = g_get_environ();
    if (OWN_GSK) env = g_environ_unsetenv(env, "GSK_RENDERER");
    g_spawn_async(NULL, (char **)argv, env, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                  G_SPAWN_STDERR_TO_DEV_NULL, new_session, NULL, NULL, NULL);
    g_strfreev(env);
}

// select row i of a menu list and scroll it into view (the keyboard stays in the search field)
static int menu_select(GtkWidget *list, GtkWidget *scroll, int i, int n) {
    if (n <= 0) { gtk_list_box_unselect_all(GTK_LIST_BOX(list)); return -1; }
    i = CLAMP(i, 0, n - 1);
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i);
    if (!row) return -1;
    gtk_list_box_select_row(GTK_LIST_BOX(list), row);
    graphene_point_t pt;
    if (scroll && gtk_widget_compute_point(GTK_WIDGET(row), list, &GRAPHENE_POINT_INIT(0, 0), &pt)) {
        GtkAdjustment *va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        double h = gtk_widget_get_height(GTK_WIDGET(row)), v = gtk_adjustment_get_value(va),
               page = gtk_adjustment_get_page_size(va);
        if (pt.y < v) gtk_adjustment_set_value(va, pt.y);
        else if (pt.y + h > v + page) gtk_adjustment_set_value(va, pt.y + h - page);
    }
    return i;
}

static int menu_step(int sel, int d, int n, gboolean wrap) {
    if (n <= 0) return -1;
    if (sel < 0) return 0;
    int i = sel + d;
    if (wrap) return ((i % n) + n) % n;
    return CLAMP(i, 0, n - 1);
}

static gboolean all_digits(const char *s) {
    if (!*s) return FALSE;
    for (; *s; s++) if (!g_ascii_isdigit(*s)) return FALSE;
    return TRUE;
}

typedef struct {
    char *id, *file, *name, *fold, *num;   // fold: casefolded name for matching; num: "12"
    int rank;                              // place in the recent list, KEEP_RECENT = not recent
    GtkWidget *row, *tag;
} LApp;

static struct {
    GPtrArray *apps;                       // LApp, A-Z: index + 1 = the app's number
    GArray *order;                         // int: the apps shown, top to bottom
    GtkWidget *text, *list, *scroll;
    int sel, row_h;
    char *cache, *recent_path, *py;
    char **recent;                         // app ids, newest first
    GSubprocess *builder;
    gboolean again;
    guint rebuild_timer;
} L;

static Panel launcherp, sessionp;

static void lapp_free(gpointer d) {
    LApp *a = d;
    g_free(a->id); g_free(a->file); g_free(a->name); g_free(a->fold); g_free(a->num);
    if (a->row) g_object_unref(a->row);
    g_free(a);
}

static void launcher_filter(void);

// cache line: id \t desktop-file \t "NAME NUM rm" \t markup \t tail (scripts/launcher.py)
static void launcher_load(void) {
    char *text = NULL;
    if (!g_file_get_contents(L.cache, &text, NULL, NULL)) return;
    if (L.list) gtk_list_box_remove_all(GTK_LIST_BOX(L.list));
    g_array_set_size(L.order, 0);
    g_ptr_array_set_size(L.apps, 0);
    char **lines = g_strsplit(text, "\n", -1);
    g_free(text);
    for (char **l = lines; *l; l++) {
        char **f = g_strsplit(*l, "\t", 4);
        char *rm, *sp;
        if (g_strv_length(f) >= 3 && (rm = strrchr(f[2], ' '))) {
            *rm = 0;
            if ((sp = strrchr(f[2], ' '))) {
                *sp = 0;
                LApp *a = g_new0(LApp, 1);
                a->id = g_strdup(f[0]);
                a->file = g_strdup(f[1]);
                a->name = g_strdup(f[2]);
                a->num = g_strdup(sp + 1);
                a->fold = g_utf8_casefold(f[2], -1);
                a->rank = KEEP_RECENT;
                g_ptr_array_add(L.apps, a);
            }
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    int width = snprintf(NULL, 0, "%u", L.apps->len);
    for (guint i = 0; i < L.apps->len; i++) {
        LApp *a = g_ptr_array_index(L.apps, i);
        GtkWidget *h = HBOX(14, NULL);
        GtkWidget *num = gtk_label_new(a->num);
        gtk_label_set_xalign(GTK_LABEL(num), 1);
        gtk_label_set_width_chars(GTK_LABEL(num), width);
        gtk_widget_add_css_class(num, "num");
        GtkWidget *name = lbl(a->name, "name", 0);
        gtk_widget_set_hexpand(name, TRUE);
        a->tag = lbl("recent", "tag", 1);
        gtk_widget_set_visible(a->tag, FALSE);
        gtk_box_append(GTK_BOX(h), num);
        gtk_box_append(GTK_BOX(h), name);
        gtk_box_append(GTK_BOX(h), a->tag);
        a->row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(a->row), h);
        gtk_widget_set_focusable(a->row, FALSE);   // a click must not take the keyboard from the search field
        gtk_widget_set_cursor_from_name(a->row, "pointer");
        g_object_ref_sink(a->row);                 // kept while filtered out of the list
    }
    if (current == &launcherp) launcher_filter();
}

static gboolean mtime_after(const char *path, const struct timespec *t) {
    struct stat st;
    return stat(path, &st) == 0 &&
           (st.st_mtim.tv_sec > t->tv_sec || (st.st_mtim.tv_sec == t->tv_sec && st.st_mtim.tv_nsec > t->tv_nsec));
}

static GPtrArray *app_dirs(void) {
    GPtrArray *d = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(d, g_build_filename(g_get_user_data_dir(), "applications", NULL));
    for (const char *const *s = g_get_system_data_dirs(); *s; s++)
        g_ptr_array_add(d, g_build_filename(*s, "applications", NULL));
    return d;
}

// cache missing, or older than an applications folder, a .desktop file in one or the builder
static gboolean launcher_stale(void) {
    struct stat cs;
    if (stat(L.cache, &cs) < 0 || cs.st_size == 0) return TRUE;
    gboolean hit = mtime_after(L.py, &cs.st_mtim);
    GPtrArray *dirs = app_dirs();
    for (guint i = 0; i < dirs->len && !hit; i++) {
        const char *dir = g_ptr_array_index(dirs, i);
        if (mtime_after(dir, &cs.st_mtim)) { hit = TRUE; break; }
        GDir *gd = g_dir_open(dir, 0, NULL);
        if (!gd) continue;
        for (const char *e; !hit && (e = g_dir_read_name(gd));) {
            char *f = g_build_filename(dir, e, NULL);
            hit = mtime_after(f, &cs.st_mtim);
            g_free(f);
        }
        g_dir_close(gd);
    }
    g_ptr_array_free(dirs, TRUE);
    return hit;
}

static void launcher_rebuild(void);

static void rebuild_done(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d;
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, NULL);
    g_clear_object(&L.builder);
    if (ok) launcher_load();
    if (L.again) { L.again = FALSE; launcher_rebuild(); }
}

// Python + Gio parses .desktop files correctly but costs ~150 ms: run in the background, never on a key press
static void launcher_rebuild(void) {
    if (L.builder) { L.again = TRUE; return; }
    char *dir = g_path_get_dirname(L.cache);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    L.builder = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE, NULL, "python3", "-I", L.py, L.cache, NULL);
    if (L.builder) g_subprocess_wait_check_async(L.builder, NULL, rebuild_done, NULL);
}

static gboolean rebuild_later(gpointer d) { (void)d; L.rebuild_timer = 0; launcher_rebuild(); return G_SOURCE_REMOVE; }

static gboolean is_desktop_file(GFile *f) {
    if (!f) return FALSE;
    char *n = g_file_get_basename(f);
    gboolean yes = n && g_str_has_suffix(n, ".desktop");
    g_free(n);
    return yes;
}

static void apps_changed(GFileMonitor *m, GFile *file, GFile *other, GFileMonitorEvent ev, gpointer d) {
    (void)m; (void)d;
    if (ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED || ev == G_FILE_MONITOR_EVENT_PRE_UNMOUNT) return;
    if (!is_desktop_file(file) && !is_desktop_file(other)) return;
    if (L.rebuild_timer) g_source_remove(L.rebuild_timer);
    L.rebuild_timer = g_timeout_add(1000, rebuild_later, NULL);   // one rebuild per pacman transaction
}

static void launcher_init(void) {
    char *cdir = g_build_filename(g_get_user_cache_dir(), "hyprminimal", NULL);
    L.cache = g_build_filename(cdir, "launcher-apps", NULL);
    g_free(cdir);
    const char *xs = getenv("XDG_STATE_HOME");
    L.recent_path = xs && *xs ? g_build_filename(xs, "hyprminimal", "launcher-recent", NULL)
                              : g_build_filename(g_get_home_dir(), ".local", "state", "hyprminimal", "launcher-recent", NULL);
    L.py = g_build_filename(REPO, "scripts", "launcher.py", NULL);
    L.apps = g_ptr_array_new_with_free_func(lapp_free);
    L.order = g_array_new(FALSE, FALSE, sizeof(int));
    GPtrArray *dirs = app_dirs();
    for (guint i = 0; i < dirs->len; i++) {
        GFile *f = g_file_new_for_path(g_ptr_array_index(dirs, i));
        GFileMonitor *mon = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
        if (mon) g_signal_connect(mon, "changed", G_CALLBACK(apps_changed), NULL);   // lives as long as we do
        g_object_unref(f);
    }
    g_ptr_array_free(dirs, TRUE);
}

static void read_recent(void) {
    g_clear_pointer(&L.recent, g_strfreev);
    char *text = NULL;
    GPtrArray *ids = g_ptr_array_new();
    if (g_file_get_contents(L.recent_path, &text, NULL, NULL)) {
        char **v = g_strsplit_set(text, " \t\n", -1);
        for (char **s = v; *s; s++) if (**s) g_ptr_array_add(ids, g_strdup(*s));
        g_strfreev(v);
    }
    g_free(text);
    g_ptr_array_add(ids, NULL);
    L.recent = (char **)g_ptr_array_free(ids, FALSE);
    // the first KEEP_RECENT ids still installed are pinned (uninstalled apps drop out)
    for (guint i = 0; i < L.apps->len; i++) ((LApp *)g_ptr_array_index(L.apps, i))->rank = KEEP_RECENT;
    int n = 0;
    for (char **r = L.recent; *r && n < KEEP_RECENT; r++)
        for (guint i = 0; i < L.apps->len; i++) {
            LApp *a = g_ptr_array_index(L.apps, i);
            if (a->rank == KEEP_RECENT && !strcmp(a->id, *r)) { a->rank = n++; break; }
        }
    for (guint i = 0; i < L.apps->len; i++) {
        LApp *a = g_ptr_array_index(L.apps, i);
        gtk_widget_set_visible(a->tag, a->rank < KEEP_RECENT);
    }
}

static void push_recent(const char *id) {
    GString *s = g_string_new(id);
    g_string_append_c(s, '\n');
    int kept = 1;
    for (char **r = L.recent; r && *r && kept < KEEP_RECENT; r++)
        if (strcmp(*r, id)) { g_string_append_printf(s, "%s\n", *r); kept++; }
    char *dir = g_path_get_dirname(L.recent_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_file_set_contents(L.recent_path, s->str, -1, NULL);   // atomic: temp file + rename
    g_string_free(s, TRUE);
}

// one search word against one app: 0 name starts with it (or the exact number), 1 a word in the name
// starts with it, 2 inside the name, 3 its letters in order ("ffx" finds Firefox), -1 no match
static int word_tier(const LApp *a, const char *w, int *pos) {
    *pos = 0;
    if (all_digits(w)) {
        if (!strcmp(a->num, w)) return 0;
        if (g_str_has_prefix(a->num, w)) return 1;
    }
    const char *n = a->fold;
    if (g_str_has_prefix(n, w)) return 0;
    int inside = -1;
    for (const char *p = strstr(n, w); p; p = strstr(p + 1, w)) {
        if (!g_ascii_isalnum(p[-1])) { *pos = p - n; return 1; }
        if (inside < 0) inside = p - n;
    }
    if (inside >= 0) { *pos = inside; return 2; }
    const char *q = w;
    int first = -1;
    for (const char *p = n; *p && *q; p++)
        if (*p == *q) { if (first < 0) first = p - n; q++; }
    if (*q) return -1;
    *pos = first;
    return 3;
}

typedef struct { int i, score, rank; } Hit;
static int hit_cmp(const void *x, const void *y) {
    const Hit *a = x, *b = y;
    if (a->score != b->score) return a->score - b->score;
    if (a->rank != b->rank) return a->rank - b->rank;
    return a->i - b->i;   // A-Z
}

// typed text -> search words; "rm NAME" searches NAME
static char **query_words(void) {
    char *q = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(L.text)), -1);
    char *s = g_strstrip(q);
    if (!strcmp(s, "rm")) s += 2;
    else if (g_str_has_prefix(s, "rm ")) s += 3;
    char **v = g_strsplit_set(s, " \t", -1);
    g_free(q);
    return v;
}

static void launcher_filter(void) {
    if (!L.list) return;
    char **words = query_words();
    gtk_list_box_remove_all(GTK_LIST_BOX(L.list));
    g_array_set_size(L.order, 0);
    Hit *hits = g_new(Hit, L.apps->len + 1);
    int nh = 0;
    for (guint i = 0; i < L.apps->len; i++) {
        LApp *a = g_ptr_array_index(L.apps, i);
        int score = 0, k = 0;
        gboolean ok = TRUE;
        for (char **w = words; *w && ok; w++) {
            if (!**w) continue;
            int pos, tier = word_tier(a, *w, &pos);
            if (tier < 0) ok = FALSE;
            else score += tier * 1000 + (k++ ? 0 : MIN(pos, 999));
        }
        // empty search: the recent apps first, then A-Z
        if (ok) hits[nh++] = (Hit){(int)i, score, a->rank};
    }
    qsort(hits, nh, sizeof *hits, hit_cmp);
    for (int h = 0; h < nh; h++) {
        g_array_append_val(L.order, hits[h].i);
        gtk_list_box_append(GTK_LIST_BOX(L.list), ((LApp *)g_ptr_array_index(L.apps, hits[h].i))->row);
    }
    g_free(hits);
    g_strfreev(words);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(L.scroll)), 0);
    L.sel = menu_select(L.list, NULL, 0, nh);
}

static void on_query(GtkEditable *e, gpointer d) { (void)e; (void)d; launcher_filter(); }

// Enter: a typed number launches that app, otherwise the selected row; "rm ..." uninstalls instead
static void launcher_activate(int sel, gboolean clicked) {
    char *t = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(L.text))));
    char *rest = t;
    gboolean remove = FALSE;
    if (!strcmp(t, "rm") || g_str_has_prefix(t, "rm ")) {
        remove = TRUE;
        rest = g_strchug(t + 2);
    }
    int pick = -1;
    if (remove && !*rest) { g_free(t); return; }   // bare "rm": never guess which app to remove
    if (!clicked && all_digits(rest) && strlen(rest) < 9) {
        long v = atol(rest);
        if (v >= 1 && v <= (long)L.apps->len) pick = v - 1;   // "12" + Enter = app number 12
    }
    g_free(t);
    if (pick < 0 && sel >= 0 && sel < (int)L.order->len) pick = g_array_index(L.order, int, sel);
    if (pick < 0) return;
    LApp *a = g_ptr_array_index(L.apps, pick);
    char *id = g_strdup(a->id), *file = g_strdup(a->file), *name = g_strdup(a->name);
    manager_close();
    if (remove) {
        char *title = g_strdup_printf("Remove %s", name), *script = g_build_filename(REPO, "scripts", "remove-app.sh", NULL);
        const char *argv[] = {"kitty", "--class", "hyprminimal-remove", "--title", title, script, file, name, NULL};
        spawn_app(argv);
        g_free(title); g_free(script);
    } else {
        const char *argv[] = {"gio", "launch", file, NULL};
        spawn_app(argv);
        push_recent(id);
    }
    g_free(id); g_free(file); g_free(name);
}

static void on_app_row(GtkListBox *l, GtkListBoxRow *row, gpointer d) {
    (void)l; (void)d;
    launcher_activate(gtk_list_box_row_get_index(row), TRUE);
}

static gboolean launcher_key(Panel *p, guint kv) {
    (void)p;
    int n = L.order->len, s = L.sel;
    switch (kv) {
    case GDK_KEY_Down: case GDK_KEY_KP_Down: case GDK_KEY_Tab: s = menu_step(s, 1, n, TRUE); break;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: case GDK_KEY_ISO_Left_Tab: s = menu_step(s, -1, n, TRUE); break;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: s = menu_step(s, LAUNCHER_LINES, n, FALSE); break;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: s = menu_step(s, -LAUNCHER_LINES, n, FALSE); break;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter: launcher_activate(L.sel, FALSE); return TRUE;
    default: return FALSE;   // typing goes to the search field
    }
    L.sel = menu_select(L.list, L.scroll, s, n);
    return TRUE;
}

static void launcher_build(Panel *p, GtkWidget *body) {
    (void)p;
    gtk_widget_add_css_class(body, "launcher");
    gtk_box_set_spacing(GTK_BOX(body), 10);
    GtkWidget *in = HBOX(12, "launcher-input");
    gtk_box_append(GTK_BOX(in), lbl("\U000F0349", "prompt", 0.5));
    L.text = gtk_text_new();
    gtk_widget_set_hexpand(L.text, TRUE);
    gtk_text_set_placeholder_text(GTK_TEXT(L.text), "Search, type a number, or rm NAME to uninstall");
    g_signal_connect(L.text, "changed", G_CALLBACK(on_query), NULL);
    gtk_box_append(GTK_BOX(in), L.text);
    gtk_box_append(GTK_BOX(body), in);
    L.list = gtk_list_box_new();
    gtk_widget_add_css_class(L.list, "menu");
    gtk_widget_set_focusable(L.list, FALSE);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(L.list), GTK_SELECTION_SINGLE);
    g_signal_connect(L.list, "row-activated", G_CALLBACK(on_app_row), NULL);
    L.scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(L.scroll), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(L.scroll), L.list);
    gtk_box_append(GTK_BOX(body), L.scroll);
    launcher_load();
    if (launcher_stale()) launcher_rebuild();
}

static void launcher_opened(Panel *p) {
    (void)p;
    read_recent();
    g_signal_handlers_block_by_func(L.text, on_query, NULL);
    gtk_editable_set_text(GTK_EDITABLE(L.text), "");
    g_signal_handlers_unblock_by_func(L.text, on_query, NULL);
    launcher_filter();
    // always LAUNCHER_LINES rows tall, so the box never jumps while typing
    if (L.apps->len) {
        int mn, nat;
        GtkWidget *row = ((LApp *)g_ptr_array_index(L.apps, 0))->row;
        gtk_widget_measure(row, GTK_ORIENTATION_VERTICAL, -1, &mn, &nat, NULL, NULL);
        if (nat > 0 && nat != L.row_h) {
            L.row_h = nat;
            gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(L.scroll), nat * LAUNCHER_LINES);
            gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(L.scroll), nat * LAUNCHER_LINES);
        }
    }
    gtk_widget_grab_focus(L.text);
}

static Panel launcherp = {.kind = "launcher", .width = 600, .middle = TRUE, .build = launcher_build,
                          .opened = launcher_opened, .key = launcher_key};

// --------------------------------------------------------------------------- session menu (Super+Esc)
// Lock, Lock & Suspend, Reboot, Shut Down: keys 1-4, or arrows + Enter
static const struct { const char *icon, *label; } SESSION[] = {
    {"\U000F033E", "Lock"}, {"\U000F03E4", "Lock & Suspend"}, {"\U000F0453", "Reboot"}, {"\U000F0425", "Shut Down"},
};
static struct { GtkWidget *list; int sel; } SM;

static void session_run(int i) {
    manager_close();
    char *msg = g_build_filename(REPO, "bin", "hyprminimal-msg", NULL);
    const char *lock[] = {msg, "lock", NULL};
    const char *suspend[] = {"sh", "-c", "\"$0\" lock & sleep 1; systemctl suspend", msg, NULL};
    const char *reboot[] = {"systemctl", "reboot", NULL}, *off[] = {"systemctl", "poweroff", NULL};
    const char *const *cmds[] = {lock, suspend, reboot, off};
    if (i >= 0 && i < (int)G_N_ELEMENTS(cmds)) spawn_app(cmds[i]);
    g_free(msg);
}

static void on_session_row(GtkListBox *l, GtkListBoxRow *row, gpointer d) {
    (void)l; (void)d;
    session_run(gtk_list_box_row_get_index(row));
}

static gboolean session_key(Panel *p, guint kv) {
    (void)p;
    int n = G_N_ELEMENTS(SESSION), s = SM.sel;
    if (kv >= GDK_KEY_1 && kv < GDK_KEY_1 + (guint)n) { session_run(kv - GDK_KEY_1); return TRUE; }
    if (kv >= GDK_KEY_KP_1 && kv < GDK_KEY_KP_1 + (guint)n) { session_run(kv - GDK_KEY_KP_1); return TRUE; }
    switch (kv) {
    case GDK_KEY_Down: case GDK_KEY_KP_Down: case GDK_KEY_Tab: s = menu_step(s, 1, n, TRUE); break;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: case GDK_KEY_ISO_Left_Tab: s = menu_step(s, -1, n, TRUE); break;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter: case GDK_KEY_space:
        session_run(SM.sel); return TRUE;
    default: return FALSE;
    }
    SM.sel = menu_select(SM.list, NULL, s, n);
    return TRUE;
}

static void session_build(Panel *p, GtkWidget *body) {
    (void)p;
    gtk_widget_add_css_class(body, "session");
    SM.list = gtk_list_box_new();
    gtk_widget_add_css_class(SM.list, "menu");
    gtk_widget_set_focusable(SM.list, FALSE);
    g_signal_connect(SM.list, "row-activated", G_CALLBACK(on_session_row), NULL);
    for (size_t i = 0; i < G_N_ELEMENTS(SESSION); i++) {
        GtkWidget *h = HBOX(12, NULL);
        gtk_box_append(GTK_BOX(h), lbl(SESSION[i].icon, "glyph", 0.5));
        GtkWidget *name = lbl(SESSION[i].label, "name", 0);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_box_append(GTK_BOX(h), name);
        char key[4];
        snprintf(key, sizeof key, "%zu", i + 1);
        gtk_box_append(GTK_BOX(h), lbl(key, "num", 1));
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), h);
        gtk_widget_set_focusable(row, FALSE);
        gtk_widget_set_cursor_from_name(row, "pointer");
        if (i == G_N_ELEMENTS(SESSION) - 1) gtk_widget_add_css_class(row, "urgent");
        gtk_list_box_append(GTK_LIST_BOX(SM.list), row);
    }
    gtk_box_append(GTK_BOX(body), SM.list);
}

static void session_opened(Panel *p) {
    (void)p;
    SM.sel = menu_select(SM.list, NULL, 0, G_N_ELEMENTS(SESSION));
}

static Panel sessionp = {.kind = "session", .width = 260, .build = session_build, .opened = session_opened,
                         .key = session_key};

// --------------------------------------------------------------------------- manager
static Panel *PANELS[] = {&calendar, &battery, &network, &sysp, &launcherp, &sessionp};
static GtkWidget *catcher;
static char *external;                  // a popup run as its own program (rofi): closed with pkill

static Panel *find_panel(const char *kind) {
    for (size_t i = 0; i < G_N_ELEMENTS(PANELS); i++)
        if (strcmp(PANELS[i]->kind, kind) == 0) return PANELS[i];
    return NULL;
}

static void manager_close(void) {
    if (external) {
        const char *argv[] = {"pkill", "-x", external, NULL};
        spawn_detached(argv);
        g_clear_pointer(&external, g_free);
    }
    if (current) {
        Panel *p = current;
        current = NULL;
        panel_closed(p);
        gtk_widget_set_visible(p->win, FALSE);
    }
    if (catcher) gtk_widget_set_visible(catcher, FALSE);
}

static void manager_open(Panel *p, GdkMonitor *mon) {
    manager_close();
    hide_control_center();
    close_python_panel();
    if (LS) {
        // exclusive like the Super+S control center: with follow_mouse an on-demand popup loses the
        // keyboard as soon as the pointer leaves it, and Esc went to the window underneath
        gtk_layer_set_keyboard_mode(GTK_WINDOW(p->win), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_layer_set_monitor(GTK_WINDOW(p->win), mon);
    }
    current = p;
    panel_opened(p);
    gtk_window_present(GTK_WINDOW(p->win));
}

static void manager_toggle(const char *kind) {
    Panel *p = find_panel(kind);
    if (!p) return;
    if (current == p) manager_close();
    else manager_open(p, NULL);
}

static void catcher_pressed(GtkGestureClick *g, int n, double x, double y, gpointer d) {
    (void)g; (void)n; (void)x; (void)y; (void)d;
    manager_close();
}

// invisible full-screen layer under a rofi popup: a click anywhere outside closes it
static GtkWidget *make_catcher(void) {
    GtkWidget *w = gtk_window_new();
    gtk_window_set_application(GTK_WINDOW(w), app);
    gtk_window_set_decorated(GTK_WINDOW(w), FALSE);
    gtk_widget_add_css_class(w, "hyprminimal-catcher");
    gtk_layer_init_for_window(GTK_WINDOW(w));
    gtk_layer_set_namespace(GTK_WINDOW(w), "hyprminimal-catcher");
    gtk_layer_set_layer(GTK_WINDOW(w), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(w), -1);
    for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++) gtk_layer_set_anchor(GTK_WINDOW(w), e, TRUE);
    GtkWidget *b = HBOX(0, NULL);
    gtk_widget_set_hexpand(b, TRUE);
    gtk_widget_set_vexpand(b, TRUE);
    gtk_window_set_child(GTK_WINDOW(w), b);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(catcher_pressed), NULL);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(click));
    return w;
}

// outside clicks close another program's popup too (launcher, session menu): `prog` is its process
// name while it runs; "" = it closed by itself, just drop the catcher
static void manager_catch(const char *prog) {
    if (!*prog) {
        if (external && !current && catcher) gtk_widget_set_visible(catcher, FALSE);
        g_clear_pointer(&external, g_free);
        return;
    }
    manager_close();
    external = g_strdup(prog);
    if (!LS) return;
    if (!catcher) catcher = make_catcher();
    gtk_widget_set_visible(catcher, TRUE);
}

// --------------------------------------------------------------------------- clock click strips
// Invisible strip over the bar's date + clock (one per monitor): a click toggles the calendar there,
// a right click opens the control center. Geometry from .generated/clock_zone.json.
static GPtrArray *zones;

static double json_num(const char *js, const char *key, double def) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(js, k);
    if (!p || !(p = strchr(p + strlen(k), ':'))) return def;
    return g_ascii_strtod(p + 1, NULL);
}

static gboolean json_has(const char *js, const char *key, const char *val) {
    char k[96];
    snprintf(k, sizeof k, "\"%s\": \"%s\"", key, val);
    return strstr(js, k) != NULL;
}

static void zone_pressed(GtkGestureClick *g, int n, double x, double y, gpointer mon) {
    (void)n; (void)x; (void)y;
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)) == 3) {
        char *msg = g_build_filename(REPO, "bin", "hyprminimal-msg", NULL);
        const char *argv[] = {msg, "control-center", NULL};
        spawn_detached(argv);
        g_free(msg);
    } else if (current == &calendar) {
        manager_close();
    } else {
        manager_open(&calendar, mon);
    }
}

static void reload_zones(void) {
    if (zones) g_ptr_array_free(zones, TRUE);
    zones = g_ptr_array_new_with_free_func((GDestroyNotify)gtk_window_destroy);
    if (!LS) return;
    char *path = g_build_filename(GEN, "clock_zone.json", NULL), *js = NULL;
    gboolean ok = g_file_get_contents(path, &js, NULL, NULL);
    g_free(path);
    if (!ok) return;
    int w = (int)json_num(js, "width", 0), h = (int)json_num(js, "height", 0);
    double margin = json_num(js, "margin", 0), offset = json_num(js, "offset", 0);
    gboolean bottom = json_has(js, "position", "bottom");
    g_free(js);
    if (w <= 0 || h <= 0) return;
    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    for (guint i = 0; i < g_list_model_get_n_items(mons); i++) {
        GdkMonitor *mon = g_list_model_get_item(mons, i);
        GtkWidget *z = gtk_window_new();
        GtkWindow *zw = GTK_WINDOW(z);
        gtk_window_set_application(zw, app);
        gtk_window_set_decorated(zw, FALSE);
        gtk_widget_add_css_class(z, "hyprminimal-clickzone");
        gtk_layer_init_for_window(zw);
        gtk_layer_set_namespace(zw, "hyprminimal-clickzone");
        gtk_layer_set_layer(zw, GTK_LAYER_SHELL_LAYER_OVERLAY);   // above waybar (layer "top")
        gtk_layer_set_keyboard_mode(zw, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_exclusive_zone(zw, -1);                    // sits on the bar, not below it
        gtk_layer_set_monitor(zw, mon);
        GtkLayerShellEdge edge = bottom ? GTK_LAYER_SHELL_EDGE_BOTTOM : GTK_LAYER_SHELL_EDGE_TOP;
        gtk_layer_set_anchor(zw, edge, TRUE);
        gtk_layer_set_margin(zw, edge, (int)margin);
        gtk_layer_set_anchor(zw, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        GdkRectangle geo;
        gdk_monitor_get_geometry(mon, &geo);
        gtk_layer_set_margin(zw, GTK_LAYER_SHELL_EDGE_LEFT, MAX(0, (int)lround((geo.width - w) / 2.0 + offset)));
        GtkWidget *b = HBOX(0, NULL);
        gtk_widget_set_size_request(b, w, h);
        gtk_window_set_child(zw, b);
        gtk_window_set_default_size(zw, w, h);
        gtk_widget_set_cursor_from_name(z, "pointer");
        GtkGesture *click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
        // the monitor outlives the strip: strips are rebuilt whenever the monitor list changes
        g_signal_connect_data(click, "pressed", G_CALLBACK(zone_pressed), mon, (GClosureNotify)(void (*)(void))g_object_unref, 0);
        gtk_widget_add_controller(z, GTK_EVENT_CONTROLLER(click));
        gtk_window_present(zw);
        g_ptr_array_add(zones, z);
    }
}

// --------------------------------------------------------------------------- desktop background
// No wallpaper program: one BACKGROUND layer per monitor with the theme colour, the Arch logo
// (.generated/arch-logo.png) and a wordmark (styled in .generated/panels.css); both follow the theme live. Drawn once, nothing ticks.
// Hyprland paints the same colour itself (misc.background_color, .generated/hypr-theme.lua) whenever we're
// not running, so a restart never flashes black.
static GPtrArray *bgs;

// .generated/desktop (Settings → General → Desktop): "logo=on|off" and "name=TEXT" lines
static void desktop_conf(gboolean *logo, char **name) {
    char *path = g_build_filename(GEN, "desktop", NULL), *text = NULL;
    *logo = TRUE;
    *name = g_strdup("hyprminimal");
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l; l++) {
            if (g_str_has_prefix(*l, "logo=")) *logo = strcmp(*l + 5, "off") != 0;
            else if (g_str_has_prefix(*l, "name=")) { g_free(*name); *name = g_strdup(*l + 5); }
        }
        g_strfreev(lines);
    }
    g_free(text);
    g_free(path);
}

static void reload_backgrounds(void) {
    if (!LS) return;
    gboolean show_logo;
    char *name;
    desktop_conf(&show_logo, &name);
    if (!bgs) bgs = g_ptr_array_new_with_free_func((GDestroyNotify)gtk_window_destroy);
    g_ptr_array_set_size(bgs, 0);
    GListModel *ms = gdk_display_get_monitors(gdk_display_get_default());
    for (guint i = 0; i < g_list_model_get_n_items(ms); i++) {
        GdkMonitor *mon = g_list_model_get_item(ms, i);
        GtkWindow *w = GTK_WINDOW(gtk_window_new());
        gtk_window_set_application(w, app);
        gtk_window_set_decorated(w, FALSE);
        gtk_widget_add_css_class(GTK_WIDGET(w), "hyprminimal-background");
        gtk_layer_init_for_window(w);
        gtk_layer_set_namespace(w, "hyprminimal-background");
        gtk_layer_set_layer(w, GTK_LAYER_SHELL_LAYER_BACKGROUND);
        gtk_layer_set_monitor(w, mon);
        gtk_layer_set_exclusive_zone(w, -1);
        gtk_layer_set_keyboard_mode(w, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++) gtk_layer_set_anchor(w, e, TRUE);
        GtkWidget *v = VBOX(0, "bg-mark");
        gtk_widget_set_halign(v, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
        gtk_widget_set_can_target(v, FALSE);
        // logo: .generated/arch-logo.png in the theme colour (hyprminimal-settings draws it at 2x: shown at
        // half size it stays sharp on scaled screens); missing (never rendered yet) = just the wordmark
        char *png = g_build_filename(GEN, "arch-logo.png", NULL);
        GdkTexture *tex = show_logo ? gdk_texture_new_from_filename(png, NULL) : NULL;
        if (tex) {
            GtkWidget *logo = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));   // square: one size
            gtk_widget_add_css_class(logo, "bg-logo");
            gtk_image_set_pixel_size(GTK_IMAGE(logo), gdk_texture_get_width(tex) / 2);
            gtk_box_append(GTK_BOX(v), logo);
            g_object_unref(tex);
        }
        g_free(png);
        if (*name) {
            GtkWidget *word = lbl(name, "bg-word", 0.5);
            gtk_label_set_ellipsize(GTK_LABEL(word), PANGO_ELLIPSIZE_NONE);   // centred, nothing to cut
            gtk_box_append(GTK_BOX(v), word);
        }
        gtk_window_set_child(w, v);
        gtk_window_present(w);
        g_ptr_array_add(bgs, w);
        g_object_unref(mon);
    }
    g_free(name);
}

// --------------------------------------------------------------------------- theme
static GtkCssProvider *css;

static void load_css(void) {
    char *path = g_build_filename(GEN, "panels.css", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {   // never rendered yet: the repo template (Tokyo Night)
        g_free(path);
        path = g_build_filename(REPO, "settings", "panels.css", NULL);
    }
    char *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) gtk_css_provider_load_from_string(css, text);
    g_free(text);
    g_free(path);
    // light or dark base for the few widgets the stylesheet doesn't fully paint (calendar arrows)
    char *tf = g_build_filename(GEN, "theme", NULL), *t = NULL;
    if (g_file_get_contents(tf, &t, NULL, NULL)) {
        gboolean dark = strstr(t, "light") == NULL;
#if GTK_CHECK_VERSION(4, 20, 0)
        g_object_set(gtk_settings_get_default(), "gtk-interface-color-scheme",
                     dark ? GTK_INTERFACE_COLOR_SCHEME_DARK : GTK_INTERFACE_COLOR_SCHEME_LIGHT, NULL);
#else
        g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", dark, NULL);
#endif
    }
    g_free(t);
    g_free(tf);
}

static guint css_timer, zone_timer, bg_timer;
static gboolean bgs_later(gpointer d) { (void)d; bg_timer = 0; reload_backgrounds(); return G_SOURCE_REMOVE; }
static gboolean css_later(gpointer d) { (void)d; css_timer = 0; load_css(); return G_SOURCE_REMOVE; }
static gboolean zones_later(gpointer d) { (void)d; zone_timer = 0; reload_zones(); return G_SOURCE_REMOVE; }

static void gen_changed(GFileMonitor *m, GFile *file, GFile *other, GFileMonitorEvent ev, gpointer d) {
    (void)m; (void)d;
    if (ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED || ev == G_FILE_MONITOR_EVENT_PRE_UNMOUNT) return;
    GFile *f = (ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_IN) && other ? other : file;
    char *name = g_file_get_basename(f);
    // debounced: one reload per write burst
    if (!strcmp(name, "panels.css") || !strcmp(name, "theme")) {
        if (css_timer) g_source_remove(css_timer);
        css_timer = g_timeout_add(150, css_later, NULL);
    } else if (!strcmp(name, "arch-logo.png") || !strcmp(name, "desktop")) {   // theme colour, name, logo
        if (bg_timer) g_source_remove(bg_timer);
        bg_timer = g_timeout_add(150, bgs_later, NULL);
    } else if (!strcmp(name, "clock_zone.json")) {
        if (zone_timer) g_source_remove(zone_timer);
        zone_timer = g_timeout_add(600, zones_later, NULL);   // after waybar has re-laid out
    }
    g_free(name);
}

static void monitors_changed(GListModel *l, guint pos, guint rem, guint add, gpointer d) {
    (void)l; (void)pos; (void)rem; (void)add; (void)d;
    if (current) manager_close();
    if (zone_timer) g_source_remove(zone_timer);
    zone_timer = g_timeout_add(300, zones_later, NULL);
    if (bg_timer) g_source_remove(bg_timer);
    bg_timer = g_timeout_add(300, bgs_later, NULL);
}

// --------------------------------------------------------------------------- application
static void act_panel(GSimpleAction *a, GVariant *v, gpointer d) { (void)a; (void)d; manager_toggle(g_variant_get_string(v, NULL)); }
static void act_close(GSimpleAction *a, GVariant *v, gpointer d) { (void)a; (void)v; (void)d; if (current) manager_close(); }
static void act_catch(GSimpleAction *a, GVariant *v, gpointer d) { (void)a; (void)d; manager_catch(g_variant_get_string(v, NULL)); }
static void act_quit(GSimpleAction *a, GVariant *v, gpointer d) { (void)a; (void)v; (void)d; g_application_quit(G_APPLICATION(app)); }

static void startup(GApplication *a, gpointer d) {
    (void)d;
    g_application_hold(a);              // resident: windows are hidden most of the time
    LS = gtk_layer_is_supported();
    css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    load_css();
    launcher_init();
    static const GActionEntry acts[] = {
        {"panel", act_panel, "s", NULL, NULL, {0}}, {"panel-close", act_close, NULL, NULL, NULL, {0}},
        {"catch", act_catch, "s", NULL, NULL, {0}}, {"quit", act_quit, NULL, NULL, NULL, {0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(a), acts, G_N_ELEMENTS(acts), NULL);
    for (size_t i = 0; i < G_N_ELEMENTS(PANELS); i++) panel_create(PANELS[i]);   // first click is instant
    reload_zones();
    reload_backgrounds();
    g_signal_connect(gdk_display_get_monitors(gdk_display_get_default()), "items-changed",
                     G_CALLBACK(monitors_changed), NULL);
    GFile *gen = g_file_new_for_path(GEN);
    GFileMonitor *mon = g_file_monitor_directory(gen, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (mon) g_signal_connect(mon, "changed", G_CALLBACK(gen_changed), NULL);   // lives as long as we do
    g_object_unref(gen);
}

// `panels` starts it; `panels --panel KIND` toggles a popup (in the running copy, over D-Bus)
static int command_line(GApplication *a, GApplicationCommandLine *cl, gpointer d) {
    (void)a; (void)d;
    int argc;
    char **argv = g_application_command_line_get_arguments(cl, &argc);
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--panel")) manager_toggle(argv[i + 1]);
    g_strfreev(argv);
    return 0;
}

int main(int argc, char **argv) {
    const char *env = getenv("HYPRMINIMAL_DIR");
    if (env && *env) REPO = g_strdup(env);
    else {   // .generated/bin/panels -> repo
        char *exe = g_file_read_link("/proc/self/exe", NULL);
        char *a = exe ? g_path_get_dirname(exe) : g_strdup("."), *b = g_path_get_dirname(a);
        REPO = g_path_get_dirname(b);
        g_free(exe); g_free(a); g_free(b);
    }
    GEN = g_build_filename(REPO, ".generated", NULL);
    // software rendering: these popups are small and mostly still, and the GPU renderers map the whole
    // Vulkan/GL driver stack (~165 MB RSS vs ~75 MB). GSK_RENDERER in the environment still wins.
    OWN_GSK = !getenv("GSK_RENDERER");   // then apps we launch must not inherit ours
    g_setenv("GSK_RENDERER", "cairo", FALSE);
    app = gtk_application_new("dev.hyprminimal.Panels", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(command_line), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
