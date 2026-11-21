// hyprminimal greeter: the lock screen (hyprlock.conf) as a greetd login screen.
// Runs as the `greeter` user inside cage (see bin/hyprminimal-greeter), talks to greetd over $GREETD_SOCK.
// Colours, font and sizes come from the hyprlock.conf copied next to it, so the login screen follows the theme.
//   greeter            real greeter
//   greeter --test     windowed preview with a fake greetd (password "test"); Esc quits
// pkg-config: gtk4 json-c
#include <gtk/gtk.h>
#include <json-c/json.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SHARE "/usr/local/share/hyprminimal-greeter"
#define STATE "/var/cache/hyprminimal-greeter/last-user"

static gboolean test_mode;
static GtkWidget *clock_lbl, *date_lbl, *user_lbl, *entry, *prev_btn, *next_btn;
static char **users;   // login names (NULL-terminated)
static char **names;   // display names
static int n_users, cur;
static gboolean busy;

// ---------------------------------------------------------------- hyprlock.conf -> CSS
// Tiny reader: "$var = value", "section {", "key = value", "}". Only what the lock screen uses.
typedef struct { char key[64]; char val[256]; } kv;
static kv vars[64], keys[256];
static int n_vars, n_keys;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

static const char *subst(const char *v) {   // resolve a leading $var
    if (v[0] != '$') return v;
    for (int i = 0; i < n_vars; i++)
        if (!strcmp(vars[i].key, v + 1)) return vars[i].val;
    return v;
}

static void read_conf(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512], section[64] = "", lastlabel[64] = "";
    int labels = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *brace = strchr(s, '{');
        if (brace) {
            *brace = 0;
            snprintf(section, sizeof section, "%s", trim(s));
            if (!strcmp(section, "label")) snprintf(section, sizeof section, "label%d", labels++);
            snprintf(lastlabel, sizeof lastlabel, "%s", section);
            continue;
        }
        if (*s == '}') { section[0] = 0; continue; }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (*k == '$' && !section[0] && n_vars < 64) {
            snprintf(vars[n_vars].key, 64, "%s", k + 1);
            snprintf(vars[n_vars++].val, 256, "%s", v);
        } else if (section[0] && n_keys < 256) {
            snprintf(keys[n_keys].key, 64, "%s.%s", section, k);
            snprintf(keys[n_keys++].val, 256, "%s", subst(v));
        }
    }
    fclose(f);
}

static const char *get(const char *key, const char *def) {
    for (int i = 0; i < n_keys; i++)
        if (!strcmp(keys[i].key, key)) return keys[i].val;
    return def;
}

// rgb(rrggbb) / rgba(rrggbbaa) -> "rgba(r, g, b, a)"
static const char *css_color(const char *v, char *buf, size_t n) {
    unsigned r = 0, g = 0, b = 0, a = 255;
    const char *p = strchr(v, '(');
    if (p && strlen(p + 1) >= 6) {
        char hex[9] = {0};
        strncpy(hex, p + 1, 8);
        char *close = strchr(hex, ')');
        if (close) *close = 0;
        if (strlen(hex) == 8) sscanf(hex, "%2x%2x%2x%2x", &r, &g, &b, &a);
        else sscanf(hex, "%2x%2x%2x", &r, &g, &b);
    }
    snprintf(buf, n, "rgba(%u, %u, %u, %.3f)", r, g, b, a / 255.0);
    return buf;
}
#define C(key, def) css_color(get(key, def), (char[48]){0}, 48)

static int num(const char *key, int def, int idx) {   // "460, 150" -> idx 0 / 1
    const char *v = get(key, NULL);
    if (!v) return def;
    int a = def, b = def;
    if (sscanf(v, "%d , %d", &a, &b) < 1) return def;
    return idx ? b : a;
}

