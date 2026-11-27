// settings.json and everything rendered from it (.generated/*): the bar's config and CSS, popup theme,
// notification position, lock screen, startup commands, desktop logo, terminal and Hyprland colours.
// Runs inside the resident panels program (Settings) and as `panels --apply` / `--toggle-bar` (no window).

static char *SETTINGS_PATH, *RUN_DIR;
static JsonNode *CFG;                 // settings.json merged over DEFAULTS

#define BAR_HEIGHT 45   // fixed: waybar/style.css margins and the battery are placed for this height, at the top

static const char *DEFAULTS_JSON =
    "{\"general\": {\"font\": \"Noto Sans\", \"theme\": \"" THEME_DEFAULT "\"},"
    // desktop background (helpers/panels.c): the name under the logo ("" = no text), logo on/off
    " \"desktop\": {\"name\": \"hyprminimal\", \"logo\": true},"
    " \"bar\": {\"font_size\": 16, \"icon_size\": 20, \"clock_size\": 18, \"font_weight\": 500,"
    "   \"clock_24h\": true,"
    "   \"hidden\": false,"   // bar off: waybar starts hidden (no exclusive zone), windows use the full screen
    "   \"widgets\": {\"workspaces\": true, \"date\": true, \"clock\": true, \"media\": true, \"notifications\": true,"
    "     \"wifi\": true, \"bluetooth\": false, \"volume\": false, \"tray\": false, \"battery\": true,"
    "     \"network_speed\": false}},"
    " \"notifications\": {\"position\": \"top-right\", \"timeout\": 3},"
    " \"nightlight\": {\"enabled\": false, \"temperature\": 4000},"
    // run once per login by `hyprminimal start` (.generated/startup): [{"command": str, "enabled": bool}]
    " \"startup\": {\"commands\": []}}";

static const struct { const char *key, *label, *side, *module; } WIDGETS[] = {
    {"workspaces", "Workspaces", "left", "ext/workspaces"},
    {"date", "Date", "center", "clock#date"}, {"clock", "Clock", "center", "clock"},
    {"media", "Media", "right", "custom/media"}, {"notifications", "Notifications", "right", "custom/notification"},
    {"wifi", "Wi-Fi", "right", "network#wifi"}, {"bluetooth", "Bluetooth", "right", "bluetooth"},
    {"volume", "Volume", "right", "pulseaudio"}, {"tray", "System tray", "right", "tray"},
    {"battery", "Battery", "right", "battery"}, {"network_speed", "Network speed", "right", "network#speed"},
};

static void paths_init(void) {
    if (SETTINGS_PATH) return;
    SETTINGS_PATH = g_build_filename(REPO, "settings.json", NULL);
    const char *rt = getenv("XDG_RUNTIME_DIR");
    RUN_DIR = g_build_filename(rt && *rt ? rt : "/tmp", "hyprminimal", NULL);
}

// ---------------------------------------------------------------- JSON
static JsonNode *json_parse_text(const char *text, GError **err) {
    JsonParser *p = json_parser_new();
    JsonNode *n = NULL;
    if (json_parser_load_from_data(p, text, -1, err)) n = json_node_copy(json_parser_get_root(p));
    g_object_unref(p);
    return n;
}

static void deep_merge(JsonObject *base, JsonObject *over) {
    JsonObjectIter it;
    const char *k;
    JsonNode *v;
    json_object_iter_init(&it, over);
    while (json_object_iter_next(&it, &k, &v)) {
        JsonNode *b = json_object_get_member(base, k);
        if (JSON_NODE_HOLDS_OBJECT(v) && b && JSON_NODE_HOLDS_OBJECT(b))
            deep_merge(json_node_get_object(b), json_node_get_object(v));
        else json_object_set_member(base, k, json_node_copy(v));
    }
}

// Python's json.dumps(indent=2, ensure_ascii=False): the files stay readable and diffable
static void json_string_out(GString *s, const char *v) {
    g_string_append_c(s, '"');
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        switch (*p) {
        case '"': g_string_append(s, "\\\""); break;
        case '\\': g_string_append(s, "\\\\"); break;
        case '\n': g_string_append(s, "\\n"); break;
        case '\r': g_string_append(s, "\\r"); break;
        case '\t': g_string_append(s, "\\t"); break;
        case '\b': g_string_append(s, "\\b"); break;
        case '\f': g_string_append(s, "\\f"); break;
        default:
            if (*p < 0x20) g_string_append_printf(s, "\\u%04x", *p);
            else g_string_append_c(s, (char)*p);
        }
    }
    g_string_append_c(s, '"');
}

