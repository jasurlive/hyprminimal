// hyprminimal themes (all dark): Charcoal + Hygge, Spruce, Fjord (from the django-react template,
// front/src/css/theme.css) and Tokyo Night, turned into the colour roles every hyprminimal part uses.
// Solid surfaces only: no blur, no translucency.
//
// The repo's own style files are written in Tokyo Night Moon. recolor() swaps each of those colours for the
// chosen theme's colour in the same role, so every file (waybar, panels: popups, notifications, OSD, launcher,
// Settings; hyprlock, kitty) follows the theme without keeping eight copies of each one.

#define THEME_DEFAULT "charcoal"

typedef struct {
    const char *id, *label, *desc;
    const char *bg, *bg_elev, *surface, *text, *text_strong, *muted, *accent, *accent_hover, *danger;
} Theme;

static const Theme THEMES[] = {   // the order Settings lists them in
    {"charcoal", "Charcoal", "Neutral dark grey, soft grey accent",
     "#181818", "#202020", "#292929", "#d4d4d4", "#f5f5f5", "#999999", "#a3a3a3", "#d4d4d4", "#ef7777"},
    {"hygge", "Hygge", "Matte slate, linen text, smoked-oak accent",
     "#1d2023", "#23272a", "#2a2e32", "#d6d0c6", "#eee9e1", "#8f8a82", "#c4a57c", "#d6bc98", "#d08a80"},
    {"spruce", "Spruce", "Nordic forest at dusk, soft moss accent",
     "#1a1f1d", "#202624", "#27302c", "#cfd5cd", "#e9eee6", "#869087", "#9db59a", "#b6cbb3", "#d08a80"},
    {"fjord", "Fjord", "Deep slate-blue night, misty blue accent",
     "#191e24", "#1f252c", "#262d35", "#cdd4dc", "#e8edf2", "#858f9a", "#93abc2", "#afc3d6", "#d08a80"},
    // hyprminimal's original palette (Tokyo Night Moon): the repo's style files are written in it
    {"tokyo", "Tokyo Night", "Tokyo Night Moon, periwinkle and pink",
     "#1f2335", "#262a40", "#2c314a", "#c0caf5", "#c0caf5", "#a9b1d6", "#7a88cf", "#9aa5e0", "#f7768e"},
};

enum {
    R_PRIMARY, R_ACCENT_TEXT, R_SECONDARY, R_TERTIARY, R_ERROR, R_WARNING, R_SURFACE, R_SIDEBAR, R_DIALOG,
    R_CARD, R_SURFACE_VAR, R_BORDER_SOFT, R_OUTLINE, R_PLACEHOLDER, R_ON_SURFACE, R_ON_SURFACE_VAR, R_SHADOW,
    R_STATE_CHARGE, R_STATE_WARN, R_STATE_CRIT, R_STATE_LOW, R_STATE_BOLT, R_LINEN, R_WALL_BG, R_ON_PRIMARY,
    R_COUNT
};
static const char *ROLE_NAME[R_COUNT] = {
    "primary", "accent_text", "secondary", "tertiary", "error", "warning", "surface", "sidebar", "dialog",
    "card", "surface_var", "border_soft", "outline", "placeholder", "on_surface", "on_surface_var", "shadow",
    "state_charge", "state_warn", "state_crit", "state_low", "state_bolt", "linen", "wall_bg", "on_primary",
};
// Tokyo Night Moon colour (as written in the repo's files) of each role; on_primary has none
static const char *TOKYO[R_COUNT] = {
    "7a88cf", "9aa5e0", "d7729f", "9cd58a", "f7768e", "e6c384", "1f2335", "1a1e2e", "24283b",
    "262a40", "2c314a", "343a5c", "4b517a", "737aa2", "a9b1d6", "c0caf5", "181b2a",
    "8ea4c8", "c8b48e", "cf8f9a", "e5534b", "e8d5a6", "e6ddd0", "222436", NULL,
};
// rgba(r, g, b, a) spellings
static const struct { int r, g, b, role; } TOKYO_RGB[] = {
    {31, 35, 53, R_SURFACE}, {44, 49, 74, R_SURFACE_VAR}, {122, 136, 207, R_PRIMARY}, {24, 27, 42, R_SHADOW},
    {192, 202, 245, R_ON_SURFACE_VAR}, {169, 177, 214, R_ON_SURFACE}, {247, 118, 142, R_ERROR},
};

typedef struct { char c[R_COUNT][8]; const Theme *t; } Roles;

static const Theme *theme_get(const char *id) {
    for (size_t i = 0; i < G_N_ELEMENTS(THEMES); i++)
        if (id && !strcmp(THEMES[i].id, id)) return &THEMES[i];
    return &THEMES[0];
}

static void hex_rgb(const char *h, int rgb[3]) {
    if (*h == '#') h++;
    for (int i = 0; i < 3; i++) {
        char b[3] = {h[2 * i], h[2 * i + 1], 0};
        rgb[i] = (int)strtol(b, NULL, 16);
    }
}

