// ~/.config/hypr/keybind.lua read and edited line by line, keeping its style (Settings → Keybinds).

typedef struct {
    int line;
    char *combo, *key_ref, *literal, *fmt, *description, *action, *section;
    gboolean readonly;                         // defined in a loop: edit keybind.lua itself
} Keybind;

typedef struct {
    char *path, *main_mod;
    GPtrArray *lines;                          // char*
    GHashTable *keydefs;                       // KEY table NAME -> KeyDef
    GHashTable *vars;                          // local NAME = "string" (for "Run: …" summaries)
    GPtrArray *binds;                          // Keybind
    GHashTable *norm_count;                    // normalised combo -> how many editable binds use it
} KeybindFile;

typedef struct { int line; char *combo; } KeyDef;

static const char *MOD_ORDER[] = {"SUPER", "CTRL", "ALT", "SHIFT"};

static void keybind_free(gpointer p) {
    Keybind *b = p;
    g_free(b->combo); g_free(b->key_ref); g_free(b->literal); g_free(b->fmt); g_free(b->description);
    g_free(b->action); g_free(b->section);
    g_free(b);
}
static void keydef_free(gpointer p) { KeyDef *k = p; g_free(k->combo); g_free(k); }

// ---------------------------------------------------------------- combos: "SUPER + SHIFT + R"
static int mod_index(const char *up) {
    for (int i = 0; i < 4; i++) if (!strcmp(MOD_ORDER[i], up)) return i;
    return -1;
}

static const char *mod_alias(const char *up) {
    if (!strcmp(up, "CONTROL")) return "CTRL";
    if (!strcmp(up, "MOD4") || !strcmp(up, "WIN")) return "SUPER";
    if (!strcmp(up, "MOD1")) return "ALT";
    return up;
}

// modifiers (bit i = MOD_ORDER[i]) and the key as typed
static void split_combo(const char *combo, int *mods, char **key) {
    *mods = 0;
    *key = g_strdup("");
    char **parts = g_strsplit(combo ? combo : "", "+", -1);
    for (char **p = parts; *p; p++) {
        char *t = g_strstrip(*p);
        if (!*t) continue;
        char *up = g_ascii_strup(t, -1);
        int m = mod_index(mod_alias(up));
        if (m >= 0) *mods |= 1 << m;
        else { g_free(*key); *key = g_strdup(t); }
        g_free(up);
    }
    g_strfreev(parts);
}

static char *join_combo(int mods, const char *key) {
    GString *s = g_string_new(NULL);
    for (int i = 0; i < 4; i++)
        if (mods & (1 << i)) g_string_append_printf(s, "%s%s", s->len ? " + " : "", MOD_ORDER[i]);
    g_string_append_printf(s, "%s%s", s->len ? " + " : "", key);
    return g_string_free(s, FALSE);
}

static char *norm_combo(const char *combo) {   // for comparing: key uppercase too
    int mods;
    char *key;
    split_combo(combo, &mods, &key);
    char *up = g_ascii_strup(key, -1), *r = join_combo(mods, up);
    g_free(up); g_free(key);
    return r;
}

// modifiers uppercase in a fixed order, key name kept as typed (xkb names can be case-sensitive)
static char *canon_combo(const char *combo) {
    int mods;
    char *key;
    split_combo(combo, &mods, &key);
    char *r = join_combo(mods, key);
    g_free(key);
    return r;
}

