"""hyprminimal themes: Charcoal (from the django-react template, front/src/css/theme.css) and Tokyo Night,
turned into the colour roles every hyprminimal part uses, plus the "liquid glass" look.

The repo's own style files are written in Tokyo Night Moon. recolor() swaps each of those colours
for the chosen theme's colour in the same role, so every file (waybar, swaync, OSD, rofi, hyprlock,
Settings, bar popups) follows the theme without keeping seven copies of each one.
"""
import re

DEFAULT = "charcoal"

# theme tokens (Charcoal copied from the django-react theme.css)
TEMPLATE = {
    "charcoal": dict(label="Charcoal", dark=True, bg="#181818", bg_elev="#202020", surface="#292929",
                     text="#d4d4d4", text_strong="#f5f5f5", muted="#999999", accent="#a3a3a3",
                     accent_hover="#d4d4d4", danger="#ef7777"),
    # hyprminimal's original palette (Tokyo Night Moon): the repo's style files are written in it
    "tokyo":    dict(label="Tokyo Night", dark=True, bg="#1f2335", bg_elev="#262a40", surface="#2c314a",
                     text="#c0caf5", text_strong="#c0caf5", muted="#a9b1d6", accent="#7a88cf",
                     accent_hover="#9aa5e0", danger="#f7768e"),
}
ORDER = ["charcoal", "tokyo"]
DESCRIPTION = {
    "charcoal": "Neutral dark grey, soft grey accent",
    "tokyo": "Tokyo Night Moon, periwinkle and pink",
}


def _rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def _hex(rgb):
    return "#" + "".join(f"{max(0, min(255, round(c))):02x}" for c in rgb)


def mix(a, b, t):
    """a blended toward b by t (0..1)."""
    ra, rb = _rgb(a), _rgb(b)
    return _hex(x + (y - x) * t for x, y in zip(ra, rb))


def get(name):
    return name if name in TEMPLATE else DEFAULT


def roles(name):
    """The hyprminimal colour roles of a theme (same names as colors/palette.css)."""
    if get(name) == "tokyo":   # exactly the original colours: recolor() leaves every file as it is
        r = {role: "#" + hx for hx, role in TOKYO.items()}
        r.update(on_primary="#1f2335", dark=True, label="Tokyo Night")
        return r
    t = TEMPLATE[get(name)]
    dark, bg, ts, tx = t["dark"], t["bg"], t["text_strong"], t["text"]
    r = {
        "primary": t["accent"],
        "accent_text": mix(t["accent"], ts, 0.3),          # accent used as text (titles, links)
        "on_primary": bg if dark else t["bg_elev"],
        "secondary": mix(t["accent"], ts, 0.4) if dark else mix(t["accent"], ts, 0.35),
        "tertiary": "#9cd58a" if dark else "#3f8f4f",
        "error": t["danger"],
        "warning": "#e6c384" if dark else "#b7791f",
        "surface": bg,
        "sidebar": mix(bg, "#000000", 0.2) if dark else mix(bg, ts, 0.03),
        "dialog": mix(bg, ts, 0.05) if dark else t["bg_elev"],
        "card": t["bg_elev"],
        "surface_var": t["surface"] if dark else mix(bg, ts, 0.07),
        "border_soft": mix(bg, ts, 0.13),
        "outline": mix(bg, ts, 0.24),
        "placeholder": mix(bg, t["muted"], 0.8),
        "on_surface": mix(tx, t["muted"], 0.35),
        "on_surface_var": tx,
        "shadow": mix(bg, "#000000", 0.4) if dark else mix(ts, bg, 0.2),
        "state_charge": mix(t["accent"], tx, 0.35),
        "state_warn": "#c8b48e" if dark else "#9a7b3c",
        "state_crit": mix(t["danger"], bg, 0.2),
        "state_low": t["danger"],
        "state_bolt": "#e8d5a6" if dark else "#a07b1e",
        "linen": tx,
        "wall_bg": mix(bg, "#000000", 0.1) if dark else bg,
    }
    r["dark"] = dark
    r["label"] = t["label"]
    return r


