// Settings → Keybinds: every shortcut in ~/.config/hypr/keybind.lua by section, search, edit, add, remove.
// Edits go straight into the file and are live right away; a backup is kept and a change Hyprland rejects
// is undone (kb_write_checked, in a worker thread).

static struct {
    KeybindFile *kf;
    GtkWidget *search, *box;
    GtkWidget *combo, *desc, *cmd, *rec, *preview, *warn, *save;   // the open dialog
    Keybind *editing;                                              // NULL: adding
    gboolean busy;
} KB;

static void kb_fill(void);

static GtkWidget *keycaps(const char *combo) {
    GtkWidget *b = HBOX(3, NULL);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    char **parts = pretty_parts(combo);
    for (char **p = parts; *p; p++) {
        if (p != parts) gtk_box_append(GTK_BOX(b), lbl("+", "keycap plus", 0.5));
        gtk_box_append(GTK_BOX(b), lbl(*p, "keycap", 0.5));
    }
    g_strfreev(parts);
    return b;
}

static void kb_saved(GObject *src, GAsyncResult *res, gpointer okmsg) {
    (void)src;
    char *err = g_task_propagate_pointer(G_TASK(res), NULL);
    KB.busy = FALSE;
    GError *e = NULL;
    if (KB.kf) kb_reload(KB.kf, &e);
    g_clear_error(&e);
    kb_fill();
    char *t = err ? g_strdup_printf("Not saved, Hyprland reported: %s", err) : g_strdup(okmsg);
    settings_toast(t, 5);
    g_free(t);
    g_free(err);
}

static void kb_save_work(GTask *t, gpointer s, gpointer d, GCancellable *c) {
    (void)s; (void)c;
    char **pt = d;   // path, text
    g_task_return_pointer(t, kb_write_checked(pt[0], pt[1]), g_free);
}

// write the edited lines (keeps KB.kf as edited until the reload in kb_saved)
static void kb_save(const char *okmsg) {
    KB.busy = TRUE;
    char **pt = g_new0(char *, 3);
    pt[0] = g_strdup(KB.kf->path);
    pt[1] = kb_text(KB.kf);
    GTask *t = g_task_new(NULL, NULL, kb_saved, (gpointer)okmsg);
    g_task_set_task_data(t, pt, (GDestroyNotify)g_strfreev);
    g_task_run_in_thread(t, kb_save_work);
    g_object_unref(t);
}

static void kb_refresh_dialog(void) {
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(KB.combo));
    char *t = g_strstrip(g_strdup(txt));
    GtkWidget *suf = g_object_get_data(G_OBJECT(KB.preview), "hm-suffix");
    for (GtkWidget *c; (c = gtk_widget_get_first_child(suf));) gtk_box_remove(GTK_BOX(suf), c);
    gtk_box_append(GTK_BOX(suf), *t ? keycaps(t) : lbl("—", NULL, 1));
    char *clash = *t ? kb_conflicts(KB.kf, t, KB.editing) : NULL;
    char *w = clash ? g_strdup_printf("Already used by: %s", clash) : g_strdup("");
    gtk_label_set_label(GTK_LABEL(KB.warn), w);
    gtk_widget_set_visible(KB.warn, clash != NULL);
    int mods;
    char *key;
    split_combo(t, &mods, &key);
    char *cmd = KB.cmd ? g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(KB.cmd)))) : NULL;
    gboolean ok = *key && (KB.editing || (cmd && *cmd));
    g_free(cmd);
    gtk_widget_set_sensitive(KB.save, ok && !KB.busy);
    g_free(key); g_free(w); g_free(clash); g_free(t);
}
static void kb_dialog_changed(GtkEditable *e, gpointer d) { (void)e; (void)d; kb_refresh_dialog(); }

static void kb_record_toggled(GtkToggleButton *b, gpointer d) { (void)d; set_class(GTK_WIDGET(b), "recording", gtk_toggle_button_get_active(b)); }