// keycap labels: "Super", "Shift", "Enter"
static char **pretty_parts(const char *combo) {
    static const char *names[][2] = {
        {"SPACE", "Space"}, {"TAB", "Tab"}, {"RETURN", "Enter"}, {"ESCAPE", "Esc"}, {"PRINT", "Print"},
        {"LEFT", "←"}, {"RIGHT", "→"}, {"UP", "↑"}, {"DOWN", "↓"}, {"MOUSE_DOWN", "Scroll ↓"}, {"MOUSE_UP", "Scroll ↑"},
        {"MOUSE:272", "Left click"}, {"MOUSE:273", "Right click"}, {"MOUSE:274", "Middle click"},
        {"XF86AUDIOPLAY", "Play"}, {"XF86AUDIONEXT", "Next"}, {"XF86AUDIOPREV", "Previous"},
        {"XF86AUDIORAISEVOLUME", "Volume +"}, {"XF86AUDIOLOWERVOLUME", "Volume −"}, {"XF86AUDIOMUTE", "Mute"},
        {"XF86MONBRIGHTNESSUP", "Brightness +"}, {"XF86MONBRIGHTNESSDOWN", "Brightness −"}};
    static const char *mod_title[] = {"Super", "Ctrl", "Alt", "Shift"};
    int mods;
    char *key;
    split_combo(combo, &mods, &key);
    GPtrArray *out = g_ptr_array_new();
    for (int i = 0; i < 4; i++) if (mods & (1 << i)) g_ptr_array_add(out, g_strdup(mod_title[i]));
    if (*key) {
        char *up = g_ascii_strup(key, -1), *label = NULL;
        for (size_t i = 0; i < G_N_ELEMENTS(names) && !label; i++)
            if (!strcmp(names[i][0], up)) label = g_strdup(names[i][1]);
        g_ptr_array_add(out, label ? label : strlen(key) > 1 ? g_strdup(key) : g_strdup(up));
        g_free(up);
    }
    g_free(key);
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

static char *lua_str(const char *s) {
    GString *o = g_string_new("\"");
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '"') g_string_append_c(o, '\\');
        g_string_append_c(o, *p);
    }
    g_string_append_c(o, '"');
    return g_string_free(o, FALSE);
}

// group n of a match, NULL when it took no part (Python's None)
static char *grp(GMatchInfo *m, int n) {
    int s, e;
    if (!g_match_info_fetch_pos(m, n, &s, &e) || s < 0) return NULL;
    return g_match_info_fetch(m, n);
}

// ---------------------------------------------------------------- reading
static GRegex *BIND_RE, *LOCALSTR_RE, *KEYDEF_RE, *DESC_RE, *SECTION_RE, *LOCALFMT_RE, *MAINMOD_RE, *KEYTABLE_RE;

static void kb_regexes(void) {
    if (BIND_RE) return;
    BIND_RE = g_regex_new("^(\\s*)hl\\.bind\\(\\s*(KEY\\.(\\w+)|\"([^\"]*)\"|\\(\"([^\"]*)\"\\):format\\(mainMod\\)|(\\w+))"
                          "\\s*,\\s*(.*)\\)\\s*$", 0, 0, NULL);
    LOCALSTR_RE = g_regex_new("^\\s*local\\s+(\\w+)\\s*=\\s*(?:(\\w+)\\s*\\.\\.\\s*)?\"([^\"]*)\"\\s*$", 0, 0, NULL);
    KEYDEF_RE = g_regex_new("^(\\s*)(\\w+)\\s*=\\s*(\\(\"([^\"]*)\"\\):format\\(mainMod\\)|\"([^\"]*)\")\\s*,\\s*$", 0, 0, NULL);
    DESC_RE = g_regex_new("description\\s*=\\s*\"((?:[^\"\\\\]|\\\\.)*)\"", 0, 0, NULL);
    SECTION_RE = g_regex_new("^\\s*--\\s*\\d+\\.\\s*(.+?)\\s*$", 0, 0, NULL);
    LOCALFMT_RE = g_regex_new("^\\s*local\\s+(\\w+)\\s*=\\s*\\(\"([^\"]*)\"\\):format\\(mainMod,\\s*\\w+\\)", 0, 0, NULL);
    MAINMOD_RE = g_regex_new("local\\s+mainMod\\s*=\\s*\"([^\"]+)\"", 0, 0, NULL);
    KEYTABLE_RE = g_regex_new("^\\s*local\\s+KEY\\s*=\\s*\\{", 0, 0, NULL);
}

static char *replace_all(const char *s, const char *a, const char *b) {
    char **v = g_strsplit(s, a, -1), *r = g_strjoinv(b, v);
    g_strfreev(v);
    return r;
}

