// Settings → Network: everyday NetworkManager jobs without nm-connection-editor.
// Airplane mode · Wi-Fi (current network with password/details, nearby, saved) · Ethernet (every wired adapter,
// incl. USB tethering) · Hotspot (name, password, band, connected devices) · VPN & other connections (on/off,
// import, remove). Every connection has an Advanced page, opened inside this page: name, priority, metered,
// IPv4/IPv6 (automatic, manual addresses, gateway, DNS), MTU and MAC address.
// Every nmcli call runs in a worker thread (nm.c), so the window never stutters.

static const char *SIGNAL_ICONS[] = {"network-wireless-signal-none-symbolic", "network-wireless-signal-weak-symbolic",
                                     "network-wireless-signal-ok-symbolic", "network-wireless-signal-good-symbolic",
                                     "network-wireless-signal-excellent-symbolic"};

typedef struct { char *uuid, *name, *type, *dev; gboolean active, alongside, remove; } ConnRef;   // outlives a reload

static ConnRef *cref(const NmConn *c) {
    ConnRef *r = g_new0(ConnRef, 1);
    r->uuid = g_strdup(c->uuid); r->name = g_strdup(c->name); r->type = g_strdup(c->type);
    r->dev = g_strdup(c->dev); r->active = c->active; r->alongside = c->alongside;
    return r;
}
static void cref_free(gpointer p) {
    ConnRef *r = p;
    if (!r) return;
    g_free(r->uuid); g_free(r->name); g_free(r->type); g_free(r->dev);
    g_free(r);
}

static struct {
    GtkWidget *stack, *page, *adv;
    GPtrArray *spin_btns;                  // refresh/scan: animated dots while nmcli works
    guint dots, dot_step;
    gboolean busy, again, saved_all, nearby_all, built;
    NmState *d;                            // what the page shows now
    char *hs_ssid, *hs_psk, *hs_band;      // hotspot settings before it was ever started
} NP;

static void np_reload(gboolean rescan);
static void np_open_advanced(ConnRef *c);

// ---------------------------------------------------------------- jobs: nmcli steps in a worker thread
typedef struct { char **argv; int timeout; char *input; gboolean helper; } Step;   // helper: argv after pkexec helper
typedef struct {
    GPtrArray *steps;
    gboolean (*fn)(gpointer data, char **err);   // or a custom job
    gpointer data;
    GDestroyNotify free;
    char *ok_msg;
} Job;

static void job_free(gpointer p) {
    Job *j = p;
    for (guint i = 0; j->steps && i < j->steps->len; i++) {
        Step *s = g_ptr_array_index(j->steps, i);
        g_strfreev(s->argv);
        g_free(s->input);
        g_free(s);
    }
    if (j->steps) g_ptr_array_free(j->steps, TRUE);
    if (j->free) j->free(j->data);
    g_free(j->ok_msg);
    g_free(j);
}

static Job *job_new(const char *ok_msg) {
    Job *j = g_new0(Job, 1);
    j->steps = g_ptr_array_new();
    j->ok_msg = g_strdup(ok_msg);
    return j;
}

static void job_nmcli(Job *j, int timeout, const char *first, ...) {   // add `nmcli ARGS…`
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, g_strdup("nmcli"));
    va_list ap;
    va_start(ap, first);
    for (const char *s = first; s; s = va_arg(ap, const char *)) g_ptr_array_add(a, g_strdup(s));
    va_end(ap);
    g_ptr_array_add(a, NULL);
    Step *s = g_new0(Step, 1);
    s->argv = (char **)g_ptr_array_free(a, FALSE);
    s->timeout = timeout;
    g_ptr_array_add(j->steps, s);
}

static void job_nmcli_v(Job *j, int timeout, char **args) {   // takes args (NULL-terminated, without "nmcli")
    int n = g_strv_length(args);
    char **v = g_new0(char *, n + 2);
    v[0] = g_strdup("nmcli");
    for (int i = 0; i < n; i++) v[i + 1] = args[i];
    g_free(args);
    Step *s = g_new0(Step, 1);
    s->argv = v;
    s->timeout = timeout;
    g_ptr_array_add(j->steps, s);
}

static void job_helper_start(Job *j, const char *dev, const char *ssid, const char *psk) {
    Step *s = g_new0(Step, 1);
    s->argv = g_strdupv((char *[]){"start", (char *)dev, NULL});
    s->input = g_strdup_printf("%s\n%s\n", ssid, psk);
    s->helper = TRUE;
    g_ptr_array_add(j->steps, s);
}

static void job_work(GTask *t, gpointer src, gpointer d, GCancellable *c) {
    (void)src; (void)c;
    Job *j = d;
    char *err = NULL;
    gboolean ok = TRUE;
    if (j->fn) ok = j->fn(j->data, &err);
    for (guint i = 0; ok && j->steps && i < j->steps->len; i++) {
        Step *s = g_ptr_array_index(j->steps, i);
        if (s->helper) ok = nm_iface_helper(s->argv[0], s->argv[1], s->input, &err);
        else {
            char *e = NULL;
            ok = nm_run(s->timeout, s->argv, NULL, NULL, &e);
            if (!ok) err = nm_err_text(e);
            g_free(e);
        }
    }
    g_task_return_pointer(t, ok ? NULL : (err ? err : g_strdup("failed")), g_free);
}

static gboolean np_reload_later(gpointer d) { (void)d; np_reload(FALSE); return G_SOURCE_REMOVE; }

static void job_done(GObject *src, GAsyncResult *res, gpointer d) {
    (void)src;
    Job *j = d;
    char *err = g_task_propagate_pointer(G_TASK(res), NULL);
    char *t = err ? g_strdup_printf("Failed: %s", err) : g_strdup(j->ok_msg ? j->ok_msg : "Done");
    settings_toast(t, 4);
    g_free(t);
    g_free(err);
    job_free(j);
    g_timeout_add(600, np_reload_later, NULL);
}

// toast msg, run the job, toast the outcome, reload the page
static void np_act(const char *msg, Job *j) {
    settings_toast(msg, 30);
    GTask *t = g_task_new(NULL, NULL, job_done, j);
    g_task_set_task_data(t, j, NULL);
    g_task_run_in_thread(t, job_work);
    g_object_unref(t);
}

// ---------------------------------------------------------------- loading
static void np_set_busy(gboolean on);

static void fetch_work(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)c;
    g_task_return_pointer(t, nm_fetch(GPOINTER_TO_INT(d)), (GDestroyNotify)nm_state_free);
}

static void np_render(NmState *d);

static void fetch_done(GObject *src, GAsyncResult *res, gpointer d) {
    (void)src; (void)d;
    NmState *st = g_task_propagate_pointer(G_TASK(res), NULL);
    NP.busy = FALSE;
    np_set_busy(FALSE);
    if (st) np_render(st);
    if (NP.again) { NP.again = FALSE; np_reload(FALSE); }
}