static char *build_css(void) {
    char font[128];
    snprintf(font, sizeof font, "%s", subst("$font"));
    if (!strcmp(font, "$font")) snprintf(font, sizeof font, "Noto Sans");
    return g_strdup_printf(
        "window, .bg { background: %s; }\n"
        "* { font-family: \"%s\", \"Symbols Nerd Font\"; font-weight: bold; }\n"
        ".clock { color: %s; font-size: %dpx; }\n"
        ".date { color: %s; font-size: %dpx; margin-bottom: 50px; }\n"
        ".card { background: %s; border-radius: 0; border: %dpx solid %s; min-width: %dpx; min-height: %dpx; }\n"
        ".user { color: %s; font-size: %dpx; margin: 0 14px; }\n"
        ".user-row { margin-top: 26px; margin-bottom: 14px; }\n"
        ".arrow { background: none; border: none; box-shadow: none; outline: none; color: %s;"
        " font-size: 16px; min-width: 28px; min-height: 28px; padding: 0; border-radius: 0; }\n"
        ".arrow:hover { color: %s; }\n"
        ".password { min-width: %dpx; min-height: %dpx; background: %s; color: %s;"
        " border: %dpx solid %s; border-radius: 0; box-shadow: none; outline: none; padding: 0 14px; font-size: 15px; }\n"
        ".password:focus-within { border-color: %s; }\n"
        ".password.caps { border-color: %s; }\n"
        ".password.fail { border-color: %s; }\n"
        ".password text { caret-color: %s; }\n"
        ".password text placeholder { color: %s; }\n"
        ".password.fail text placeholder { color: %s; }\n"
        ".password image { color: %s; }\n",
        C("background.color", "rgb(222436)"), font,
        C("label0.color", "rgba(c0caf5f2)"), num("label0.font_size", 120, 0),
        C("label1.color", "rgba(a9b1d6d9)"), num("label1.font_size", 18, 0),
        C("shape.color", "rgb(1f2335)"), num("shape.border_size", 1, 0), C("shape.border_color", "rgb(4b517a)"),
        num("shape.size", 460, 0), num("shape.size", 150, 1),
        C("label2.color", "rgb(c0caf5)"), num("label2.font_size", 15, 0),
        C("label1.color", "rgb(a9b1d6)"), C("input-field.check_color", "rgb(7a88cf)"),
        num("input-field.size", 380, 0), num("input-field.size", 48, 1),
        C("input-field.inner_color", "rgb(2c314a)"), C("input-field.font_color", "rgb(c0caf5)"),
        num("input-field.outline_thickness", 2, 0), C("input-field.outer_color", "rgb(4b517a)"),
        C("input-field.check_color", "rgb(7a88cf)"), C("input-field.capslock_color", "rgb(e6c384)"),
        C("input-field.fail_color", "rgb(f7768e)"), C("input-field.check_color", "rgb(7a88cf)"),
        C("label1.color", "rgb(a9b1d6)"), C("input-field.fail_color", "rgb(f7768e)"),
        C("label1.color", "rgb(a9b1d6)"));
}

// ---------------------------------------------------------------- greetd IPC
// 4-byte native-endian length + JSON, both ways.
static int gfd = -1;

static gboolean rd(void *buf, size_t n) {
    for (size_t got = 0; got < n;) {
        ssize_t r = read(gfd, (char *)buf + got, n - got);
        if (r <= 0) return FALSE;
        got += r;
    }
    return TRUE;
}

static json_object *call(json_object *req) {
    const char *s = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);
    uint32_t len = strlen(s);
    gboolean ok = write(gfd, &len, 4) == 4 && write(gfd, s, len) == (ssize_t)len;
    json_object_put(req);
    if (!ok || !rd(&len, 4) || len > (1 << 20)) return NULL;
    char *buf = g_malloc(len + 1);
    if (!rd(buf, len)) { g_free(buf); return NULL; }
    buf[len] = 0;
    json_object *r = json_tokener_parse(buf);
    g_free(buf);
    return r;
}

static json_object *req(const char *type) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "type", json_object_new_string(type));
    return o;
}

static const char *str(json_object *o, const char *k) {
    json_object *v;
    return o && json_object_object_get_ex(o, k, &v) ? json_object_get_string(v) : "";
}