// "hl.dsp.exec_cmd(SHELLMSG .. " launcher")" -> "Shell: launcher"
static char *kb_summarize(KeybindFile *kf, const char *action) {
    char *a = g_strstrip(g_strdup(action));
    size_t n = strlen(a);
    while (n && a[n - 1] == ',') a[--n] = 0;
    g_strstrip(a);
    static GRegex *fn_re, *exec_re;
    if (!fn_re) {
        fn_re = g_regex_new("^function\\(\\)\\s*hl\\.exec_cmd\\((.*)\\)\\s*end$", 0, 0, NULL);
        exec_re = g_regex_new("^hl\\.dsp\\.exec_cmd\\((.*)\\)$", 0, 0, NULL);
    }
    GMatchInfo *m;
    if (g_regex_match(fn_re, a, 0, &m)) {
        char *inner = g_match_info_fetch(m, 1), *b = g_strdup_printf("hl.dsp.exec_cmd(%s)", inner);
        g_free(inner); g_free(a);
        a = b;
    }
    g_match_info_free(m);
    char *res;
    if (g_regex_match(exec_re, a, 0, &m)) {
        char *inner = g_match_info_fetch(m, 1), **parts = g_strsplit(inner, "..", -1);
        GString *cmd = g_string_new(NULL);
        for (char **p = parts; *p; p++) {
            char *t = g_strstrip(*p);
            size_t l = strlen(t);
            if (l >= 2 && t[0] == '"' && t[l - 1] == '"') g_string_append_len(cmd, t + 1, l - 2);
            else {
                const char *v = g_hash_table_lookup(kf->vars, t);
                g_string_append(cmd, v ? v : t);
            }
        }
        char *c = replace_all(cmd->str, g_get_home_dir(), "~");
        res = g_str_has_prefix(c, "hyprminimal-msg ") ? g_strdup_printf("Shell: %s", c + strlen("hyprminimal-msg "))
                                                       : g_strdup_printf("Run: %s", c);
        g_free(c); g_string_free(cmd, TRUE); g_strfreev(parts); g_free(inner);
    } else res = g_strdup(g_str_has_prefix(a, "hl.dsp.") ? a + 7 : a);
    g_match_info_free(m);
    g_free(a);
    return res;
}

static void kb_index(KeybindFile *kf) {
    g_hash_table_remove_all(kf->norm_count);
    for (guint i = 0; i < kf->binds->len; i++) {
        Keybind *b = g_ptr_array_index(kf->binds, i);
        if (b->readonly) continue;
        char *n = norm_combo(b->combo);
        g_hash_table_insert(kf->norm_count, n, GINT_TO_POINTER(GPOINTER_TO_INT(g_hash_table_lookup(kf->norm_count, n)) + 1));
    }
}