static void json_out(GString *s, JsonNode *n, int depth) {
    switch (json_node_get_node_type(n)) {
    case JSON_NODE_NULL: g_string_append(s, "null"); return;
    case JSON_NODE_VALUE: {
        GType t = json_node_get_value_type(n);
        if (t == G_TYPE_BOOLEAN) g_string_append(s, json_node_get_boolean(n) ? "true" : "false");
        else if (t == G_TYPE_INT64) g_string_append_printf(s, "%" G_GINT64_FORMAT, json_node_get_int(n));
        else if (t == G_TYPE_DOUBLE) {
            char b[G_ASCII_DTOSTR_BUF_SIZE];
            double d = json_node_get_double(n);
            for (int prec = 1; prec <= 17; prec++) {   // shortest that reads back the same (like repr)
                char fmt[8];
                snprintf(fmt, sizeof fmt, "%%.%dg", prec);
                g_ascii_formatd(b, sizeof b, fmt, d);
                if (g_ascii_strtod(b, NULL) == d) break;
            }
            g_string_append(s, b);
            if (!strpbrk(b, ".eEn")) g_string_append(s, ".0");
        } else json_string_out(s, json_node_get_string(n));
        return;
    }
    case JSON_NODE_ARRAY: {
        JsonArray *a = json_node_get_array(n);
        guint len = json_array_get_length(a);
        if (!len) { g_string_append(s, "[]"); return; }
        g_string_append(s, "[\n");
        for (guint i = 0; i < len; i++) {
            g_string_append_printf(s, "%*s", 2 * (depth + 1), "");
            json_out(s, json_array_get_element(a, i), depth + 1);
            g_string_append(s, i + 1 < len ? ",\n" : "\n");
        }
        g_string_append_printf(s, "%*s]", 2 * depth, "");
        return;
    }
    case JSON_NODE_OBJECT: {
        JsonObject *o = json_node_get_object(n);
        guint len = json_object_get_size(o), i = 0;
        if (!len) { g_string_append(s, "{}"); return; }
        g_string_append(s, "{\n");
        JsonObjectIter it;
        const char *k;
        JsonNode *v;
        json_object_iter_init_ordered(&it, o);
        while (json_object_iter_next_ordered(&it, &k, &v)) {
            g_string_append_printf(s, "%*s", 2 * (depth + 1), "");
            json_string_out(s, k);
            g_string_append(s, ": ");
            json_out(s, v, depth + 1);
            g_string_append(s, ++i < len ? ",\n" : "\n");
        }
        g_string_append_printf(s, "%*s}", 2 * depth, "");
        return;
    }
    }
}

static char *json_dump(JsonNode *n) {
    GString *s = g_string_new(NULL);
    json_out(s, n, 0);
    return g_string_free(s, FALSE);
}

// ---------------------------------------------------------------- settings access ("bar.widgets", "clock")
static JsonObject *cfg_obj(const char *sect) {
    JsonObject *o = json_node_get_object(CFG);
    char **parts = g_strsplit(sect, ".", -1);
    for (char **p = parts; *p && o; p++) {
        JsonNode *n = json_object_get_member(o, *p);
        if (!n || !JSON_NODE_HOLDS_OBJECT(n)) {   // a hand-edited file may hold anything: make it an object
            json_object_set_object_member(o, *p, json_object_new());
            n = json_object_get_member(o, *p);
        }
        o = json_node_get_object(n);
    }
    g_strfreev(parts);
    return o;
}

static JsonNode *cfg_node(const char *sect, const char *key) { return json_object_get_member(cfg_obj(sect), key); }

static gint64 cfg_int(const char *sect, const char *key) {
    JsonNode *n = cfg_node(sect, key);
    if (!n || !JSON_NODE_HOLDS_VALUE(n)) return 0;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_STRING) return g_ascii_strtoll(json_node_get_string(n), NULL, 10);
    if (t == G_TYPE_DOUBLE) return (gint64)json_node_get_double(n);
    return json_node_get_int(n);
}

static gboolean cfg_bool(const char *sect, const char *key) {
    JsonNode *n = cfg_node(sect, key);
    if (!n || !JSON_NODE_HOLDS_VALUE(n)) return FALSE;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_STRING) return *json_node_get_string(n) != 0;
    if (t == G_TYPE_DOUBLE) return json_node_get_double(n) != 0;
    return json_node_get_int(n) != 0 || (t == G_TYPE_BOOLEAN && json_node_get_boolean(n));
}

static const char *cfg_str(const char *sect, const char *key) {
    JsonNode *n = cfg_node(sect, key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
}

static void cfg_set_int(const char *sect, const char *key, gint64 v) { json_object_set_int_member(cfg_obj(sect), key, v); }
static void cfg_set_bool(const char *sect, const char *key, gboolean v) { json_object_set_boolean_member(cfg_obj(sect), key, v); }
static void cfg_set_str(const char *sect, const char *key, const char *v) { json_object_set_string_member(cfg_obj(sect), key, v); }

static JsonArray *cfg_startup(void) {
    JsonObject *o = cfg_obj("startup");
    JsonNode *n = json_object_get_member(o, "commands");
    if (!n || !JSON_NODE_HOLDS_ARRAY(n)) json_object_set_array_member(o, "commands", json_array_new());
    return json_object_get_array_member(o, "commands");
}

// ---------------------------------------------------------------- failures shown in Settings and on the bar
static char *read_error(const char *comp) {
    paths_init();
    char *p = g_build_filename(RUN_DIR, "errors", comp, NULL), *t = NULL;
    if (!g_file_get_contents(p, &t, NULL, NULL)) t = g_strdup("");
    g_free(p);
    return g_strstrip(t);
}

// record (msg) or clear (NULL) a failure, and refresh the bar's red error pill
static void write_error(const char *comp, const char *msg) {
    paths_init();
    char *dir = g_build_filename(RUN_DIR, "errors", NULL), *p = g_build_filename(dir, comp, NULL);
    gboolean changed = TRUE;
    if (msg) {
        g_mkdir_with_parents(dir, 0755);
        char *t = g_strconcat(msg, "\n", NULL);
        g_file_set_contents(p, t, -1, NULL);
        g_free(t);
    } else if (g_file_test(p, G_FILE_TEST_EXISTS)) g_unlink(p);
    else changed = FALSE;
    if (changed) {
        const char *argv[] = {"pkill", "-RTMIN+9", "-x", "waybar", NULL};
        g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL);
    }
    g_free(dir);
    g_free(p);
}

