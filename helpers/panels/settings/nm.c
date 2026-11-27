// NetworkManager without a GUI: nmcli calls, Wi-Fi / Ethernet / VPN / hotspot state, hotspot on and off.
// Shared by Settings → Network and the bar's network popup. Everything here blocks: worker threads only.

#define NM_AP_IF "ap0"   // hotspot next to the Wi-Fi connection: hostapd on this second interface, run by
#define NM_IFACE_HELPER "/usr/local/lib/hyprminimal/hotspot-iface"   // this root helper (install.sh)
#define NM_HELPER_RUN "/run/hyprminimal-hotspot"
#define NM_OPENVPN "/usr/lib/NetworkManager/VPN/nm-openvpn-service.name"
#define NM_SAVED_SHOWN 5    // "Saved networks" rows before "Show all"
#define NM_NEARBY_SHOWN 5   // "Nearby networks": the strongest few, the rest behind "Show all"

typedef struct { const char *value, *label; } NmOpt;
static const NmOpt NM_BANDS[] = {{"", "Automatic"}, {"bg", "2.4 GHz"}, {"a", "5 GHz"}};
static const NmOpt NM_IP4[] = {{"auto", "Automatic (DHCP)"}, {"manual", "Manual"}, {"link-local", "Link-local only"},
                               {"shared", "Shared to other computers"}, {"disabled", "Disabled"}};
static const NmOpt NM_IP6[] = {{"auto", "Automatic"}, {"dhcp", "Automatic, DHCP only"}, {"manual", "Manual"},
                               {"link-local", "Link-local only"}, {"shared", "Shared to other computers"},
                               {"ignore", "Ignore"}, {"disabled", "Disabled"}};
static const NmOpt NM_METERED[] = {{"unknown", "Detect automatically"}, {"yes", "Yes (limit background data)"}, {"no", "No"}};
static const NmOpt NM_MACS[] = {{"", "Default"}, {"permanent", "Permanent (the adapter's own)"},
                                {"stable", "Stable (per network)"}, {"random", "Random (each connection)"},
                                {"preserve", "Keep current"}};
static const NmOpt NM_OTHER_TYPES[] = {{"vpn", "VPN"}, {"wireguard", "WireGuard"}, {"gsm", "Mobile broadband"},
                                       {"cdma", "Mobile broadband"}, {"bluetooth", "Bluetooth tethering"},
                                       {"pppoe", "DSL (PPPoE)"}};

static const char *nm_other_label(const char *type) {
    for (size_t i = 0; i < G_N_ELEMENTS(NM_OTHER_TYPES); i++)
        if (!strcmp(NM_OTHER_TYPES[i].value, type)) return NM_OTHER_TYPES[i].label;
    return NULL;
}

typedef struct {
    char *name, *uuid, *type, *dev, *mode, *ssid, *band, *psk;
    gint64 ts;
    gboolean autoconnect, active, alongside;   // alongside: hotspot running next to the Wi-Fi connection
    GPtrArray *clients;                        // hotspot: "name", "ip", "name", "ip", …
    GPtrArray *details;                        // "IP address", "192.168.1.4", …
} NmConn;

typedef struct { char *ssid, *sec, *band; int signal; gboolean inuse; } NmAp;

typedef struct {
    char *dev, *type, *state, *conn;
    int speed;                                 // Mb/s, 0 unknown
    gboolean usb;
    NmConn *profile;                           // the active connection on it (not owned)
    GPtrArray *details;
} NmDev;

typedef struct {
    gboolean radio, airplane, ap, five, concurrent;
    char *wdev;
    GPtrArray *conns, *devs;                   // owned: NmConn, NmDev
    GPtrArray *saved, *other, *eth;            // not owned: NmConn (Wi-Fi profiles, VPN & co), NmDev (wired)
    GPtrArray *nearby;                         // owned NmAp, strongest first
    NmConn *active, *hotspot;                  // not owned
} NmState;