static gboolean kb_reload(KeybindFile *kf, GError **err) {
    kb_regexes();
    char *text = NULL;
    if (!g_file_get_contents(kf->path, &text, NULL, err)) return FALSE;
    g_ptr_array_set_size(kf->lines, 0);
    char **v = g_strsplit(text, "\n", -1);
    for (char **l = v; *l; l++) g_ptr_array_add(kf->lines, g_strdup(*l));
    g_strfreev(v);
    GMatchInfo *m;
    g_free(kf->main_mod);
    kf->main_mod = g_regex_match(MAINMOD_RE, text, 0, &m) ? g_match_info_fetch(m, 1) : g_strdup("SUPER");
    g_match_info_free(m);
    g_free(text);
    g_hash_table_remove_all(kf->keydefs);
    g_hash_table_remove_all(kf->vars);
    g_ptr_array_set_size(kf->binds, 0);
    g_hash_table_insert(kf->vars, g_strdup("scrPath"), g_strdup("~/.config/hypr/Scripts"));
    g_hash_table_insert(kf->vars, g_strdup("SHELLMSG"), g_strdup("hyprminimal-msg"));
    GHashTable *local_fmts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char *section = g_strdup("Other");
    gboolean in_key_table = FALSE;
    for (guint i = 0; i < kf->lines->len; i++) {
        const char *raw = g_ptr_array_index(kf->lines, i);
        if (g_regex_match(KEYTABLE_RE, raw, 0, NULL)) { in_key_table = TRUE; continue; }
        if (in_key_table) {
            const char *t = raw;
            while (g_ascii_isspace(*t)) t++;
            if (*t == '}') { in_key_table = FALSE; continue; }
            if (g_regex_match(KEYDEF_RE, raw, 0, &m)) {
                char *name = g_match_info_fetch(m, 2), *f = grp(m, 4), *lit = grp(m, 5);
                KeyDef *k = g_new0(KeyDef, 1);
                k->line = i;
                k->combo = f ? replace_all(f, "%s", kf->main_mod) : g_strdup(lit ? lit : "");
                g_hash_table_insert(kf->keydefs, name, k);
                g_free(f); g_free(lit);
            }
            g_match_info_free(m);
            continue;
        }
        if (g_regex_match(SECTION_RE, raw, 0, &m)) {
            g_free(section);
            section = g_match_info_fetch(m, 1);
            g_match_info_free(m);
            continue;
        }
        g_match_info_free(m);
        if (g_regex_match(LOCALSTR_RE, raw, 0, &m)) {
            char *name = g_match_info_fetch(m, 1), *pre = grp(m, 2), *val = g_match_info_fetch(m, 3);
            if (strcmp(name, "mainMod")) {
                const char *pv = pre ? g_hash_table_lookup(kf->vars, pre) : NULL;
                g_hash_table_insert(kf->vars, g_strdup(name), g_strconcat(pv ? pv : "", val, NULL));
            }
            g_free(name); g_free(pre); g_free(val);
        }
        g_match_info_free(m);
        if (g_regex_match(LOCALFMT_RE, raw, 0, &m)) {
            char *name = g_match_info_fetch(m, 1), *f = g_match_info_fetch(m, 2);
            char *a = replace_all(f, "%s", kf->main_mod), *b = replace_all(a, "%d", "1…9");
            g_hash_table_insert(local_fmts, name, b);
            g_free(a); g_free(f);
            g_match_info_free(m);
            continue;
        }
        g_match_info_free(m);
        const char *lt = raw;
        while (g_ascii_isspace(*lt)) lt++;
        if (!g_regex_match(BIND_RE, raw, 0, &m) || g_str_has_prefix(lt, "--")) { g_match_info_free(m); continue; }
        char *rest = g_match_info_fetch(m, 7), *desc = g_strdup("");
        GMatchInfo *dm;
        if (g_regex_match(DESC_RE, rest, 0, &dm)) { g_free(desc); char *d = g_match_info_fetch(dm, 1); desc = replace_all(d, "\\\"", "\""); g_free(d); }
        g_match_info_free(dm);
        // the action is everything before the last ", {" (the options table)
        char *action = g_strdup(rest);
        static GRegex *opt_re;
        if (!opt_re) opt_re = g_regex_new(",\\s*\\{", 0, 0, NULL);
        GMatchInfo *om;
        int last = -1;
        g_regex_match(opt_re, rest, 0, &om);
        for (; g_match_info_matches(om); g_match_info_next(om, NULL)) { int s, e; g_match_info_fetch_pos(om, 0, &s, &e); last = s; }
        g_match_info_free(om);
        if (last >= 0) action[last] = 0;
        char *summary = kb_summarize(kf, action);
        g_free(action);
        Keybind *b = g_new0(Keybind, 1);
        b->line = i;
        b->key_ref = grp(m, 3);
        b->literal = grp(m, 4);
        char *fmt = grp(m, 5), *word = grp(m, 6);
        if (b->key_ref) {
            KeyDef *kd = g_hash_table_lookup(kf->keydefs, b->key_ref);
            b->combo = g_strdup(kd ? kd->combo : b->key_ref);
            b->readonly = kd == NULL;
        } else if (b->literal) b->combo = g_strdup(b->literal);
        else if (fmt) { b->fmt = fmt; fmt = NULL; b->combo = replace_all(b->fmt, "%s", kf->main_mod); }
        else {
            const char *lf = word ? g_hash_table_lookup(local_fmts, word) : NULL;
            b->combo = g_strdup(lf ? lf : word ? word : "");
            b->readonly = TRUE;
            const char *after = strstr(rest, "description");
            char *tail = g_strndup(after ? after : rest, 40);
            size_t dl = strlen(desc);
            if ((dl && desc[dl - 1] == ' ') || strstr(tail, " .. ")) {
                char *d = g_strconcat(g_strchomp(desc), " 1–9", NULL);
                g_free(desc);
                desc = d;
            }
            g_free(tail);
        }
        g_free(fmt); g_free(word);
        b->description = desc;
        if (g_utf8_strlen(summary, -1) > 90) { char *c = g_utf8_substring(summary, 0, 90); g_free(summary); summary = c; }
        b->action = summary;
        b->section = g_strdup(section);
        g_ptr_array_add(kf->binds, b);
        g_free(rest);
        g_match_info_free(m);
    }
    g_free(section);
    g_hash_table_unref(local_fmts);
    kb_index(kf);
    return TRUE;
}