// ---------------------------------------------------------------- settings.json
static gboolean CFG_BROKEN;   // settings.json exists but isn't valid JSON: never overwrite it

// settings.json as an object node (NULL when missing or broken); a broken file sets CFG_BROKEN and the error
static JsonNode *read_settings_file(void) {
    char *text = NULL;
    JsonNode *n = NULL;
    CFG_BROKEN = FALSE;
    if (g_file_get_contents(SETTINGS_PATH, &text, NULL, NULL)) {
        GError *e = NULL;
        n = json_parse_text(text, &e);
        if (!n || !JSON_NODE_HOLDS_OBJECT(n)) {
            CFG_BROKEN = TRUE;
            char *msg = g_strdup_printf("settings.json is not valid JSON (%s); your file was not changed",
                                        e ? e->message : "not an object");
            write_error("settings", msg);
            g_free(msg);
            if (n) json_node_unref(n);
            n = NULL;
        }
        g_clear_error(&e);
    }
    g_free(text);
    return n;
}

static void load_settings(void) {
    paths_init();
    if (CFG) json_node_unref(CFG);
    CFG = json_parse_text(DEFAULTS_JSON, NULL);
    JsonNode *over = read_settings_file();
    if (over) {
        deep_merge(json_node_get_object(CFG), json_node_get_object(over));
        json_node_unref(over);
    }
}

// write path only on a real change (programs that watch it reload once), atomically
static gboolean write_if_changed(const char *path, const char *data, gssize len) {
    if (len < 0) len = strlen(data);
    char *old = NULL;
    gsize olen = 0;
    gboolean same = g_file_get_contents(path, &old, &olen, NULL) && olen == (gsize)len && !memcmp(old, data, len);
    g_free(old);
    if (same) return TRUE;
    return g_file_set_contents(path, data, len, NULL);
}

// FALSE: settings.json is broken on disk (also when it broke while Settings was open), nothing written
static gboolean save_settings(void) {
    JsonNode *disk = read_settings_file();
    if (disk) json_node_unref(disk);
    if (CFG_BROKEN) return FALSE;   // fix or delete settings.json first
    char *s = json_dump(CFG), *t = g_strconcat(s, "\n", NULL);
    write_if_changed(SETTINGS_PATH, t, -1);
    g_free(s);
    g_free(t);
    return TRUE;
}

static void gen_write(const char *name, const char *text) {
    char *p = g_build_filename(GEN, name, NULL);
    write_if_changed(p, text, -1);
    g_free(p);
}

// ---------------------------------------------------------------- pieces of the bar's CSS
static char *battery_css(int bar_h, int icon, int weight) {
    // CSS-drawn battery: an outline + nub with the percentage inside; pixel-placed for the bar height
    int h = MAX(12, (int)rint(icon * 0.8)), w = (int)rint(h * 1.8), st = icon >= 16 ? 2 : 1;
    int nub_w = 2, nub_h = MAX(4, h / 2), y = (bar_h - h) / 2;
    int rects[5][4] = {{0, y, w, st}, {0, y + h - st, w, st}, {0, y, st, h}, {w - st, y, st, h},
                       {w, y + (h - nub_h) / 2, nub_w, nub_h}};
    GString *pos = g_string_new(NULL), *size = g_string_new(NULL);
    for (int i = 0; i < 5; i++) {
        g_string_append_printf(pos, "%s%dpx %dpx", i ? ", " : "", rects[i][0], rects[i][1]);
        g_string_append_printf(size, "%s%dpx %dpx", i ? ", " : "", rects[i][2], rects[i][3]);
    }
#define BLOCK(col) do { g_string_append(s, "  background-image: "); \
        for (int i = 0; i < 5; i++) g_string_append_printf(s, "%slinear-gradient(%s, %s)", i ? ", " : "", col, col); \
        g_string_append(s, ";\n"); } while (0)
    GString *s = g_string_new("#battery {\n");
    g_string_append_printf(s, "  min-width: %dpx; padding: 0 %dpx 0 0;\n", w, nub_w + 2);
    g_string_append_printf(s, "  font-size: %dpx; font-weight: %d;\n", MAX(9, (int)rint(h * 0.68)), MAX(600, weight));
    g_string_append(s, "  background-repeat: no-repeat;\n");
    BLOCK("@on_surface");
    g_string_append_printf(s, "  background-position: %s;\n  background-size: %s;\n}\n", pos->str, size->str);
    // low battery is red; charging comes last so it wins over warning/critical
    static const char *cls[][2] = {{"warning", "@state_low"}, {"critical", "@state_low"},
                                   {"charging", "@state_charge"}, {"plugged", "@state_charge"}};
    for (int c = 0; c < 4; c++) {
        g_string_append_printf(s, "#battery.%s { color: %s;\n", cls[c][0], cls[c][1]);
        BLOCK(cls[c][1]);
        g_string_append(s, "}\n");
    }
#undef BLOCK
    g_string_free(pos, TRUE);
    g_string_free(size, TRUE);
    return g_string_free(s, FALSE);
}