# Tokyo Night Moon colour (as written in the repo's files) -> role
TOKYO = {
    "7a88cf": "primary", "9aa5e0": "accent_text", "d7729f": "secondary", "9cd58a": "tertiary",
    "f7768e": "error", "e6c384": "warning", "1f2335": "surface", "1a1e2e": "sidebar",
    "24283b": "dialog", "262a40": "card", "2c314a": "surface_var", "343a5c": "border_soft",
    "4b517a": "outline", "737aa2": "placeholder", "a9b1d6": "on_surface", "c0caf5": "on_surface_var",
    "181b2a": "shadow", "8ea4c8": "state_charge", "c8b48e": "state_warn", "cf8f9a": "state_crit",
    "e5534b": "state_low", "e8d5a6": "state_bolt", "e6ddd0": "linen", "222436": "wall_bg",
}
TOKYO_RGB = {  # rgba(r, g, b, a) spellings
    (31, 35, 53): "surface", (44, 49, 74): "surface_var", (122, 136, 207): "primary",
    (24, 27, 42): "shadow", (192, 202, 245): "on_surface_var", (169, 177, 214): "on_surface",
    (247, 118, 142): "error",
}
_HEX_RE = re.compile(r"(?<![0-9a-zA-Z])(" + "|".join(TOKYO) + r")", re.I)
_RGB_RE = re.compile(r"rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(,|\))")


def recolor(text, name):
    """Swap the repo's Tokyo Night Moon colours for theme `name`, role by role, in one pass."""
    r = roles(name)

    def hx(m):
        return r[TOKYO[m.group(1).lower()]].lstrip("#")

    def rgb(m):
        key = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        if key not in TOKYO_RGB:
            return m.group(0)
        c = _rgb(r[TOKYO_RGB[key]])
        head = m.group(0).split("(")[0]
        return f"{head}({c[0]}, {c[1]}, {c[2]}{m.group(4)}"
    return _RGB_RE.sub(rgb, _HEX_RE.sub(hx, text))


def palette_css(name):
    """GTK @define-color block for waybar and swaync (replaces colors/palette.css)."""
    r = roles(name)
    keys = ["primary", "on_primary", "secondary", "tertiary", "error", "warning", "surface", "surface_var",
            "on_surface", "on_surface_var", "outline", "shadow", "state_charge", "state_warn", "state_crit",
            "state_low", "state_bolt", "linen"]
    out = [f"/* hyprminimal palette: {r['label']} (generated by hyprminimal-settings) */"]
    out += [f"@define-color {k:<14} {r[k]};" for k in keys]
    return "\n".join(out) + "\n"


def glass_edge(name, strength=1.0):
    """Light catching the rim of a glass pane: a bright hairline on dark themes, white on light."""
    dark = roles(name)["dark"]
    return f"rgba(255, 255, 255, {0.10 * strength:.2f})" if dark else f"rgba(255, 255, 255, {0.65 * strength:.2f})"


def _rgba(hexc, a):
    c = _rgb(hexc)
    return f"rgba({c[0]}, {c[1]}, {c[2]}, {a:.2f})"


# ---------------------------------------------------------------- liquid glass overrides
# Appended after each component's own CSS. Translucent tinted surfaces (Hyprland blurs what's behind
# them, see glass_hypr), a bright hairline rim, an inner top highlight and soft deep shadows.
def glass_gtk3(name, part):
    """waybar / swaync (GTK3 CSS, uses the @define-color names)."""
    r = roles(name)
    edge, hi = glass_edge(name), glass_edge(name, 0.6)
    drop = "rgba(0, 0, 0, 0.35)" if r["dark"] else "rgba(0, 0, 0, 0.12)"
    if part == "waybar":
        return (f"window#waybar {{ border-radius: 14px; border: 1px solid {edge}; }}\n"
                f"#custom-media {{ background-color: alpha(@surface_var, 0.55); border: 1px solid {hi}; }}\n"
                f"tooltip {{ background: alpha(@surface, 0.80); border: 1px solid {edge}; border-radius: 12px; }}\n")
    return (  # swaync
        f".notification {{ border-radius: 18px; border: 1px solid {edge};"
        f" box-shadow: inset 0 1px {hi}, 0 8px 24px {drop}; }}\n"
        f".notification.critical {{ border-color: alpha(@error, 0.8); }}\n"
        f".notification-default-action, .notification-action {{ border-radius: 18px; }}\n"
        f".notification-action {{ border-radius: 12px; background: alpha(@surface_var, 0.5); border-color: {edge}; }}\n"
        f".control-center {{ background: alpha(@surface, 0.90); border-radius: 24px; border: 1px solid {edge};"
        f" box-shadow: inset 0 1px {hi}, 0 14px 40px {drop}; }}\n"
        f".control-center .notification {{ background-color: alpha(@surface_var, 0.45); box-shadow: none; }}\n"
        f".widget-mpris .widget-mpris-player {{ border-radius: 18px; background-color: alpha(@surface_var, 0.45);"
        f" border-color: {edge}; }}\n"
        f".widget-mpris .widget-mpris-player .mpris-overlay .widget-mpris-album-art {{ border-radius: 12px; }}\n"
        f".widget-buttons-grid flowboxchild > button {{ background: alpha(@surface_var, 0.42); border-color: {edge};"
        f" box-shadow: inset 0 1px {hi}; }}\n"
        f".widget-title > button {{ background: alpha(@surface_var, 0.45); border-color: {edge}; }}\n"
        f".inline-reply-entry {{ border-radius: 12px; }}\n")


