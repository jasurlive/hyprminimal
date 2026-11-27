// The Settings window: sidebar with search, lazily built pages, show / park / toggle (Super+I), Esc.

static SPage PAGES[] = {
    {"general", "General", "applications-system-symbolic", "font theme desktop background logo name arch", "settings", page_general, NULL},
    {"bar", "Bar", "view-app-grid-symbolic", "bar widgets clock date wifi bluetooth volume tray battery media hide", "waybar", page_bar, NULL},
    {"notifications", "Notifications", "preferences-system-notifications-symbolic", "notifications dnd do not disturb timeout popup",
     "panels", page_notifications, NULL},
    {"startup", "Startup", "system-run-symbolic", "startup autostart autorun login boot launch apps commands scripts exec", NULL,
     page_startup, NULL},
    {"network", "Network", "network-workgroup-symbolic", "network wifi wi-fi wireless ethernet wired lan cable hotspot tethering vpn "
     "wireguard openvpn airplane password internet connect ip dns", NULL, page_network, NULL},
    {"nightlight", "Night Light", "night-light-symbolic", "night light temperature warm", NULL, page_nightlight, NULL},
    {"keybinds", "Keybinds", "input-keyboard-symbolic", "keybinds shortcuts keys hotkeys", NULL, page_keybinds, NULL},
    {"system", "System", "preferences-system-symbolic", "audio display network bluetooth shell restart", NULL, page_system, NULL},
    {"about", "About", "help-about-symbolic", "about version", NULL, page_about, NULL},
};
static int n_pages(void) { return G_N_ELEMENTS(PAGES); }

static char *ui_state_path(void) {   // remembers the page shown last
    const char *xs = getenv("XDG_STATE_HOME");
    return xs && *xs ? g_build_filename(xs, "hyprminimal", "settings-window.json", NULL)
                     : g_build_filename(g_get_home_dir(), ".local", "state", "hyprminimal", "settings-window.json", NULL);
}

static char *last_page(void) {
    char *p = ui_state_path(), *t = NULL, *page = NULL;
    if (g_file_get_contents(p, &t, NULL, NULL)) {
        JsonNode *n = json_parse_text(t, NULL);
        if (n && JSON_NODE_HOLDS_OBJECT(n)) page = g_strdup(json_object_get_string_member_with_default(json_node_get_object(n), "page", ""));
        if (n) json_node_unref(n);
    }
    g_free(t); g_free(p);
    if (!page || !*page) { g_free(page); page = g_strdup("general"); }
    return page;
}

static void remember_page(const char *id) {
    char *p = ui_state_path(), *dir = g_path_get_dirname(p), *t = g_strdup_printf("{\"page\": \"%s\"}\n", id);
    g_mkdir_with_parents(dir, 0755);
    write_if_changed(p, t, -1);
    g_free(p); g_free(dir); g_free(t);
}

static gboolean grab_kb_search(gpointer d) { (void)d; if (KB.search) gtk_widget_grab_focus(KB.search); return G_SOURCE_REMOVE; }
static gboolean grab_search(gpointer d) { (void)d; gtk_widget_grab_focus(SW.search); return G_SOURCE_REMOVE; }

static void on_nav(GtkListBox *l, GtkListBoxRow *row, gpointer d) {
    (void)l; (void)d;
    if (!row) return;
    SPage *p = g_object_get_data(G_OBJECT(row), "hm-page");
    if (!gtk_stack_get_child_by_name(GTK_STACK(SW.stack), p->id)) {
        GtkWidget *w = p->build();
        pointerize(w);
        gtk_stack_add_named(GTK_STACK(SW.stack), w, p->id);
    } else if (!strcmp(p->id, "keybinds") && KB.kf && !KB.busy) {
        GError *e = NULL;   // edited by hand meanwhile? show the file as it is now
        kb_reload(KB.kf, &e);
        g_clear_error(&e);
        kb_fill();
    }
    if (!strcmp(p->id, "network") && NP.stack) gtk_stack_set_visible_child_name(GTK_STACK(NP.stack), "main");
    gtk_stack_set_visible_child_name(GTK_STACK(SW.stack), p->id);
    if (!SW.quiet && (!SW.remembered || strcmp(SW.remembered, p->id))) {
        g_free(SW.remembered);
        SW.remembered = g_strdup(p->id);
        remember_page(p->id);
    }
    gtk_label_set_label(GTK_LABEL(SW.title), p->label);
    update_banners();
    if (!strcmp(p->id, "keybinds")) g_idle_add(grab_kb_search, NULL);
}