static void np_reload(gboolean rescan) {
    if (!NP.page) return;
    if (NP.busy) { NP.again = TRUE; return; }
    NP.busy = TRUE;
    np_set_busy(TRUE);
    GTask *t = g_task_new(NULL, NULL, fetch_done, NULL);
    g_task_set_task_data(t, GINT_TO_POINTER(rescan), NULL);
    g_task_run_in_thread(t, fetch_work);
    g_object_unref(t);
}

static gboolean dots_tick(gpointer d) {
    (void)d;
    static const char *frames[] = {"• · ·", "· • ·", "· · •"};
    for (guint i = 0; i < NP.spin_btns->len; i++)
        gtk_button_set_label(GTK_BUTTON(g_ptr_array_index(NP.spin_btns, i)), frames[NP.dot_step % 3]);
    NP.dot_step++;
    return G_SOURCE_CONTINUE;
}

static void np_set_busy(gboolean on) {
    if (NP.dots) { g_source_remove(NP.dots); NP.dots = 0; }
    for (guint i = 0; i < NP.spin_btns->len; i++) {
        GtkWidget *b = g_ptr_array_index(NP.spin_btns, i);
        gtk_widget_set_sensitive(b, !on);
        if (!on) gtk_button_set_icon_name(GTK_BUTTON(b), "view-refresh-symbolic");
    }
    if (on && NP.spin_btns->len) { dots_tick(NULL); NP.dots = g_timeout_add(280, dots_tick, NULL); }
}

static void refresh_clicked(GtkButton *b, gpointer rescan) { (void)b; np_reload(GPOINTER_TO_INT(rescan)); }

static GtkWidget *spin_button(gboolean rescan, const char *tip) {
    GtkWidget *b = icon_btn("view-refresh-symbolic", tip);
    gtk_widget_add_css_class(b, "hm-scan");
    g_signal_connect(b, "clicked", G_CALLBACK(refresh_clicked), GINT_TO_POINTER(rescan));
    g_ptr_array_add(NP.spin_btns, b);
    return b;
}

// ---------------------------------------------------------------- small actions
static void on_airplane(GtkSwitch *sw, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    gboolean on = gtk_switch_get_active(sw);
    Job *j = job_new(NULL);
    job_nmcli(j, 15, "radio", "all", on ? "off" : "on", NULL);
    np_act(on ? "Airplane mode on…" : "Airplane mode off…", j);
}

static void on_radio(GtkSwitch *sw, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    gboolean on = gtk_switch_get_active(sw);
    Job *j = job_new(NULL);
    job_nmcli(j, 15, "radio", "wifi", on ? "on" : "off", NULL);
    np_act(on ? "Turning Wi-Fi on…" : "Turning Wi-Fi off…", j);
}

static void conn_down(GtkButton *b, gpointer uuid) {
    (void)b;
    Job *j = job_new(NULL);
    job_nmcli(j, 15, "connection", "down", (char *)uuid, NULL);
    np_act("Disconnecting…", j);
}

static void dev_disconnect(GtkButton *b, gpointer dev) {
    (void)b;
    Job *j = job_new(NULL);
    job_nmcli(j, 15, "device", "disconnect", (char *)dev, NULL);
    np_act("Disconnecting…", j);
}

static void dev_connect(GtkButton *b, gpointer dev) {
    (void)b;
    Job *j = job_new("Connected");
    job_nmcli(j, 40, "device", "connect", (char *)dev, NULL);
    np_act("Connecting…", j);
}

static GtkWidget *small_btn(const char *label, GCallback cb, char *data_owned) {
    GtkWidget *b = text_btn(label, "hm-btn flat");
    g_signal_connect_data(b, "clicked", cb, data_owned, (GClosureNotify)(void (*)(void))g_free, 0);
    return b;
}

static void details_rows(GtkWidget *list, GPtrArray *det) {   // IP, gateway, DNS, MAC with copy buttons
    for (guint i = 0; det && i + 1 < det->len; i += 2) {
        GtkWidget *r = row_add(list, g_ptr_array_index(det, i), g_ptr_array_index(det, i + 1));
        gtk_widget_add_css_class(r, "property");
        row_sub_selectable(r);
        row_suffix(r, copy_button(g_ptr_array_index(det, i + 1), g_ptr_array_index(det, i)));
    }
}

typedef struct { ConnRef *c; char *psk; } PwData;
static void pw_apply(GtkWidget *e, gpointer d) {
    ConnRef *c = d;
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(e));
    if (g_utf8_strlen(pw, -1) < 8) { settings_toast("Wi-Fi passwords are at least 8 characters", 3); return; }
    Job *j = job_new("Password saved");
    job_nmcli(j, 15, "connection", "modify", c->uuid, "802-11-wireless-security.psk", pw, NULL);
    if (c->active) job_nmcli(j, 40, "connection", "up", c->uuid, NULL);   // reconnect so the new password is used now
    np_act("Saving password…", j);
}

// saved password: hidden (peek icon shows it), copy button, Enter / ✓ saves a new one
static void password_row(GtkWidget *list, const NmConn *c) {
    GtkWidget *row;
    ConnRef *r = cref(c);
    GtkWidget *e = entry_row_full(list, "Password", c->psk, TRUE, pw_apply, r, &row);
    g_object_set_data_full(G_OBJECT(e), "hm-cref", r, cref_free);
    row_suffix(row, copy_button(c->psk, "Password"));
}

static void auto_switched(GtkSwitch *sw, GParamSpec *ps, gpointer uuid) {
    (void)ps;
    Job *j = job_new("Saved");
    job_nmcli(j, 15, "connection", "modify", (char *)uuid, "connection.autoconnect", gtk_switch_get_active(sw) ? "yes" : "no", NULL);
    np_act("Saving…", j);
}

static void auto_row(GtkWidget *list, const NmConn *c) {
    GtkWidget *sw = switch_row(list, "Connect automatically", NULL, c->autoconnect);
    g_signal_connect_data(sw, "notify::active", G_CALLBACK(auto_switched), g_strdup(c->uuid), (GClosureNotify)(void (*)(void))g_free, 0);
}

static void advanced_activate(GtkWidget *w, gpointer c) { (void)w; np_open_advanced(c); }

static void advanced_row(GtkWidget *list, const NmConn *c) {
    GtkWidget *r = row_add(list, "Advanced", "IP address, DNS, MTU, MAC, metered…");
    GtkWidget *go = gtk_image_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(go, "dim");
    row_suffix(r, go);
    ConnRef *ref = cref(c);
    g_object_set_data_full(G_OBJECT(r), "hm-cref", ref, cref_free);
    row_on_activate(r, advanced_activate, ref);
}

static void do_forget(gpointer d) {
    ConnRef *c = d;
    Job *j = job_new("Done");
    job_nmcli(j, 15, "connection", "delete", c->uuid, NULL);
    char *m = g_strdup_printf("%s %s…", c->remove ? "Removing" : "Forgetting", c->name);
    np_act(m, j);
    g_free(m);
}