static KeybindFile *kb_open(GError **err) {
    KeybindFile *kf = g_new0(KeybindFile, 1);
    kf->path = g_build_filename(g_get_home_dir(), ".config", "hypr", "keybind.lua", NULL);
    kf->lines = g_ptr_array_new_with_free_func(g_free);
    kf->keydefs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, keydef_free);
    kf->vars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    kf->binds = g_ptr_array_new_with_free_func(keybind_free);
    kf->norm_count = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!kb_reload(kf, err)) {
        g_ptr_array_unref(kf->lines); g_hash_table_unref(kf->keydefs); g_hash_table_unref(kf->vars);
        g_ptr_array_unref(kf->binds); g_hash_table_unref(kf->norm_count); g_free(kf->path); g_free(kf);
        return NULL;
    }
    return kf;
}

static gboolean kb_has_conflict(KeybindFile *kf, Keybind *b) {
    if (b->readonly) return FALSE;
    char *n = norm_combo(b->combo);
    int c = GPOINTER_TO_INT(g_hash_table_lookup(kf->norm_count, n));
    g_free(n);
    return c > 1;
}

// editable binds using combo (but not exclude): "desc, desc" or NULL
static char *kb_conflicts(KeybindFile *kf, const char *combo, Keybind *exclude) {
    char *n = norm_combo(combo);
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < kf->binds->len; i++) {
        Keybind *b = g_ptr_array_index(kf->binds, i);
        if (b == exclude || b->readonly) continue;
        char *bn = norm_combo(b->combo);
        if (!strcmp(bn, n)) g_string_append_printf(s, "%s%s", s->len ? ", " : "", *b->description ? b->description : b->action);
        g_free(bn);
    }
    g_free(n);
    return s->len ? g_string_free(s, FALSE) : (g_string_free(s, TRUE), NULL);
}

// ---------------------------------------------------------------- editing (then kb_save)
static void kb_set_line(KeybindFile *kf, int i, char *text) {   // takes text
    g_free(g_ptr_array_index(kf->lines, i));
    g_ptr_array_index(kf->lines, i) = text;
}

static char *kb_combo_lua(KeybindFile *kf, const char *combo) {
    int mods;
    char *key;
    split_combo(combo, &mods, &key);
    char *mm = g_ascii_strup(kf->main_mod, -1), *r;
    int main_bit = mod_index(mm);
    int first = -1;
    for (int i = 0; i < 4 && first < 0; i++) if (mods & (1 << i)) first = i;
    if (first >= 0 && first == main_bit) {
        char *rest = join_combo(mods & ~(1 << first), key);
        r = g_strdup_printf("(\"%%s + %s\"):format(mainMod)", rest);
        g_free(rest);
    } else {
        char *j = join_combo(mods, key);
        r = lua_str(j);
        g_free(j);
    }
    g_free(mm);
    g_free(key);
    return r;
}

static void kb_set_combo(KeybindFile *kf, Keybind *b, const char *combo) {
    KeyDef *kd = b->key_ref ? g_hash_table_lookup(kf->keydefs, b->key_ref) : NULL;
    if (kd) {
        GMatchInfo *m;
        if (g_regex_match(KEYDEF_RE, g_ptr_array_index(kf->lines, kd->line), 0, &m)) {
            char *ind = g_match_info_fetch(m, 1), *cl = kb_combo_lua(kf, combo);
            kb_set_line(kf, kd->line, g_strdup_printf("%s%s = %s,", ind, b->key_ref, cl));
            g_free(ind); g_free(cl);
        }
        g_match_info_free(m);
        return;
    }
    const char *line = g_ptr_array_index(kf->lines, b->line);
    char *with;
    GRegex *re;
    if (b->fmt) {
        re = g_regex_new("hl\\.bind\\(\\s*\\(\"[^\"]*\"\\):format\\(mainMod\\)", 0, 0, NULL);
        char *cl = kb_combo_lua(kf, combo);
        with = g_strconcat("hl.bind(", cl, NULL);
        g_free(cl);
    } else {
        re = g_regex_new("hl\\.bind\\(\\s*\"[^\"]*\"", 0, 0, NULL);
        char *c = canon_combo(combo), *ls = lua_str(c);
        with = g_strconcat("hl.bind(", ls, NULL);
        g_free(c); g_free(ls);
    }
    GMatchInfo *m;
    if (g_regex_match(re, line, 0, &m)) {
        int s, e;
        g_match_info_fetch_pos(m, 0, &s, &e);
        kb_set_line(kf, b->line, g_strdup_printf("%.*s%s%s", s, line, with, line + e));
    }
    g_match_info_free(m);
    g_regex_unref(re);
    g_free(with);
}