static void nm_conn_free(gpointer p) {
    NmConn *c = p;
    g_free(c->name); g_free(c->uuid); g_free(c->type); g_free(c->dev); g_free(c->mode); g_free(c->ssid);
    g_free(c->band); g_free(c->psk);
    if (c->clients) g_ptr_array_unref(c->clients);
    if (c->details) g_ptr_array_unref(c->details);
    g_free(c);
}

static void nm_ap_free(gpointer p) { NmAp *a = p; g_free(a->ssid); g_free(a->sec); g_free(a->band); g_free(a); }

static void nm_dev_free(gpointer p) {
    NmDev *d = p;
    g_free(d->dev); g_free(d->type); g_free(d->state); g_free(d->conn);
    if (d->details) g_ptr_array_unref(d->details);
    g_free(d);
}

static void nm_state_free(NmState *d) {
    if (!d) return;
    g_free(d->wdev);
    g_ptr_array_unref(d->saved); g_ptr_array_unref(d->other); g_ptr_array_unref(d->eth);
    g_ptr_array_unref(d->nearby); g_ptr_array_unref(d->devs); g_ptr_array_unref(d->conns);
    g_free(d);
}

// run argv (with a timeout), stdin text optional: TRUE on exit 0; *out / *err trimmed (may be NULL)
static gboolean nm_run(int timeout_s, char **argv, const char *input, char **out, char **err) {
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup("timeout"));
    g_ptr_array_add(a, g_strdup_printf("%d", timeout_s));
    for (char **p = argv; *p; p++) g_ptr_array_add(a, g_strdup(*p));
    g_ptr_array_add(a, NULL);
    GSubprocessFlags fl = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE |
                          (input ? G_SUBPROCESS_FLAGS_STDIN_PIPE : 0);
    GError *e = NULL;
    GSubprocess *p = g_subprocess_newv((const char *const *)a->pdata, fl, &e);
    g_ptr_array_unref(a);
    char *o = NULL, *r = NULL;
    gboolean ok = FALSE;
    if (p && g_subprocess_communicate_utf8(p, input, NULL, &o, &r, &e)) ok = g_subprocess_get_successful(p);
    if (e) { g_free(r); r = g_strdup(e->message); g_error_free(e); }
    if (p) g_object_unref(p);
    if (out) *out = g_strstrip(o ? o : g_strdup("")); else g_free(o);
    if (err) *err = g_strstrip(r ? r : g_strdup("")); else g_free(r);
    return ok;
}

// nmcli ARGS… (NULL-terminated): TRUE on success
static gboolean nmcli_t(int timeout_s, char **out, char **err, const char *first, ...) {
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, (char *)"nmcli");
    va_list ap;
    va_start(ap, first);
    for (const char *s = first; s; s = va_arg(ap, const char *)) g_ptr_array_add(a, (char *)s);
    va_end(ap);
    g_ptr_array_add(a, NULL);
    gboolean ok = nm_run(timeout_s, (char **)a->pdata, NULL, out, err);
    g_ptr_array_free(a, TRUE);
    return ok;
}
#define nmcli(out, err, ...) nmcli_t(15, out, err, __VA_ARGS__, NULL)

static gboolean nmcli_v(int timeout_s, GPtrArray *args, char **out, char **err) {   // args: char* without NULL
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, (char *)"nmcli");
    for (guint i = 0; i < args->len; i++) g_ptr_array_add(a, g_ptr_array_index(args, i));
    g_ptr_array_add(a, NULL);
    gboolean ok = nm_run(timeout_s, (char **)a->pdata, NULL, out, err);
    g_ptr_array_free(a, TRUE);
    return ok;
}

static char *nm_out(const char *first, ...) {   // stdout of a quick nmcli call, "" on failure
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, (char *)"nmcli");
    va_list ap;
    va_start(ap, first);
    for (const char *s = first; s; s = va_arg(ap, const char *)) g_ptr_array_add(a, (char *)s);
    va_end(ap);
    g_ptr_array_add(a, NULL);
    char *o = NULL;
    nm_run(15, (char **)a->pdata, NULL, &o, NULL);
    g_ptr_array_free(a, TRUE);
    return o;
}