// pixel width of bar text (Pango, same fonts as waybar), for fixed-width modules
static int text_px(const char *markup, const char *family, int px, int weight) {
    static PangoContext *ctx;
    if (!ctx) ctx = pango_font_map_create_context(pango_cairo_font_map_get_default());
    PangoLayout *lay = pango_layout_new(ctx);
    char *f = g_strdup_printf("%s, Symbols Nerd Font", family);
    PangoFontDescription *fd = pango_font_description_from_string(f);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_font_description_set_weight(fd, weight);
    pango_layout_set_font_description(lay, fd);
    pango_layout_set_markup(lay, markup, -1);
    int w, h;
    pango_layout_get_pixel_size(lay, &w, &h);
    pango_font_description_free(fd);
    g_free(f);
    g_object_unref(lay);
    return w;
}

// dots per column, bottom up. helpers/media.c steps through these frames (class f0, f1, …) 3 times a second
// while something plays (a GTK3 CSS animation would wake waybar every display frame). Paused: no timer.
static const int EQ_FRAMES[4][5] = {{3, 1, 4, 2, 3}, {2, 3, 2, 4, 1}, {4, 2, 3, 1, 2}, {1, 4, 1, 3, 4}};
#define EQ_ROWS 4

// square-dot equalizer sized to the bar's icon size, in whole pixels (crisp, no scaling)
static void eq_geometry(int icon_px, int *w, int *h, int *d, int *g) {
    *d = MAX(2, (int)rint(icon_px * 0.16));
    *g = MAX(1, *d / 2);
    *w = 5 * *d + 4 * *g;
    *h = EQ_ROWS * *d + (EQ_ROWS - 1) * *g;
}

// network speed and the media capsule keep one width whatever they show: no jumping bar
static char *fixed_widths_css(const char *font, int T, int I, int W) {
    char *pct = g_strdup_printf("<span size='%d%%' rise='-1pt'>\U000f0120</span>  ", (int)rint(100.0 * I / T));
    static const char *vals[] = {"888MB/s", "888kB/s", "888B/s"};   // waybar's {bandwidthDownBytes}
    int net = 0;
    for (int i = 0; i < 3; i++) {
        char *m = g_strdup_printf("%s<span font_features='tnum'>%s</span>", pct, vals[i]);
        net = MAX(net, text_px(m, font, T, W));
        g_free(m);
    }
    g_free(pct);
    // typical short title (helpers/media.c cuts at 26 characters)
    int media = MAX(text_px("ooooooooooooo…", font, T, W), text_px("Nothing Playi…", font, T, W));
    int w, h, d, g;
    eq_geometry(I, &w, &h, &d, &g);
    GString *s = g_string_new(NULL);
    // the equalizer image sits in the left padding (background-image), the title is the label
    g_string_append_printf(s, "#network.speed { min-width: %dpx; font-feature-settings: \"tnum\"; }\n", net + 2);
    g_string_append_printf(s, "#custom-media { min-width: %dpx; padding-left: %dpx; background-image: url(\"%s/media-paused.png\");"
                           " background-repeat: no-repeat; background-position: 14px center; background-size: %dpx auto; }\n",
                           media + 2, 14 + w + 8, GEN, w);
    for (int i = 0; i < 4; i++)
        g_string_append_printf(s, "#custom-media.playing.f%d { background-image: url(\"%s/media-playing-%d.png\"); }\n", i, GEN, i);
    return g_string_free(s, FALSE);
}

static cairo_status_t png_to_bytes(void *closure, const unsigned char *data, unsigned int len) {
    g_byte_array_append(closure, data, len);
    return CAIRO_STATUS_SUCCESS;
}

static void write_png(const char *name, cairo_surface_t *surf) {
    GByteArray *b = g_byte_array_new();
    cairo_surface_write_to_png_stream(surf, png_to_bytes, b);
    char *p = g_build_filename(GEN, name, NULL);
    write_if_changed(p, (const char *)b->data, b->len);
    g_free(p);
    g_byte_array_free(b, TRUE);
}

// the media capsule's icons: equalizer frames of square dots in the accent colour while playing
// (media-playing-N.png), one flat row of dots in the muted text colour while paused (media-paused.png)
static void media_icons(const Roles *r, int icon_px) {
    int w, h, d, g;
    eq_geometry(icon_px, &w, &h, &d, &g);
    for (int f = 0; f <= 4; f++) {
        static const int flat[5] = {1, 1, 1, 1, 1};
        const int *heights = f < 4 ? EQ_FRAMES[f] : flat;
        int c[3];
        hex_rgb(r->c[f < 4 ? R_PRIMARY : R_ON_SURFACE_VAR], c);
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(s);
        cairo_set_source_rgb(cr, c[0] / 255.0, c[1] / 255.0, c[2] / 255.0);
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < heights[col]; row++)   // rows count from the bottom
                cairo_rectangle(cr, col * (d + g), h - row * (d + g) - d, d, d);
        cairo_fill(cr);
        cairo_destroy(cr);
        char name[40];
        if (f < 4) snprintf(name, sizeof name, "media-playing-%d.png", f);
        else snprintf(name, sizeof name, "media-paused.png");
        write_png(name, s);
        cairo_surface_destroy(s);
    }
}

