// Settings widget kit: preference pages, groups and rows (switch, spin, combo, entry, expander, button),
// toasts and in-window dialogs. Plain GTK4, styled by settings/panels.css (window.hm-settings).

typedef void (*RowFn)(GtkWidget *w, gpointer d);

static GtkWidget *page_new(GtkWidget **content) {   // scrolled page; *content: VBOX to put groups in
    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sc, TRUE);
    GtkWidget *v = VBOX(26, "hm-page");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), v);
    *content = v;
    return sc;
}

static void list_row_activated(GtkListBox *l, GtkListBoxRow *row, gpointer d) {
    (void)l; (void)d;
    RowFn fn = g_object_get_data(G_OBJECT(row), "hm-activate");
    if (fn) fn(GTK_WIDGET(row), g_object_get_data(G_OBJECT(row), "hm-activate-data"));
}

static GtkWidget *boxed_list(void) {
    GtkWidget *l = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(l), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(l, "hm-list");
    gtk_widget_set_overflow(l, GTK_OVERFLOW_HIDDEN);   // row hovers clip to the card's rounded corners
    g_signal_connect(l, "row-activated", G_CALLBACK(list_row_activated), NULL);
    return l;
}

// a titled group of rows; returns its list (rows go in with group_add). desc may contain markup.
static GtkWidget *group_new(GtkWidget *page, const char *title, const char *desc) {
    GtkWidget *g = VBOX(8, "hm-group"), *head = HBOX(12, NULL), *texts = VBOX(2, NULL), *suffix = HBOX(4, "hm-group-suffix");
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
    if (title && *title) gtk_box_append(GTK_BOX(texts), lbl(title, "hm-group-title", 0));
    if (desc && *desc) {
        GtkWidget *d = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(d), desc);
        gtk_label_set_wrap(GTK_LABEL(d), TRUE);
        gtk_label_set_xalign(GTK_LABEL(d), 0);
        gtk_widget_add_css_class(d, "hm-group-desc");
        gtk_box_append(GTK_BOX(texts), d);
    }
    gtk_box_append(GTK_BOX(head), texts);
    gtk_box_append(GTK_BOX(head), suffix);
    if ((title && *title) || (desc && *desc)) gtk_box_append(GTK_BOX(g), head);
    GtkWidget *l = boxed_list();
    gtk_box_append(GTK_BOX(g), l);
    g_object_set_data(G_OBJECT(l), "hm-suffix", suffix);
    g_object_set_data(G_OBJECT(l), "hm-group", g);
    gtk_box_append(GTK_BOX(page), g);
    return l;
}

static void group_suffix(GtkWidget *list, GtkWidget *w) {
    gtk_box_append(GTK_BOX(g_object_get_data(G_OBJECT(list), "hm-suffix")), w);
}

static void group_add(GtkWidget *list, GtkWidget *row) { gtk_list_box_append(GTK_LIST_BOX(list), row); }

static GtkWidget *row_of(GtkWidget *w) { return gtk_widget_get_ancestor(w, GTK_TYPE_LIST_BOX_ROW); }