static void confirm_forget(ConnRef *c, gboolean remove) {
    char *h = g_strdup_printf("%s %s?", remove ? "Remove" : "Forget", c->name);
    ConnRef *copy = g_new0(ConnRef, 1);
    copy->uuid = g_strdup(c->uuid); copy->name = g_strdup(c->name);
    copy->remove = remove;
    confirm(h, remove ? "Its settings and keys are deleted." : "Its saved password is deleted. You'll need it again to reconnect.",
            remove ? "Remove" : "Forget", do_forget, copy, cref_free);
    g_free(h);
}

static void forget_activate(GtkWidget *w, gpointer c) { (void)w; confirm_forget(c, FALSE); }

static void forget_row(GtkWidget *list, const NmConn *c) {
    ConnRef *ref = cref(c);
    GtkWidget *r = button_row(list, "Forget this network", "destructive", forget_activate, ref);
    g_object_set_data_full(G_OBJECT(r), "hm-cref", ref, cref_free);
}

// ---------------------------------------------------------------- Wi-Fi
static void join_apply(GtkWidget *e, gpointer ssid) {   // unknown secured network: password typed
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(e));
    if (!*pw) return;
    char *ok = g_strdup_printf("Connected to %s", (char *)ssid), *m = g_strdup_printf("Connecting to %s…", (char *)ssid);
    Job *j = job_new(ok);
    job_nmcli(j, 40, "device", "wifi", "connect", (char *)ssid, "password", pw, NULL);
    np_act(m, j);
    g_free(ok); g_free(m);
}

typedef struct { char *ssid, *uuid; } Join;
static void join_free(gpointer p) { Join *j = p; g_free(j->ssid); g_free(j->uuid); g_free(j); }
static void join_activate(GtkWidget *w, gpointer d) {   // saved or open network
    (void)w;
    Join *n = d;
    char *ok = g_strdup_printf("Connected to %s", n->ssid), *m = g_strdup_printf("Connecting to %s…", n->ssid);
    Job *j = job_new(ok);
    if (n->uuid) job_nmcli(j, 40, "connection", "up", n->uuid, NULL);
    else job_nmcli(j, 40, "device", "wifi", "connect", n->ssid, NULL);
    np_act(m, j);
    g_free(ok); g_free(m);
}

static GtkWidget *signal_icon(int signal) { return gtk_image_new_from_icon_name(SIGNAL_ICONS[MIN(4, MAX(0, signal) / 20)]); }

static GtkWidget *nearby_row(GtkWidget *list, NmAp *n, NmConn *saved) {
    GtkWidget *out;
    gboolean secured = *n->sec && strcmp(n->sec, "--");
    GString *sub = g_string_new(n->band);
    g_string_append_printf(sub, "%s%s", sub->len ? " · " : "", secured ? n->sec : "Open");
    if (saved) g_string_append(sub, " · Saved");
    if (saved || !secured) {
        GtkWidget *r = row_add(list, n->ssid, sub->str);
        row_prefix(r, signal_icon(n->signal));
        GtkWidget *go = gtk_image_new_from_icon_name("go-next-symbolic");
        gtk_widget_add_css_class(go, "dim");
        row_suffix(r, go);
        Join *j = g_new0(Join, 1);
        j->ssid = g_strdup(n->ssid);
        j->uuid = saved ? g_strdup(saved->uuid) : NULL;
        g_object_set_data_full(G_OBJECT(r), "hm-join", j, join_free);
        row_on_activate(r, join_activate, j);
        out = r;
    } else {
        GtkWidget *x = expander_row(list, n->ssid, sub->str, TRUE);
        row_prefix(x, signal_icon(n->signal));
        char *ssid = g_strdup(n->ssid);
        GtkWidget *e = entry_row_full(expander_list(x), "Password", "", TRUE, join_apply, ssid, NULL);
        g_object_set_data_full(G_OBJECT(e), "hm-ssid", ssid, g_free);
        out = x;
    }
    g_string_free(sub, TRUE);
    return out;
}

static void hidden_go(GtkWidget *e, gpointer name) {
    char *ssid = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(name))));
    if (!*ssid) { settings_toast("Type the network name", 3); g_free(ssid); return; }
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(e));
    char *ok = g_strdup_printf("Connected to %s", ssid), *m = g_strdup_printf("Connecting to %s…", ssid);
    Job *j = job_new(ok);
    if (*pw) job_nmcli(j, 40, "device", "wifi", "connect", ssid, "hidden", "yes", "password", pw, NULL);
    else job_nmcli(j, 40, "device", "wifi", "connect", ssid, "hidden", "yes", NULL);
    np_act(m, j);
    g_free(ok); g_free(m); g_free(ssid);
}

static void hidden_row(GtkWidget *list) {
    GtkWidget *x = expander_row(list, "Hidden network…", "Join a network that doesn't broadcast its name", TRUE);
    GtkWidget *name = entry_row(expander_list(x), "Network name", "", NULL, NULL);
    entry_row_full(expander_list(x), "Password (empty if open)", "", TRUE, hidden_go, name, NULL);
}

typedef struct { GtkWidget *row; ConnRef *c; gboolean loaded; } SavedRow;
static void saved_free(gpointer p) { SavedRow *s = p; cref_free(s->c); g_free(s); }

static void saved_psk_done(GObject *src, GAsyncResult *res, gpointer d) {
    (void)src;
    GtkWidget *row = d;
    char *psk = g_task_propagate_pointer(G_TASK(res), NULL);
    SavedRow *s = g_object_get_data(G_OBJECT(row), "hm-saved");
    if (s && gtk_widget_get_parent(row)) {   // still on the page
        GtkWidget *inner = expander_list(row);
        NmConn c = {.uuid = s->c->uuid, .name = s->c->name, .type = s->c->type, .dev = s->c->dev, .psk = psk ? psk : "",
                    .active = s->c->active};
        for (guint i = 0; NP.d && i < NP.d->saved->len; i++) {
            NmConn *x = g_ptr_array_index(NP.d->saved, i);
            if (!strcmp(x->uuid, c.uuid)) c.autoconnect = x->autoconnect;
        }
        if (psk && *psk) password_row(inner, &c);
        auto_row(inner, &c);
        advanced_row(inner, &c);
        forget_row(inner, &c);
    }
    g_free(psk);
    g_object_unref(row);
}

static void psk_work(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)c;
    g_task_return_pointer(t, nm_out("-s", "-g", "802-11-wireless-security.psk", "connection", "show", (char *)d, NULL), g_free);
}

static void saved_opened(GtkWidget *row, gpointer d) {
    SavedRow *s = d;
    if (s->loaded) return;
    s->loaded = TRUE;
    GTask *t = g_task_new(NULL, NULL, saved_psk_done, g_object_ref(row));
    g_task_set_task_data(t, g_strdup(s->c->uuid), g_free);
    g_task_run_in_thread(t, psk_work);
    g_object_unref(t);
}