// The desktop's Arch logo as a PNG (.generated/arch-logo.png, shown by helpers/panels.c at half size, so it
// stays sharp on scaled screens). Drawn from the Symbols Nerd Font glyph, minus the glyph's small trademark
// sign; one solid colour: the accent blended into the background.
static void arch_logo(const Roles *r) {
    const int px = 300, w = px + px / 4, h = w;
    char col[8];
    mix(col, r->c[R_PRIMARY], r->c[R_WALL_BG], 0.45);
    int c[3];
    hex_rgb(col, c);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    PangoLayout *lay = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Symbols Nerd Font");
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_layout_set_font_description(lay, fd);
    pango_layout_set_text(lay, "", -1);
    PangoRectangle ink;
    pango_layout_get_pixel_extents(lay, &ink, NULL);
    cairo_move_to(cr, -ink.x + (w - ink.width) / 2, -ink.y + (h - ink.height) / 2);
    cairo_set_source_rgb(cr, c[0] / 255.0, c[1] / 255.0, c[2] / 255.0);
    pango_cairo_show_layout(cr, lay);
    pango_font_description_free(fd);
    g_object_unref(lay);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    // keep the logo's big connected shapes, drop small separate bits (the "TM" sign)
    int stride = cairo_image_surface_get_stride(surf);
    unsigned char *buf = cairo_image_surface_get_data(surf);
    int *comp = g_new0(int, w * h);   // component number + 1 per lit pixel
    GArray *sizes = g_array_new(FALSE, TRUE, sizeof(int));
    int *stack = g_new(int, w * h);
    for (int start = 0; start < w * h; start++) {
        if (comp[start] || buf[(start / w) * stride + (start % w) * 4 + 3] <= 40) continue;
        int id = sizes->len + 1, n = 0, sp = 0;
        stack[sp++] = start;
        comp[start] = id;
        while (sp) {
            int i = stack[--sp], x = i % w, y = i / w;
            n++;
            int nb[4] = {x ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y ? i - w : -1, y < h - 1 ? i + w : -1};
            for (int k = 0; k < 4; k++) {
                int j = nb[k];
                if (j >= 0 && !comp[j] && buf[(j / w) * stride + (j % w) * 4 + 3] > 40) { comp[j] = id; stack[sp++] = j; }
            }
        }
        g_array_append_val(sizes, n);
    }
    int biggest = 0;
    for (guint i = 0; i < sizes->len; i++) biggest = MAX(biggest, g_array_index(sizes, int, i));
    for (int i = 0; i < w * h; i++) {
        if (!comp[i] || g_array_index(sizes, int, comp[i] - 1) >= biggest * 0.05) continue;
        int x = i % w, y = i / w;   // also clears the soft edge pixels around it
        for (int yy = MAX(0, y - 2); yy < MIN(h, y + 3); yy++)
            for (int xx = MAX(0, x - 2); xx < MIN(w, x + 3); xx++) memset(buf + yy * stride + xx * 4, 0, 4);
    }
    cairo_surface_mark_dirty(surf);
    write_png("arch-logo.png", surf);
    g_free(stack);
    g_free(comp);
    g_array_free(sizes, TRUE);
    cairo_surface_destroy(surf);
}

static char *read_repo(const char *rel) {
    char *p = g_build_filename(REPO, rel, NULL), *t = NULL;
    if (!g_file_get_contents(p, &t, NULL, NULL)) t = NULL;
    g_free(p);
    return t;
}

// a repo stylesheet recoloured for the theme, without its @import of colors/palette.css
static char *themed_css(const char *rel, const char *th) {
    char *t = read_repo(rel);
    if (!t) return g_strdup("");
    GRegex *re = g_regex_new("^@import \"\\.\\./colors/palette\\.css\";\\n", G_REGEX_MULTILINE, 0, NULL);
    char *a = g_regex_replace_literal(re, t, -1, 0, "", 0, NULL), *b = recolor(a, th), *c = g_strconcat(b, "\n", NULL);
    g_regex_unref(re);
    g_free(t); g_free(a); g_free(b);
    return c;
}

// every inline icon in widget formats uses the same icon size (relative to the text size)
static void resize_spans(JsonNode *n, GRegex *re, const char *span) {
    if (JSON_NODE_HOLDS_OBJECT(n)) {
        JsonObjectIter it;
        const char *k;
        JsonNode *v;
        json_object_iter_init(&it, json_node_get_object(n));
        while (json_object_iter_next(&it, &k, &v)) resize_spans(v, re, span);
    } else if (JSON_NODE_HOLDS_ARRAY(n)) {
        JsonArray *a = json_node_get_array(n);
        for (guint i = 0; i < json_array_get_length(a); i++) resize_spans(json_array_get_element(a, i), re, span);
    } else if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING) {
        char *s = g_regex_replace_literal(re, json_node_get_string(n), -1, 0, span, 0, NULL);
        json_node_set_string(n, s);
        g_free(s);
    }
}

static JsonArray *str_array(GPtrArray *items) {
    JsonArray *a = json_array_new();
    for (guint i = 0; i < items->len; i++) json_array_add_string_element(a, g_ptr_array_index(items, i));
    return a;
}