static void show_page(const char *id) {
    char *last = NULL;
    if (!id || !strcmp(id, "last")) id = last = last_page();
    if (!strcmp(id, "wifi") || !strcmp(id, "ethernet") || !strcmp(id, "hotspot") || !strcmp(id, "vpn")) id = "network";
    SPage *p = &PAGES[0];
    for (int i = 0; i < n_pages(); i++) if (!strcmp(PAGES[i].id, id)) p = &PAGES[i];
    g_free(last);
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(p->row);
    if (!gtk_widget_get_visible(p->row)) {   // filtered out by an old search
        gtk_editable_set_text(GTK_EDITABLE(SW.search), "");
        for (int i = 0; i < n_pages(); i++) gtk_widget_set_visible(PAGES[i].row, TRUE);
    }
    if (gtk_list_box_get_selected_row(GTK_LIST_BOX(SW.nav)) == row) on_nav(GTK_LIST_BOX(SW.nav), row, NULL);
    else gtk_list_box_select_row(GTK_LIST_BOX(SW.nav), row);
}

static void filter_nav(GtkSearchEntry *e, gpointer d) {   // type to jump: the first page matching every word
    (void)d;
    char *q = g_utf8_strdown(gtk_editable_get_text(GTK_EDITABLE(e)), -1), **words = g_strsplit_set(g_strstrip(q), " \t", -1);
    SPage *first = NULL;
    for (int i = 0; i < n_pages(); i++) {
        char *hay = g_strdup_printf("%s %s", PAGES[i].label, PAGES[i].words), *low = g_utf8_strdown(hay, -1);
        gboolean ok = TRUE;
        for (char **w = words; *w && ok; w++) if (**w && !strstr(low, *w)) ok = FALSE;
        gtk_widget_set_visible(PAGES[i].row, ok);
        if (ok && !first) first = &PAGES[i];
        g_free(hay); g_free(low);
    }
    if (*q && first) gtk_list_box_select_row(GTK_LIST_BOX(SW.nav), GTK_LIST_BOX_ROW(first->row));
    g_strfreev(words);
    g_free(q);
}

// Esc closes the window; first it closes an open dialog, or clears a search that has text
static gboolean settings_key(GtkEventControllerKey *c, guint kv, guint code, GdkModifierType st, gpointer d) {
    (void)c; (void)code; (void)d;
    if (kv != GDK_KEY_Escape || (st & gtk_accelerator_get_default_mod_mask())) return FALSE;
    if (SW.dlg) {
        if (KB.rec && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(KB.rec))) return FALSE;   // recording Esc
        dialog_close();
        return TRUE;
    }
    GtkWidget *f = gtk_root_get_focus(GTK_ROOT(SW.win));
    GtkWidget *se = f ? gtk_widget_get_ancestor(f, GTK_TYPE_SEARCH_ENTRY) : NULL;
    if (se && *gtk_editable_get_text(GTK_EDITABLE(se))) return FALSE;   // the search entry clears itself
    settings_hide();
    return TRUE;
}

static gboolean settings_close_request(GtkWindow *w, gpointer d) { (void)w; (void)d; settings_hide(); return TRUE; }

static void retry_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    gtk_widget_set_visible(SW.err_banner, FALSE);
    if (!SW.err_comp) return;
    if (!strcmp(SW.err_comp, "settings")) { changed(CH_BAR); return; }
    char *bin = g_build_filename(REPO, "bin", "hyprminimal", NULL);
    const char *argv[] = {bin, "respawn", SW.err_comp, NULL};   // clears the error, tries the current settings
    spawn_quiet(argv);
    g_free(bin);
}

static void start_clicked(GtkButton *b, gpointer d) { (void)b; (void)d; shell_cmd("start"); }

// Hyprland matches its window rules on the app id: ours is dev.hyprminimal.Panels, the window says Settings.
// GTK sends the app id when it gives the surface its toplevel role (gtk_window_present), and ignores a change
// before that: so on "map", which still comes before the first frame (Hyprland maps on the first buffer).
static void settings_mapped(GtkWidget *w, gpointer d) {
    (void)d;
    GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(w));
    if (s && GDK_IS_WAYLAND_TOPLEVEL(s)) gdk_wayland_toplevel_set_application_id(GDK_TOPLEVEL(s), SETTINGS_APP_ID);
}