// prefix | title over subtitle | suffix, stored on owner (the row) for row_prefix() and friends
static GtkWidget *row_layout(GtkWidget *owner, const char *title, const char *sub) {
    GtkWidget *h = HBOX(12, "hm-row-box"), *pre = HBOX(8, NULL), *texts = VBOX(2, NULL), *suf = HBOX(6, NULL);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(pre, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(suf, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(pre, FALSE);   // shown by row_prefix/row_suffix: an empty box would still take the row's spacing
    gtk_widget_set_visible(suf, FALSE);
    GtkWidget *t = gtk_label_new(title ? title : "");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_wrap(GTK_LABEL(t), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(t), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(t, "hm-row-title");
    gtk_widget_set_visible(t, title != NULL);
    GtkWidget *s = gtk_label_new(sub ? sub : "");
    gtk_label_set_xalign(GTK_LABEL(s), 0);
    gtk_label_set_wrap(GTK_LABEL(s), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(s), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(s, "hm-row-sub");
    gtk_widget_set_visible(s, sub && *sub);
    gtk_box_append(GTK_BOX(texts), t);
    gtk_box_append(GTK_BOX(texts), s);
    gtk_box_append(GTK_BOX(h), pre);
    gtk_box_append(GTK_BOX(h), texts);
    gtk_box_append(GTK_BOX(h), suf);
    g_object_set_data(G_OBJECT(owner), "hm-prefix", pre);
    g_object_set_data(G_OBJECT(owner), "hm-suffix", suf);
    g_object_set_data(G_OBJECT(owner), "hm-title", t);
    g_object_set_data(G_OBJECT(owner), "hm-sub", s);
    g_object_set_data(G_OBJECT(owner), "hm-texts", texts);
    return h;
}

static GtkWidget *row_new(const char *title, const char *sub) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_set_focusable(row, FALSE);
    gtk_widget_add_css_class(row, "hm-row");
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_layout(row, title, sub));
    return row;
}

// a prefix/suffix box is visible while any of its children is, so text and fields line up with the card edges
static void slot_sync(GtkWidget *box) {
    gboolean any = FALSE;
    for (GtkWidget *c = gtk_widget_get_first_child(box); c && !any; c = gtk_widget_get_next_sibling(c)) any = gtk_widget_get_visible(c);
    gtk_widget_set_visible(box, any);
}
static void slot_child_visible(GtkWidget *w, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    GtkWidget *box = gtk_widget_get_parent(w);
    if (box) slot_sync(box);
}
static void slot_append(GtkWidget *box, GtkWidget *w) {
    gtk_box_append(GTK_BOX(box), w);
    g_signal_connect(w, "notify::visible", G_CALLBACK(slot_child_visible), NULL);
    slot_sync(box);
}
static void row_prefix(GtkWidget *row, GtkWidget *w) { slot_append(g_object_get_data(G_OBJECT(row), "hm-prefix"), w); }
static void row_suffix(GtkWidget *row, GtkWidget *w) { slot_append(g_object_get_data(G_OBJECT(row), "hm-suffix"), w); }
static void row_set_title(GtkWidget *row, const char *t) {
    GtkWidget *l = g_object_get_data(G_OBJECT(row), "hm-title");
    gtk_label_set_label(GTK_LABEL(l), t);
    gtk_widget_set_visible(l, TRUE);
}
static void row_sub_selectable(GtkWidget *row) { gtk_label_set_selectable(g_object_get_data(G_OBJECT(row), "hm-sub"), TRUE); }

static void row_on_activate(GtkWidget *row, RowFn fn, gpointer d) {
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    gtk_widget_set_cursor_from_name(row, "pointer");
    g_object_set_data(G_OBJECT(row), "hm-activate", fn);
    g_object_set_data(G_OBJECT(row), "hm-activate-data", d);
}

static GtkWidget *row_add(GtkWidget *list, const char *title, const char *sub) {
    GtkWidget *r = row_new(title, sub);
    group_add(list, r);
    return r;
}

static GtkWidget *icon_btn(const char *icon, const char *tip) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(b, "pointer");
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    return b;
}

static GtkWidget *text_btn(const char *label, const char *classes) {
    GtkWidget *b = gtk_button_new_with_label(label);
    add_classes(b, classes);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(b, "pointer");
    return b;
}

static void toggle_switch(GtkWidget *row, gpointer sw) { (void)row; gtk_switch_set_active(sw, !gtk_switch_get_active(sw)); }

// row with a switch; connect "notify::active" on the returned switch
static GtkWidget *switch_row(GtkWidget *list, const char *title, const char *sub, gboolean on) {
    GtkWidget *r = row_add(list, title, sub), *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), on);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(sw, "pointer");
    row_suffix(r, sw);
    row_on_activate(r, toggle_switch, sw);
    return sw;
}

static void focus_widget(GtkWidget *row, gpointer w) { (void)row; gtk_widget_grab_focus(w); }

// row with a number field; connect "value-changed" on the returned spin button
static GtkWidget *spin_row(GtkWidget *list, const char *title, const char *sub, double lo, double hi, double step, double v) {
    GtkWidget *r = row_add(list, title, sub), *sp = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp), v);
    gtk_widget_set_valign(sp, GTK_ALIGN_CENTER);
    row_suffix(r, sp);
    row_on_activate(r, focus_widget, sp);
    return sp;
}

static void open_dropdown(GtkWidget *row, gpointer dd) { (void)row; gtk_widget_activate(dd); }

// row with a drop-down; connect "notify::selected" on the returned GtkDropDown
static GtkWidget *combo_row(GtkWidget *list, const char *title, const char *sub, const char *const *labels, int sel) {
    GtkWidget *r = row_add(list, title, sub), *dd = gtk_drop_down_new_from_strings(labels);
    if (sel >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), sel);
    gtk_widget_set_valign(dd, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(dd, "pointer");
    row_suffix(r, dd);
    row_on_activate(r, open_dropdown, dd);
    return dd;
}