// nmcli -t fields: ':' separates, '\:' and '\\' are escapes
static char **nm_split(const char *line) {
    GPtrArray *f = g_ptr_array_new();
    GString *cur = g_string_new(NULL);
    for (const char *p = line; *p; p++) {
        if (*p == '\\' && p[1]) g_string_append_c(cur, *++p);
        else if (*p == ':') { g_ptr_array_add(f, g_string_free(cur, FALSE)); cur = g_string_new(NULL); }
        else g_string_append_c(cur, *p);
    }
    g_ptr_array_add(f, g_string_free(cur, FALSE));
    g_ptr_array_add(f, NULL);
    return (char **)g_ptr_array_free(f, FALSE);
}

// the line an nmcli error is about, without "Error: "
static char *nm_err_text(const char *err) {
    if (!err || !*err) return g_strdup("failed");
    char **l = g_strsplit(err, "\n", -1);
    char *r = NULL;
    for (char **x = l; *x && !r; x++)
        if (g_str_has_prefix(*x, "Error:")) {
            r = g_strstrip(g_strdup(*x + 6));
            size_t n = strlen(r);
            if (n && r[n - 1] == '.') r[n - 1] = 0;
        }
    if (!r) {
        int n = g_strv_length(l);
        r = g_strdup(n ? l[n - 1] : "failed");
    }
    g_strfreev(l);
    return r;
}

static char *nm_ago(gint64 ts) {
    if (!ts) return g_strdup("never used");
    gint64 d = g_get_real_time() / G_USEC_PER_SEC - ts;
    static const struct { const char *u; gint64 s; } units[] = {{"year", 31536000}, {"month", 2592000}, {"day", 86400},
                                                               {"hour", 3600}, {"minute", 60}};
    for (size_t i = 0; i < G_N_ELEMENTS(units); i++)
        if (d >= units[i].s) {
            gint64 n = d / units[i].s;
            return g_strdup_printf("%" G_GINT64_FORMAT " %s%s ago", n, units[i].u, n > 1 ? "s" : "");
        }
    return g_strdup("just now");
}

static const char *nm_band(int mhz) { return mhz >= 5925 ? "6 GHz" : mhz >= 4900 ? "5 GHz" : mhz ? "2.4 GHz" : ""; }

// IP address, gateway, DNS, IPv6, MAC of a device, as name/value pairs
static GPtrArray *nm_details(const char *dev) {
    static const char *keys[][2] = {{"IP4.ADDRESS", "IP address"}, {"IP4.GATEWAY", "Gateway"}, {"IP4.DNS", "DNS"},
                                    {"IP6.ADDRESS", "IPv6"}, {"GENERAL.HWADDR", "MAC address"}};
    char *vals[5] = {0};
    char *out = nm_out("-t", "-f", "GENERAL.HWADDR,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS,IP6.ADDRESS", "device", "show", dev, NULL);
    char **lines = g_strsplit(out, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *c = strchr(*l, ':');
        if (!c) continue;
        *c = 0;
        char *br = strchr(*l, '['), *val = c + 1;
        if (br) *br = 0;
        if (!*val || !strcmp(val, "--")) continue;
        for (int i = 0; i < 5; i++) {
            if (strcmp(*l, keys[i][0])) continue;
            if (i == 3 && vals[i]) break;   // first IPv6 address only
            char *v = vals[i] ? g_strdup_printf("%s, %s", vals[i], val) : g_strdup(val);
            g_free(vals[i]);
            vals[i] = v;
        }
    }
    g_strfreev(lines);
    g_free(out);
    GPtrArray *d = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < 5; i++)
        if (vals[i]) { g_ptr_array_add(d, g_strdup(keys[i][1])); g_ptr_array_add(d, vals[i]); }
    return d;
}

