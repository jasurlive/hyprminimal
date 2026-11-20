"""hyprminimal themes: Charcoal + Hygge, Spruce, Fjord, Linen, Matte, Sage (from the django-react template,
front/src/css/theme.css) and Tokyo Night,
turned into the colour roles every hyprminimal part uses. Solid surfaces only: no blur, no translucency.

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
    # the template's six extra themes (calm scandinavian palettes), same tokens
    "linen":    dict(label="Linen", dark=False, bg="#f4f1ec", bg_elev="#fbfaf7", surface="#ece8e1",
                     text="#3a3835", text_strong="#23211f", muted="#77726a", accent="#7a6a56",
                     accent_hover="#66584a", danger="#b4534b"),
    "matte":    dict(label="Matte", dark=False, bg="#e8e6e3", bg_elev="#f0eeeb", surface="#e1dfdb",
                     text="#3b3936", text_strong="#262422", muted="#75716b", accent="#8a6f5e",
                     accent_hover="#745c4d", danger="#b0504a"),
    "sage":     dict(label="Sage", dark=False, bg="#f1f1ec", bg_elev="#f9f9f5", surface="#e7e8e1",
                     text="#353a35", text_strong="#212521", muted="#737a72", accent="#6f8a6f",
                     accent_hover="#5c745c", danger="#b0504a"),
    "hygge":    dict(label="Hygge", dark=True, bg="#1d2023", bg_elev="#23272a", surface="#2a2e32",
                     text="#d6d0c6", text_strong="#eee9e1", muted="#8f8a82", accent="#c4a57c",
                     accent_hover="#d6bc98", danger="#d08a80"),
    "spruce":   dict(label="Spruce", dark=True, bg="#1a1f1d", bg_elev="#202624", surface="#27302c",
                     text="#cfd5cd", text_strong="#e9eee6", muted="#869087", accent="#9db59a",
                     accent_hover="#b6cbb3", danger="#d08a80"),
    "fjord":    dict(label="Fjord", dark=True, bg="#191e24", bg_elev="#1f252c", surface="#262d35",
                     text="#cdd4dc", text_strong="#e8edf2", muted="#858f9a", accent="#93abc2",
                     accent_hover="#afc3d6", danger="#d08a80"),
    # hyprminimal's original palette (Tokyo Night Moon): the repo's style files are written in it
    "tokyo":    dict(label="Tokyo Night", dark=True, bg="#1f2335", bg_elev="#262a40", surface="#2c314a",
                     text="#c0caf5", text_strong="#c0caf5", muted="#a9b1d6", accent="#7a88cf",
                     accent_hover="#9aa5e0", danger="#f7768e"),
}
ORDER = ["charcoal", "hygge", "spruce", "fjord", "linen", "matte", "sage", "tokyo"]
DESCRIPTION = {
    "charcoal": "Neutral dark grey, soft grey accent",
    "hygge": "Matte slate, linen text, smoked-oak accent",
    "spruce": "Nordic forest at dusk, soft moss accent",
    "fjord": "Deep slate-blue night, misty blue accent",
    "linen": "Warm off-white, oak accent (light)",
    "matte": "Soft stone grey, muted clay accent (light)",
    "sage": "Oat white, dried-eucalyptus green (light)",
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


def hypr_lua(name):
    """Hyprland window colours as a Lua chunk, loaded by hypr/layerrules.lua."""
    r = roles(name)
    h = lambda k: "rgb(" + r[k].lstrip("#") + ")"
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
            # the desktop: plain theme colour (helpers/panels.c draws the logo on top), no wallpaper program
            f"    misc = {{ background_color = \"{h('wall_bg')}\" }},\n"
            "})\n"
            "return true\n")


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