// ---------------------------------------------------------------- entry rows
// Title over a text field (no title: just the field, on one line with the row's buttons). With apply: a ✓ button shows while the text differs from what was applied
// last; it and Enter call apply(entry, data).
static void entry_apply(GtkWidget *e) {
    RowFn fn = g_object_get_data(G_OBJECT(e), "hm-apply");
    if (!fn) return;
    g_object_set_data_full(G_OBJECT(e), "hm-orig", g_strdup(gtk_editable_get_text(GTK_EDITABLE(e))), g_free);
    gtk_widget_set_visible(g_object_get_data(G_OBJECT(e), "hm-ok"), FALSE);
    fn(e, g_object_get_data(G_OBJECT(e), "hm-apply-data"));
}
static void entry_activate(GtkWidget *e, gpointer d) { (void)d; entry_apply(e); }
static void entry_ok_clicked(GtkButton *b, gpointer e) { (void)b; entry_apply(e); }
static void entry_changed(GtkEditable *e, gpointer d) {
    (void)d;
    GtkWidget *ok = g_object_get_data(G_OBJECT(e), "hm-ok");
    const char *orig = g_object_get_data(G_OBJECT(e), "hm-orig");
    if (ok) gtk_widget_set_visible(ok, strcmp(gtk_editable_get_text(e), orig ? orig : ""));
}

static void entry_set_text(GtkWidget *e, const char *t) {   // as if applied: no ✓
    g_object_set_data_full(G_OBJECT(e), "hm-orig", g_strdup(t ? t : ""), g_free);
    gtk_editable_set_text(GTK_EDITABLE(e), t ? t : "");
}

static void entry_row_pressed(GtkGestureClick *g, int n, double x, double y, gpointer e) {
    (void)g; (void)n; (void)x; (void)y;
    if (!gtk_widget_has_focus(e) && !gtk_widget_is_ancestor(gtk_root_get_focus(gtk_widget_get_root(e)), e)) gtk_widget_grab_focus(e);
}

static void entry_peek_clicked(GtkButton *b, gpointer e) {
    gboolean show = !gtk_entry_get_visibility(GTK_ENTRY(e));
    gtk_entry_set_visibility(GTK_ENTRY(e), show);
    gtk_button_set_icon_name(b, show ? "view-conceal-symbolic" : "view-reveal-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(b), show ? "Hide password" : "Show password");
}

static GtkWidget *entry_row_full(GtkWidget *list, const char *title, const char *text, gboolean password,
                                 RowFn apply, gpointer d, GtkWidget **row_out) {
    GtkWidget *row = row_new(NULL, NULL);
    gtk_widget_add_css_class(row, "hm-entry-row");
    GtkWidget *texts = g_object_get_data(G_OBJECT(row), "hm-texts");
    if (title) {
        row_set_title(row, title);
        gtk_widget_add_css_class(g_object_get_data(G_OBJECT(row), "hm-title"), "hm-entry-title");
        // buttons beside the field sit level with it, not centred on title + field
        gtk_widget_set_valign(g_object_get_data(G_OBJECT(row), "hm-suffix"), GTK_ALIGN_END);
    }
    GtkWidget *e = gtk_entry_new();
    gtk_widget_set_hexpand(e, TRUE);
    gtk_box_append(GTK_BOX(texts), e);
    entry_set_text(e, text);
    if (password) {   // hidden; the eye sits beside the field (with ✓ / copy), not inside it
        gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
        GtkWidget *eye = icon_btn("view-reveal-symbolic", "Show password");
        g_signal_connect(eye, "clicked", G_CALLBACK(entry_peek_clicked), e);
        row_suffix(row, eye);
    }
    if (apply) {
        GtkWidget *ok = icon_btn("object-select-symbolic", "Apply");
        gtk_widget_add_css_class(ok, "hm-apply");
        gtk_widget_set_visible(ok, FALSE);
        row_suffix(row, ok);
        g_object_set_data(G_OBJECT(e), "hm-ok", ok);
        g_object_set_data(G_OBJECT(e), "hm-apply", apply);
        g_object_set_data(G_OBJECT(e), "hm-apply-data", d);
        g_signal_connect(ok, "clicked", G_CALLBACK(entry_ok_clicked), e);
        g_signal_connect(e, "changed", G_CALLBACK(entry_changed), NULL);
    }
    g_signal_connect(e, "activate", G_CALLBACK(entry_activate), NULL);
    GtkGesture *click = gtk_gesture_click_new();   // the whole row is a click target for its field
    g_signal_connect(click, "pressed", G_CALLBACK(entry_row_pressed), e);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    gtk_widget_set_cursor_from_name(row, "text");
    if (list) group_add(list, row);
    if (row_out) *row_out = row;
    return e;
}
#define entry_row(list, title, text, apply, d) entry_row_full(list, title, text, FALSE, apply, d, NULL)

// ---------------------------------------------------------------- expander rows
static void expander_toggle(GtkGestureClick *g, int n, double x, double y, gpointer row) {
    (void)g; (void)n; (void)x; (void)y;
    GtkWidget *inner = g_object_get_data(G_OBJECT(row), "hm-inner");
    if (!g_object_get_data(G_OBJECT(row), "hm-expandable")) return;
    gboolean open = !gtk_widget_get_visible(inner);
    gtk_widget_set_visible(inner, open);
    set_class(row, "open", open);
    RowFn fn = g_object_get_data(G_OBJECT(row), "hm-expand-fn");
    if (open && fn) fn(row, g_object_get_data(G_OBJECT(row), "hm-expand-data"));
}

// a row that opens to show more rows: returns the row; its inner list via expander_list(row)
static GtkWidget *expander_row(GtkWidget *list, const char *title, const char *sub, gboolean expandable) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_set_focusable(row, FALSE);
    gtk_widget_add_css_class(row, "hm-row");
    gtk_widget_add_css_class(row, "hm-expander");
    GtkWidget *v = VBOX(0, NULL), *hbox = row_layout(row, title, sub);
    gtk_box_append(GTK_BOX(v), hbox);
    GtkWidget *arrow = gtk_image_new_from_icon_name("pan-down-symbolic");
    gtk_widget_add_css_class(arrow, "hm-arrow");
    gtk_widget_set_visible(arrow, expandable);
    gtk_box_append(GTK_BOX(hbox), arrow);
    GtkWidget *inner = boxed_list();   // shown while open
    gtk_widget_add_css_class(inner, "hm-sublist");
    gtk_widget_set_visible(inner, FALSE);
    gtk_box_append(GTK_BOX(v), inner);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), v);
    g_object_set_data(G_OBJECT(row), "hm-inner", inner);
    g_object_set_data(G_OBJECT(row), "hm-expandable", GINT_TO_POINTER(expandable));
    if (expandable) {
        GtkGesture *g = gtk_gesture_click_new();
        g_signal_connect(g, "released", G_CALLBACK(expander_toggle), row);
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(g));
        gtk_widget_set_cursor_from_name(hbox, "pointer");
        gtk_widget_add_css_class(hbox, "hm-clickable");   // only the header hovers, not the rows it opens
    }
    if (list) group_add(list, row);
    return row;
}