// Can this adapter stay on a Wi-Fi network and run an access point at the same time? From `iw phy … info`:
// a combination allowing managed and AP together, e.g. `#{ managed } <= 1, #{ AP } <= 1, total <= 2`
static gboolean nm_sta_ap_concurrent(const char *dev) {
    static GHashTable *cache;
    G_LOCK_DEFINE_STATIC(cache);
    G_LOCK(cache);
    if (!cache) cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gpointer hit;
    gboolean known = g_hash_table_lookup_extended(cache, dev, NULL, &hit);
    G_UNLOCK(cache);
    if (known) return GPOINTER_TO_INT(hit);
    gboolean ok = FALSE;
    char *link = g_strdup_printf("/sys/class/net/%s/phy80211", dev), *real = realpath(link, NULL);
    if (real) {
        char *phy = g_path_get_basename(real), *out = NULL;
        char *argv[] = {"iw", "phy", phy, "info", NULL};
        nm_run(3, argv, NULL, &out, NULL);
        const char *text = out ? strstr(out, "valid interface combinations:") : NULL;
        if (text) {
            GRegex *cre = g_regex_new("\\*\\s*(#\\{.*?total <= \\d+)", G_REGEX_DOTALL, 0, NULL);
            GRegex *gre = g_regex_new("#\\{([^}]*)\\}\\s*<=\\s*(\\d+)", 0, 0, NULL);
            GRegex *tre = g_regex_new("total <= (\\d+)", 0, 0, NULL);
            GMatchInfo *m;
            g_regex_match(cre, text, 0, &m);
            for (; g_match_info_matches(m) && !ok; g_match_info_next(m, NULL)) {
                char *combo = g_match_info_fetch(m, 1);
                gboolean sta = FALSE, ap = FALSE, shared = FALSE;
                GMatchInfo *gm;
                g_regex_match(gre, combo, 0, &gm);
                for (; g_match_info_matches(gm); g_match_info_next(gm, NULL)) {
                    char *names = g_match_info_fetch(gm, 1), *lim = g_match_info_fetch(gm, 2);
                    char **nv = g_strsplit(names, ",", -1);
                    gboolean has_m = FALSE, has_a = FALSE;
                    for (char **n = nv; *n; n++) {
                        g_strstrip(*n);
                        if (!strcmp(*n, "managed")) has_m = TRUE;
                        if (!strcmp(*n, "AP")) has_a = TRUE;
                    }
                    sta |= has_m;
                    ap |= has_a;
                    if (has_m && has_a && atoi(lim) < 2) shared = TRUE;
                    g_strfreev(nv); g_free(names); g_free(lim);
                }
                g_match_info_free(gm);
                GMatchInfo *tm;
                int total = 0;
                if (g_regex_match(tre, combo, 0, &tm)) { char *t = g_match_info_fetch(tm, 1); total = atoi(t); g_free(t); }
                g_match_info_free(tm);
                if (sta && ap && total >= 2 && !shared) ok = TRUE;
                g_free(combo);
            }
            g_match_info_free(m);
            g_regex_unref(cre); g_regex_unref(gre); g_regex_unref(tre);
        }
        g_free(phy); g_free(out); free(real);
    }
    g_free(link);
    G_LOCK(cache);
    g_hash_table_insert(cache, g_strdup(dev), GINT_TO_POINTER(ok));
    G_UNLOCK(cache);
    return ok;
}

// run the root helper that starts/stops the side-by-side hotspot (no password: polkit rule)
static gboolean nm_iface_helper(const char *cmd, const char *dev, const char *input, char **err) {
    char *argv[] = {"pkexec", NM_IFACE_HELPER, (char *)cmd, (char *)dev, NULL};
    char *e = NULL;
    gboolean ok = nm_run(90, argv, input, NULL, &e);
    if (!ok && (!e || !*e)) { g_free(e); e = g_strdup("the hotspot helper failed"); }
    if (err) *err = e; else g_free(e);
    return ok;
}

static gboolean nm_helper_running(void) {   // is the helper's hostapd (hotspot next to Wi-Fi) up?
    char b[32], comm[64];
    if (!read_small(NM_HELPER_RUN "/hostapd.pid", b, sizeof b)) return FALSE;
    char *path = g_strdup_printf("/proc/%d/comm", atoi(b));
    gboolean ok = read_small(path, comm, sizeof comm) && !strcmp(g_strstrip(comm), "hostapd");
    g_free(path);
    return ok;
}