// "Show all (N)" / "Show less" over the rows in extra; *all (NP.saved_all, NP.nearby_all) survives reloads
static void more_toggle(GtkButton *b, gpointer all) {
    gboolean *on = all;
    *on = !*on;
    GPtrArray *extra = g_object_get_data(G_OBJECT(b), "hm-extra");
    for (guint i = 0; i < extra->len; i++) gtk_widget_set_visible(g_ptr_array_index(extra, i), *on);
    char *l = *on ? g_strdup("Show less") : g_strdup_printf("Show all (%d)", GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "hm-n")));
    gtk_button_set_label(b, l);
    g_free(l);
}

// rows past the first few start hidden (unless *all); the group gets the toggle. Takes extra.
static void more_button(GtkWidget *group, GPtrArray *extra, int total, gboolean *all) {
    if (!extra->len) { g_ptr_array_unref(extra); return; }
    for (guint i = 0; i < extra->len; i++) gtk_widget_set_visible(g_ptr_array_index(extra, i), *all);
    char *l = *all ? g_strdup("Show less") : g_strdup_printf("Show all (%d)", total);
    GtkWidget *more = text_btn(l, "hm-btn flat");
    g_free(l);
    g_object_set_data_full(G_OBJECT(more), "hm-extra", extra, (GDestroyNotify)g_ptr_array_unref);
    g_object_set_data(G_OBJECT(more), "hm-n", GINT_TO_POINTER(total));
    g_signal_connect(more, "clicked", G_CALLBACK(more_toggle), all);
    group_suffix(group, more);
}

static void render_wifi(NmState *d) {
    GtkWidget *g = group_new(NP.page, "Wi-Fi", NULL);
    GtkWidget *radio = switch_row(g, "Wi-Fi", d->wdev ? d->wdev : "No Wi-Fi adapter", d->radio);
    gtk_widget_set_sensitive(row_of(radio), d->wdev != NULL);
    g_signal_connect(radio, "notify::active", G_CALLBACK(on_radio), NULL);
    if (!d->radio || !d->wdev) return;
    if (d->hotspot && d->hotspot->active) { dim_row(g, "Wi-Fi is sharing a hotspot", "Turn the hotspot off to join networks"); return; }
    NmConn *a = d->active;
    if (a) {
        NmAp *here = NULL;
        for (guint i = 0; i < d->nearby->len && !here; i++) {
            NmAp *n = g_ptr_array_index(d->nearby, i);
            if (!strcmp(n->ssid, a->ssid) || n->inuse) here = n;
        }
        GString *sub = g_string_new(NULL);
        if (here) {
            g_string_append_printf(sub, "%d%%", here->signal);
            if (*here->band) g_string_append_printf(sub, " · %s", here->band);
            if (*here->sec) g_string_append_printf(sub, " · %s", here->sec);
        }
        GtkWidget *x = expander_row(g, a->name, sub->len ? sub->str : "Connected", TRUE);
        g_string_free(sub, TRUE);
        row_prefix(x, signal_icon(here ? here->signal : 0));
        row_suffix(x, small_btn("Disconnect", G_CALLBACK(conn_down), g_strdup(a->uuid)));
        GtkWidget *in = expander_list(x);
        if (*a->psk) password_row(in, a);
        details_rows(in, a->details);
        auto_row(in, a);
        advanced_row(in, a);
        forget_row(in, a);
    }
    // the 5 strongest; the rest behind "Show all"
    g = group_new(NP.page, "Nearby networks", NULL);
    GPtrArray *more = g_ptr_array_new();
    int shown = 0;
    for (guint i = 0; i < d->nearby->len; i++) {
        NmAp *n = g_ptr_array_index(d->nearby, i);
        if (a && !strcmp(n->ssid, a->ssid)) continue;
        NmConn *known = NULL;
        for (guint k = 0; k < d->saved->len && !known; k++) {
            NmConn *c = g_ptr_array_index(d->saved, k);
            if (!strcmp(c->ssid, n->ssid)) known = c;
        }
        GtkWidget *r = nearby_row(g, n, known);
        if (shown++ >= NM_NEARBY_SHOWN) g_ptr_array_add(more, r);
    }
    if (!shown) dim_row(g, "No other networks found", NULL);
    hidden_row(g);
    more_button(g, more, shown, &NP.nearby_all);
    group_suffix(g, spin_button(TRUE, "Scan again"));

    GPtrArray *saved = g_ptr_array_new();   // not connected, not in range
    for (guint i = 0; i < d->saved->len; i++) {
        NmConn *c = g_ptr_array_index(d->saved, i);
        gboolean in_range = FALSE;
        for (guint k = 0; k < d->nearby->len && !in_range; k++) in_range = !strcmp(((NmAp *)g_ptr_array_index(d->nearby, k))->ssid, c->ssid);
        if (!c->active && !in_range) g_ptr_array_add(saved, c);
    }
    if (saved->len) {
        g = group_new(NP.page, "Saved networks", "Not in range right now");
        GPtrArray *extra = g_ptr_array_new();
        for (guint i = 0; i < saved->len; i++) {
            NmConn *c = g_ptr_array_index(saved, i);
            char *ago = nm_ago(c->ts);
            GtkWidget *x = expander_row(g, c->name, ago, TRUE);
            g_free(ago);
            SavedRow *s = g_new0(SavedRow, 1);
            s->c = cref(c);
            g_object_set_data_full(G_OBJECT(x), "hm-saved", s, saved_free);
            expander_on_open(x, saved_opened, s);
            if (i >= NM_SAVED_SHOWN) g_ptr_array_add(extra, x);
        }
        more_button(g, extra, saved->len, &NP.saved_all);   // only the most recent few by default
    }
    g_ptr_array_free(saved, TRUE);
}

// ---------------------------------------------------------------- Ethernet
static void render_ethernet(NmState *d) {
    GtkWidget *g = group_new(NP.page, "Ethernet", NULL);
    if (!d->eth->len) {
        dim_row(g, "No wired adapter", "Plug in a cable, a USB adapter or a phone with USB tethering");
        return;
    }
    for (guint i = 0; i < d->eth->len; i++) {
        NmDev *x = g_ptr_array_index(d->eth, i);
        const char *st = x->state;
        char *sub;
        if (!strcmp(st, "connected")) {
            GString *s = g_string_new("Connected");
            if (x->speed) g_string_append_printf(s, " · %d Mb/s", x->speed);
            if (x->profile) g_string_append_printf(s, " · %s", x->profile->name);
            sub = g_string_free(s, FALSE);
        } else sub = g_strdup(!strcmp(st, "unavailable") ? "Cable unplugged" : g_str_has_prefix(st, "connecting") ? "Connecting…"
                              : !strcmp(st, "unmanaged") ? "Not managed by NetworkManager" : "Disconnected");
        char *title = g_strdup_printf("%s (%s)", x->usb ? "USB tethering" : "Ethernet", x->dev);
        GtkWidget *row = expander_row(g, title, sub, !strcmp(st, "connected"));
        g_free(title); g_free(sub);
        row_prefix(row, gtk_image_new_from_icon_name("network-wired-symbolic"));
        if (!strcmp(st, "connected") || g_str_has_prefix(st, "connecting"))
            row_suffix(row, small_btn("Disconnect", G_CALLBACK(dev_disconnect), g_strdup(x->dev)));
        else if (!strcmp(st, "disconnected"))
            row_suffix(row, small_btn("Connect", G_CALLBACK(dev_connect), g_strdup(x->dev)));
        GtkWidget *in = expander_list(row);
        details_rows(in, x->details);
        if (x->profile) { auto_row(in, x->profile); advanced_row(in, x->profile); }
    }
}