def glass_osd(name):
    edge, hi = glass_edge(name), glass_edge(name, 0.6)
    r = roles(name)
    return (f"window#osd {{ background: {_rgba(r['surface'], 0.85)}; border: 1px solid {edge};"
            f" border-radius: 22px; box-shadow: inset 0 1px {hi}; }}\n"
            f"window#osd trough {{ border-radius: 999px; background: {_rgba(r['surface_var'], 0.7)}; }}\n"
            "window#osd progress, window#osd progressbar { border-radius: 999px; }\n")


def glass_panels(name):
    """Bar popups (GTK4: helpers/panels.c via .generated/panels.css, and settings/panels.py)."""
    r = roles(name)
    edge, hi = glass_edge(name), glass_edge(name, 0.6)
    drop = "rgba(0, 0, 0, 0.35)" if r["dark"] else "rgba(0, 0, 0, 0.12)"
    sv = _rgba(r["surface_var"], 0.42)
    return (f".panel {{ background: {_rgba(r['surface'], 0.92)}; border: 1px solid {edge}; border-radius: 22px;"
            f" box-shadow: inset 0 1px {hi}, 0 10px 32px {drop}; }}\n"
            f".panel .card {{ background: {sv}; border: 1px solid {edge}; border-radius: 14px; }}\n"
            f".panel button.tile {{ background: {sv}; border: 1px solid {edge}; border-radius: 14px; }}\n"
            f".panel button.tile:hover {{ background: {_rgba(r['primary'], 0.28)}; }}\n"
            f".panel .art {{ border-radius: 12px; border-color: {edge}; }}\n"
            ".panel levelbar trough, .panel levelbar block.filled, .panel scale.timeline trough,"
            " .panel scale.timeline highlight { border-radius: 999px; }\n"
            ".panel calendar > grid > label.today, .panel calendar > grid > label:selected { border-radius: 8px; }\n"
            f".panel .wifi-pw {{ border-radius: 12px; background: {sv}; border-color: {edge}; }}\n")


def glass_settings(name):
    """Settings window (libadwaita): translucent window, frosted sidebar, glass cards."""
    r = roles(name)
    edge = glass_edge(name)
    return (":root {"
            f" --window-bg-color: {_rgba(r['surface'], 0.94)};"
            f" --view-bg-color: transparent; --headerbar-bg-color: transparent;"
            f" --sidebar-bg-color: {_rgba(r['sidebar'], 0.6)};"
            f" --secondary-sidebar-bg-color: {_rgba(r['sidebar'], 0.45)};"
            f" --card-bg-color: {_rgba(r['card'], 0.55)};"
            f" --dialog-bg-color: {_rgba(r['dialog'], 0.92)};"
            f" --popover-bg-color: {_rgba(r['dialog'], 0.94)}; }}\n"
            f".boxed-list, .card {{ border: 1px solid {edge}; border-radius: 14px; }}\n"
            ".preferences row, row.entry { border-radius: 14px; }\n"
            ".navigation-sidebar > row { border-radius: 10px; }\n"
            ".keycap { border-radius: 7px; }\n"
            ".wall { border-radius: 12px; }\n"
            "searchbar > revealer > box, entry.search { border-radius: 10px; }\n")