// devices on the hotspot: dnsmasq leases, else the neighbour table
static GPtrArray *nm_hotspot_clients(const char *dev) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    char *leases = !strcmp(dev, NM_AP_IF) ? g_strdup(NM_HELPER_RUN "/dnsmasq.leases")
                                          : g_strdup_printf("/var/lib/NetworkManager/dnsmasq-%s.leases", dev);
    char *text = NULL;
    if (g_file_get_contents(leases, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l; l++) {
            char **f = g_strsplit_set(*l, " \t", -1);
            if (g_strv_length(f) >= 4) {
                g_ptr_array_add(out, g_strdup(strcmp(f[3], "*") ? f[3] : f[1]));
                g_ptr_array_add(out, g_strdup(f[2]));
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    } else {
        char *argv[] = {"ip", "-4", "neigh", "show", "dev", (char *)dev, NULL}, *o = NULL;
        nm_run(3, argv, NULL, &o, NULL);
        char **lines = g_strsplit(o ? o : "", "\n", -1);
        for (char **l = lines; *l; l++) {
            char **f = g_strsplit(*l, " ", -1);
            int n = g_strv_length(f), ll = -1;
            for (int i = 0; i < n; i++) if (!strcmp(f[i], "lladdr")) ll = i;
            if (ll >= 0 && ll + 1 < n && (!strcmp(f[n - 1], "REACHABLE") || !strcmp(f[n - 1], "STALE") ||
                                          !strcmp(f[n - 1], "DELAY"))) {
                g_ptr_array_add(out, g_strdup(f[ll + 1]));
                g_ptr_array_add(out, g_strdup(f[0]));
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(o);
    }
    g_free(text);
    g_free(leases);
    return out;
}

static gboolean nm_have_hostapd(void) {
    char *p = g_find_program_in_path("hostapd");
    if (!p) p = g_file_test("/usr/sbin/hostapd", G_FILE_TEST_IS_EXECUTABLE) ? g_strdup("x") : NULL;
    gboolean ok = p != NULL;
    g_free(p);
    return ok;
}

static gint nm_saved_cmp(gconstpointer a, gconstpointer b) {
    const NmConn *x = *(NmConn *const *)a, *y = *(NmConn *const *)b;
    return x->ts < y->ts ? 1 : x->ts > y->ts ? -1 : 0;
}

// Radio, Wi-Fi device and what it can do, saved/active Wi-Fi profiles, the hotspot, every connection and
// device: everything but the (slow) scan and the per-device details
static NmState *nm_base_state(void) {
    NmState *d = g_new0(NmState, 1);
    d->conns = g_ptr_array_new_with_free_func(nm_conn_free);
    d->devs = g_ptr_array_new_with_free_func(nm_dev_free);
    d->saved = g_ptr_array_new();
    d->other = g_ptr_array_new();
    d->eth = g_ptr_array_new();
    d->nearby = g_ptr_array_new_with_free_func(nm_ap_free);
    char *r = nm_out("radio", "wifi", NULL);
    d->radio = !strcmp(r, "enabled");
    g_free(r);
    if (!d->radio) { r = nm_out("radio", "wwan", NULL); d->airplane = strcmp(r, "enabled") != 0; g_free(r); }

    char *out = nm_out("-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", NULL);
    char **lines = g_strsplit(out, "\n", -1);
    for (char **l = lines; *l; l++) {
        char **f = nm_split(*l);
        if (g_strv_length(f) >= 4) {
            NmDev *x = g_new0(NmDev, 1);
            x->dev = g_strdup(f[0]); x->type = g_strdup(f[1]); x->state = g_strdup(f[2]);
            x->conn = g_strdup(strcmp(f[3], "--") ? f[3] : "");
            g_ptr_array_add(d->devs, x);
            if (!d->wdev && !strcmp(x->type, "wifi") && strcmp(x->dev, NM_AP_IF)) d->wdev = g_strdup(x->dev);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(out);
    if (d->wdev) {
        char *props = nm_out("-t", "-f", "WIFI-PROPERTIES.AP,WIFI-PROPERTIES.5GHZ", "device", "show", d->wdev, NULL);
        d->ap = strstr(props, "AP:yes") != NULL;
        d->five = strstr(props, "5GHZ:yes") != NULL;
        g_free(props);
        d->concurrent = d->ap && access(NM_IFACE_HELPER, X_OK) == 0 && nm_have_hostapd() && nm_sta_ap_concurrent(d->wdev);
    }

    GPtrArray *wifi = g_ptr_array_new();
    out = nm_out("-t", "-f", "NAME,UUID,TYPE,TIMESTAMP,AUTOCONNECT,ACTIVE,DEVICE", "connection", "show", NULL);
    lines = g_strsplit(out, "\n", -1);
    for (char **l = lines; *l; l++) {
        char **f = nm_split(*l);
        if (g_strv_length(f) >= 7) {
            NmConn *c = g_new0(NmConn, 1);
            c->name = g_strdup(f[0]); c->uuid = g_strdup(f[1]); c->type = g_strdup(f[2]);
            c->ts = g_ascii_strtoll(f[3], NULL, 10);
            c->autoconnect = !strcmp(f[4], "yes");
            c->active = !strcmp(f[5], "yes");
            c->dev = g_strdup(strcmp(f[6], "--") ? f[6] : "");
            c->mode = g_strdup(""); c->ssid = g_strdup(c->name); c->band = g_strdup(""); c->psk = g_strdup("");
            g_ptr_array_add(d->conns, c);
            if (!strcmp(c->type, "802-11-wireless")) g_ptr_array_add(wifi, c);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(out);
    // one call for every Wi-Fi profile's mode: access-point ones are hotspots, not networks to join.
    // 4 lines per profile, but an empty value is an empty line too: anchor on each profile's UUID
    if (wifi->len) {
        GPtrArray *a = g_ptr_array_new();
        const char *head[] = {"-t", "-g", "connection.uuid,802-11-wireless.mode,802-11-wireless.ssid,802-11-wireless.band",
                              "connection", "show"};
        for (size_t i = 0; i < G_N_ELEMENTS(head); i++) g_ptr_array_add(a, (char *)head[i]);
        for (guint i = 0; i < wifi->len; i++) g_ptr_array_add(a, ((NmConn *)g_ptr_array_index(wifi, i))->uuid);
        char *o = NULL;
        nmcli_v(15, a, &o, NULL);
        g_ptr_array_free(a, TRUE);
        char **bl = g_strsplit(o, "\n", -1);
        int n = g_strv_length(bl);
        for (int i = 0; i < n; i++)
            for (guint j = 0; j < wifi->len; j++) {
                NmConn *c = g_ptr_array_index(wifi, j);
                if (strcmp(bl[i], c->uuid)) continue;
                g_free(c->mode); g_free(c->ssid); g_free(c->band);
                c->mode = g_strdup(i + 1 < n ? bl[i + 1] : "");
                c->ssid = g_strdup(i + 2 < n && *bl[i + 2] ? bl[i + 2] : c->name);
                c->band = g_strdup(i + 3 < n ? bl[i + 3] : "");
            }
        g_strfreev(bl);
        g_free(o);
    }
    NmConn *hs = NULL;
    for (guint i = 0; i < wifi->len; i++) {
        NmConn *c = g_ptr_array_index(wifi, i);
        if (strcmp(c->mode, "ap")) continue;
        if (!hs || (c->active && !hs->active) || (c->active == hs->active && c->ts > hs->ts)) hs = c;
    }
    if (hs) {
        g_free(hs->psk);
        hs->psk = nm_out("-s", "-g", "802-11-wireless-security.psk", "connection", "show", hs->uuid, NULL);
        if (!hs->active && nm_helper_running()) {   // the profile only holds its name and password
            hs->active = hs->alongside = TRUE;
            g_free(hs->dev);
            hs->dev = g_strdup(NM_AP_IF);
        }
        if (hs->active) hs->clients = nm_hotspot_clients(*hs->dev ? hs->dev : d->wdev ? d->wdev : "");
        d->hotspot = hs;
    }
    for (guint i = 0; i < wifi->len; i++) {
        NmConn *c = g_ptr_array_index(wifi, i);
        if (!strcmp(c->mode, "ap")) continue;
        g_ptr_array_add(d->saved, c);
        if (c->active) d->active = c;
    }
    g_ptr_array_sort(d->saved, nm_saved_cmp);
    g_ptr_array_free(wifi, TRUE);
    return d;
}

static gint nm_ap_cmp(gconstpointer a, gconstpointer b) {
    return (*(NmAp *const *)b)->signal - (*(NmAp *const *)a)->signal;
}

static gint nm_other_cmp(gconstpointer a, gconstpointer b) {
    const NmConn *x = *(NmConn *const *)a, *y = *(NmConn *const *)b;
    if (x->active != y->active) return x->active ? -1 : 1;
    return g_utf8_collate(x->name, y->name);
}

// everything Settings → Network shows: base state + nearby networks, details, wired adapters, VPN & co
static NmState *nm_fetch(gboolean rescan) {
    NmState *d = nm_base_state();
    if (d->radio && d->wdev && !(d->hotspot && d->hotspot->active)) {
        char *o = NULL;
        nmcli_t(25, &o, NULL, "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY,FREQ", "device", "wifi", "list",
                "--rescan", rescan ? "yes" : "auto", NULL);
        GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
        char **lines = g_strsplit(o, "\n", -1);
        for (char **l = lines; *l; l++) {
            char **f = nm_split(*l);
            if (g_strv_length(f) >= 5 && *f[1]) {
                int sig = atoi(f[2]), mhz = atoi(f[4]);
                NmAp *old = g_hash_table_lookup(seen, f[1]);
                if (!old || sig > old->signal) {
                    NmAp *a = old ? old : g_new0(NmAp, 1);
                    if (old) { g_free(a->sec); g_free(a->band); }
                    else { a->ssid = g_strdup(f[1]); g_ptr_array_add(d->nearby, a); g_hash_table_insert(seen, a->ssid, a); }
                    a->signal = sig;
                    a->sec = g_strstrip(g_strdup(f[3]));
                    a->band = g_strdup(nm_band(mhz));
                    a->inuse = !strcmp(f[0], "*");
                } else if (!strcmp(f[0], "*")) old->inuse = TRUE;
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_hash_table_unref(seen);
        g_free(o);
        g_ptr_array_sort(d->nearby, nm_ap_cmp);
    }
    if (d->active) {
        NmConn *a = d->active;
        g_free(a->psk);
        a->psk = nm_out("-s", "-g", "802-11-wireless-security.psk", "connection", "show", a->uuid, NULL);
        a->details = nm_details(*a->dev ? a->dev : d->wdev);
    }
    // wired adapters (USB tethering from a phone shows up here too)
    for (guint i = 0; i < d->devs->len; i++) {
        NmDev *x = g_ptr_array_index(d->devs, i);
        if (strcmp(x->type, "ethernet")) continue;
        char *p = g_strdup_printf("/sys/class/net/%s/speed", x->dev);
        long long sp = read_ll(p, -1);
        g_free(p);
        x->speed = sp > 0 ? (int)sp : 0;
        p = g_strdup_printf("/sys/class/net/%s/device", x->dev);
        char *real = realpath(p, NULL);
        x->usb = real && strstr(real, "usb");
        free(real);
        g_free(p);
        for (guint j = 0; j < d->conns->len; j++) {
            NmConn *c = g_ptr_array_index(d->conns, j);
            if (c->active && !strcmp(c->dev, x->dev)) { x->profile = c; break; }
        }
        x->details = !strcmp(x->state, "connected") ? nm_details(x->dev) : g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(d->eth, x);
    }
    for (guint j = 0; j < d->conns->len; j++) {
        NmConn *c = g_ptr_array_index(d->conns, j);
        if (nm_other_label(c->type)) g_ptr_array_add(d->other, c);
    }
    g_ptr_array_sort(d->other, nm_other_cmp);
    return d;
}

// ---------------------------------------------------------------- hotspot
// ssid, psk, band: what to share as. hs: its saved profile or NULL. *err on failure.
static gboolean nm_hotspot_up(NmState *d, NmConn *hs, const char *ssid, const char *psk, const char *band, char **err) {
    // NetworkManager's shared mode hands out addresses with dnsmasq; without it the access point comes up,
    // fails at the IP step and takes the current Wi-Fi connection down with it
    if (!g_file_test("/usr/bin/dnsmasq", G_FILE_TEST_IS_EXECUTABLE) && !g_file_test("/usr/sbin/dnsmasq", G_FILE_TEST_IS_EXECUTABLE)) {
        *err = g_strdup("dnsmasq is not installed (sudo pacman -S dnsmasq)");
        return FALSE;
    }
    if (!d->radio) { nmcli(NULL, NULL, "radio", "wifi", "on"); g_usleep(1500000); }
    char *input = g_strdup_printf("%s\n%s\n", ssid, psk);
    gboolean ok;
    if (d->concurrent && d->active) {
        // stay on the Wi-Fi network: the helper runs the hotspot on a second interface, on the network's
        // channel; the profile keeps its name and password (made here the first time)
        ok = TRUE;
        if (!hs) {
            char *e = NULL;
            ok = nmcli(NULL, &e, "connection", "add", "type", "wifi", "ifname", d->wdev, "con-name", "Hotspot",
                       "autoconnect", "no", "ssid", ssid, "802-11-wireless.mode", "ap", "ipv4.method", "shared",
                       "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", psk);
            if (!ok) *err = e; else g_free(e);
        }
        if (ok) ok = nm_iface_helper("start", d->wdev, input, err);
        g_free(input);
        return ok;
    }
    g_free(input);
    if (nm_helper_running()) nm_iface_helper("stop", NULL, NULL, NULL);
    char *e = NULL;
    if (hs) {
        ok = nmcli(NULL, &e, "connection", "modify", hs->uuid, "connection.interface-name", d->wdev, "802-11-wireless.channel", "0");
        if (ok) { g_free(e); e = NULL; ok = nmcli_t(40, NULL, &e, "connection", "up", hs->uuid, NULL); }
    } else if (band && *band) {
        ok = nmcli_t(40, NULL, &e, "device", "wifi", "hotspot", "ifname", d->wdev, "con-name", "Hotspot",
                     "ssid", ssid, "password", psk, "band", band, NULL);
    } else {
        ok = nmcli_t(40, NULL, &e, "device", "wifi", "hotspot", "ifname", d->wdev, "con-name", "Hotspot",
                     "ssid", ssid, "password", psk, NULL);
    }
    if (!ok) { *err = nm_err_text(e); g_free(e); } else g_free(e);
    return ok;
}

static gboolean nm_hotspot_down(NmConn *hs, char **err) {
    if (hs->alongside) return nm_iface_helper("stop", NULL, NULL, err);
    char *e = NULL;
    gboolean ok = nmcli(NULL, &e, "connection", "down", hs->uuid);
    if (!ok) *err = nm_err_text(e);
    g_free(e);
    return ok;
}

static char *nm_new_psk(void) {   // 12 random URL-safe characters
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char *s = g_malloc(13);
    for (int i = 0; i < 12; i++) s[i] = abc[g_random_int_range(0, 64)];
    s[12] = 0;
    return s;
}

// turn the hotspot on/off from its saved profile (the bar's network popup)
static gboolean nm_hotspot_set(gboolean on, char **err) {
    NmState *d = nm_base_state();
    NmConn *hs = d->hotspot;
    gboolean ok = TRUE;
    if (!d->wdev || !d->ap) { *err = g_strdup("this Wi-Fi adapter can't host a hotspot"); ok = FALSE; }
    else if (!on) { if (hs && hs->active) ok = nm_hotspot_down(hs, err); }
    else if (!(hs && hs->active)) {
        char host[256] = "hyprminimal", *psk = hs ? NULL : nm_new_psk();
        gethostname(host, sizeof host - 1);
        ok = nm_hotspot_up(d, hs, hs ? hs->ssid : host, hs ? hs->psk : psk, hs ? hs->band : "", err);
        g_free(psk);
    }
    nm_state_free(d);
    return ok;
}