// NULL once the session is queued to start, else an error message (g_free it)
static char *greetd_login(const char *user, const char *password) {
    if (test_mode) { g_usleep(500000); return strcmp(password, "test") ? g_strdup("Wrong password") : NULL; }
    if (gfd < 0) {
        const char *path = g_getenv("GREETD_SOCK");
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        if (!path) return g_strdup("GREETD_SOCK not set");
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        gfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (gfd < 0 || connect(gfd, (struct sockaddr *)&a, sizeof a) < 0) {
            if (gfd >= 0) close(gfd);
            gfd = -1;
            return g_strdup("Cannot reach greetd");
        }
    }
    json_object *q = req("create_session");
    json_object_object_add(q, "username", json_object_new_string(user));
    json_object *r = call(q);
    gboolean sent = FALSE;
    char *err = NULL;
    while (r && !strcmp(str(r, "type"), "auth_message")) {
        const char *t = str(r, "auth_message_type");
        q = req("post_auth_message_response");
        if (!strcmp(t, "secret") || !strcmp(t, "visible")) {
            if (sent) {   // a second prompt (expired password, OTP...) we can't answer here
                err = g_strstrip(g_strdup(str(r, "auth_message")));
                json_object_put(q);
                json_object_put(r);
                r = NULL;
                break;
            }
            json_object_object_add(q, "response", json_object_new_string(password));
            sent = TRUE;
        }
        json_object_put(r);
        r = call(q);
    }
    if (r && !strcmp(str(r, "type"), "success")) {
        json_object_put(r);
        q = req("start_session");
        json_object *cmd = json_object_new_array(), *env = json_object_new_array();
        json_object_array_add(cmd, json_object_new_string("start-hyprland"));
        json_object_array_add(env, json_object_new_string("XDG_SESSION_TYPE=wayland"));
        json_object_array_add(env, json_object_new_string("XDG_SESSION_DESKTOP=Hyprland"));
        json_object_array_add(env, json_object_new_string("XDG_CURRENT_DESKTOP=Hyprland"));
        json_object_object_add(q, "cmd", cmd);
        json_object_object_add(q, "env", env);
        r = call(q);
        if (r && !strcmp(str(r, "type"), "success")) { json_object_put(r); return NULL; }
    }
    if (!err) {
        if (!r) err = g_strdup("greetd closed the connection");
        else if (!strcmp(str(r, "error_type"), "auth_error")) err = g_strdup("Wrong password");
        else err = g_strdup(*str(r, "description") ? str(r, "description") : "Login failed");
    }
    if (r) json_object_put(r);
    json_object *c = call(req("cancel_session"));
    if (c) json_object_put(c);
    else if (gfd >= 0) { close(gfd); gfd = -1; }
    return err;
}

// ---------------------------------------------------------------- users
static void load_users(void) {
    GPtrArray *u = g_ptr_array_new(), *n = g_ptr_array_new();
    struct passwd *p;
    setpwent();
    while ((p = getpwent()))
        if (p->pw_uid >= 1000 && p->pw_uid < 60000 && !g_str_has_suffix(p->pw_shell, "nologin")
            && !g_str_has_suffix(p->pw_shell, "false")) {
            char *full = g_strdup(p->pw_gecos ? p->pw_gecos : "");
            char *comma = strchr(full, ',');
            if (comma) *comma = 0;
            g_ptr_array_add(u, g_strdup(p->pw_name));
            g_ptr_array_add(n, *full ? full : g_strdup(p->pw_name));
            if (!*full) g_free(full);
        }
    endpwent();
    if (!u->len) { g_ptr_array_add(u, g_strdup("root")); g_ptr_array_add(n, g_strdup("root")); }
    n_users = u->len;
    g_ptr_array_add(u, NULL);
    g_ptr_array_add(n, NULL);
    users = (char **)g_ptr_array_free(u, FALSE);
    names = (char **)g_ptr_array_free(n, FALSE);

    char *last = NULL;
    if (g_file_get_contents(STATE, &last, NULL, NULL)) {
        g_strstrip(last);
        for (int i = 0; i < n_users; i++)
            if (!strcmp(users[i], last)) cur = i;
        g_free(last);
    }
}

// ---------------------------------------------------------------- UI
static gboolean tick(gpointer _) {
    GDateTime *now = g_date_time_new_now_local();
    char *t = g_date_time_format(now, "%H:%M"), *d = g_date_time_format(now, "%A, %d %B");
    gtk_label_set_label(GTK_LABEL(clock_lbl), t);
    gtk_label_set_label(GTK_LABEL(date_lbl), d);
    g_free(t); g_free(d);
    g_date_time_unref(now);
    GdkDevice *kb = gdk_seat_get_keyboard(gdk_display_get_default_seat(gdk_display_get_default()));
    if (kb && gdk_device_get_caps_lock_state(kb)) gtk_widget_add_css_class(entry, "caps");
    else gtk_widget_remove_css_class(entry, "caps");
    return G_SOURCE_CONTINUE;
}

static void show_user(void) {
    char *s = g_strdup_printf("󰌾   %s", names[cur]);
    gtk_label_set_label(GTK_LABEL(user_lbl), s);
    g_free(s);
}

static void switch_user(int step) {
    if (busy || n_users < 2) return;
    cur = (cur + step + n_users) % n_users;
    show_user();
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    gtk_widget_grab_focus(entry);
}
static void on_prev(GtkButton *b, gpointer _) { switch_user(-1); }
static void on_next(GtkButton *b, gpointer _) { switch_user(1); }

typedef struct { char *user, *password, *err; } job;