def glass_rofi(name):
    """Appended to the rofi theme (rasi): later rules win."""
    r = roles(name)
    a = "eb" if r["dark"] else "f0"
    edge = "#ffffff1a" if r["dark"] else "#ffffffa6"
    return ("\n/* liquid glass (hyprminimal theme) */\n"
            f"* {{ surface: {r['surface']}{a}; surface-var: {r['surface_var']}80; }}\n"
            f"window {{ border-radius: 22px; border-color: {edge}; }}\n"
            "element { border-radius: 12px; }\n"
            f"inputbar {{ border-radius: 14px; border-color: {edge}; }}\n"
            "entry { border-radius: 10px; }\n"
            "message { border-radius: 12px; }\n")


def hypr_lua(name, glass):
    """Hyprland window colours (+ glass blur) as a Lua chunk, loaded by hypr/layerrules.lua."""
    r = roles(name)
    h = lambda k: "rgb(" + r[k].lstrip("#") + ")"
    blur = ""
    if glass:
        blur = ("    decoration = { blur = { enabled = true, size = 8, passes = 3, vibrancy = 0.18,"
                " noise = 0.015, contrast = 1.0, brightness = 1.0, popups = true, new_optimizations = true,"
                " ignore_opacity = true } },\n")
    return ("-- generated by hyprminimal-settings (theme: " + r["label"] + "), edit Settings -> General instead\n"
            "hl.config({\n"
            "    general = { col = {\n"
            f"        active_border = \"{h('primary')}\", inactive_border = \"{h('border_soft')}\",\n"
            "    } },\n"
            "    group = {\n"
            f"        col = {{ border_active = \"{h('secondary')}\", border_inactive = \"{h('border_soft')}\",\n"
            f"                border_locked_active = \"{h('error')}\", border_locked_inactive = \"{h('border_soft')}\" }},\n"
            f"        groupbar = {{ col = {{ active = \"{h('secondary')}\", inactive = \"{h('surface_var')}\",\n"
            f"                             locked_active = \"{h('error')}\", locked_inactive = \"{h('surface_var')}\" }},\n"
            f"                     text_color = \"{h('on_primary')}\", text_color_inactive = \"{h('on_surface')}\" }},\n"
            "    },\n"
            + blur +
            "})\n"
            "return " + ("true" if glass else "false") + "\n")


# terminal ANSI colours: one calm set for dark themes, one for light ones
ANSI_DARK = ["#1c1c1c", "#e06c75", "#98c379", "#d7b46a", "#7aa2c7", "#b48ead", "#78b6ad", "#c8c8c8",
             "#5c5c5c", "#ef8a92", "#b0d696", "#e5c88a", "#98bbdc", "#c9a6c4", "#97ccc4", "#f0f0f0"]
ANSI_LIGHT = ["#2b2b2b", "#b4443c", "#3f7d45", "#9a6a00", "#2f5f9a", "#8a4f8a", "#2f7f7a", "#c8c8c8",
              "#6b6b6b", "#c9564d", "#4f9157", "#b07d0a", "#3f73b3", "#9f63a0", "#3a948e", "#efefef"]


ANSI_TOKYO = ["#181b2a", "#f7768e", "#9cd58a", "#e6c384", "#7a88cf", "#d7729f", "#86c1d6", "#a9b1d6",
              "#4b517a", "#cf8f9a", "#b4e3a4", "#e8d5a6", "#8ea4c8", "#e391b5", "#a6d4e4", "#c0caf5"]


def kitty_conf(name):
    r, t = roles(name), TEMPLATE[get(name)]
    ansi = ANSI_TOKYO if get(name) == "tokyo" else ANSI_DARK if r["dark"] else ANSI_LIGHT
    lines = [f"# hyprminimal kitty colours: {r['label']} (generated by hyprminimal-settings)",
             f"foreground            {t['text']}", f"background            {t['bg']}",
             f"selection_foreground  {r['on_primary']}", f"selection_background  {r['primary']}",
             f"cursor                {r['primary']}", f"cursor_text_color     {r['on_primary']}",
             f"url_color             {r['state_charge']}",
             f"active_border_color   {r['primary']}", f"inactive_border_color {r['outline']}",
             f"active_tab_foreground   {r['on_primary']}", f"active_tab_background   {r['primary']}",
             f"inactive_tab_foreground {r['on_surface']}", f"inactive_tab_background {r['surface_var']}",
             f"tab_bar_background      {t['bg']}"]
    lines += [f"color{i:<2} {c}" for i, c in enumerate(ansi)]
    return "\n".join(lines) + "\n"


def swatches(name):
    r = roles(name)
    return [TEMPLATE[get(name)]["bg"], r["surface_var"], r["primary"], r["on_surface_var"]]