// Write .generated/* from the repo configs + settings. FALSE (and *err) when something could not be read.
static gboolean render(GError **err) {
    g_mkdir_with_parents(GEN, 0755);
    int T = cfg_int("bar", "font_size"), I = cfg_int("bar", "icon_size"), C = cfg_int("bar", "clock_size");
    int W = cfg_int("bar", "font_weight");
    if (T <= 0 || I <= 0 || C <= 0 || W <= 0) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_INVAL, "bar sizes must be positive numbers");
        return FALSE;
    }
    char *font = g_strdup(cfg_str("general", "font"));
    for (char *p = font; *p; p++) if (*p == '"') memmove(p, p + 1, strlen(p));   // no quotes in CSS strings
    if (!*font) { g_free(font); font = g_strdup("Noto Sans"); }
    const char *th = theme_get(cfg_str("general", "theme"))->id;
    Roles r;
    roles(th, &r);

    // waybar: the repo config (comment lines stripped) with position, widgets, clock format and icon size
    char *jc = read_repo("waybar/config.jsonc");
    if (!jc) { g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_NOENT, "cannot read waybar/config.jsonc"); g_free(font); return FALSE; }
    GRegex *cm = g_regex_new("^\\s*//.*$", G_REGEX_MULTILINE, 0, NULL);
    char *js = g_regex_replace_literal(cm, jc, -1, 0, "", 0, NULL);
    g_regex_unref(cm);
    g_free(jc);
    JsonNode *wbn = json_parse_text(js, err);
    g_free(js);
    if (!wbn || !JSON_NODE_HOLDS_OBJECT(wbn)) {
        if (wbn) json_node_unref(wbn);
        if (err && !*err) g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_INVAL, "waybar/config.jsonc is not an object");
        g_free(font);
        return FALSE;
    }
    JsonObject *wb = json_node_get_object(wbn);
    json_object_set_string_member(wb, "position", "top");
    json_object_set_int_member(wb, "height", BAR_HEIGHT);
    if (json_object_has_member(wb, "margin-bottom")) json_object_remove_member(wb, "margin-bottom");
    json_object_set_int_member(wb, "margin-top", 4);
    json_object_set_boolean_member(wb, "start_hidden", cfg_bool("bar", "hidden"));
    GPtrArray *cols[3] = {g_ptr_array_new(), g_ptr_array_new(), g_ptr_array_new()};
    g_ptr_array_add(cols[2], (char *)"custom/errors");   // always there; empty (hidden) unless something failed
    for (size_t i = 0; i < G_N_ELEMENTS(WIDGETS); i++)
        if (cfg_bool("bar.widgets", WIDGETS[i].key))
            g_ptr_array_add(cols[!strcmp(WIDGETS[i].side, "left") ? 0 : !strcmp(WIDGETS[i].side, "center") ? 1 : 2],
                            (char *)WIDGETS[i].module);
    json_object_set_array_member(wb, "modules-left", str_array(cols[0]));
    json_object_set_array_member(wb, "modules-center", str_array(cols[1]));
    json_object_set_array_member(wb, "modules-right", str_array(cols[2]));
    for (int i = 0; i < 3; i++) g_ptr_array_free(cols[i], TRUE);
    gboolean h24 = cfg_bool("bar", "clock_24h"), clock_on = cfg_bool("bar.widgets", "clock");
    JsonNode *clk = json_object_get_member(wb, "clock");
    if (clk && JSON_NODE_HOLDS_OBJECT(clk))
        json_object_set_string_member(json_node_get_object(clk), "format", h24 ? "{:%H:%M}" : "{:%I:%M %p}");
    char *span = g_strdup_printf("<span size='%d%%' rise='-1pt'>", (int)rint(100.0 * I / T));
    GRegex *sre = g_regex_new("<span size='\\d+%'(?: rise='[^']*')?>", 0, 0, NULL);
    resize_spans(wbn, sre, span);
    g_regex_unref(sre);
    g_free(span);
    // 15.11 Sun | 17:04; the separator only when the clock follows
    JsonNode *date = json_object_get_member(wb, "clock#date");
    if (date && JSON_NODE_HOLDS_OBJECT(date))
        json_object_set_string_member(json_node_get_object(date), "format", clock_on ? "{:%d.%m %a} |" : "{:%d.%m %a}");
    char *wjs = json_dump(wbn);
    gen_write("waybar.json", wjs);
    g_free(wjs);
    json_node_unref(wbn);

    char *pal = palette_css(th), *style = themed_css("waybar/style.css", th), *bat = battery_css(BAR_HEIGHT, I, W);
    char *fixed = fixed_widths_css(font, T, I, W);
    char *wcss = g_strdup_printf(
        "%s%s* { font-family: \"%s\", \"Symbols Nerd Font\"; font-size: %dpx; font-weight: %d; }\n"
        "window#waybar { background: @surface; }\n"
        "#clock { font-size: %dpx; font-weight: %d; }\n"
        "#custom-notification { font-size: %dpx; }\n"
        "#workspaces button { font-size: %dpx; font-weight: %d; min-width: %dpx; min-height: %dpx; border-radius: %dpx; }\n"
        "#workspaces button.active { min-width: %dpx; }\n"
        "#custom-media { border-radius: %dpx; }\n%s%s",
        pal, style, font, T, W, C, W, I, (int)rint(T * 0.8), W, (int)rint(I * 1.2), (int)rint(I * 1.2),
        MAX(4, (int)rint(I * 0.25)), (int)rint(I * 2.6), (int)rint(I * 0.85), bat, fixed);
    gen_write("waybar.css", wcss);
    g_free(wcss); g_free(style); g_free(bat); g_free(fixed);

    // where the date + clock sit on the bar, for the calendar's click area (helpers/panels.c)
    int parts = 0, width = 0;
    if (cfg_bool("bar.widgets", "date")) { width += text_px(clock_on ? "88.88 Wed |" : "88.88 Wed", font, T, W); parts++; }
    if (clock_on) { width += text_px(h24 ? "88:88" : "88:88 PM", font, C, W); parts++; }
    width += 10 * MAX(0, parts - 1);   // waybar config "spacing"
    // bar hidden: no strip at all (width 0 = panels.c makes none), or clicks there would open the calendar
    char *zone = g_strdup_printf("{\"position\": \"top\", \"height\": %d, \"margin\": 4, \"width\": %d, \"offset\": 0}\n",
                                 BAR_HEIGHT, parts && !cfg_bool("bar", "hidden") ? width + 24 : 0);
    gen_write("clock_zone.json", zone);
    g_free(zone);

    // notifications (helpers/panels.c watches this file): popup corner and seconds on screen
    int timeout = cfg_int("notifications", "timeout");
    char *nc = g_strdup_printf("position=%s\ntimeout=%d\n", cfg_str("notifications", "position"), timeout);
    gen_write("notify.conf", nc);
    g_free(nc);

    // hyprlock, and the login screen's copy (install made it ours, so the theme follows)
    char *lock = read_repo("hyprlock/hyprlock.conf");
    if (lock) {
        char *fl = g_strdup_printf("$font     = %s", font), **sp = g_strsplit(lock, "$font     = Noto Sans", -1);
        char *joined = g_strjoinv(fl, sp), *col = recolor(joined, th);
        gen_write("hyprlock.conf", col);
        const char *greeter = "/usr/local/share/hyprminimal-greeter/hyprlock.conf";
        if (access(greeter, W_OK) == 0) write_if_changed(greeter, col, -1);
        g_free(fl); g_strfreev(sp); g_free(joined); g_free(col); g_free(lock);
    }
    // also at the default path, so a bare `hyprlock` finds the same config. Never replaces a real file.
    char *link = g_build_filename(g_get_home_dir(), ".config", "hypr", "hyprlock.conf", NULL);
    if (g_file_test(link, G_FILE_TEST_IS_SYMLINK) || !g_file_test(link, G_FILE_TEST_EXISTS)) {
        char *dir = g_path_get_dirname(link), *target = g_build_filename(GEN, "hyprlock.conf", NULL);
        char *now = g_file_read_link(link, NULL);
        if (!now || strcmp(now, target)) {
            g_mkdir_with_parents(dir, 0755);
            g_unlink(link);
            if (symlink(target, link) != 0) { /* the next render tries again */ }
        }
        g_free(now); g_free(dir); g_free(target);
    }
    g_free(link);

    // startup commands: one enabled shell line per line, run by `hyprminimal start` once per login
    GString *su = g_string_new(NULL);
    JsonArray *cmds = cfg_startup();
    for (guint i = 0; i < json_array_get_length(cmds); i++) {
        JsonNode *n = json_array_get_element(cmds, i);
        if (!JSON_NODE_HOLDS_OBJECT(n)) continue;
        JsonObject *o = json_node_get_object(n);
        if (json_object_has_member(o, "enabled") && !json_object_get_boolean_member(o, "enabled")) continue;
        const char *cmd = json_object_get_string_member_with_default(o, "command", "");
        char **lines = g_strsplit(cmd, "\n", -1), *one = g_strjoinv(" ", lines);
        g_strstrip(one);
        if (*one) g_string_append_printf(su, "%s\n", one);
        g_strfreev(lines);
        g_free(one);
    }
    gen_write("startup", su->str);
    g_string_free(su, TRUE);

    // everything else that follows the theme: palette, terminal, Hyprland borders, media icons
    char *tl = g_strdup_printf("%s dark\n", th);
    gen_write("theme", tl);
    g_free(tl);
    gen_write("palette.css", pal);
    g_free(pal);
    char *kc = kitty_conf(th), *hl = hypr_lua(th);
    gen_write("kitty-colors.conf", kc);
    gen_write("hypr-theme.lua", hl);
    g_free(kc); g_free(hl);
    media_icons(&r, I);

    // popups, notifications, OSD, launcher, Settings (helpers/panels.c watches this file and restyles live)
    char *pt = read_repo("settings/panels.css");
    if (pt) {
        char *pc = recolor(pt, th);
        char *all = g_strdup_printf("%swindow.hyprminimal-panel, window.hyprminimal-background, window.hyprminimal-notifications, "
                                    "window.hyprminimal-osd, window.hm-settings { font-family: \"%s\", \"Symbols Nerd Font\"; }\n"
                                    ".notif-popups .notif.countdown { animation-duration: %ds; }\n", pc, font, timeout);
        gen_write("panels.css", all);
        g_free(all); g_free(pc); g_free(pt);
    }

    // desktop background: helpers/panels.c watches this file and redraws the desktop when it changes
    gboolean logo = cfg_node("desktop", "logo") ? cfg_bool("desktop", "logo") : TRUE;
    if (logo) arch_logo(&r);
    char **words = g_strsplit_set(cfg_str("desktop", "name"), " \t\n\r\f\v", -1);
    GString *nm = g_string_new(NULL);
    for (char **w = words; *w; w++) if (**w) g_string_append_printf(nm, "%s%s", nm->len ? " " : "", *w);
    g_strfreev(words);
    char *dc = g_strdup_printf("logo=%s\nname=%s\n", logo ? "on" : "off", nm->str);
    gen_write("desktop", dc);
    g_free(dc);
    g_string_free(nm, TRUE);

    char *nl = g_strdup_printf("%s %d\n", cfg_bool("nightlight", "enabled") ? "on" : "off",
                               (int)cfg_int("nightlight", "temperature"));
    gen_write("nightlight", nl);   // bin/hyprminimal-msg nightlight, hyprminimal start
    g_free(nl);
    g_free(font);
    return TRUE;
}