static GtkWidget *banner(const char *text, const char *button, const char *cls, GtkWidget **lbl_out, GtkWidget **btn_out) {
    GtkWidget *h = HBOX(12, cls), *l = gtk_label_new(text), *b = text_btn(button, "hm-btn");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(h), l);
    gtk_box_append(GTK_BOX(h), b);
    gtk_widget_set_visible(h, FALSE);
    if (lbl_out) *lbl_out = l;
    if (btn_out) *btn_out = b;
    return h;
}

static void settings_build(void) {
    if (!CFG) load_settings();
    SW.win = gtk_window_new();
    GtkWindow *w = GTK_WINDOW(SW.win);
    gtk_window_set_application(w, app);
    gtk_window_set_title(w, "hyprminimal settings");
    gtk_window_set_default_size(w, 1280, 860);
    // undecorated: no client-side shadow border (12px), which shifted the first frame of a re-show
    gtk_window_set_decorated(w, FALSE);
    gtk_widget_add_css_class(SW.win, "hm-settings");
    g_signal_connect(SW.win, "map", G_CALLBACK(settings_mapped), NULL);
    g_signal_connect(SW.win, "close-request", G_CALLBACK(settings_close_request), NULL);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);   // a focused search entry eats Esc
    g_signal_connect(keys, "key-pressed", G_CALLBACK(settings_key), NULL);
    gtk_widget_add_controller(SW.win, keys);

    GtkWidget *overlay = gtk_overlay_new(), *main = HBOX(0, NULL);
    gtk_window_set_child(w, overlay);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), main);

    GtkWidget *side = VBOX(10, "hm-sidebar");
    gtk_widget_set_size_request(side, 270, -1);
    gtk_box_append(GTK_BOX(side), lbl("Settings", "hm-title", 0));
    SW.search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(SW.search), "Search settings…");
    g_signal_connect(SW.search, "search-changed", G_CALLBACK(filter_nav), NULL);
    gtk_box_append(GTK_BOX(side), SW.search);
    SW.nav = gtk_list_box_new();
    gtk_widget_add_css_class(SW.nav, "hm-nav");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(SW.nav), GTK_SELECTION_SINGLE);
    for (int i = 0; i < n_pages(); i++) {
        GtkWidget *row = gtk_list_box_row_new(), *h = HBOX(12, NULL);
        gtk_box_append(GTK_BOX(h), gtk_image_new_from_icon_name(PAGES[i].icon));
        gtk_box_append(GTK_BOX(h), lbl(PAGES[i].label, NULL, 0));
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), h);
        gtk_widget_set_focusable(row, FALSE);
        gtk_widget_set_cursor_from_name(row, "pointer");
        g_object_set_data(G_OBJECT(row), "hm-page", &PAGES[i]);
        PAGES[i].row = row;
        gtk_list_box_append(GTK_LIST_BOX(SW.nav), row);
    }
    g_signal_connect(SW.nav, "row-selected", G_CALLBACK(on_nav), NULL);
    GtkWidget *nsc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(nsc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(nsc, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(nsc), SW.nav);
    gtk_box_append(GTK_BOX(side), nsc);
    gtk_box_append(GTK_BOX(main), side);

    GtkWidget *content = VBOX(0, "hm-content");
    gtk_widget_set_hexpand(content, TRUE);
    GtkWidget *head = HBOX(0, "hm-header");
    SW.title = lbl("", "hm-page-title", 0.5);
    gtk_widget_set_hexpand(SW.title, TRUE);
    gtk_box_append(GTK_BOX(head), SW.title);
    gtk_box_append(GTK_BOX(content), head);
    GtkWidget *sb;
    SW.run_banner = banner("hyprminimal isn't running. These settings take effect once it starts.", "Start hyprminimal",
                           "hm-banner", NULL, &sb);
    g_signal_connect(sb, "clicked", G_CALLBACK(start_clicked), NULL);
    gtk_box_append(GTK_BOX(content), SW.run_banner);
    // red banner: the part of hyprminimal this page configures failed (bin/hyprminimal watchdog)
    SW.err_banner = banner("", "Retry", "hm-banner error", &SW.err_lbl, &SW.err_btn);
    g_signal_connect(SW.err_btn, "clicked", G_CALLBACK(retry_clicked), NULL);
    gtk_box_append(GTK_BOX(content), SW.err_banner);
    SW.stack = gtk_stack_new();
    gtk_widget_set_vexpand(SW.stack, TRUE);
    gtk_box_append(GTK_BOX(content), SW.stack);
    gtk_box_append(GTK_BOX(main), content);

    SW.toast_lbl = SW.toast = lbl("", "hm-toast", 0.5);
    gtk_widget_set_halign(SW.toast, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(SW.toast, GTK_ALIGN_END);
    gtk_widget_set_can_target(SW.toast, FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(SW.toast_lbl), PANGO_ELLIPSIZE_NONE);
    gtk_label_set_wrap(GTK_LABEL(SW.toast_lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(SW.toast_lbl), 70);
    gtk_widget_set_visible(SW.toast, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), SW.toast);

    SW.scrim = VBOX(0, "hm-scrim");   // dialogs: a dimmed layer over the window, the card in the middle
    gtk_widget_set_visible(SW.scrim, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), SW.scrim);
    pointerize(SW.win);
}