// a blended toward b by t (0..1); rounds half to even like Python's round()
static void mix(char out[8], const char *a, const char *b, double t) {
    int x[3], y[3], o[3];
    hex_rgb(a, x);
    hex_rgb(b, y);
    for (int i = 0; i < 3; i++) o[i] = CLAMP((int)rint(x[i] + (y[i] - x[i]) * t), 0, 255);
    snprintf(out, 8, "#%02x%02x%02x", o[0], o[1], o[2]);
}

static void roles(const char *id, Roles *r) {
    const Theme *t = theme_get(id);
    r->t = t;
    if (!strcmp(t->id, "tokyo")) {   // exactly the original colours: recolor() leaves every file as it is
        for (int i = 0; i < R_COUNT; i++)
            if (TOKYO[i]) snprintf(r->c[i], 8, "#%s", TOKYO[i]);
        snprintf(r->c[R_ON_PRIMARY], 8, "#1f2335");
        return;
    }
    const char *bg = t->bg, *ts = t->text_strong, *tx = t->text;
#define SET(role, v) snprintf(r->c[role], 8, "%s", v)
    SET(R_PRIMARY, t->accent);
    mix(r->c[R_ACCENT_TEXT], t->accent, ts, 0.3);
    SET(R_ON_PRIMARY, bg);
    mix(r->c[R_SECONDARY], t->accent, ts, 0.4);
    SET(R_TERTIARY, "#9cd58a");
    SET(R_ERROR, t->danger);
    SET(R_WARNING, "#e6c384");
    SET(R_SURFACE, bg);
    mix(r->c[R_SIDEBAR], bg, "#000000", 0.2);
    mix(r->c[R_DIALOG], bg, ts, 0.05);
    SET(R_CARD, t->bg_elev);
    SET(R_SURFACE_VAR, t->surface);
    mix(r->c[R_BORDER_SOFT], bg, ts, 0.13);
    mix(r->c[R_OUTLINE], bg, ts, 0.24);
    mix(r->c[R_PLACEHOLDER], bg, t->muted, 0.8);
    mix(r->c[R_ON_SURFACE], tx, t->muted, 0.35);
    SET(R_ON_SURFACE_VAR, tx);
    mix(r->c[R_SHADOW], bg, "#000000", 0.4);
    mix(r->c[R_STATE_CHARGE], t->accent, tx, 0.35);
    SET(R_STATE_WARN, "#c8b48e");
    mix(r->c[R_STATE_CRIT], t->danger, bg, 0.2);
    SET(R_STATE_LOW, t->danger);
    SET(R_STATE_BOLT, "#e8d5a6");
    SET(R_LINEN, tx);
    mix(r->c[R_WALL_BG], bg, "#000000", 0.1);
#undef SET
}

static gboolean recolor_hex(const GMatchInfo *m, GString *out, gpointer d) {
    const Roles *r = d;
    char *raw = g_match_info_fetch(m, 1), *hex = g_ascii_strdown(raw, -1);
    g_free(raw);
    for (int i = 0; i < R_COUNT; i++)
        if (TOKYO[i] && !strcmp(TOKYO[i], hex)) { g_string_append(out, r->c[i] + 1); break; }
    g_free(hex);
    return FALSE;
}

static gboolean recolor_rgb(const GMatchInfo *m, GString *out, gpointer d) {
    const Roles *r = d;
    char *all = g_match_info_fetch(m, 0), *v[5];
    for (int i = 1; i <= 4; i++) v[i] = g_match_info_fetch(m, i);
    int key[3] = {atoi(v[1]), atoi(v[2]), atoi(v[3])}, role = -1;
    for (size_t i = 0; i < G_N_ELEMENTS(TOKYO_RGB); i++)
        if (TOKYO_RGB[i].r == key[0] && TOKYO_RGB[i].g == key[1] && TOKYO_RGB[i].b == key[2]) role = TOKYO_RGB[i].role;
    if (role < 0) g_string_append(out, all);
    else {
        int c[3];
        hex_rgb(r->c[role], c);
        char *head = g_strndup(all, strchr(all, '(') - all);
        g_string_append_printf(out, "%s(%d, %d, %d%s", head, c[0], c[1], c[2], v[4]);
        g_free(head);
    }
    g_free(all);
    for (int i = 1; i <= 4; i++) g_free(v[i]);
    return FALSE;
}

// swap the repo's Tokyo Night Moon colours for theme id's, role by role
static char *recolor(const char *text, const char *id) {
    static GRegex *hex_re, *rgb_re;
    if (!hex_re) {
        GString *p = g_string_new("(?<![0-9a-zA-Z])(");
        for (int i = 0, n = 0; i < R_COUNT; i++)
            if (TOKYO[i]) g_string_append_printf(p, "%s%s", n++ ? "|" : "", TOKYO[i]);
        g_string_append(p, ")");
        hex_re = g_regex_new(p->str, G_REGEX_CASELESS, 0, NULL);
        g_string_free(p, TRUE);
        rgb_re = g_regex_new("rgba?\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*(,|\\))", 0, 0, NULL);
    }
    Roles r;
    roles(id, &r);
    char *a = g_regex_replace_eval(hex_re, text, -1, 0, 0, recolor_hex, &r, NULL);
    char *b = g_regex_replace_eval(rgb_re, a, -1, 0, 0, recolor_rgb, &r, NULL);
    g_free(a);
    return b;
}