static void kb_set_description(KeybindFile *kf, Keybind *b, const char *desc) {
    const char *line = g_ptr_array_index(kf->lines, b->line);
    char *ls = lua_str(desc);
    GMatchInfo *m;
    if (g_regex_match(DESC_RE, line, 0, &m)) {
        int s, e;
        g_match_info_fetch_pos(m, 0, &s, &e);
        kb_set_line(kf, b->line, g_strdup_printf("%.*sdescription = %s%s", s, line, ls, line + e));
    } else {
        char *t = g_strchomp(g_strdup(line)), *close = strrchr(t, ')');
        if (close) { *close = 0; kb_set_line(kf, b->line, g_strdup_printf("%s, { description = %s })", t, ls)); }
        g_free(t);
    }
    g_match_info_free(m);
    g_free(ls);
}

static void kb_remove(KeybindFile *kf, Keybind *b) {
    const char *line = g_ptr_array_index(kf->lines, b->line);
    while (g_ascii_isspace(*line)) line++;
    kb_set_line(kf, b->line, g_strdup_printf("-- [removed in hyprminimal settings] %s", line));
}

static void kb_add(KeybindFile *kf, const char *combo, const char *desc, const char *command) {
    static const char *marker = "-- Custom shortcuts (added in hyprminimal settings)";
    char *c = canon_combo(combo), *lc = lua_str(c), *lcmd = lua_str(command), *ld = lua_str(desc);
    char *bind = g_strdup_printf("hl.bind(%s, hl.dsp.exec_cmd(%s), { description = %s })", lc, lcmd, ld);
    g_free(c); g_free(lc); g_free(lcmd); g_free(ld);
    int mi = -1;
    for (guint i = 0; i < kf->lines->len; i++) if (!strcmp(g_ptr_array_index(kf->lines, i), marker)) { mi = i; break; }
    if (mi < 0) {
        int ret = kf->lines->len;
        for (guint i = 0; i < kf->lines->len; i++) {
            char *t = g_strstrip(g_strdup(g_ptr_array_index(kf->lines, i)));
            if (!strcmp(t, "return true")) ret = i;
            g_free(t);
        }
        g_ptr_array_insert(kf->lines, ret, g_strdup(marker));
        g_ptr_array_insert(kf->lines, ret, g_strdup(""));
        mi = ret + 1;
    }
    guint j = mi + 1;
    while (j < kf->lines->len && g_str_has_prefix(g_ptr_array_index(kf->lines, j), "hl.bind(")) j++;
    g_ptr_array_insert(kf->lines, j, bind);
}

static char *hypr_ctl(const char *cmd);   // Hyprland IPC request (window.c)

// Write, reload Hyprland, roll back if Hyprland reports config errors. Returns the error text or NULL.
// Blocks ~0.5 s: worker thread.
static char *kb_write_checked(const char *path, const char *text) {
    char *old = NULL;
    if (!g_file_get_contents(path, &old, NULL, NULL)) return g_strdup("cannot read keybind.lua");
    char *dir = g_build_filename(g_get_home_dir(), ".config", "hypr", ".pre-hyprminimal-backup", NULL);
    g_mkdir_with_parents(dir, 0755);
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S"), *bname = g_strdup_printf("keybind.lua.%s", stamp);
    char *bpath = g_build_filename(dir, bname, NULL);
    g_file_set_contents(bpath, old, -1, NULL);
    g_date_time_unref(now);
    g_free(stamp); g_free(bname); g_free(bpath); g_free(dir);
    char *err = NULL;
    if (!g_file_set_contents(path, text, -1, NULL)) err = g_strdup("cannot write keybind.lua");
    else {
        g_free(hypr_ctl("reload"));
        g_usleep(400000);
        char *e = hypr_ctl("configerrors");
        if (e && *g_strstrip(e)) {
            err = e;
            e = NULL;
            g_file_set_contents(path, old, -1, NULL);
            g_free(hypr_ctl("reload"));
        }
        g_free(e);
    }
    g_free(old);
    return err;
}

static char *kb_text(KeybindFile *kf) {
    g_ptr_array_add(kf->lines, NULL);
    char *t = g_strjoinv("\n", (char **)kf->lines->pdata);
    g_ptr_array_remove_index(kf->lines, kf->lines->len - 1);
    return t;
}