static gboolean wake_pointer(gpointer d) {
    // Hyprland gives a window that appears under a still cursor no pointer-enter, so GTK ignores the scroll
    // wheel until the mouse moves: nudge the cursor 1px and back (invisible)
    (void)d;
    char *pos = hypr_ctl("cursorpos");
    int x, y;
    if (pos && sscanf(pos, "%d, %d", &x, &y) == 2) {
        for (int i = 0; i < 2; i++) {
            char *c = g_strdup_printf("hl.dsp.cursor.move({ x = %d, y = %d })", i ? x : x + 1, y);
            hypr_dispatch(c);
            g_free(c);
        }
    }
    g_free(pos);
    return G_SOURCE_REMOVE;
}

static void settings_present(const char *page) {
    gboolean fresh = !SW.win;
    if (fresh) settings_build();
    show_page(page);
    update_banners();
    char *addr = NULL;
    if (!fresh && gtk_widget_get_mapped(SW.win) && settings_client(&addr, NULL, NULL)) {
        // mapped (parked, or open on another workspace): move it here and focus it, no remap
        gint64 ws;
        active_workspace(NULL, &ws);
        char *c = g_strdup_printf("hl.dsp.window.move({ workspace = %" G_GINT64_FORMAT ", follow = false, window = \"address:%s\" })", ws, addr);
        if (ws != G_MININT64) hypr_dispatch(c);
        g_free(c);
        c = g_strdup_printf("hl.dsp.focus({ window = \"address:%s\" })", addr);
        hypr_dispatch(c);
        g_free(c);
    }
    g_free(addr);
    gtk_window_present(GTK_WINDOW(SW.win));
    SW.shown = TRUE;
    // type to jump: the sidebar search gets the keyboard (Keybinds keeps its own search), cleared
    SPage *p = current_page();
    if (!p || strcmp(p->id, "keybinds")) {
        if (*gtk_editable_get_text(GTK_EDITABLE(SW.search))) gtk_editable_set_text(GTK_EDITABLE(SW.search), "");
        g_idle_add(grab_search, NULL);
    }
    g_timeout_add(100, wake_pointer, NULL);
}

// close: park it on a special workspace (instant next time); unmap when Hyprland isn't there
static void settings_hide(void) {
    if (!SW.win) return;
    SW.shown = FALSE;
    dialog_close();
    char *addr = NULL;
    if (settings_client(&addr, NULL, NULL)) {
        char *c = g_strdup_printf("hl.dsp.window.move({ workspace = \"%s\", follow = false, window = \"address:%s\" })", PARK, addr);
        hypr_dispatch(c);
        g_free(c);
    } else gtk_widget_set_visible(SW.win, FALSE);
    g_free(addr);
}

// Super+I: hide when it shows on the workspace you look at (wherever the keyboard is), else show it here
static void settings_toggle(void) {
    char *ws = NULL, *mine = NULL;
    gboolean here = FALSE;
    if (SW.win && SW.shown && settings_client(NULL, &mine, NULL)) {
        active_workspace(&ws, NULL);
        here = !strcmp(mine, ws);
    }
    g_free(ws); g_free(mine);
    if (here) settings_hide();
    else settings_present("last");
}