// ---------------------------------------------------------------- Hotspot
typedef struct { char *ssid, *psk, *band; } HsUp;
static void hsup_free(gpointer p) { HsUp *h = p; g_free(h->ssid); g_free(h->psk); g_free(h->band); g_free(h); }

static gboolean hotspot_up_fn(gpointer data, char **err) {   // fresh state in the worker, then up
    HsUp *h = data;
    NmState *d = nm_base_state();
    gboolean ok = nm_hotspot_up(d, d->hotspot, h->ssid, h->psk, h->band, err);
    nm_state_free(d);
    return ok;
}

static gboolean hotspot_down_fn(gpointer data, char **err) {
    (void)data;
    NmState *d = nm_base_state();
    gboolean ok = !d->hotspot || !d->hotspot->active || nm_hotspot_down(d->hotspot, err);
    nm_state_free(d);
    return ok;
}

static void hs_cur(const char **ssid, const char **psk, const char **band) {
    NmConn *hs = NP.d ? NP.d->hotspot : NULL;
    *ssid = hs ? hs->ssid : NP.hs_ssid;
    *psk = hs ? hs->psk : NP.hs_psk;
    *band = hs ? hs->band : NP.hs_band;
}

static void hs_switched(GtkSwitch *sw, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    Job *j;
    if (gtk_switch_get_active(sw)) {
        const char *ssid, *psk, *band;
        hs_cur(&ssid, &psk, &band);
        HsUp *h = g_new0(HsUp, 1);
        h->ssid = g_strdup(ssid); h->psk = g_strdup(psk); h->band = g_strdup(band);
        j = job_new("Hotspot on");
        j->fn = hotspot_up_fn; j->data = h; j->free = hsup_free;
        np_act("Starting hotspot…", j);
    } else {
        j = job_new("Hotspot off");
        j->fn = hotspot_down_fn;
        np_act("Stopping hotspot…", j);
    }
}

static void hs_save(const char *key, const char *value) {
    if (!strcmp(key, "ssid") && (strlen(value) < 1 || strlen(value) > 32)) { settings_toast("Name must be 1 to 32 characters", 3); return; }
    if (!strcmp(key, "psk") && (strlen(value) < 8 || strlen(value) > 63)) { settings_toast("Password must be 8 to 63 characters", 3); return; }
    NmConn *hs = NP.d ? NP.d->hotspot : NULL;
    if (!hs) {   // nothing saved yet: used the first time the hotspot starts
        char **slot = !strcmp(key, "ssid") ? &NP.hs_ssid : !strcmp(key, "psk") ? &NP.hs_psk : &NP.hs_band;
        g_free(*slot);
        *slot = g_strdup(value);
        settings_toast("Saved", 3);
        return;
    }
    const char *prop = !strcmp(key, "ssid") ? "802-11-wireless.ssid" : !strcmp(key, "psk") ? "802-11-wireless-security.psk"
                                                                                             : "802-11-wireless.band";
    Job *j = job_new("Saved");
    job_nmcli(j, 15, "connection", "modify", hs->uuid, prop, value, NULL);
    if (hs->alongside) {
        const char *ssid = !strcmp(key, "ssid") ? value : hs->ssid, *psk = !strcmp(key, "psk") ? value : hs->psk;
        job_helper_start(j, NP.d->wdev, ssid, psk);
    } else if (hs->active) job_nmcli(j, 40, "connection", "up", hs->uuid, NULL);
    np_act(hs->active ? "Saving… the hotspot restarts" : "Saving…", j);
}

static void hs_name_apply(GtkWidget *e, gpointer d) {
    (void)d;
    char *v = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(e))));
    hs_save("ssid", v);
    g_free(v);
}
static void hs_pw_apply(GtkWidget *e, gpointer d) { (void)d; hs_save("psk", gtk_editable_get_text(GTK_EDITABLE(e))); }
static void hs_band(GtkDropDown *dd, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    guint i = gtk_drop_down_get_selected(dd);
    if (i < G_N_ELEMENTS(NM_BANDS)) hs_save("band", NM_BANDS[i].value);
}
static void hs_gear(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    if (!NP.d || !NP.d->hotspot) return;
    ConnRef *r = cref(NP.d->hotspot);
    np_open_advanced(r);
    cref_free(r);
}

static void render_hotspot(NmState *d) {
    NmConn *hs = d->hotspot;
    gboolean on = hs && hs->active;
    if (!d->ap) {
        GtkWidget *g = group_new(NP.page, "Hotspot", NULL);
        dim_row(g, "Hotspot not supported", "This Wi-Fi adapter can't act as an access point");
        return;
    }
    gboolean alongside = on && hs->alongside;   // running next to the Wi-Fi connection
    const char *extra = on || !d->active ? "" : d->concurrent ? " It shares your Wi-Fi network too, on the same channel."
                                                              : " Your Wi-Fi network disconnects while it's on.";
    char *desc = g_strdup_printf("Share this computer's internet over Wi-Fi.%s", extra);
    GtkWidget *g = group_new(NP.page, "Hotspot", desc);
    g_free(desc);
    const char *ssid, *psk, *band;
    hs_cur(&ssid, &psk, &band);
    char *sub = on ? g_strdup_printf("Sharing as “%s”", ssid) : g_strdup("Off");
    GtkWidget *sw = switch_row(g, "Hotspot", sub, on);
    g_free(sub);
    g_signal_connect(sw, "notify::active", G_CALLBACK(hs_switched), NULL);
    entry_row(g, "Network name", ssid, hs_name_apply, NULL);
    GtkWidget *prow;
    entry_row_full(g, "Password", psk, TRUE, hs_pw_apply, NULL, &prow);
    row_suffix(prow, copy_button(psk, "Password"));
    if (hs) {
        GtkWidget *gear = icon_btn("emblem-system-symbolic", "Advanced (address range, MTU…)");
        g_signal_connect(gear, "clicked", G_CALLBACK(hs_gear), NULL);
        group_suffix(g, gear);
    }
    if (d->five && alongside) dim_row(g, "Band", "Follows your Wi-Fi network while both are on");   // one radio, one channel
    else if (d->five) {
        const char *labels[G_N_ELEMENTS(NM_BANDS) + 1] = {0};
        int sel = 0;
        for (size_t i = 0; i < G_N_ELEMENTS(NM_BANDS); i++) {
            labels[i] = NM_BANDS[i].label;
            if (!strcmp(NM_BANDS[i].value, band)) sel = i;
        }
        GtkWidget *dd = combo_row(g, "Band", "5 GHz is faster, 2.4 GHz reaches further and suits older devices", labels, sel);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(hs_band), NULL);
    }
    if (on) {
        guint n = hs->clients ? hs->clients->len / 2 : 0;
        char *cs = n ? g_strdup_printf("%u", n) : g_strdup("None yet");
        GtkWidget *cl = expander_row(g, "Connected devices", cs, n > 0);
        g_free(cs);
        for (guint i = 0; i < n; i++) {
            GtkWidget *r = row_add(expander_list(cl), g_ptr_array_index(hs->clients, 2 * i), g_ptr_array_index(hs->clients, 2 * i + 1));
            row_sub_selectable(r);
        }
    }
}