// recording: the next key press with its modifiers becomes the combo
static gboolean kb_record_key(GtkEventControllerKey *c, guint kv, guint code, GdkModifierType st, gpointer d) {
    (void)c; (void)d;
    if (!KB.rec || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(KB.rec))) return FALSE;
    const char *name = gdk_keyval_name(kv);
    static const char *modkeys[] = {"Super", "Control", "Alt", "Shift", "Meta", "Hyper", "ISO_Level"};
    for (size_t i = 0; i < G_N_ELEMENTS(modkeys); i++) if (name && g_str_has_prefix(name, modkeys[i])) return TRUE;
    guint base = 0;
    const char *bn = NULL;
    if (gdk_display_translate_key(gdk_display_get_default(), code, 0, 0, &base, NULL, NULL, NULL)) bn = gdk_keyval_name(base);
    if (!bn) bn = name ? name : "";
    GString *s = g_string_new(NULL);
    if (st & GDK_SUPER_MASK) g_string_append(s, "SUPER + ");
    if (st & GDK_CONTROL_MASK) g_string_append(s, "CTRL + ");
    if (st & GDK_ALT_MASK) g_string_append(s, "ALT + ");
    if (st & GDK_SHIFT_MASK) g_string_append(s, "SHIFT + ");
    if (strlen(bn) == 1) { char *u = g_ascii_strup(bn, -1); g_string_append(s, u); g_free(u); }
    else g_string_append(s, bn);
    gtk_editable_set_text(GTK_EDITABLE(KB.combo), s->str);
    g_string_free(s, TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(KB.rec), FALSE);
    return TRUE;
}

static void kb_do_save(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    if (KB.busy) return;
    char *c = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(KB.combo))));
    char *ds = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(KB.desc))));
    if (!KB.editing) {
        char *cmd = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(KB.cmd))));
        kb_add(KB.kf, c, *ds ? ds : cmd, cmd);
        g_free(cmd);
    } else {
        char *a = norm_combo(c), *o = norm_combo(KB.editing->combo);
        if (strcmp(a, o)) kb_set_combo(KB.kf, KB.editing, c);
        if (strcmp(ds, KB.editing->description)) kb_set_description(KB.kf, KB.editing, ds);
        g_free(a); g_free(o);
    }
    g_free(c); g_free(ds);
    dialog_close();
    KB.editing = NULL;
    kb_save("Shortcut saved and live");
}

static void kb_do_remove(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    if (KB.busy || !KB.editing) return;
    kb_remove(KB.kf, KB.editing);
    dialog_close();
    KB.editing = NULL;
    kb_save("Shortcut removed (commented out in keybind.lua)");
}