// GTK @define-color block for waybar (replaces colors/palette.css)
static char *palette_css(const char *id) {
    static const int keys[] = {R_PRIMARY, R_ON_PRIMARY, R_SECONDARY, R_TERTIARY, R_ERROR, R_WARNING, R_SURFACE,
                               R_SURFACE_VAR, R_ON_SURFACE, R_ON_SURFACE_VAR, R_OUTLINE, R_SHADOW, R_STATE_CHARGE,
                               R_STATE_WARN, R_STATE_CRIT, R_STATE_LOW, R_STATE_BOLT, R_LINEN};
    Roles r;
    roles(id, &r);
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "/* hyprminimal palette: %s (generated by hyprminimal-settings) */", r.t->label);
    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++)
        g_string_append_printf(s, "\n@define-color %-14s %s;", ROLE_NAME[keys[i]], r.c[keys[i]]);
    g_string_append_c(s, '\n');
    return g_string_free(s, FALSE);
}

// Hyprland window colours as a Lua chunk, loaded by hypr/layerrules.lua
static char *hypr_lua(const char *id) {
    Roles r;
    roles(id, &r);
#define H(role) (r.c[role] + 1)
    return g_strdup_printf(
        "-- generated by hyprminimal-settings (theme: %s), edit Settings -> General instead\n"
        "hl.config({\n"
        "    general = { col = {\n"
        "        active_border = \"rgb(%s)\", inactive_border = \"rgb(%s)\",\n"
        "    } },\n"
        "    group = {\n"
        "        col = { border_active = \"rgb(%s)\", border_inactive = \"rgb(%s)\",\n"
        "                border_locked_active = \"rgb(%s)\", border_locked_inactive = \"rgb(%s)\" },\n"
        "        groupbar = { col = { active = \"rgb(%s)\", inactive = \"rgb(%s)\",\n"
        "                             locked_active = \"rgb(%s)\", locked_inactive = \"rgb(%s)\" },\n"
        "                     text_color = \"rgb(%s)\", text_color_inactive = \"rgb(%s)\" },\n"
        "    },\n"
        "    misc = { background_color = \"rgb(%s)\" },\n"
        "})\n"
        "return true\n",
        r.t->label, H(R_PRIMARY), H(R_BORDER_SOFT), H(R_SECONDARY), H(R_BORDER_SOFT), H(R_ERROR), H(R_BORDER_SOFT),
        H(R_SECONDARY), H(R_SURFACE_VAR), H(R_ERROR), H(R_SURFACE_VAR), H(R_ON_PRIMARY), H(R_ON_SURFACE), H(R_WALL_BG));
#undef H
}

// terminal ANSI colours: one calm set, and Tokyo's own
static const char *ANSI_DARK[16] = {"#1c1c1c", "#e06c75", "#98c379", "#d7b46a", "#7aa2c7", "#b48ead", "#78b6ad", "#c8c8c8",
                                    "#5c5c5c", "#ef8a92", "#b0d696", "#e5c88a", "#98bbdc", "#c9a6c4", "#97ccc4", "#f0f0f0"};
static const char *ANSI_TOKYO[16] = {"#181b2a", "#f7768e", "#9cd58a", "#e6c384", "#7a88cf", "#d7729f", "#86c1d6", "#a9b1d6",
                                     "#4b517a", "#cf8f9a", "#b4e3a4", "#e8d5a6", "#8ea4c8", "#e391b5", "#a6d4e4", "#c0caf5"};

static char *kitty_conf(const char *id) {
    Roles r;
    roles(id, &r);
    const Theme *t = r.t;
    const char **ansi = !strcmp(t->id, "tokyo") ? ANSI_TOKYO : ANSI_DARK;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s,
        "# hyprminimal kitty colours: %s (generated by hyprminimal-settings)\n"
        "foreground            %s\nbackground            %s\n"
        "selection_foreground  %s\nselection_background  %s\n"
        "cursor                %s\ncursor_text_color     %s\n"
        "url_color             %s\n"
        "active_border_color   %s\ninactive_border_color %s\n"
        "active_tab_foreground   %s\nactive_tab_background   %s\n"
        "inactive_tab_foreground %s\ninactive_tab_background %s\n"
        "tab_bar_background      %s\n",
        t->label, t->text, t->bg, r.c[R_ON_PRIMARY], r.c[R_PRIMARY], r.c[R_PRIMARY], r.c[R_ON_PRIMARY],
        r.c[R_STATE_CHARGE], r.c[R_PRIMARY], r.c[R_OUTLINE], r.c[R_ON_PRIMARY], r.c[R_PRIMARY],
        r.c[R_ON_SURFACE], r.c[R_SURFACE_VAR], t->bg);
    for (int i = 0; i < 16; i++) g_string_append_printf(s, "color%-2d %s\n", i, ansi[i]);
    return g_string_free(s, FALSE);
}