// ---------------------------------------------------------------- VPN & other connections
static void vpn_picked(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    char *path = g_file_get_path(f);
    Job *j = job_new("Imported");
    job_nmcli(j, 15, "connection", "import", "type", g_str_has_suffix(path, ".ovpn") ? "openvpn" : "wireguard", "file", path, NULL);
    np_act("Importing…", j);
    g_free(path);
    g_object_unref(f);
}

static void vpn_import(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "VPN configuration");
    gtk_file_filter_add_pattern(f, "*.conf");
    if (g_file_test(NM_OPENVPN, G_FILE_TEST_EXISTS)) gtk_file_filter_add_pattern(f, "*.ovpn");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, f);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Import VPN");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(SW.win), NULL, vpn_picked, NULL);
    g_object_unref(dlg);
    g_object_unref(filters);
    g_object_unref(f);
}

static void other_switched(GtkSwitch *sw, GParamSpec *ps, gpointer d) {
    (void)ps;
    ConnRef *c = d;
    gboolean on = gtk_switch_get_active(sw);
    Job *j = job_new(NULL);
    job_nmcli(j, 60, "connection", on ? "up" : "down", c->uuid, NULL);
    char *m = g_strdup_printf("%s %s…", on ? "Connecting" : "Disconnecting", c->name);
    np_act(m, j);
    g_free(m);
}
static void other_gear(GtkButton *b, gpointer c) { (void)b; np_open_advanced(c); }
static void other_remove(GtkButton *b, gpointer c) { (void)b; confirm_forget(c, TRUE); }

static void render_other(NmState *d) {
    gboolean ovpn = g_file_test(NM_OPENVPN, G_FILE_TEST_EXISTS);
    GtkWidget *g = group_new(NP.page, "VPN & other connections", NULL);
    GtkWidget *imp = icon_btn("list-add-symbolic", ovpn ? "Import a WireGuard (.conf) or OpenVPN (.ovpn) file"
                                                       : "Import a WireGuard (.conf) file");
    g_signal_connect(imp, "clicked", G_CALLBACK(vpn_import), NULL);
    group_suffix(g, imp);
    if (!d->other->len) dim_row(g, "None set up", ovpn ? "Add one with + (WireGuard config or OpenVPN profile)" : "Add one with + (WireGuard config)");
    for (guint i = 0; i < d->other->len; i++) {
        NmConn *c = g_ptr_array_index(d->other, i);
        char *sub = c->active && *c->dev ? g_strdup_printf("%s · %s", nm_other_label(c->type), c->dev) : g_strdup(nm_other_label(c->type));
        GtkWidget *sw = switch_row(g, c->name, sub, c->active), *row = row_of(sw);
        g_free(sub);
        ConnRef *ref = cref(c);
        g_object_set_data_full(G_OBJECT(row), "hm-cref", ref, cref_free);
        g_signal_connect(sw, "notify::active", G_CALLBACK(other_switched), ref);
        GtkWidget *gear = icon_btn("emblem-system-symbolic", "Advanced"), *rm = icon_btn("user-trash-symbolic", "Remove");
        g_signal_connect(gear, "clicked", G_CALLBACK(other_gear), ref);
        g_signal_connect(rm, "clicked", G_CALLBACK(other_remove), ref);
        GtkWidget *suf = g_object_get_data(G_OBJECT(row), "hm-suffix");
        gtk_box_prepend(GTK_BOX(suf), rm);
        gtk_box_prepend(GTK_BOX(suf), gear);
    }
}

// ---------------------------------------------------------------- the page
static void np_render(NmState *d) {
    for (GtkWidget *c; (c = gtk_widget_get_first_child(NP.page));) gtk_box_remove(GTK_BOX(NP.page), c);
    g_ptr_array_set_size(NP.spin_btns, 0);
    nm_state_free(NP.d);
    NP.d = d;
    GtkWidget *g = group_new(NP.page, NULL, NULL);
    GtkWidget *air = switch_row(g, "Airplane mode", "Turn off Wi-Fi and mobile radios", d->airplane);
    g_signal_connect(air, "notify::active", G_CALLBACK(on_airplane), NULL);
    if (!(d->radio && d->wdev) || (d->hotspot && d->hotspot->active)) {
        // no "Nearby networks" section (and its scan button) below: refresh lives up here instead
        GtkWidget *grp = g_object_get_data(G_OBJECT(g), "hm-group"), *head = HBOX(0, NULL);
        GtkWidget *b = spin_button(FALSE, "Refresh");
        gtk_widget_set_halign(head, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(head), b);
        gtk_box_prepend(GTK_BOX(grp), head);
    }
    render_wifi(d);
    render_ethernet(d);
    if (d->wdev) render_hotspot(d);
    render_other(d);
}

static void np_mapped(GtkWidget *w, gpointer d) { (void)w; (void)d; np_reload(FALSE); }

static GtkWidget *page_network(void) {
    if (!NP.spin_btns) NP.spin_btns = g_ptr_array_new();
    char host[256] = "hyprminimal";
    gethostname(host, sizeof host - 1);
    NP.hs_ssid = g_strdup(host);
    NP.hs_psk = nm_new_psk();
    NP.hs_band = g_strdup("");
    GtkWidget *sc = page_new(&NP.page);
    GtkWidget *g = group_new(NP.page, NULL, NULL);
    dim_row(g, "Loading networks…", NULL);
    NP.stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(NP.stack), sc, "main");
    g_signal_connect(sc, "map", G_CALLBACK(np_mapped), NULL);   // fresh every time the page shows
    return NP.stack;
}

// ---------------------------------------------------------------- Advanced (one connection profile)
typedef struct {
    ConnRef *c;
    GHashTable *props, *changed;   // property -> value as read; property -> new value (only real changes)
    char *kind;                    // "802-11-wireless" / "802-3-ethernet" / NULL
} Adv;

static Adv *ADV;

static void adv_free(Adv *a) {
    if (!a) return;
    cref_free(a->c);
    g_hash_table_unref(a->props);
    g_hash_table_unref(a->changed);
    g_free(a->kind);
    g_free(a);
}