static void kb_dialog(Keybind *b) {   // edit b, or add (b NULL)
    KB.editing = b;
    GtkWidget *body = dialog_new(b ? "Edit shortcut" : "Add shortcut", "Save", NULL, &KB.save);
    gtk_widget_set_size_request(body, 520, -1);
    GtkWidget *list = boxed_list();
    gtk_box_append(GTK_BOX(body), list);
    KB.desc = entry_row(list, "Description", b ? b->description : "", NULL, NULL);
    char *canon = b ? canon_combo(b->combo) : g_strdup("");
    GtkWidget *crow;
    KB.combo = entry_row_full(list, "Keys (e.g. SUPER + SHIFT + R)", canon, FALSE, NULL, NULL, &crow);
    g_free(canon);
    KB.rec = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(KB.rec), "media-record-symbolic");
    gtk_widget_add_css_class(KB.rec, "flat");
    gtk_widget_set_valign(KB.rec, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(KB.rec, "Record: press the new keys");
    gtk_widget_set_cursor_from_name(KB.rec, "pointer");
    g_signal_connect(KB.rec, "toggled", G_CALLBACK(kb_record_toggled), NULL);
    row_suffix(crow, KB.rec);
    KB.preview = row_add(list, "Preview", NULL);
    KB.cmd = NULL;
    if (!b) KB.cmd = entry_row(list, "Command to run (e.g. firefox)", "", NULL, NULL);
    else {
        row_add(list, "Action", b->action);
        if (b->key_ref) {
            GString *o = g_string_new(NULL);
            for (guint i = 0; i < KB.kf->binds->len; i++) {
                Keybind *x = g_ptr_array_index(KB.kf->binds, i);
                if (x != b && x->key_ref && !strcmp(x->key_ref, b->key_ref))
                    g_string_append_printf(o, "%s%s", o->len ? ", " : "", *x->description ? x->description : x->action);
            }
            if (o->len) {
                char *n = g_strdup_printf("KEY.%s is also used by %s", b->key_ref, o->str);
                row_add(list, "Note", n);
                g_free(n);
            }
            g_string_free(o, TRUE);
        }
    }
    KB.warn = lbl("", "conflict", 0);
    gtk_label_set_wrap(GTK_LABEL(KB.warn), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(KB.warn), PANGO_ELLIPSIZE_NONE);
    gtk_box_append(GTK_BOX(body), KB.warn);
    if (b) {
        GtkWidget *rm = text_btn("Remove shortcut", "hm-btn destructive");
        gtk_widget_set_halign(rm, GTK_ALIGN_START);
        g_signal_connect(rm, "clicked", G_CALLBACK(kb_do_remove), NULL);
        gtk_box_append(GTK_BOX(body), rm);
    }
    g_signal_connect(KB.combo, "changed", G_CALLBACK(kb_dialog_changed), NULL);
    if (KB.cmd) g_signal_connect(KB.cmd, "changed", G_CALLBACK(kb_dialog_changed), NULL);
    g_signal_connect(KB.save, "clicked", G_CALLBACK(kb_do_save), NULL);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(kb_record_key), NULL);
    gtk_widget_add_controller(gtk_widget_get_parent(body), keys);
    kb_refresh_dialog();
    gtk_widget_grab_focus(KB.desc);
}

static void kb_edit_clicked(GtkWidget *w, gpointer b) { (void)w; kb_dialog(b); }
static void kb_add_clicked(GtkButton *b, gpointer d) { (void)b; (void)d; if (KB.kf) kb_dialog(NULL); }

static gboolean kb_matches(Keybind *b, char **words) {
    char **pp = pretty_parts(b->combo), *pretty = g_strjoinv(" ", pp);
    char *hay = g_strdup_printf("%s %s %s %s %s", b->description, b->combo, pretty, b->action, b->section);
    char *low = g_utf8_strdown(hay, -1);
    gboolean ok = TRUE;
    for (char **w = words; *w && ok; w++) if (**w && !strstr(low, *w)) ok = FALSE;
    g_strfreev(pp); g_free(pretty); g_free(hay); g_free(low);
    return ok;
}