static gboolean done(gpointer data) {
    job *j = data;
    if (!j->err) {
        if (!test_mode) g_file_set_contents(STATE, j->user, -1, NULL);
        g_application_quit(g_application_get_default());   // greetd starts the session once we exit
    } else {
        busy = FALSE;
        gtk_widget_set_sensitive(entry, TRUE);
        gtk_widget_add_css_class(entry, "fail");
        g_object_set(entry, "placeholder-text", j->err, NULL);
        gtk_widget_grab_focus(entry);
    }
    g_free(j->user); g_free(j->err); g_free(j);
    return G_SOURCE_REMOVE;
}

static gpointer work(gpointer data) {
    job *j = data;
    j->err = greetd_login(j->user, j->password);
    memset(j->password, 0, strlen(j->password));
    g_free(j->password);
    g_idle_add(done, j);
    return NULL;
}

static void submit(GtkPasswordEntry *e, gpointer _) {
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (busy || !*pw) return;
    busy = TRUE;
    job *j = g_new0(job, 1);
    j->user = g_strdup(users[cur]);
    j->password = g_strdup(pw);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    gtk_widget_set_sensitive(entry, FALSE);
    g_object_set(entry, "placeholder-text", "Logging in…", NULL);
    g_thread_unref(g_thread_new("login", work, j));
}

static void on_changed(GtkEditable *e, gpointer _) {
    if (gtk_widget_has_css_class(entry, "fail")) {
        gtk_widget_remove_css_class(entry, "fail");
        g_object_set(entry, "placeholder-text", "Password…", NULL);
    }
}

static gboolean on_key(GtkEventControllerKey *c, guint key, guint code, GdkModifierType st, gpointer app) {
    if (key == GDK_KEY_Up || key == GDK_KEY_Page_Up) { switch_user(-1); return TRUE; }
    if (key == GDK_KEY_Down || key == GDK_KEY_Page_Down) { switch_user(1); return TRUE; }
    if (test_mode && key == GDK_KEY_Escape) { g_application_quit(app); return TRUE; }
    return FALSE;
}

static GtkWidget *build_ui(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    clock_lbl = gtk_label_new(NULL);
    date_lbl = gtk_label_new(NULL);
    gtk_widget_add_css_class(clock_lbl, "clock");
    gtk_widget_add_css_class(date_lbl, "date");
    gtk_box_append(GTK_BOX(box), clock_lbl);
    gtk_box_append(GTK_BOX(box), date_lbl);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(row, "user-row");
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    prev_btn = gtk_button_new_with_label("󰅁");
    next_btn = gtk_button_new_with_label("󰅂");
    user_lbl = gtk_label_new(NULL);
    gtk_widget_add_css_class(user_lbl, "user");
    for (int i = 0; i < 2; i++) {
        GtkWidget *b = i ? next_btn : prev_btn;
        gtk_widget_add_css_class(b, "arrow");
        gtk_widget_set_focusable(b, FALSE);
        gtk_widget_set_visible(b, n_users > 1);
    }
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_prev), NULL);
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_next), NULL);
    gtk_box_append(GTK_BOX(row), prev_btn);
    gtk_box_append(GTK_BOX(row), user_lbl);
    gtk_box_append(GTK_BOX(row), next_btn);
    gtk_box_append(GTK_BOX(card), row);

    entry = gtk_password_entry_new();
    gtk_widget_add_css_class(entry, "password");
    gtk_widget_set_halign(entry, GTK_ALIGN_CENTER);
    g_object_set(entry, "placeholder-text", "Password…", NULL);
    g_signal_connect(entry, "activate", G_CALLBACK(submit), NULL);
    g_signal_connect(entry, "changed", G_CALLBACK(on_changed), NULL);
    gtk_box_append(GTK_BOX(card), entry);
    gtk_box_append(GTK_BOX(box), card);
    show_user();
    return box;
}

static void activate(GtkApplication *app, gpointer _) {
    char *conf = test_mode && g_getenv("GREETER_CONF") ? g_strdup(g_getenv("GREETER_CONF"))
                                                       : g_strdup(SHARE "/hyprlock.conf");
    read_conf(conf);
    g_free(conf);
    char *css = build_css();
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_free(css);

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_child(GTK_WINDOW(win), build_ui());
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), app);
    gtk_widget_add_controller(win, keys);
    if (test_mode) gtk_window_set_default_size(GTK_WINDOW(win), 1280, 720);
    else gtk_window_fullscreen(GTK_WINDOW(win));
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(entry);
    tick(NULL);
    g_timeout_add_seconds(1, tick, NULL);
}

int main(int argc, char **argv) {
    test_mode = argc > 1 && !strcmp(argv[1], "--test");
    load_users();
    GtkApplication *app = gtk_application_new("dev.hyprminimal.greeter", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int st = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return st;
}