static void adv_track(const char *prop, const char *v) {
    const char *orig = g_hash_table_lookup(ADV->props, prop);
    if (!strcmp(v, orig ? orig : "")) g_hash_table_remove(ADV->changed, prop);
    else g_hash_table_insert(ADV->changed, g_strdup(prop), g_strdup(v));
}

static void adv_entry_changed(GtkEditable *e, gpointer prop) {
    char *v = g_strstrip(g_strdup(gtk_editable_get_text(e)));
    adv_track(prop, v);
    g_free(v);
}

static GtkWidget *adv_entry(GtkWidget *list, const char *title, const char *prop, const char *hint) {
    const char *v = g_hash_table_lookup(ADV->props, prop);
    GtkWidget *e = entry_row(list, title, v ? v : "", NULL, NULL);
    if (hint) gtk_widget_set_tooltip_text(e, hint);
    g_signal_connect(e, "changed", G_CALLBACK(adv_entry_changed), (gpointer)g_intern_string(prop));
    return e;
}

typedef struct { const char *prop; GPtrArray *values; } AdvCombo;
static void adv_combo_free(gpointer p) { AdvCombo *c = p; g_ptr_array_unref(c->values); g_free(c); }
static void adv_combo_changed(GtkDropDown *dd, GParamSpec *ps, gpointer d) {
    (void)ps;
    AdvCombo *c = d;
    guint i = gtk_drop_down_get_selected(dd);
    if (i < c->values->len) adv_track(c->prop, g_ptr_array_index(c->values, i));
}

static GtkWidget *adv_combo(GtkWidget *list, const char *title, const char *prop, const NmOpt *opts, size_t n, const char *sub) {
    const char *cur = g_hash_table_lookup(ADV->props, prop);
    if (!cur) cur = "";
    AdvCombo *c = g_new0(AdvCombo, 1);
    c->prop = g_intern_string(prop);
    c->values = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    int sel = -1;
    for (size_t i = 0; i < n; i++) {
        g_ptr_array_add(c->values, g_strdup(opts[i].value));
        g_ptr_array_add(labels, g_strdup(opts[i].label));
        if (!strcmp(opts[i].value, cur)) sel = i;
    }
    if (sel < 0) {   // an explicit value (e.g. a MAC address) we don't list
        sel = c->values->len;
        g_ptr_array_add(c->values, g_strdup(cur));
        g_ptr_array_add(labels, g_strdup(cur));
    }
    g_ptr_array_add(labels, NULL);
    GtkWidget *dd = combo_row(list, title, sub, (const char *const *)labels->pdata, sel);
    g_ptr_array_unref(labels);
    g_signal_connect_data(dd, "notify::selected", G_CALLBACK(adv_combo_changed), c, (GClosureNotify)(void (*)(void))adv_combo_free, 0);
    return dd;
}

static void adv_switch_changed(GtkSwitch *sw, GParamSpec *ps, gpointer d) {
    (void)ps;
    gboolean invert = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sw), "hm-invert"));
    adv_track(d, gtk_switch_get_active(sw) != invert ? "yes" : "no");
}

static void adv_switch(GtkWidget *list, const char *title, const char *prop, gboolean invert, const char *sub) {
    const char *v = g_hash_table_lookup(ADV->props, prop);
    GtkWidget *sw = switch_row(list, title, sub, (v && !strcmp(v, "yes")) != invert);
    g_object_set_data(G_OBJECT(sw), "hm-invert", GINT_TO_POINTER(invert));
    g_signal_connect(sw, "notify::active", G_CALLBACK(adv_switch_changed), (gpointer)g_intern_string(prop));
}

static void adv_prio(GtkSpinButton *sp, gpointer d) {
    (void)d;
    char v[16];
    snprintf(v, sizeof v, "%d", (int)gtk_spin_button_get_value(sp));
    adv_track("connection.autoconnect-priority", v);
}

typedef struct { GtkWidget *method, *dns; GtkWidget *rows[2]; const NmOpt *opts; size_t n; } IpSync;
static void ip_sync(GtkDropDown *dd, GParamSpec *ps, gpointer d) {
    (void)ps;
    IpSync *s = d;
    guint i = gtk_drop_down_get_selected(dd);
    const char *m = i < s->n ? s->opts[i].value : NULL;
    gboolean manual = m ? !strcmp(m, "manual") : TRUE, off = m && (!strcmp(m, "disabled") || !strcmp(m, "ignore"));
    for (int k = 0; k < 2; k++) gtk_widget_set_sensitive(row_of(s->rows[k]), manual);
    gtk_widget_set_sensitive(row_of(s->dns), !off);
}

static void adv_back(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    gtk_stack_set_visible_child_name(GTK_STACK(NP.stack), "main");
}

static void adv_saved(GObject *src, GAsyncResult *res, gpointer d) {
    (void)src; (void)d;
    char *err = g_task_propagate_pointer(G_TASK(res), NULL);
    if (err) {
        char *t = g_strdup_printf("Not saved: %s", err);
        settings_toast(t, 5);
        g_free(t);
    } else {
        settings_toast(ADV && ADV->c->active ? "Saved and reconnected" : "Saved", 3);
        gtk_stack_set_visible_child_name(GTK_STACK(NP.stack), "main");
        np_reload(FALSE);
    }
    g_free(err);
}

static void adv_save(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    if (!g_hash_table_size(ADV->changed)) { adv_back(NULL, NULL); return; }
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, g_strdup("connection"));
    g_ptr_array_add(args, g_strdup("modify"));
    g_ptr_array_add(args, g_strdup(ADV->c->uuid));
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, ADV->changed);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        gboolean mtu = g_str_has_suffix(k, ".mtu"), autov = !*(char *)v || !strcmp(v, "auto");
        if (mtu && !autov) {
            for (const char *p = v; *p; p++)
                if (!g_ascii_isdigit(*p)) {
                    settings_toast("MTU must be a number or auto", 3);
                    g_ptr_array_set_free_func(args, g_free);
                    g_ptr_array_free(args, TRUE);
                    return;
                }
        }
        g_ptr_array_add(args, g_strdup(k));
        g_ptr_array_add(args, g_strdup(mtu && autov ? "0" : v));   // 0 = automatic
    }
    g_ptr_array_add(args, NULL);
    Job *j = job_new(NULL);
    job_nmcli_v(j, 15, (char **)g_ptr_array_free(args, FALSE));
    if (ADV->c->active) job_nmcli(j, 40, "connection", "up", ADV->c->uuid, NULL);   // apply now
    settings_toast("Saving…", 30);
    GTask *t = g_task_new(NULL, NULL, adv_saved, NULL);
    g_task_set_task_data(t, j, job_free);
    g_task_run_in_thread(t, job_work);
    g_object_unref(t);
}