static void kb_fill(void) {
    for (GtkWidget *c; (c = gtk_widget_get_first_child(KB.box));) gtk_box_remove(GTK_BOX(KB.box), c);
    if (!KB.kf) return;
    char *q = g_utf8_strdown(gtk_editable_get_text(GTK_EDITABLE(KB.search)), -1), **words = g_strsplit_set(g_strstrip(q), " \t", -1);
    const char *section = NULL;
    GtkWidget *list = NULL;
    int shown = 0;
    for (guint i = 0; i < KB.kf->binds->len; i++) {
        Keybind *b = g_ptr_array_index(KB.kf->binds, i);
        if (!kb_matches(b, words)) continue;
        if (!section || strcmp(section, b->section)) {   // sections come in file order
            section = b->section;
            char *up = g_utf8_strup(section, -1);
            gtk_box_append(GTK_BOX(KB.box), lbl(up, "section-label", 0));
            g_free(up);
            list = boxed_list();
            gtk_box_append(GTK_BOX(KB.box), list);
        }
        GtkWidget *row = row_add(list, *b->description ? b->description : "(no description)", b->action);
        gtk_label_set_wrap(g_object_get_data(G_OBJECT(row), "hm-sub"), FALSE);
        gtk_label_set_ellipsize(g_object_get_data(G_OBJECT(row), "hm-sub"), PANGO_ELLIPSIZE_END);
        if (kb_has_conflict(KB.kf, b)) {
            GtkWidget *w = gtk_image_new_from_icon_name("dialog-warning-symbolic");
            gtk_widget_add_css_class(w, "conflict");
            gtk_widget_set_tooltip_text(w, "Another shortcut uses the same keys");
            row_prefix(row, w);
        }
        row_suffix(row, keycaps(b->combo));
        if (b->readonly) {
            GtkWidget *l = lbl("loop", "hm-row-sub", 0.5);
            gtk_widget_set_tooltip_text(l, "Defined in a loop in keybind.lua; edit it there");
            row_suffix(row, l);
        } else {
            GtkWidget *e = icon_btn("document-edit-symbolic", "Edit");
            gtk_widget_set_can_target(e, FALSE);
            row_suffix(row, e);
            row_on_activate(row, kb_edit_clicked, b);
        }
        shown++;
    }
    if (!shown) gtk_box_append(GTK_BOX(KB.box), lbl("No shortcuts found", "hm-empty", 0.5));
    g_strfreev(words);
    g_free(q);
}

static void kb_search_changed(GtkSearchEntry *e, gpointer d) { (void)e; (void)d; kb_fill(); }
static gboolean kb_fill_idle(gpointer d) { (void)d; kb_fill(); return G_SOURCE_REMOVE; }

static GtkWidget *page_keybinds(void) {
    GtkWidget *outer = VBOX(0, NULL), *top = HBOX(8, "hm-kb-top");
    KB.search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(KB.search), "Search shortcuts (name, key or action)…");
    gtk_widget_set_hexpand(KB.search, TRUE);
    g_signal_connect(KB.search, "search-changed", G_CALLBACK(kb_search_changed), NULL);
    gtk_box_append(GTK_BOX(top), KB.search);
    GtkWidget *add = gtk_button_new();
    GtkWidget *ac = HBOX(6, NULL);
    gtk_box_append(GTK_BOX(ac), gtk_image_new_from_icon_name("list-add-symbolic"));
    gtk_box_append(GTK_BOX(ac), gtk_label_new("Add shortcut"));
    gtk_button_set_child(GTK_BUTTON(add), ac);
    add_classes(add, "hm-btn suggested");
    gtk_widget_set_cursor_from_name(add, "pointer");
    g_signal_connect(add, "clicked", G_CALLBACK(kb_add_clicked), NULL);
    gtk_box_append(GTK_BOX(top), add);
    gtk_box_append(GTK_BOX(outer), top);
    GtkWidget *hint = gtk_label_new("Edits go straight into ~/.config/hypr/keybind.lua and are live right away. "
                                    "A backup is kept, and a change Hyprland rejects is undone automatically.");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_add_css_class(hint, "hm-kb-hint");
    gtk_box_append(GTK_BOX(outer), hint);
    KB.box = VBOX(10, "hm-page");
    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), KB.box);
    gtk_box_append(GTK_BOX(outer), sc);
    GError *err = NULL;
    KB.kf = kb_open(&err);
    if (!KB.kf) {
        char *t = g_strdup_printf("Cannot read ~/.config/hypr/keybind.lua: %s", err ? err->message : "?");
        gtk_box_append(GTK_BOX(KB.box), lbl(t, "hm-empty", 0));
        g_free(t);
        g_clear_error(&err);
    } else g_idle_add(kb_fill_idle, NULL);   // the window appears first, the list fills next
    return outer;
}