static GtkWidget *expander_list(GtkWidget *row) { return g_object_get_data(G_OBJECT(row), "hm-inner"); }
static void expander_on_open(GtkWidget *row, RowFn fn, gpointer d) {
    g_object_set_data(G_OBJECT(row), "hm-expand-fn", fn);
    g_object_set_data(G_OBJECT(row), "hm-expand-data", d);
}

// a whole-row button ("Forget this network", "Save")
static GtkWidget *button_row(GtkWidget *list, const char *label, const char *cls, RowFn fn, gpointer d) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_focusable(row, FALSE);
    gtk_widget_add_css_class(row, "hm-row");
    gtk_widget_add_css_class(row, "hm-button-row");
    if (cls) gtk_widget_add_css_class(row, cls);
    GtkWidget *l = lbl(label, NULL, 0.5);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
    row_on_activate(row, fn, d);
    if (list) group_add(list, row);
    return row;
}

// copy text to the clipboard (the button's label says what)
static void settings_toast(const char *text, int seconds);
static void copy_clicked(GtkButton *b, gpointer d) {
    (void)d;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(b)), g_object_get_data(G_OBJECT(b), "hm-text"));
    char *t = g_strdup_printf("%s copied", (char *)g_object_get_data(G_OBJECT(b), "hm-what"));
    settings_toast(t, 3);
    g_free(t);
}

static GtkWidget *copy_button(const char *text, const char *what) {
    char *tip = g_strdup_printf("Copy %s", what);
    for (char *p = tip + 5; *p; p++) *p = g_ascii_tolower(*p);
    GtkWidget *b = icon_btn("edit-copy-symbolic", tip);
    g_free(tip);
    g_object_set_data_full(G_OBJECT(b), "hm-text", g_strdup(text), g_free);
    g_object_set_data_full(G_OBJECT(b), "hm-what", g_strdup(what), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(copy_clicked), NULL);
    return b;
}

static GtkWidget *dim_row(GtkWidget *list, const char *title, const char *sub) {
    GtkWidget *r = row_add(list, title, sub);
    gtk_widget_add_css_class(r, "dim");
    return r;
}