static void adv_build(Adv *a) {
    adv_free(ADV);
    ADV = a;
    if (NP.adv) gtk_stack_remove(GTK_STACK(NP.stack), NP.adv);
    GtkWidget *outer = VBOX(0, NULL), *head = HBOX(10, "hm-subhead");
    GtkWidget *back = icon_btn("go-previous-symbolic", "Back");
    g_signal_connect(back, "clicked", G_CALLBACK(adv_back), NULL);
    gtk_box_append(GTK_BOX(head), back);
    gtk_box_append(GTK_BOX(head), lbl(a->c->name, "hm-subhead-title", 0));
    gtk_box_append(GTK_BOX(outer), head);
    GtkWidget *p, *sc = page_new(&p);
    gtk_box_append(GTK_BOX(outer), sc);
    GtkWidget *g = group_new(p, "General", NULL);
    adv_entry(g, "Name", "connection.id", NULL);
    const char *pr = g_hash_table_lookup(a->props, "connection.autoconnect-priority");
    GtkWidget *prio = spin_row(g, "Priority", "Higher connects first when several are available", -999, 999, 1, pr ? atoi(pr) : 0);
    g_signal_connect(prio, "value-changed", G_CALLBACK(adv_prio), NULL);
    adv_combo(g, "Metered", "connection.metered", NM_METERED, G_N_ELEMENTS(NM_METERED), "Metered connections avoid big background downloads");
    if (a->kind && !strcmp(a->kind, "802-11-wireless"))
        adv_switch(g, "Hidden network", "802-11-wireless.hidden", FALSE, "Connect even when it doesn't broadcast its name");
    for (int ver = 4; ver <= 6; ver += 2) {
        char t[8], prop[40];
        snprintf(t, sizeof t, "IPv%d", ver);
        g = group_new(p, t, NULL);
        IpSync *s = g_new0(IpSync, 1);
        s->opts = ver == 4 ? NM_IP4 : NM_IP6;
        s->n = ver == 4 ? G_N_ELEMENTS(NM_IP4) : G_N_ELEMENTS(NM_IP6);
        snprintf(prop, sizeof prop, "ipv%d.method", ver);
        s->method = adv_combo(g, "Method", prop, s->opts, s->n, NULL);
        snprintf(prop, sizeof prop, "ipv%d.addresses", ver);
        s->rows[0] = adv_entry(g, "Addresses", prop, ver == 4 ? "Comma separated, with prefix: 192.168.1.50/24"
                                                              : "Comma separated, with prefix: 2001:db8::50/64");
        snprintf(prop, sizeof prop, "ipv%d.gateway", ver);
        s->rows[1] = adv_entry(g, "Gateway", prop, NULL);
        snprintf(prop, sizeof prop, "ipv%d.dns", ver);
        s->dns = adv_entry(g, "DNS servers", prop, ver == 4 ? "Comma separated, e.g. 1.1.1.1, 9.9.9.9" : "Comma separated");
        snprintf(prop, sizeof prop, "ipv%d.ignore-auto-dns", ver);
        adv_switch(g, "Automatic DNS", prop, TRUE, "Off: use only the DNS servers above");
        g_signal_connect_data(s->method, "notify::selected", G_CALLBACK(ip_sync), s, (GClosureNotify)(void (*)(void))g_free, 0);
        ip_sync(GTK_DROP_DOWN(s->method), NULL, s);
    }
    if (a->kind) {
        g = group_new(p, "Hardware", NULL);
        char prop[48];
        snprintf(prop, sizeof prop, "%s.mtu", a->kind);
        adv_entry(g, "MTU", prop, "auto, or bytes (e.g. 1500)");
        snprintf(prop, sizeof prop, "%s.cloned-mac-address", a->kind);
        adv_combo(g, "MAC address", prop, NM_MACS, G_N_ELEMENTS(NM_MACS), "Random or stable addresses make this computer harder to track");
    }
    g = group_new(p, NULL, NULL);
    button_row(g, "Save", "suggested", adv_save, NULL);
    NP.adv = outer;
    gtk_stack_add_named(GTK_STACK(NP.stack), outer, "adv");
    gtk_stack_set_visible_child_name(GTK_STACK(NP.stack), "adv");
}

static void adv_load_work(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)c;
    ConnRef *cr = d;
    Adv *a = g_new0(Adv, 1);
    a->kind = !strcmp(cr->type, "802-11-wireless") || !strcmp(cr->type, "802-3-ethernet") ? g_strdup(cr->type) : NULL;
    GString *f = g_string_new("connection.id,connection.autoconnect-priority,connection.metered,ipv4.method,ipv4.addresses,"
                              "ipv4.gateway,ipv4.dns,ipv4.ignore-auto-dns,ipv6.method,ipv6.addresses,ipv6.gateway,ipv6.dns,"
                              "ipv6.ignore-auto-dns");
    if (a->kind) g_string_append_printf(f, ",%s.mtu,%s.cloned-mac-address", a->kind, a->kind);
    if (a->kind && !strcmp(a->kind, "802-11-wireless")) g_string_append(f, ",802-11-wireless.hidden");
    char *out = NULL, *err = NULL;
    gboolean ok = nmcli(&out, &err, "-t", "-f", f->str, "connection", "show", cr->uuid);
    g_string_free(f, TRUE);
    a->props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    a->changed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!ok) {
        g_hash_table_insert(a->props, g_strdup("_error"), nm_err_text(err));
    } else {
        char **lines = g_strsplit(out, "\n", -1);
        for (char **l = lines; *l; l++) {
            char *c = strchr(*l, ':');
            if (!c) continue;
            *c = 0;
            char *a1 = replace_all(c + 1, "\\:", ":"), *a2 = replace_all(a1, "\\\\", "\\");
            g_hash_table_insert(a->props, g_strdup(*l), a2);
            g_free(a1);
        }
        g_strfreev(lines);
    }
    g_free(out); g_free(err);
    g_task_return_pointer(t, a, NULL);
}

static void adv_loaded(GObject *src, GAsyncResult *res, gpointer d) {
    (void)src;
    Adv *a = g_task_propagate_pointer(G_TASK(res), NULL);
    a->c = d;
    const char *e = g_hash_table_lookup(a->props, "_error");
    if (e) {
        char *t = g_strdup_printf("Can't read %s: %s", a->c->name, e);
        settings_toast(t, 5);
        g_free(t);
        adv_free(a);
        return;
    }
    adv_build(a);
}

static void np_open_advanced(ConnRef *c) {   // takes a copy of c
    ConnRef *copy = g_new0(ConnRef, 1);
    copy->uuid = g_strdup(c->uuid); copy->name = g_strdup(c->name); copy->type = g_strdup(c->type ? c->type : "");
    copy->dev = g_strdup(c->dev ? c->dev : ""); copy->active = c->active;
    GTask *t = g_task_new(NULL, NULL, adv_loaded, copy);
    g_task_set_task_data(t, copy, NULL);
    g_task_run_in_thread(t, adv_load_work);
    g_object_unref(t);
}