// ---------------------------------------------------------------- other apps: light or dark, one font
static gboolean theme_dir_exists(const char *name) {
    char *dirs[3] = {g_build_filename("/usr/share/themes", name, "gtk-3.0", NULL),
                     g_build_filename(g_get_home_dir(), ".local/share/themes", name, "gtk-3.0", NULL),
                     g_build_filename(g_get_home_dir(), ".themes", name, "gtk-3.0", NULL)};
    gboolean ok = FALSE;
    for (int i = 0; i < 3; i++) { ok = ok || g_file_test(dirs[i], G_FILE_TEST_IS_DIR); g_free(dirs[i]); }
    return ok;
}

// Dark for other apps' GTK windows (every theme is dark). libadwaita/GTK4 follow color-scheme; GTK3 apps
// (blueman, nwg-displays, …) need a theme that exists: GTK3 has no built-in Adwaita-dark (that one is
// gnome-themes-extra), so an unknown name falls back to light Adwaita. adw-gtk3 also looks like libadwaita apps.
static void gtk_theme(void) {
    const gboolean dark = TRUE;
    const char *font = *cfg_str("general", "font") ? cfg_str("general", "font") : "Noto Sans";
    char name[128];
    snprintf(name, sizeof name, "%s", theme_dir_exists("adw-gtk3") ? "adw-gtk3" : "Adwaita");
    char dname[160];
    snprintf(dname, sizeof dname, "%s-dark", name);
    if (dark && theme_dir_exists(dname)) g_strlcpy(name, dname, sizeof name);
    char *uifont = g_strdup_printf("%s 11", font);
    GSettingsSchema *schema = g_settings_schema_source_lookup(g_settings_schema_source_get_default(),
                                                              "org.gnome.desktop.interface", TRUE);
    if (schema) {
        GSettings *gs = g_settings_new("org.gnome.desktop.interface");
        if (g_settings_schema_has_key(schema, "color-scheme"))
            g_settings_set_string(gs, "color-scheme", dark ? "prefer-dark" : "prefer-light");
        g_settings_set_string(gs, "gtk-theme", name);
        // one font everywhere: other apps' UI text follows the Settings font, terminals get Noto Sans Mono
        g_settings_set_string(gs, "font-name", uifont);
        g_settings_set_string(gs, "document-font-name", uifont);
        g_settings_set_string(gs, "monospace-font-name", "Noto Sans Mono 11");
        g_settings_sync();
        g_object_unref(gs);
        g_settings_schema_unref(schema);
    }
    // settings.ini: what GTK reads without gsettings (X11 apps); prefer-dark there would darken a light theme
    const char *keys[3] = {"gtk-theme-name", "gtk-application-prefer-dark-theme", "gtk-font-name"};
    const char *vals[3] = {name, dark ? "1" : "0", uifont};
    for (int v = 0; v < 2; v++) {
        char *f = g_build_filename(g_get_home_dir(), ".config", v ? "gtk-4.0" : "gtk-3.0", "settings.ini", NULL), *text = NULL;
        if (!g_file_get_contents(f, &text, NULL, NULL)) text = g_strdup("[Settings]");
        char **lines = g_strsplit(text, "\n", -1);
        int n = g_strv_length(lines);
        if (n && !*lines[n - 1]) { g_free(lines[n - 1]); lines[n - 1] = NULL; n--; }   // trailing newline
        gboolean done[3] = {0};
        GString *out = g_string_new(NULL);
        for (int i = 0; i < n; i++) {
            char *eq = strchr(lines[i], '='), *k = g_strstrip(g_strndup(lines[i], eq ? eq - lines[i] : (long)strlen(lines[i])));
            int hit = -1;
            for (int j = 0; j < 3; j++) if (!done[j] && !strcmp(k, keys[j])) hit = j;
            if (hit >= 0) { g_string_append_printf(out, "%s=%s\n", keys[hit], vals[hit]); done[hit] = TRUE; }
            else g_string_append_printf(out, "%s\n", lines[i]);
            g_free(k);
        }
        for (int j = 0; j < 3; j++) if (!done[j]) g_string_append_printf(out, "%s=%s\n", keys[j], vals[j]);
        char *dir = g_path_get_dirname(f);
        g_mkdir_with_parents(dir, 0755);
        write_if_changed(f, out->str, -1);
        g_free(dir); g_string_free(out, TRUE); g_strfreev(lines); g_free(text); g_free(f);
    }
    g_free(uifont);
}

// `panels --apply`: regenerate everything from settings.json; `panels --toggle-bar`: flip Bar → Hide bar
static int settings_cli(const char *what) {
    load_settings();
    if (CFG_BROKEN) {   // keep the file and the last working .generated/: bin/hyprminimal shows this line
        char *why = read_error("settings");
        fprintf(stderr, "%s. Fix it or delete it\n", why);
        g_free(why);
        return 1;
    }
    if (!strcmp(what, "--toggle-bar")) cfg_set_bool("bar", "hidden", !cfg_bool("bar", "hidden"));
    if (!strcmp(what, "--toggle-bar") || !g_file_test(SETTINGS_PATH, G_FILE_TEST_EXISTS)) save_settings();
    GError *err = NULL;
    if (!render(&err)) {
        fprintf(stderr, "%s\n", err ? err->message : "render failed");
        g_clear_error(&err);
        return 1;
    }
    if (!strcmp(what, "--apply")) gtk_theme();
    return 0;
}
