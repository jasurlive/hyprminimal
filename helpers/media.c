// pkg-config: gio-2.0
// Bar media capsule (waybar custom/media): the title of what plays, and the equalizer frame as a class.
// Listens to playerctld on D-Bus (the "current player", started by bin/hyprminimal): no playerctl process.
// While playing it steps the class f0..f3 FPS times a second, so waybar repaints just the small icon
// (images .generated/media-*.png, drawn by hyprminimal-settings). Paused or stopped: no timer, no wakeups.
// Nothing playing: empty text, waybar hides the capsule.
#define _GNU_SOURCE
#include <gio/gio.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>

#define FPS 3
#define FRAMES 4    // = EQ_FRAMES in helpers/panels/settings/render.c
#define TITLE_MAX 26      // characters of the title shown in the capsule

static GDBusProxy *P;
static guint tick;
static int frame;
static char *text;  // JSON-escaped title, NULL = nothing playing
static const char *status = "stopped";

static void out(void) {
    int n;
    if (!text) n = printf("{\"text\":\"\",\"class\":\"stopped\"}\n");
    else if (strcmp(status, "playing") == 0) n = printf("{\"text\":\"%s\",\"class\":[\"playing\",\"f%d\"]}\n", text, frame);
    else n = printf("{\"text\":\"%s\",\"class\":\"%s\"}\n", text, status);
    if (n < 0 || fflush(stdout) != 0) exit(0);   // waybar went away
}

static gboolean on_tick(gpointer d) {
    (void)d;
    frame = (frame + 1) % FRAMES;
    out();
    return G_SOURCE_CONTINUE;
}

static char *json_escape(const char *s) {
    GString *g = g_string_new(NULL);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') g_string_append_c(g, '\\');
        if ((unsigned char)*s < 0x20) g_string_append_c(g, ' ');
        else g_string_append_c(g, *s);
    }
    return g_string_free(g, FALSE);
}

static void update(void) {
    GVariant *st = g_dbus_proxy_get_cached_property(P, "PlaybackStatus");
    GVariant *md = g_dbus_proxy_get_cached_property(P, "Metadata");
    const char *s = st ? g_variant_get_string(st, NULL) : "";
    status = !strcmp(s, "Playing") ? "playing" : !strcmp(s, "Paused") ? "paused" : "stopped";
    g_clear_pointer(&text, g_free);
    if (strcmp(status, "stopped") != 0) {
        const char *t = NULL;
        if (md) g_variant_lookup(md, "xesam:title", "&s", &t);
        if (!t || !*t) t = "Nothing Playing";
        char *head = g_utf8_strlen(t, -1) > TITLE_MAX ? g_utf8_substring(t, 0, TITLE_MAX - 1) : NULL;
        char *cut = head ? g_strconcat(head, "…", NULL) : g_strdup(t);
        text = json_escape(cut);
        g_free(cut);
        g_free(head);
    }
    if (st) g_variant_unref(st);
    if (md) g_variant_unref(md);
    gboolean playing = strcmp(status, "playing") == 0;
    if (playing && !tick) tick = g_timeout_add(1000 / FPS, on_tick, NULL);
    if (!playing && tick) { g_source_remove(tick); tick = 0; }
    out();
}

static void changed(GDBusProxy *p, GVariant *ch, GStrv inv, gpointer d) { (void)p; (void)ch; (void)inv; (void)d; update(); }
static void owner(GObject *o, GParamSpec *ps, gpointer d) { (void)o; (void)ps; (void)d; update(); }

int main(void) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);   // never outlive waybar
    signal(SIGPIPE, SIG_IGN);           // a failed write exits in out()
    P = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL,
                                      "org.mpris.MediaPlayer2.playerctld", "/org/mpris/MediaPlayer2",
                                      "org.mpris.MediaPlayer2.Player", NULL, NULL);
    if (!P) return 1;                   // no session bus: waybar restarts us (restart-interval)
    g_signal_connect(P, "g-properties-changed", G_CALLBACK(changed), NULL);
    g_signal_connect(P, "notify::g-name-owner", G_CALLBACK(owner), NULL);
    update();
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}
