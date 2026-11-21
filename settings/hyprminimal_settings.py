#!/usr/bin/env python3
"""hyprminimal settings: one place to control the hyprminimal shell and your Hyprland keybinds.

    hyprminimal-settings                 open the settings window
    hyprminimal-settings --page keybinds open on a page (general, bar, notifications,
                                      startup, nightlight, keybinds, system, about);
                                      without it, the page you had open last
    hyprminimal-settings --apply         regenerate configs from settings.json (no window)
    hyprminimal-settings --toggle-bar    flip Bar → Hide bar and regenerate (no restart; Super+M signals waybar)
"""
import copy
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # -I drops the script dir
import themes  # noqa: E402

DIR = Path(__file__).resolve().parent.parent           # the cloned repo (linked at ~/.local/share/hyprminimal)
SETTINGS = DIR / "settings.json"
GEN = DIR / ".generated"
RUN = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp")) / "hyprminimal"
ERRORS = RUN / "errors"   # NAME: why that part failed (written by bin/hyprminimal and render errors below)
# libgtk4-layer-shell is already loaded into this process: don't hand it to the programs we start
# (waybar is GTK3 and crashes when a GTK4 library is preloaded into it)
os.environ.pop("LD_PRELOAD", None)
# Settings page -> the component whose failure is shown on it (red banner)
PAGE_COMPONENT = {"bar": "waybar", "notifications": "swaync",
                  "general": "settings", "system": "swayosd-server"}
COMPONENT_NAME = {"waybar": "Bar", "swaync": "Notifications",
                  "settings": "Settings", "swayosd-server": "Volume/brightness OSD"}


def read_error(comp):
    try:
        return (ERRORS / comp).read_text().strip()
    except OSError:
        return ""


def write_error(comp, msg):
    """Record (msg) or clear (None) a failure, and refresh the bar's red error pill."""
    try:
        if msg:
            ERRORS.mkdir(parents=True, exist_ok=True)
            (ERRORS / comp).write_text(msg + "\n")
        elif (ERRORS / comp).exists():
            (ERRORS / comp).unlink()
        else:
            return
    except OSError:
        return
    subprocess.run(["pkill", "-RTMIN+9", "-x", "waybar"], capture_output=True)
HOME = Path.home()
HYPR = HOME / ".config/hypr"
KEYBIND_LUA = HYPR / "keybind.lua"
BACKUPS = HYPR / ".pre-hyprminimal-backup"
UI_STATE = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state") / "hyprminimal" / "settings-window.json"
IDLE_QUIT_SECONDS = 600                       # hidden window stays resident this long for instant reopen

# --------------------------------------------------------------------------- settings
DEFAULTS = {
    "general": {"font": "Noto Sans", "theme": themes.DEFAULT,
                # Settings starts hidden at login so Super+I opens instantly; off = cold start on demand
                "preload": True},
    # desktop background (helpers/panels.c): the name under the logo ("" = no text), logo on/off
    "desktop": {"name": "hyprminimal", "logo": True},
    "bar": {
        "font_size": 16, "icon_size": 20, "clock_size": 18, "font_weight": 500,   # one type scale
        "clock_24h": True,
        "hidden": False,          # bar off: waybar starts hidden (no exclusive zone), windows use the full screen
        "widgets": {
            "workspaces": True, "date": True, "clock": True,
            "media": True, "notifications": True, "wifi": False, "bluetooth": False,
            "volume": False, "tray": False, "battery": True, "network_speed": True,
        },
    },
    "notifications": {"position": "top-right", "timeout": 3},
    "nightlight": {"enabled": False, "temperature": 4000},
    # run once per login by `hyprminimal start` (see .generated/startup): [{"command": str, "enabled": bool}]
    "startup": {"commands": []},
}

WIDGET_LABELS = [
    ("workspaces", "Workspaces", "left"),
    ("date", "Date", "center"), ("clock", "Clock", "center"), ("media", "Media", "right"),
    ("notifications", "Notifications", "right"), ("wifi", "Wi-Fi", "right"),
    ("bluetooth", "Bluetooth", "right"), ("volume", "Volume", "right"), ("tray", "System tray", "right"),
    ("battery", "Battery", "right"), ("network_speed", "Network speed", "right"),
]
WIDGET_MODULE = {
    "workspaces": "ext/workspaces", "date": "clock#date",
    "clock": "clock", "media": "custom/media", "notifications": "custom/notification",
    "wifi": "network#wifi", "bluetooth": "bluetooth", "volume": "pulseaudio", "tray": "tray",
    "battery": "battery", "network_speed": "custom/net",
}


def deep_merge(base, over):
    out = copy.deepcopy(base)
    for k, v in (over or {}).items():
        out[k] = deep_merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def load_settings():
    try:
        return deep_merge(DEFAULTS, json.loads(SETTINGS.read_text()))
    except (OSError, ValueError):
        return copy.deepcopy(DEFAULTS)


def last_page():
    """The page Settings showed last (opening it without --page goes back there)."""
    try:
        return json.loads(UI_STATE.read_text()).get("page") or "general"
    except (OSError, ValueError, AttributeError):
        return "general"


def remember_page(pid):
    try:
        UI_STATE.parent.mkdir(parents=True, exist_ok=True)
        UI_STATE.write_text(json.dumps({"page": pid}) + "\n")
    except OSError:
        pass


def save_settings(s):
    tmp = SETTINGS.with_suffix(".tmp")
    tmp.write_text(json.dumps(s, indent=2) + "\n")
    tmp.replace(SETTINGS)


def tray_item_count():
    """How many apps currently registered a tray icon (StatusNotifierItem)."""
    out = subprocess.run(["busctl", "--user", "call", "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                          "org.freedesktop.DBus.Properties", "Get", "ss", "org.kde.StatusNotifierWatcher",
                          "RegisteredStatusNotifierItems"], capture_output=True, text=True, timeout=2).stdout.split()
    try:
        return int(out[2])          # 'v as N ...'
    except (IndexError, ValueError):
        return 0



def pointerize(root):
    """GTK4 has no CSS cursor: give every clickable widget under root a hand pointer."""
    from gi.repository import Adw, Gtk
    clickable = (Gtk.Button, Gtk.Switch, Gtk.Scale, Gtk.DropDown, Gtk.CheckButton, Gtk.Calendar,
                 Gtk.FlowBoxChild, Gtk.MenuButton, Gtk.FontDialogButton, Adw.ComboRow, Adw.SwitchRow)
    stack = [root]
    while stack:
        w = stack.pop()
        if isinstance(w, clickable) or (isinstance(w, Gtk.ListBoxRow) and w.get_activatable()
                                        and isinstance(w, Adw.ActionRow) and w.get_activatable_widget()):
            w.set_cursor_from_name("pointer")
        c = w.get_first_child()
        while c is not None:
            stack.append(c)
            c = c.get_next_sibling()


def hyprminimal_active():
    return (RUN / "active").exists()


def running(proc):
    return subprocess.run(["pgrep", "-x", proc], capture_output=True).returncode == 0


# Settings hidden while "Keep Settings ready" is on: the window stays mapped, parked on this special workspace
# nothing ever shows. Bringing it back is a move (~2 ms), not a remap + first frame (~80 ms), and a parked
# window gets no frame callbacks, so it costs nothing. hypr/layerrules.lua maps the preloaded window here.
PARK = "special:hyprminimal-settings"
PARK_TITLE = "hyprminimal settings (parked)"


def hypr(cmd):
    """One Hyprland IPC request over its socket (what hyprctl does, without starting a process)."""
    path = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp")) / "hypr" / os.environ.get("HYPRLAND_INSTANCE_SIGNATURE", "") / ".socket.sock"
    try:
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(1)
            s.connect(str(path))
            s.sendall(cmd.encode())
            out = b""
            while (b := s.recv(65536)):
                out += b
        return out.decode(errors="replace")
    except OSError:
        return ""


def hypr_json(cmd):
    try:
        return json.loads(hypr("j/" + cmd))
    except ValueError:
        return None


def sh(*cmd, detach=False):
    if detach:
        subprocess.Popen(list(cmd), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         stdin=subprocess.DEVNULL, start_new_session=True)
        return None
    return subprocess.run(list(cmd), capture_output=True, text=True)


# --------------------------------------------------------------------------- render
def read_jsonc(path):
    text = path.read_text(encoding="utf-8")
    return json.loads(re.sub(r"^\s*//.*$", "", text, flags=re.M))



def battery_css(bar_h, icon=20, weight=600):
    """CSS-drawn battery for waybar: an outline + nub with the percentage number inside.
    Scales with the icon size; pixel-placed, so it depends on the bar height."""
    h = max(12, round(icon * 0.8))              # outline height
    w = round(h * 1.8)                          # wide enough for "100"
    st = 2 if icon >= 16 else 1                 # stroke
    nub_w, nub_h = 2, max(4, h // 2)
    y = (bar_h - h) // 2
    rects = [(0, y, w, st), (0, y + h - st, w, st), (0, y, st, h), (w - st, y, st, h),   # frame
             (w, y + (h - nub_h) // 2, nub_w, nub_h)]                                    # nub
    def block(col):
        return (f"  background-image: {', '.join([f'linear-gradient({col}, {col})'] * len(rects))};\n")
    css = ("#battery {\n"
           f"  min-width: {w}px; padding: 0 {nub_w + 2}px 0 0;\n"
           f"  font-size: {max(9, round(h * 0.68))}px; font-weight: {max(600, weight)};\n"
           "  background-repeat: no-repeat;\n"
           + block("@on_surface") +
           f"  background-position: {', '.join(f'{x}px {yy}px' for x, yy, _, _ in rects)};\n"
           f"  background-size: {', '.join(f'{rw}px {rh}px' for _, _, rw, rh in rects)};\n"
           "}\n")
    # low battery is red; charging comes last so it wins over warning/critical
    for cls, col in (("warning", "@state_low"), ("critical", "@state_low"),
                     ("charging", "@state_charge"), ("plugged", "@state_charge")):
        css += f"#battery.{cls} {{ color: {col};\n" + block(col) + "}\n"
    return css


def text_px(markup, family, px, weight):
    """Pixel width of bar text (Pango, same fonts as waybar), for fixed-width modules."""
    import gi
    gi.require_version("Pango", "1.0")
    gi.require_version("PangoCairo", "1.0")
    from gi.repository import Pango, PangoCairo
    lay = Pango.Layout.new(PangoCairo.FontMap.get_default().create_context())
    fd = Pango.FontDescription.from_string(f"{family}, Symbols Nerd Font")
    fd.set_absolute_size(px * Pango.SCALE)
    fd.set_weight(weight)
    lay.set_font_description(fd)
    lay.set_markup(markup, -1)
    return lay.get_pixel_size()[0]


def fixed_widths_css(font, T, I, W):
    """Network speed and media capsule keep one width whatever they show: no jumping bar."""
    icon = lambda g: f"<span size='{round(100 * I / T)}%' rise='-1pt'>{g}</span>  "   # as the scripts print it
    tnum = lambda s: f"<span font_features='tnum'>{s}</span>"
    net = max(text_px(icon("\U000f0120") + tnum(v), font, T, W) for v in ("888.8 MB/s", "888.8 kB/s", "888 B/s"))
    title = "o" * 13 + "…"                      # media.sh cuts titles to 14 characters (typical letters)
    media = max(text_px(title, font, T, W), text_px("Nothing Playi…", font, T, W))
    w, _h, _d, _g = eq_geometry(I)
    # the equalizer image sits in the left padding (background-image), the title is the label
    return (f'#custom-net {{ min-width: {net + 2}px; font-feature-settings: "tnum"; }}\n'
            f'#custom-media {{ min-width: {media + 2}px; padding-left: {14 + w + 8}px;'
            f' background-image: url("{GEN}/media-paused.png"); background-repeat: no-repeat;'
            f' background-position: 14px center; background-size: {w}px auto; }}\n'
            + "".join(f'#custom-media.playing.f{i} {{ background-image: url("{GEN}/media-playing-{i}.png"); }}\n'
                      for i in range(len(EQ_FRAMES))))


# dots per column, bottom up. scripts/media.sh steps through these frames (class f0, f1, …) a few times a
# second while something plays: every column moves on every frame, never flat. Paused: no frames, no timer.
EQ_FRAMES = [[3, 1, 4, 2, 3], [2, 3, 2, 4, 1], [4, 2, 3, 1, 2], [1, 4, 1, 3, 4]]
EQ_ROWS = 4


def eq_geometry(icon_px):
    """Square-dot equalizer sized to the bar's icon size, in whole pixels (crisp, no scaling)."""
    d = max(2, round(icon_px * 0.16))   # dot side
    g = max(1, d // 2)                  # gap
    n = len(EQ_FRAMES[0])
    return n * d + (n - 1) * g, EQ_ROWS * d + (EQ_ROWS - 1) * g, d, g


def arch_logo(th, px=300):
    """The desktop's Arch logo as a PNG (.generated/arch-logo.png, shown by helpers/panels.c at half size,
    so it stays sharp on scaled screens). Drawn from the Symbols Nerd Font glyph, minus the glyph's small
    trademark sign; one solid colour: the accent blended into the background."""
    import array
    import io
    import cairo
    import gi
    gi.require_version("Pango", "1.0")
    gi.require_version("PangoCairo", "1.0")
    from gi.repository import Pango, PangoCairo
    r = themes.roles(th)
    col = themes.mix(r["primary"], r["wall_bg"], 0.45)
    w = h = px + px // 4
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
    cr = cairo.Context(surf)
    lay = PangoCairo.create_layout(cr)
    fd = Pango.FontDescription.from_string("Symbols Nerd Font")
    fd.set_absolute_size(px * Pango.SCALE)
    lay.set_font_description(fd)
    lay.set_text("\uf303", -1)
    ink, _log = lay.get_pixel_extents()
    cr.move_to(-ink.x + (w - ink.width) // 2, -ink.y + (h - ink.height) // 2)
    cr.set_source_rgb(*(int(col[i:i + 2], 16) / 255 for i in (1, 3, 5)))
    PangoCairo.show_layout(cr, lay)
    surf.flush()
    # keep the logo's big connected shapes, drop small separate bits (the "TM" sign)
    stride, buf = surf.get_stride(), surf.get_data()
    on = bytearray(1 if buf[y * stride + x * 4 + 3] > 40 else 0 for y in range(h) for x in range(w))
    seen, parts = bytearray(w * h), []
    for start in range(w * h):
        if not on[start] or seen[start]:
            continue
        stack, comp = [start], array.array("i")
        seen[start] = 1
        while stack:
            i = stack.pop()
            comp.append(i)
            x, y = i % w, i // w
            for j in (i - 1 if x else -1, i + 1 if x < w - 1 else -1, i - w if y else -1, i + w if y < h - 1 else -1):
                if j >= 0 and on[j] and not seen[j]:
                    seen[j] = 1
                    stack.append(j)
        parts.append(comp)
    biggest = max((len(c) for c in parts), default=0)
    for comp in parts:
        if len(comp) < biggest * 0.05:
            for i in comp:   # also clears the soft edge pixels around it
                x, y = i % w, i // w
                for yy in range(max(0, y - 2), min(h, y + 3)):
                    for xx in range(max(0, x - 2), min(w, x + 3)):
                        o = yy * stride + xx * 4
                        buf[o:o + 4] = b"\0\0\0\0"
    surf.mark_dirty()
    out = io.BytesIO()
    surf.write_to_png(out)
    data, path = out.getvalue(), GEN / "arch-logo.png"
    try:
        if path.read_bytes() == data:
            return
    except OSError:
        pass
    path.write_bytes(data)


def write_png(path, w, h, px):
    """Tiny RGBA PNG writer (no image library): px(x, y) -> (r, g, b, a)."""
    import struct
    import zlib
    raw = b"".join(b"\0" + b"".join(bytes(px(x, y)) for x in range(w)) for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    try:
        if path.read_bytes() == data:
            return
    except OSError:
        pass
    path.write_bytes(data)


def media_icons(th, icon_px=None):
    """The media capsule's icons: equalizer frames of square dots in the accent colour while playing
    (media-playing-N.png), one flat row of dots in the muted text colour while paused (media-paused.png)."""
    if icon_px is None:
        icon_px = int(load_settings()["bar"]["icon_size"])
    r = themes.roles(th)
    w, h, d, g = eq_geometry(icon_px)
    icons = [(f"playing-{i}", f, r["primary"]) for i, f in enumerate(EQ_FRAMES)]
    icons.append(("paused", [1] * len(EQ_FRAMES[0]), r["on_surface_var"]))
    for name, heights, col in icons:
        c = tuple(int(col.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4)) + (255,)

        def px(x, y, heights=heights, c=c):
            col_i, row_i = divmod(x, d + g)[0], divmod(h - 1 - y, d + g)[0]   # rows count from the bottom
            if x % (d + g) >= d or (h - 1 - y) % (d + g) >= d:
                return (0, 0, 0, 0)                                           # gap
            return c if row_i < heights[col_i] else (0, 0, 0, 0)
        write_png(GEN / f"media-{name}.png", w, h, px)


def clock_zone(b, font, T, I, C, W):
    """Where the date + clock sit on the bar, for the calendar's click area (helpers/panels.c)."""
    spacing = 10                                          # waybar config "spacing"
    on = b["widgets"]
    parts = []
    if on.get("date"):
        parts.append(text_px("88.88 Wed |" if on.get("clock") else "88.88 Wed", font, T, W))
    if on.get("clock"):
        parts.append(text_px("88:88" if b["clock_24h"] else "88:88 PM", font, C, W))
    width = sum(parts) + spacing * max(0, len(parts) - 1)
    # bar hidden: no strip at all (width 0 = panels.c makes none), or clicks there would open the calendar
    zone = {"position": "top", "height": BAR_HEIGHT, "margin": 4,
            "width": width + 24 if parts and not b.get("hidden") else 0, "offset": 0}
    (GEN / "clock_zone.json").write_text(json.dumps(zone) + "\n")


BAR_HEIGHT = 45   # fixed: waybar/style.css margins and the battery are placed for this height, at the top


def render(s):
    """Write .generated/* from the repo configs + settings.json."""
    GEN.mkdir(exist_ok=True)
    b = s["bar"]

    # waybar
    wb = read_jsonc(DIR / "waybar/config.jsonc")
    wb["position"] = "top"
    wb["height"] = BAR_HEIGHT
    wb.pop("margin-bottom", None)
    wb["margin-top"] = 4
    wb["start_hidden"] = bool(b.get("hidden"))
    cols = {"left": [], "center": [], "right": []}
    for key, _label, side in WIDGET_LABELS:
        if b["widgets"].get(key):
            cols[side].append(WIDGET_MODULE[key])
    wb["modules-left"], wb["modules-center"], wb["modules-right"] = cols["left"], cols["center"], cols["right"]
    wb["modules-right"].insert(0, "custom/errors")   # always there; empty (hidden) unless something failed
    wb["clock"]["format"] = "{:%H:%M}" if b["clock_24h"] else "{:%I:%M %p}"
    # every inline icon in widget formats uses the same icon size (relative to the text size)
    pct = round(100 * int(b["icon_size"]) / int(b["font_size"]))
    span = f"<span size='{pct}%' rise='-1pt'>"

    def resize(v):
        if isinstance(v, str):
            return re.sub(r"<span size='\d+%'(?: rise='[^']*')?>", span, v)
        if isinstance(v, dict):
            return {k: resize(x) for k, x in v.items()}
        if isinstance(v, list):
            return [resize(x) for x in v]
        return v
    wb = resize(wb)
    # 15.11 Sun | 17:04; the separator only when the clock follows
    wb["clock#date"]["format"] = "{:%d.%m %a} |" if b["widgets"].get("clock") else "{:%d.%m %a}"
    (GEN / "waybar.json").write_text(json.dumps(wb, indent=2, ensure_ascii=False), encoding="utf-8")
    font = s["general"]["font"].replace('"', "")
    T, I, C, W = int(b["font_size"]), int(b["icon_size"]), int(b["clock_size"]), int(b["font_weight"])
    g = s["general"]
    th = themes.get(g.get("theme"))
    (GEN / "waybar.css").write_text(
        themes.palette_css(th) + themed_css(DIR / "waybar/style.css", th) +
        f'* {{ font-family: "{font}", "Symbols Nerd Font"; font-size: {T}px; font-weight: {W}; }}\n'
        'window#waybar { background: @surface; }\n'
        f'#clock {{ font-size: {C}px; font-weight: {W}; }}\n'
        f'#custom-notification {{ font-size: {I}px; }}\n'
        f'#workspaces button {{ font-size: {round(T * 0.8)}px; font-weight: {W}; min-width: {round(I * 1.2)}px; '
        f'min-height: {round(I * 1.2)}px; border-radius: {max(4, round(I * 0.25))}px; }}\n'
        f'#workspaces button.active {{ min-width: {round(I * 2.6)}px; }}\n'
        f'#custom-media {{ border-radius: {round(I * 0.85)}px; }}\n'
        + battery_css(BAR_HEIGHT, I, W)
        + fixed_widths_css(font, T, I, W))
    clock_zone(b, font, T, I, C, W)
    # scripts that print icons inline (media, network) size them relative to the text
    (GEN / "icon_scale").write_text(f"{round(100 * I / T)}\n")

    # swaync
    n = s["notifications"]
    sc = json.loads((DIR / "swaync/config.json").read_text(encoding="utf-8"))
    y, x = n["position"].split("-")
    sc["positionY"], sc["positionX"] = y, x
    sc["timeout"] = int(n["timeout"])
    sc["timeout-low"] = int(n["timeout"])        # same for all, so the countdown bar is always right
    (GEN / "swaync.json").write_text(json.dumps(sc, indent=2, ensure_ascii=False), encoding="utf-8")
    (GEN / "swaync.css").write_text(
        themes.palette_css(th) + themed_css(DIR / "swaync/style.css", th) +
        f'* {{ font-family: "{font}", "Symbols Nerd Font"; }}\n'
        '.notification { background-color: @surface; }\n'
        f'.floating-notifications .notification:not(.critical) {{ animation-duration: {int(n["timeout"])}s; }}\n')

    # hyprlock
    lock = (DIR / "hyprlock/hyprlock.conf").read_text(encoding="utf-8")
    lock = lock.replace("$font     = Noto Sans", f"$font     = {font}")
    lock = themes.recolor(lock, th)
    (GEN / "hyprlock.conf").write_text(lock, encoding="utf-8")

    # also at the default path, so a bare `hyprlock` (typed, or started by another app)
    # finds the same config instead of failing with "No config found". Never replaces a real file.
    for name in ("hyprlock.conf",):
        link = HOME / ".config/hypr" / name
        if link.is_symlink() or not link.exists():
            link.parent.mkdir(parents=True, exist_ok=True)
            link.unlink(missing_ok=True)
            link.symlink_to(GEN / name)

    # startup commands: one enabled shell line per line, run by `hyprminimal start` once per login
    (GEN / "startup").write_text("".join(
        " ".join(c["command"].split("\n")).strip() + "\n" for c in s["startup"]["commands"]
        if c.get("enabled", True) and c.get("command", "").strip()))

    render_theme(th, I)
    # bar popups (helpers/panels.c watches this file and restyles live; the Wi-Fi list reads the template)
    write_if_changed(GEN / "panels.css", themes.recolor((DIR / "settings/panels.css").read_text(encoding="utf-8"), th)
                     + f'window.hyprminimal-panel, window.hyprminimal-background {{ font-family: "{font}", "Symbols Nerd Font"; }}\n')

    # desktop background: helpers/panels.c watches this file and redraws the desktop when it changes
    d = s["desktop"]
    if d.get("logo", True):
        arch_logo(th)
    name = " ".join(str(d.get("name", "")).split())          # one line
    write_if_changed(GEN / "desktop", f"logo={'on' if d.get('logo', True) else 'off'}\nname={name}\n")

    # simple values the shell scripts read
    (GEN / "preload").write_text("on\n" if s["general"].get("preload", True) else "off\n")   # bin/hyprminimal
    (GEN / "nightlight").write_text(
        f"{'on' if s['nightlight']['enabled'] else 'off'} {int(s['nightlight']['temperature'])}\n")


def write_if_changed(path, text):
    """Rewrite only on a real change: programs that watch the file (helpers/panels.c) reload once."""
    try:
        if path.read_text(encoding="utf-8") == text:
            return
    except OSError:
        pass
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(path)


def themed_css(path, theme):
    """A repo stylesheet recoloured for the theme, without its @import of colors/palette.css."""
    text = path.read_text(encoding="utf-8")
    return themes.recolor(re.sub(r'^@import "\.\./colors/palette\.css";\n', "", text, flags=re.M), theme) + "\n"


def render_theme(th, icon_px=None):
    """Everything else that follows the theme: OSD, launcher, terminal, Hyprland borders."""
    (GEN / "theme").write_text(f"{th} {'dark' if themes.roles(th)['dark'] else 'light'}\n")
    (GEN / "palette.css").write_text(themes.palette_css(th))
    (GEN / "swayosd.css").write_text(themed_css(DIR / "swayosd/style.css", th))
    rofi = GEN / "rofi"
    rofi.mkdir(exist_ok=True)
    for f in ("theme.rasi", "launcher.rasi", "menu.rasi"):
        text = themes.recolor((DIR / "rofi" / f).read_text(encoding="utf-8"), th)
        (rofi / f).write_text(text, encoding="utf-8")
    (GEN / "kitty-colors.conf").write_text(themes.kitty_conf(th))
    (GEN / "hypr-theme.lua").write_text(themes.hypr_lua(th))
    media_icons(th, icon_px)


def gtk_theme(dark):
    """Light or dark for other apps' GTK windows. libadwaita/GTK4 follow color-scheme; GTK3 apps (blueman,
    nwg-displays, …) need a theme that exists: GTK3 has no built-in Adwaita-dark (that one is gnome-themes-extra),
    so an unknown name falls back to light Adwaita. adw-gtk3 also looks like the libadwaita apps."""
    have = lambda n: any((d / n / "gtk-3.0").is_dir() for d in
                         (Path("/usr/share/themes"), Path.home() / ".local/share/themes", Path.home() / ".themes"))
    name = "adw-gtk3" if have("adw-gtk3") else "Adwaita"
    if dark and have(name + "-dark"):
        name += "-dark"
    iface = "org.gnome.desktop.interface"
    sh("gsettings", "set", iface, "color-scheme", "prefer-dark" if dark else "prefer-light")
    sh("gsettings", "set", iface, "gtk-theme", name)
    # settings.ini: what GTK reads without gsettings (X11 apps); prefer-dark there would darken a light theme
    want = {"gtk-theme-name": name, "gtk-application-prefer-dark-theme": "1" if dark else "0"}
    for v in ("3.0", "4.0"):
        f = Path.home() / f".config/gtk-{v}/settings.ini"
        try:
            lines = f.read_text().splitlines() if f.exists() else ["[Settings]"]
            keys = dict(want)
            out = []
            for ln in lines:
                k = ln.split("=", 1)[0].strip()
                out.append(f"{k}={keys.pop(k)}" if k in keys else ln)
            out += [f"{k}={val}" for k, val in keys.items()]
            if out != lines:
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text("\n".join(out) + "\n")
        except OSError:
            pass


def apply_theme(s):
    """Live: restart the OSD, reload Hyprland (borders, blur) and kitty, tell GTK apps light or dark."""
    th = themes.get(s["general"].get("theme"))
    gtk_theme(themes.roles(th)["dark"])
    sh("pkill", "-USR1", "-x", "kitty")          # kitty re-reads its config (and our colours)
    sh("hyprctl", "reload")
    if hyprminimal_active():
        sh(str(DIR / "bin" / "hyprminimal"), "respawn", "swayosd-server")


def toggle_bar_hidden(s):
    """Super+M (bin/hyprminimal-msg bar) already showed/hid the running bar: save the new state and
    render it into waybar.json (start_hidden), without restarting waybar."""
    s["bar"]["hidden"] = not s["bar"].get("hidden")
    save_settings(s)
    render(s)
    return s["bar"]["hidden"]


# live reload of running components (only while hyprminimal is active)
def reload_bar():
    """Restart waybar with the new config through `hyprminimal respawn` (locked, so it never runs twice).
    Not SIGUSR2: waybar 0.15 can segfault on in-place reloads, which left the bar gone. A crash or
    a config waybar rejects is caught by hyprminimal's watchdog, which restarts it and notifies."""
    if hyprminimal_active():
        sh(str(DIR / "bin" / "hyprminimal"), "respawn", "waybar")


def reload_notifications():
    if hyprminimal_active() and running("swaync"):
        sh("swaync-client", "-R")
        sh("swaync-client", "-rs")



def apply_nightlight(s):
    sh("pkill", "-x", "hyprsunset")
    if s["nightlight"]["enabled"]:
        time.sleep(0.2)
        sh("hyprsunset", "-t", str(int(s["nightlight"]["temperature"])), detach=True)


# --------------------------------------------------------------------------- keybinds
KEY_NAMES = {
    "SPACE": "Space", "TAB": "Tab", "RETURN": "Enter", "ESCAPE": "Esc", "PRINT": "Print",
    "LEFT": "←", "RIGHT": "→", "UP": "↑", "DOWN": "↓", "MOUSE_DOWN": "Scroll ↓", "MOUSE_UP": "Scroll ↑",
    "MOUSE:272": "Left click", "MOUSE:273": "Right click", "MOUSE:274": "Middle click",
    "XF86AUDIOPLAY": "Play", "XF86AUDIONEXT": "Next", "XF86AUDIOPREV": "Previous",
    "XF86AUDIORAISEVOLUME": "Volume +", "XF86AUDIOLOWERVOLUME": "Volume −", "XF86AUDIOMUTE": "Mute",
    "XF86MONBRIGHTNESSUP": "Brightness +", "XF86MONBRIGHTNESSDOWN": "Brightness −",
}
MOD_ORDER = ["SUPER", "CTRL", "ALT", "SHIFT"]
MOD_ALIAS = {"CONTROL": "CTRL", "MOD4": "SUPER", "WIN": "SUPER", "MOD1": "ALT"}


def split_combo(combo):
    parts = [p.strip() for p in re.split(r"\s*\+\s*", combo.strip()) if p.strip()]
    mods, key = [], ""
    for p in parts:
        up = MOD_ALIAS.get(p.upper(), p.upper())
        if up in MOD_ORDER:
            mods.append(up)
        else:
            key = p
    mods = sorted(set(mods), key=MOD_ORDER.index)
    return mods, key


def norm_combo(combo):
    mods, key = split_combo(combo)
    return " + ".join(mods + [key.upper()])


def canon_combo(combo):
    """Modifiers uppercase in a fixed order, key name kept as typed (xkb names can be case-sensitive)."""
    mods, key = split_combo(combo)
    return " + ".join(mods + [key])


def pretty_parts(combo):
    mods, key = split_combo(combo)
    return [m.title() for m in mods] + ([KEY_NAMES.get(key.upper(), key if len(key) > 1 else key.upper())] if key else [])


def lua_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Keybind:
    __slots__ = ("line", "combo", "key_ref", "literal", "fmt", "description", "action", "section", "readonly")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))


class KeybindFile:
    """Reads and edits ~/.config/hypr/keybind.lua line by line, keeping its style."""

    BIND_RE = re.compile(r'^(\s*)hl\.bind\(\s*(KEY\.(\w+)|"([^"]*)"|\("([^"]*)"\):format\(mainMod\)|(\w+))\s*,\s*(.*)\)\s*$')
    LOCALSTR_RE = re.compile(r'^\s*local\s+(\w+)\s*=\s*(?:(\w+)\s*\.\.\s*)?"([^"]*)"\s*$')
    KEYDEF_RE = re.compile(r'^(\s*)(\w+)\s*=\s*(\("([^"]*)"\):format\(mainMod\)|"([^"]*)")\s*,\s*$')
    DESC_RE = re.compile(r'description\s*=\s*"((?:[^"\\]|\\.)*)"')
    SECTION_RE = re.compile(r"^\s*--\s*\d+\.\s*(.+?)\s*$")
    LOCALFMT_RE = re.compile(r'^\s*local\s+(\w+)\s*=\s*\("([^"]*)"\):format\(mainMod,\s*\w+\)')

    def __init__(self, path=KEYBIND_LUA):
        self.path = path
        self.reload()

    def reload(self):
        self.lines = self.path.read_text(encoding="utf-8").split("\n")
        m = re.search(r'local\s+mainMod\s*=\s*"([^"]+)"', "\n".join(self.lines))
        self.main_mod = m.group(1) if m else "SUPER"
        self._norm_count = {}
        self.keydefs = {}       # NAME -> (line index, combo)
        self.binds = []
        local_fmts = {}
        self.vars = {"scrPath": "~/.config/hypr/Scripts", "SHELLMSG": "hyprminimal-msg"}
        section = "Other"
        in_key_table = False
        for i, raw in enumerate(self.lines):
            if re.match(r"^\s*local\s+KEY\s*=\s*\{", raw):
                in_key_table = True
                continue
            if in_key_table:
                if raw.strip().startswith("}"):
                    in_key_table = False
                    continue
                m = self.KEYDEF_RE.match(raw)
                if m:
                    combo = (m.group(4).replace("%s", self.main_mod) if m.group(4) is not None else m.group(5))
                    self.keydefs[m.group(2)] = (i, combo)
                continue
            m = self.SECTION_RE.match(raw)
            if m:
                section = m.group(1)
                continue
            m = self.LOCALSTR_RE.match(raw)
            if m and m.group(1) not in ("mainMod",):
                prefix = self.vars.get(m.group(2), "") if m.group(2) else ""
                self.vars[m.group(1)] = prefix + m.group(3)
            m = self.LOCALFMT_RE.match(raw)
            if m:
                local_fmts[m.group(1)] = m.group(2).replace("%s", self.main_mod).replace("%d", "1…9")
                continue
            m = self.BIND_RE.match(raw)
            if not m or raw.lstrip().startswith("--"):
                continue
            rest = m.group(7)
            d = self.DESC_RE.search(rest)
            desc = d.group(1).replace('\\"', '"') if d else ""
            action = rest
            opts = list(re.finditer(r",\s*\{", rest))
            if opts:
                action = rest[:opts[-1].start()]
            action = self.summarize(action)
            fmt = None
            if m.group(3):
                kd = self.keydefs.get(m.group(3))
                combo, readonly = (kd[1] if kd else m.group(3)), kd is None
            elif m.group(4) is not None:
                combo, readonly = m.group(4), False
            elif m.group(5) is not None:
                fmt = m.group(5)
                combo, readonly = fmt.replace("%s", self.main_mod), False
            else:
                combo, readonly = local_fmts.get(m.group(6), m.group(6)), True
                if desc.endswith(" ") or ' .. ' in rest.split("description", 1)[-1][:40]:
                    desc = desc.rstrip() + " 1–9"
            self.binds.append(Keybind(line=i, combo=combo, key_ref=m.group(3), literal=m.group(4), fmt=fmt,
                                      description=desc, action=action[:90], section=section,
                                      readonly=readonly))
        self._index()

    def _index(self):
        self._norm_count = {}
        for b in self.binds:
            if not b.readonly:
                n = norm_combo(b.combo)
                self._norm_count[n] = self._norm_count.get(n, 0) + 1

    def summarize(self, action):
        a = action.strip().rstrip(",").strip()
        m = re.match(r"^function\(\)\s*hl\.exec_cmd\((.*)\)\s*end$", a)
        if m:
            a = "hl.dsp.exec_cmd(" + m.group(1) + ")"
        m = re.match(r"^hl\.dsp\.exec_cmd\((.*)\)$", a)
        if m:
            parts = [p.strip() for p in m.group(1).split("..")]
            out = []
            for p in parts:
                if p.startswith('"') and p.endswith('"'):
                    out.append(p[1:-1])
                else:
                    out.append(self.vars.get(p, p))
            cmd = "".join(out)
            cmd = cmd.replace(str(Path.home()), "~")
            return ("Shell: " + cmd[len("hyprminimal-msg "):]) if cmd.startswith("hyprminimal-msg ") else "Run: " + cmd
        a = re.sub(r"^hl\.dsp\.", "", a)
        return a

    def has_conflict(self, b):
        return not b.readonly and self._norm_count.get(norm_combo(b.combo), 0) > 1

    def conflicts(self, combo, exclude=None):
        n = norm_combo(combo)
        return [b for b in self.binds if b is not exclude and not b.readonly and norm_combo(b.combo) == n]

    # -- editing (each returns nothing; call save() afterwards)
    def _combo_lua(self, combo):
        mods, key = split_combo(combo)
        if mods and mods[0] == self.main_mod.upper():
            rest = " + ".join(mods[1:] + [key])
            return f'("%s + {rest}"):format(mainMod)'
        return lua_str(" + ".join(mods + [key]))

    def set_combo(self, b, combo):
        if b.key_ref and b.key_ref in self.keydefs:
            i, _ = self.keydefs[b.key_ref]
            m = self.KEYDEF_RE.match(self.lines[i])
            self.lines[i] = f"{m.group(1)}{b.key_ref} = {self._combo_lua(combo)},"
        elif b.fmt is not None:
            line = self.lines[b.line]
            self.lines[b.line] = re.sub(r'hl\.bind\(\s*\("[^"]*"\):format\(mainMod\)',
                                        lambda _m: "hl.bind(" + self._combo_lua(combo), line, count=1)
        else:
            line = self.lines[b.line]
            self.lines[b.line] = re.sub(r'hl\.bind\(\s*"[^"]*"', lambda _m: "hl.bind(" + lua_str(canon_combo(combo)),
                                        line, count=1)

    def set_description(self, b, desc):
        line = self.lines[b.line]
        if self.DESC_RE.search(line):
            line = self.DESC_RE.sub(lambda _m: "description = " + lua_str(desc), line, count=1)
        else:
            j = line.rstrip().rfind(")")
            line = line[:j] + ", { description = " + lua_str(desc) + " })"
        self.lines[b.line] = line

    def remove(self, b):
        self.lines[b.line] = "-- [removed in hyprminimal settings] " + self.lines[b.line].lstrip()

    def add(self, combo, desc, command):
        marker = "-- Custom shortcuts (added in hyprminimal settings)"
        new = (f"hl.bind({lua_str(canon_combo(combo))}, hl.dsp.exec_cmd({lua_str(command)}), "
               f"{{ description = {lua_str(desc)} }})")
        if marker not in self.lines:
            ret = max((i for i, l in enumerate(self.lines) if l.strip() == "return true"), default=len(self.lines))
            self.lines[ret:ret] = ["", marker]
        mi = self.lines.index(marker)
        j = mi + 1
        while j < len(self.lines) and self.lines[j].startswith("hl.bind("):
            j += 1
        self.lines.insert(j, new)

    def save(self):
        """Write, reload Hyprland, roll back if Hyprland reports config errors. Returns error text or ''."""
        old = self.path.read_text(encoding="utf-8")
        BACKUPS.mkdir(parents=True, exist_ok=True)
        (BACKUPS / f"keybind.lua.{time.strftime('%Y%m%d-%H%M%S')}").write_text(old, encoding="utf-8")
        self.path.write_text("\n".join(self.lines), encoding="utf-8")
        sh("hyprctl", "reload")
        time.sleep(0.4)
        err = (sh("hyprctl", "configerrors").stdout or "").strip()
        if err:
            self.path.write_text(old, encoding="utf-8")
            sh("hyprctl", "reload")
        self.reload()
        return err


# --------------------------------------------------------------------------- GUI
def run_gui(start_page):
    import gi
    gi.require_version("Gtk", "4.0")
    gi.require_version("Adw", "1")
    from gi.repository import Adw, Gdk, Gio, GLib, Gtk, Pango

    CSS = """
    :root {
      --accent-bg-color: #7a88cf; --accent-fg-color: #1f2335; --accent-color: #9aa5e0;
      --window-bg-color: #1f2335; --window-fg-color: #c0caf5;
      --view-bg-color: #1f2335; --view-fg-color: #c0caf5;
      --headerbar-bg-color: #1f2335; --headerbar-fg-color: #c0caf5; --headerbar-shade-color: transparent;
      --sidebar-bg-color: #1a1e2e; --sidebar-fg-color: #a9b1d6; --sidebar-shade-color: transparent;
      --secondary-sidebar-bg-color: #1a1e2e;
      --card-bg-color: #262a40; --card-fg-color: #c0caf5; --card-shade-color: rgba(0,0,0,.25);
      --dialog-bg-color: #24283b; --dialog-fg-color: #c0caf5;
      --popover-bg-color: #24283b; --popover-fg-color: #c0caf5;
      --border-color: #4b517a; --window-radius: 0px;
      --destructive-bg-color: #f7768e; --destructive-fg-color: #1f2335; --destructive-color: #f7768e;
      --success-color: #9cd58a; --warning-color: #e6c384; --error-color: #f7768e;
    }
    window, .background { font-family: "%FONT%"; }
    .navigation-sidebar > row { border-radius: 4px; padding: 9px 12px; margin: 1px 6px; }
    .navigation-sidebar > row:selected { background: #7a88cf; color: #1f2335; }
    .navigation-sidebar > row:selected image { color: #1f2335; }
    .navigation-sidebar > row:selected label { color: #1f2335; }
    .navigation-sidebar image { color: #7a88cf; }
    .boxed-list, .card, row.entry, .preferences row { border-radius: 4px; }
    .boxed-list { border: 1px solid #343a5c; }
    banner.error-banner > revealer > widget { background-color: rgba(247,118,142,.22); color: #f7768e; }
    banner.error-banner button { background: #f7768e; color: #1f2335; }
    .page-error image, .page-error .title { color: #f7768e; }
    .page-title { font-size: 22px; font-weight: 800; color: #9aa5e0; }
    .keycap {
      background: #2c314a; color: #c0caf5; border: 1px solid #4b517a; border-bottom-width: 2px;
      border-radius: 4px; padding: 1px 8px; font-weight: 700; font-size: 0.86em; min-height: 20px;
    }
    .keycap.plus { background: none; border: none; color: #4b517a; padding: 0 1px; }
    .conflict { color: #f7768e; font-weight: 700; }
    .section-label { color: #d7729f; font-weight: 800; font-size: 0.85em; letter-spacing: 1px; }
    .recording { background: #d7729f; color: #1f2335; }
    .banner-off { background: #2c314a; }
    .swatches { border-radius: 6px; border: 1px solid alpha(currentColor, 0.15); }
    """

    class Win(Adw.ApplicationWindow):
        def __init__(self, app):
            super().__init__(application=app, title="hyprminimal settings")
            self.set_default_size(1120, 760)
            # undecorated: GTK adds no 12px resize/shadow border around it. With one, the first frame after
            # a re-show (before Hyprland's tiled-state configure) was drawn 12px off, then snapped into place.
            # Hyprland draws the border and rounding anyway, and there are no title buttons to lose.
            self.set_decorated(False)
            # no close button: Esc (or Super+Q in Hyprland) closes. Capture phase, because a focused
            # SearchEntry eats Esc even when empty (Keybinds focuses its search on open). An open dialog
            # or a search box with text still gets the Esc instead.
            esc = Gtk.EventControllerKey(propagation_phase=Gtk.PropagationPhase.CAPTURE)
            esc.connect("key-pressed", self._on_esc)
            self.add_controller(esc)
            self.s = load_settings()
            self._pending = {}
            self.toasts = Adw.ToastOverlay()
            self.set_content(self.toasts)

            split = Adw.NavigationSplitView(min_sidebar_width=230, max_sidebar_width=260)
            split.set_sidebar_width_fraction(0.22)
            self.toasts.set_child(split)
            self.split = split

            # sidebar
            sb_view = Adw.ToolbarView()
            sb_head = Adw.HeaderBar(show_title=False, show_end_title_buttons=False, show_start_title_buttons=False)
            sb_view.add_top_bar(sb_head)
            sb_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
            title = Gtk.Label(label="Settings", xalign=0, css_classes=["page-title"], margin_start=18)
            sb_box.append(title)
            self.search = Gtk.SearchEntry(placeholder_text="Search settings…", margin_start=10, margin_end=10)
            self.search.connect("search-changed", self._filter_sidebar)
            sb_box.append(self.search)
            self.nav = Gtk.ListBox(css_classes=["navigation-sidebar"], vexpand=True)
            self.nav.connect("row-selected", self._on_nav)
            sw = Gtk.ScrolledWindow(child=self.nav, vexpand=True, hscrollbar_policy=Gtk.PolicyType.NEVER)
            sb_box.append(sw)
            sb_view.set_content(sb_box)
            split.set_sidebar(Adw.NavigationPage(title="Settings", child=sb_view))

            # content
            self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE, transition_duration=120)
            content_view = Adw.ToolbarView()
            self.content_head = Adw.HeaderBar(show_end_title_buttons=False, show_start_title_buttons=False)
            content_view.add_top_bar(self.content_head)
            self.banner = Adw.Banner(title="hyprminimal isn't running. These settings take effect once it starts.",
                                     button_label="Start hyprminimal")
            self.banner.connect("button-clicked", lambda *_: self._switch_shell("start"))
            content_view.add_top_bar(self.banner)
            # red banner: the part of hyprminimal this page configures failed (see bin/hyprminimal watchdog)
            self.err_banner = Adw.Banner(css_classes=["error-banner"], button_label="Retry")
            self.err_banner.connect("button-clicked", lambda *_: self._retry_component())
            content_view.add_top_bar(self.err_banner)
            content_view.set_content(self.stack)
            self.content_page = Adw.NavigationPage(title="General", child=content_view)
            split.set_content(self.content_page)

            self.pages = {}
            self.builders = {}
            for pid, label, icon, builder, words in [
                ("general", "General", "applications-system-symbolic", self.page_general,
                 "font theme performance preload cold start ready memory desktop background logo name arch"),
                ("bar", "Bar", "view-app-grid-symbolic", self.page_bar,
                 "bar widgets clock date wifi bluetooth volume tray battery media"),
                ("notifications", "Notifications", "preferences-system-notifications-symbolic",
                 self.page_notifications, "notifications dnd do not disturb timeout popup"),
                ("startup", "Startup", "system-run-symbolic", self.page_startup,
                 "startup autostart autorun login boot launch apps commands scripts exec"),
                ("network", "Network", "network-workgroup-symbolic", self.page_network,
                 "network wifi wi-fi wireless ethernet wired lan cable hotspot tethering vpn wireguard openvpn "
                 "airplane password internet connect ip dns"),
                ("nightlight", "Night Light", "night-light-symbolic", self.page_nightlight,
                 "night light temperature warm"),
                ("keybinds", "Keybinds", "input-keyboard-symbolic", self.page_keybinds,
                 "keybinds shortcuts keys hotkeys"),
                ("system", "System", "preferences-system-symbolic", self.page_system,
                 "audio display network bluetooth shell restart"),
                ("about", "About", "help-about-symbolic", self.page_about, "about version"),
            ]:
                self.builders[pid] = builder          # built on first visit (lazy)
                row = Gtk.ListBoxRow()
                box = Gtk.Box(spacing=12)
                box.append(Gtk.Image(icon_name=icon))
                box.append(Gtk.Label(label=label, xalign=0))
                row.set_child(box)
                row.pid, row.label, row.words = pid, label, (label + " " + words).lower()
                self.nav.append(row)
                self.pages[pid] = row
            self._update_banner()
            pointerize(self)
            self.show_page(start_page)

        def _on_esc(self, _c, keyval, _code, state):
            if keyval != Gdk.KEY_Escape or state & Gtk.accelerator_get_default_mod_mask():
                return False
            if self.get_visible_dialog() is not None:
                return False                  # the dialog closes itself
            w = self.get_focus()
            while w is not None and not isinstance(w, Gtk.SearchEntry):
                w = w.get_parent()
            if w is not None and w.get_text():
                return False                  # first Esc clears the search
            self.close()
            return True

        # ---- navigation
        def show_page(self, pid):
            if pid in (None, "last"):
                pid = last_page()
            pid = {"wifi": "network", "ethernet": "network", "hotspot": "network", "vpn": "network"}.get(pid, pid)
            row = self.pages.get(pid) or self.pages["general"]
            if self.nav.get_selected_row() is row:
                self._on_nav(self.nav, row)   # already selected: no signal, but its page may have been dropped
            else:
                self.nav.select_row(row)

        def _on_nav(self, _lb, row):
            if row:
                if self.stack.get_child_by_name(row.pid) is None:
                    try:
                        child = self.builders[row.pid]()
                    except Exception as e:   # a broken page shows its error; the rest of Settings keeps working
                        import traceback
                        traceback.print_exc()
                        child = Adw.StatusPage(icon_name="dialog-error-symbolic", css_classes=["page-error"],
                                               title=f"The {row.label} page failed to load",
                                               description=GLib.markup_escape_text(f"{type(e).__name__}: {e}\n"
                                                           "Details: $XDG_RUNTIME_DIR/hyprminimal/settings.log"))
                    self.stack.add_named(child, row.pid)
                    pointerize(self.stack.get_child_by_name(row.pid))
                self.stack.set_visible_child_name(row.pid)
                if row.pid != getattr(self, "_remembered", None) and not getattr(self, "_quiet", False):
                    self._remembered = row.pid
                    remember_page(row.pid)
                self.content_page.set_title(row.label)
                self.update_error_banner()
                self.split.set_show_content(True)
                if row.pid == "keybinds":
                    GLib.idle_add(lambda: self.kb_search.grab_focus() and False)   # must return False: run once

        def focus_search(self, page):
            """On open, type to jump: the sidebar search gets the keyboard (Keybinds keeps its own search).
            Old text is cleared so the sidebar isn't left filtered from last time."""
            row = self.nav.get_selected_row()
            if row is not None and row.pid == "keybinds":   # also when "last" resolved to it
                return
            if self.search.get_text():
                self.search.set_text("")
            GLib.idle_add(lambda: self.search.grab_focus() and False)   # after present(): run once

        def _filter_sidebar(self, entry):
            q = entry.get_text().lower().strip()
            first = None
            for row in self.pages.values():
                ok = not q or all(w in row.words for w in q.split())
                row.set_visible(ok)
                if ok and first is None:
                    first = row
            if q and first:
                self.nav.select_row(first)

        def toast(self, text, timeout=3):
            self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(text), timeout=timeout))

        def update_error_banner(self):
            row = self.nav.get_selected_row()
            comp = PAGE_COMPONENT.get(row.pid) if row else None
            msg = read_error(comp) if comp else ""
            if msg:
                self.err_banner.set_title(GLib.markup_escape_text(f"{COMPONENT_NAME[comp]} failed: {msg}"))
            self.err_banner.set_button_label("Retry" if comp != "settings" else "Apply again")
            self.err_banner.set_revealed(bool(msg))
            self.err_comp = comp

        def _retry_component(self):
            comp = self.err_comp
            self.err_banner.set_revealed(False)
            if comp == "settings":
                self.changed("bar")
                return

            def run():
                sh(str(DIR / "bin" / "hyprminimal"), "respawn", comp)   # clears the error, tries current settings
                time.sleep(6)                                        # watchdog: falls back + re-reports if it fails
                GLib.idle_add(lambda: self.update_error_banner() or False)
            threading.Thread(target=run, daemon=True).start()

        def _update_banner(self):
            self.banner.set_revealed(not hyprminimal_active())

        # ---- settings plumbing
        def changed(self, what):
            """Save + render now, reload the affected component shortly after (debounced)."""
            save_settings(self.s)
            if what == "theme":
                self.get_application().restyle(self.s)
            if what == "preload":
                self.get_application().set_preload(bool(self.s["general"].get("preload", True)))
            try:
                render(self.s)
            except Exception as e:      # keep the running shell on its last rendered files
                write_error("settings", f"{type(e).__name__}: {e}")
                self.toast(f"Not applied: {e}", timeout=6)
                self.update_error_banner()
                return
            write_error("settings", None)
            if what in self._pending:
                GLib.source_remove(self._pending[what])

            def fire():
                self._pending.pop(what, None)
                fn = {"bar": reload_bar, "notifications": reload_notifications, 
                      "theme": lambda: (reload_bar(), reload_notifications(), apply_theme(self.s)),
                      "nightlight": lambda: apply_nightlight(self.s)}.get(what)
                if fn:
                    threading.Thread(target=fn, daemon=True).start()
                return False
            self._pending[what] = GLib.timeout_add(450, fire)

        def page(self, title, desc=None):
            p = Adw.PreferencesPage()
            return p

        def group(self, page, title, desc=None):
            g = Adw.PreferencesGroup(title=title, description=desc or "")
            page.add(g)
            return g

        def switch(self, group, title, sect, key, what, sub=None):
            r = Adw.SwitchRow(title=title, subtitle=sub or "", active=bool(self.s[sect][key]))

            def on(row, _p):
                if getattr(self, "_syncing_bar", False):
                    return
                self.s[sect][key] = row.get_active()
                self.changed(what)
            r.connect("notify::active", on)
            group.add(r)
            return r

        def spin(self, group, title, sect, key, lo, hi, step, what, sub=None, digits=0):
            r = Adw.SpinRow.new_with_range(lo, hi, step)
            r.set_title(title)
            r.set_subtitle(sub or "")
            r.set_digits(digits)
            r.set_value(float(self.s[sect][key]))

            def on(row, _p):
                v = row.get_value()
                self.s[sect][key] = round(v, digits) if digits else int(v)
                self.changed(what)
            r.connect("notify::value", on)
            group.add(r)
            return r

        def combo(self, group, title, sect, key, options, what, sub=None):
            labels = [o[1] for o in options]
            r = Adw.ComboRow(title=title, subtitle=sub or "", model=Gtk.StringList.new(labels))
            values = [o[0] for o in options]
            if self.s[sect][key] in values:
                r.set_selected(values.index(self.s[sect][key]))

            def on(row, _p):
                self.s[sect][key] = values[row.get_selected()]
                self.changed(what)
            r.connect("notify::selected", on)
            group.add(r)
            return r

        def entry(self, group, title, sect, key, what):
            r = Adw.EntryRow(title=title, text=str(self.s[sect][key]), show_apply_button=True)

            def on(row):
                self.s[sect][key] = row.get_text()
                self.changed(what)
                self.toast(f"{title} saved")
            r.connect("apply", on)
            group.add(r)
            return r

        def scale_row(self, group, title, sect, key, lo, hi, step, what, fmt="{:.0%}"):
            row = Adw.ActionRow(title=title)
            sc = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, lo, hi, step)
            sc.set_value(float(self.s[sect][key]))
            sc.set_size_request(260, -1)
            sc.set_valign(Gtk.Align.CENTER)
            sc.set_draw_value(True)
            sc.set_value_pos(Gtk.PositionType.LEFT)
            sc.set_format_value_func(lambda _s, v: fmt.format(v))

            def on(scale):
                v = scale.get_value()
                self.s[sect][key] = round(v, 2) if hi <= 1 else int(v)
                self.changed(what)
            sc.connect("value-changed", on)
            row.add_suffix(sc)
            group.add(row)
            return row

        def button_row(self, group, title, sub, label, cb, icon=None, style="flat"):
            row = Adw.ActionRow(title=title, subtitle=sub or "")
            btn = Gtk.Button(label=label, valign=Gtk.Align.CENTER, css_classes=[style] if style else [])
            if icon:
                btn.set_icon_name(icon)
                btn.set_tooltip_text(label)
            btn.connect("clicked", lambda *_: cb())
            row.add_suffix(btn)
            row.set_activatable_widget(btn)
            group.add(row)
            return row

        # ---- pages
        def page_general(self):
            p = Adw.PreferencesPage()
            g = self.group(p, "Theme", "Bar, panels, notifications, OSD, launcher, terminal, lock screen "
                                       "and window borders all follow it.")
            first = None
            cur = themes.get(self.s["general"].get("theme"))
            for name in themes.ORDER:
                row = Adw.ActionRow(title=themes.TEMPLATE[name]["label"], subtitle=themes.DESCRIPTION[name])
                chk = Gtk.CheckButton(active=name == cur, valign=Gtk.Align.CENTER)
                if first:
                    chk.set_group(first)
                first = first or chk
                row.add_prefix(chk)
                row.add_suffix(self._swatches(name))
                row.set_activatable_widget(chk)
                chk.connect("toggled", lambda c, n=name: c.get_active() and self._set_theme(n))
                g.add(row)
            gd = self.group(p, "Desktop", "The background behind your windows: theme colour, Arch logo and a name. "
                                          "Logo off and an empty name: a plain colour, nothing else.")
            self.entry(gd, "Name under the logo (empty: none)", "desktop", "name", "desktop")
            self.switch(gd, "Logo", "desktop", "logo", "desktop", "Arch Linux logo in the middle of the desktop")
            gp = self.group(p, "Performance")
            self.switch(gp, "Keep Settings ready", "general", "preload", "preload",
                        "Loaded hidden at login: Super+I shows it in ~20 ms (~110 MB RAM). "
                        "Off: cold start (~0.5 s), quits 10 min after closing")
            g2 = self.group(p, "Font")
            fr = Adw.ActionRow(title="Interface font", subtitle="Used by the bar, panels and lock screen")
            fb = Gtk.FontDialogButton(dialog=Gtk.FontDialog(), level=Gtk.FontLevel.FAMILY, valign=Gtk.Align.CENTER)
            fb.set_font_desc(Pango.FontDescription.from_string(self.s["general"]["font"]))

            def on_font(btn, _p):
                fd = btn.get_font_desc()
                if fd and fd.get_family():
                    self.s["general"]["font"] = fd.get_family()
                    self.changed("bar")
                    reload_notifications()
            fb.connect("notify::font-desc", on_font)
            fr.add_suffix(fb)
            g2.add(fr)
            return p

        def _swatches(self, name):
            box = Gtk.Box(spacing=0, valign=Gtk.Align.CENTER, css_classes=["swatches"], overflow=Gtk.Overflow.HIDDEN)
            for c in themes.swatches(name):
                da = Gtk.DrawingArea(content_width=20, content_height=22)
                rgba = Gdk.RGBA()
                rgba.parse(c)
                da.set_draw_func(lambda _a, cr, w, h, col=rgba: (cr.set_source_rgba(col.red, col.green, col.blue, 1),
                                                                 cr.rectangle(0, 0, w, h), cr.fill()))
                box.append(da)
            return box

        def _set_theme(self, name):
            if self.s["general"].get("theme") == name:
                return
            self.s["general"]["theme"] = name
            self.changed("theme")

        def page_bar(self):
            p = Adw.PreferencesPage()
            gv = self.group(p, "Visibility")
            self.hide_bar_row = self.switch(gv, "Hide bar", "bar", "hidden", "bar",
                                            "Windows use the full screen; widgets and their popups are gone "
                                            "until turned back on. Super+M toggles it.")
            gt = self.group(p, "Text and icons", "One scale for the whole bar: every label uses the text size, "
                                                "every icon the icon size, all with the same weight.")
            self.spin(gt, "Text size", "bar", "font_size", 11, 24, 1, "bar", "pixels")
            self.spin(gt, "Icon size", "bar", "icon_size", 12, 32, 1, "bar", "pixels")
            self.spin(gt, "Clock size", "bar", "clock_size", 11, 32, 1, "bar", "pixels")
            self.combo(gt, "Weight", "bar", "font_weight",
                       [(400, "Regular"), (500, "Medium"), (600, "Semibold"), (700, "Bold")], "bar")
            g2 = self.group(p, "Clock")
            self.switch(g2, "24-hour clock", "bar", "clock_24h", "bar")
            for side, title in (("left", "Left"), ("center", "Center"), ("right", "Right")):
                gw = self.group(p, f"Widgets: {title}")
                for key, label, sd in WIDGET_LABELS:
                    if sd != side:
                        continue
                    r = Adw.SwitchRow(title=label, active=bool(self.s["bar"]["widgets"].get(key)))
                    if key == "tray":
                        n = tray_item_count()
                        r.set_subtitle("Icons of apps that offer one (Telegram, Discord, Steam, AnyDesk…). "
                                       + (f"{n} app{'s' if n != 1 else ''} showing an icon now." if n
                                          else "No running app offers one right now, so it stays hidden."))
                    r.connect("notify::active", lambda row, _p, k=key: (
                        self.s["bar"]["widgets"].__setitem__(k, row.get_active()), self.changed("bar")))
                    gw.add(r)
            return p

        def page_notifications(self):
            p = Adw.PreferencesPage()
            g = self.group(p, "Popups")
            self.combo(g, "Position", "notifications", "position",
                       [("top-right", "Top right"), ("top-center", "Top center"), ("top-left", "Top left"),
                        ("bottom-right", "Bottom right"), ("bottom-center", "Bottom center"),
                        ("bottom-left", "Bottom left")], "notifications")
            self.spin(g, "Show for", "notifications", "timeout", 1, 60, 1, "notifications", "seconds")
            g2 = self.group(p, "Do not disturb")
            r = Adw.SwitchRow(title="Do not disturb", subtitle="Hide popups; notifications still go to the control center")
            dnd = (sh("swaync-client", "-D").stdout or "").strip() == "true" if running("swaync") else False
            r.set_active(dnd)
            r.connect("notify::active", lambda row, _p: sh("swaync-client", "-dn" if row.get_active() else "-df"))
            g2.add(r)
            self.button_row(g2, "Send a test notification", None, "Test",
                            lambda: sh("notify-send", "-a", "hyprminimal", "Test notification", "This is how it looks"),
                            style="pill")
            return p

        # ---- startup page
        def page_startup(self):
            p = Adw.PreferencesPage()
            ga = self.group(p, "Add", "Anything you would type in a terminal: an app (telegram-desktop), a script "
                                      "(~/bin/sync.sh) or a shell line (sleep 5 &amp;&amp; discord).")
            self.su_add = Adw.EntryRow(title="Command or script")
            self.su_add.connect("entry-activated", lambda *_: self._su_add_cmd(self.su_add.get_text()))
            pick = Gtk.Button(icon_name="document-open-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                              tooltip_text="Choose a script…")
            pick.connect("clicked", self._su_pick)
            add = Gtk.Button(icon_name="list-add-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                             tooltip_text="Add")
            add.connect("clicked", lambda *_: self._su_add_cmd(self.su_add.get_text()))
            self.su_add.add_suffix(pick)
            self.su_add.add_suffix(add)
            ga.add(self.su_add)
            self.su_group = self.group(p, "Run at login",
                                       "Started once per login, after the bar is up (not again on hyprminimal restart). "
                                       f"Output goes to {RUN / 'startup.log'}.")
            self.su_rows = []
            self._su_fill()
            return p

        def _su_fill(self):
            for r in self.su_rows:
                self.su_group.remove(r)
            self.su_rows = []
            cmds = self.s["startup"]["commands"]
            if not cmds:
                r = Adw.ActionRow(title="Nothing yet", subtitle="Add a command above", css_classes=["dim-label"])
                self.su_group.add(r)
                self.su_rows.append(r)
            for item in cmds:
                r = Adw.EntryRow(title="Runs at login" if item.get("enabled", True) else "Off",
                                 text=item["command"], show_apply_button=True)

                def on_edit(row, it=item):
                    cmd = row.get_text().strip()
                    if cmd:
                        it["command"] = cmd
                        self.changed("startup")
                        self.toast("Startup command saved")
                r.connect("apply", on_edit)
                sw = Gtk.Switch(active=item.get("enabled", True), valign=Gtk.Align.CENTER,
                                tooltip_text="Run at login")

                def on_switch(s, _p, it=item, row=r):
                    it["enabled"] = s.get_active()
                    row.set_title("Runs at login" if it["enabled"] else "Off")
                    self.changed("startup")
                sw.connect("notify::active", on_switch)
                r.add_prefix(sw)
                run = Gtk.Button(icon_name="media-playback-start-symbolic", valign=Gtk.Align.CENTER,
                                 css_classes=["flat"], tooltip_text="Run now")
                run.connect("clicked", lambda _b, row=r: self._su_run(row.get_text()))
                rm = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER,
                                css_classes=["flat"], tooltip_text="Remove")
                rm.connect("clicked", lambda _b, it=item: self._su_remove(it))
                r.add_suffix(run)
                r.add_suffix(rm)
                self.su_group.add(r)
                self.su_rows.append(r)
            for r in self.su_rows:
                pointerize(r)

        def _su_add_cmd(self, cmd):
            cmd = cmd.strip()
            if not cmd:
                return
            self.s["startup"]["commands"].append({"command": cmd, "enabled": True})
            self.changed("startup")
            self.su_add.set_text("")
            self._su_fill()
            self.toast("Added: runs at your next login")

        def _su_remove(self, item):
            self.s["startup"]["commands"] = [c for c in self.s["startup"]["commands"] if c is not item]
            self.changed("startup")
            self._su_fill()

        def _su_run(self, cmd):
            cmd = cmd.strip()
            if not cmd:
                return
            RUN.mkdir(parents=True, exist_ok=True)
            with open(RUN / "startup.log", "a") as log:
                subprocess.Popen(["sh", "-c", cmd], stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                                 cwd=HOME, start_new_session=True)
            self.toast(f"Started: {cmd}")

        def _su_pick(self, *_):
            d = Gtk.FileDialog(title="Script to run at login")
            d.set_initial_folder(Gio.File.new_for_path(str(HOME)))

            def done(dlg, res):
                try:
                    path = dlg.open_finish(res).get_path()
                except GLib.Error:
                    return
                # not executable: run it through sh rather than fail silently at login
                self._su_add_cmd(shlex.quote(path) if os.access(path, os.X_OK) else f"sh {shlex.quote(path)}")
            d.open(self, None, done)

        def page_nightlight(self):
            p = Adw.PreferencesPage()
            g = self.group(p, "Night light", "Warmer colours to reduce eye strain (hyprsunset).")
            self.switch(g, "Enabled", "nightlight", "enabled", "nightlight")
            self.scale_row(g, "Temperature", "nightlight", "temperature", 2500, 6500, 100, "nightlight", "{:.0f} K")
            return p

        def page_network(self):
            sys.path.insert(0, str(Path(__file__).resolve().parent))   # -I drops the script dir
            import network_page
            return network_page.build(self)

        def page_system(self):
            p = Adw.PreferencesPage()
            g = self.group(p, "Shell")
            self.button_row(g, "Restart hyprminimal", "Reload bar, notifications and OSD", "Restart",
                            lambda: self._switch_shell("restart" if hyprminimal_active() else "start"), style="pill")
            g2 = self.group(p, "System settings")
            for title, sub, cmd in [
                ("Audio", "Output, input and app volumes", "pavucontrol"),
                ("Displays", "Resolution, scale, arrangement", "nwg-displays"),
                ("Bluetooth", "Devices", "blueman-manager"),
            ]:
                self.button_row(g2, title, sub, "Open",
                                lambda c=cmd: sh(c, detach=True) if shutil.which(c) else self.toast(f"{c} is not installed"))
            return p

        def _switch_shell(self, action):
            def run():
                sh(str(DIR / "bin/hyprminimal"), action)
                GLib.idle_add(self._after_switch)
            threading.Thread(target=run, daemon=True).start()
            self.toast({"start": "Starting hyprminimal…", "restart": "Restarting hyprminimal…"}[action])

        def _after_switch(self):
            self._update_banner()
            return False

        def page_about(self):
            p = Adw.PreferencesPage()
            g = self.group(p, "hyprminimal", "A minimal shell for Hyprland, official Arch packages only.")
            pkgs = ("hyprland", "waybar", "swaync", "swayosd", "rofi", "hyprlock")
            vers = dict(l.split()[:2] for l in sh("pacman", "-Q", *pkgs).stdout.splitlines() if " " in l)
            for pkg in pkgs:
                g.add(Adw.ActionRow(title=pkg, subtitle=vers.get(pkg, "not installed")))
            g2 = self.group(p, "Files")
            g2.add(Adw.ActionRow(title="Settings", subtitle=str(SETTINGS), subtitle_selectable=True))
            g2.add(Adw.ActionRow(title="Keybinds", subtitle=str(KEYBIND_LUA), subtitle_selectable=True))
            return p

        # ---- keybinds page
        def page_keybinds(self):
            outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            top = Gtk.Box(spacing=8, margin_top=12, margin_bottom=6, margin_start=24, margin_end=24)
            self.kb_search = Gtk.SearchEntry(placeholder_text="Search shortcuts (name, key or action)…", hexpand=True)
            self.kb_search.connect("search-changed", lambda *_: self._kb_fill())
            top.append(self.kb_search)
            add = Gtk.Button(label="Add shortcut", icon_name="list-add-symbolic", css_classes=["suggested-action"])
            add.set_child(Adw.ButtonContent(icon_name="list-add-symbolic", label="Add shortcut"))
            add.connect("clicked", lambda *_: self._kb_dialog(None))
            top.append(add)
            outer.append(top)
            hint = Gtk.Label(label="Edits go straight into ~/.config/hypr/keybind.lua and are live right away. "
                                   "A backup is kept, and a change Hyprland rejects is undone automatically.",
                             wrap=True, xalign=0, css_classes=["dim-label", "caption"], margin_start=26, margin_end=26)
            outer.append(hint)
            self.kb_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18,
                                  margin_start=24, margin_end=24, margin_top=12, margin_bottom=24)
            clamp = Adw.Clamp(maximum_size=980, child=self.kb_box)
            outer.append(Gtk.ScrolledWindow(child=clamp, vexpand=True, hscrollbar_policy=Gtk.PolicyType.NEVER))
            try:
                self.kbf = KeybindFile()
            except OSError as e:
                self.kbf = None
                self.kb_box.append(Gtk.Label(label=f"Cannot read {KEYBIND_LUA}: {e}"))
                return outer
            GLib.idle_add(lambda: self._kb_fill() or False)   # window appears first, list fills next frame
            return outer

        def _keycaps(self, combo):
            box = Gtk.Box(spacing=3, valign=Gtk.Align.CENTER)
            parts = pretty_parts(combo)
            for i, part in enumerate(parts):
                if i:
                    box.append(Gtk.Label(label="+", css_classes=["keycap", "plus"]))
                box.append(Gtk.Label(label=part, css_classes=["keycap"]))
            return box

        def _kb_fill(self):
            if not self.kbf:
                return
            while (c := self.kb_box.get_first_child()):
                self.kb_box.remove(c)
            q = self.kb_search.get_text().lower().strip()
            sections = {}
            for b in self.kbf.binds:
                hay = f"{b.description} {b.combo} {' '.join(pretty_parts(b.combo))} {b.action} {b.section}".lower()
                if q and not all(w in hay for w in q.split()):
                    continue
                sections.setdefault(b.section, []).append(b)
            if not sections:
                self.kb_box.append(Adw.StatusPage(icon_name="system-search-symbolic", title="No shortcuts found"))
                return
            for sec, binds in sections.items():
                self.kb_box.append(Gtk.Label(label=sec.upper(), xalign=0, css_classes=["section-label"]))
                lb = Gtk.ListBox(css_classes=["boxed-list"], selection_mode=Gtk.SelectionMode.NONE)
                for b in binds:
                    row = Adw.ActionRow(title=GLib.markup_escape_text(b.description or "(no description)"),
                                        subtitle=GLib.markup_escape_text(b.action))
                    row.set_subtitle_lines(1)
                    if self.kbf.has_conflict(b):
                        row.add_prefix(Gtk.Image(icon_name="dialog-warning-symbolic", css_classes=["conflict"],
                                                 tooltip_text="Another shortcut uses the same keys"))
                    row.add_suffix(self._keycaps(b.combo))
                    if b.readonly:
                        row.add_suffix(Gtk.Label(label="loop", css_classes=["dim-label", "caption"],
                                                 tooltip_text="Defined in a loop in keybind.lua; edit it there"))
                    else:
                        e = Gtk.Button(icon_name="document-edit-symbolic", valign=Gtk.Align.CENTER,
                                       css_classes=["flat"], tooltip_text="Edit")
                        e.connect("clicked", lambda _b, bb=b: self._kb_dialog(bb))
                        row.add_suffix(e)
                        row.set_activatable_widget(e)
                    lb.append(row)
                self.kb_box.append(lb)
            pointerize(self.kb_box)

        def _kb_dialog(self, b):
            """Edit (b) or add (b is None) a shortcut."""
            new = b is None
            dlg = Adw.Dialog(title="Add shortcut" if new else "Edit shortcut", content_width=520)
            tv = Adw.ToolbarView()
            hb = Adw.HeaderBar(show_end_title_buttons=False, show_start_title_buttons=False)
            cancel = Gtk.Button(label="Cancel")
            cancel.connect("clicked", lambda *_: dlg.close())
            save = Gtk.Button(label="Save", css_classes=["suggested-action"])
            hb.pack_start(cancel)
            hb.pack_end(save)
            tv.add_top_bar(hb)
            page = Adw.PreferencesPage()
            g = Adw.PreferencesGroup()
            page.add(g)
            desc = Adw.EntryRow(title="Description", text=(b.description if b else ""))
            g.add(desc)
            combo = Adw.EntryRow(title="Keys (e.g. SUPER + SHIFT + R)", text=(canon_combo(b.combo) if b else ""))
            rec = Gtk.ToggleButton(icon_name="media-record-symbolic", valign=Gtk.Align.CENTER,
                                   css_classes=["flat"], tooltip_text="Record: press the new keys")
            combo.add_suffix(rec)
            g.add(combo)
            preview = Adw.ActionRow(title="Preview")
            g.add(preview)
            warn = Gtk.Label(xalign=0, wrap=True, css_classes=["conflict"], margin_top=6)
            cmd = None
            if new:
                cmd = Adw.EntryRow(title="Command to run (e.g. firefox)")
                g.add(cmd)
            else:
                g.add(Adw.ActionRow(title="Action", subtitle=GLib.markup_escape_text(b.action)))
                if b.key_ref:
                    others = [x for x in self.kbf.binds if x.key_ref == b.key_ref and x is not b]
                    if others:
                        g.add(Adw.ActionRow(title="Note", subtitle=f"KEY.{b.key_ref} is also used by "
                                            + ", ".join(o.description or o.action for o in others)))
            g.add(warn)
            tv.set_content(page)
            dlg.set_child(tv)
            state = {"pv": None}

            def refresh(*_):
                if state["pv"]:
                    preview.remove(state["pv"])
                txt = combo.get_text().strip()
                state["pv"] = self._keycaps(txt) if txt else Gtk.Label(label="—")
                preview.add_suffix(state["pv"])
                clash = self.kbf.conflicts(txt, exclude=b) if txt else []
                warn.set_label(("Already used by: " + ", ".join(c.description or c.action for c in clash))
                               if clash else "")
                _m, key = split_combo(txt)
                save.set_sensitive(bool(key) and (not new or bool(cmd.get_text().strip())))
            combo.connect("changed", refresh)
            if cmd:
                cmd.connect("changed", refresh)
            refresh()

            keyc = Gtk.EventControllerKey(propagation_phase=Gtk.PropagationPhase.CAPTURE)

            def on_key(_c, keyval, keycode, state_mask):
                if not rec.get_active():
                    return False
                name = Gdk.keyval_name(keyval) or ""
                if name.startswith(("Super", "Control", "Alt", "Shift", "Meta", "Hyper", "ISO_Level")):
                    return True
                disp = self.get_display()
                ok, base, *_ = disp.translate_key(keycode, Gdk.ModifierType(0), 0)
                base_name = (Gdk.keyval_name(base) if ok else name) or name
                mods = []
                if state_mask & Gdk.ModifierType.SUPER_MASK:
                    mods.append("SUPER")
                if state_mask & Gdk.ModifierType.CONTROL_MASK:
                    mods.append("CTRL")
                if state_mask & Gdk.ModifierType.ALT_MASK:
                    mods.append("ALT")
                if state_mask & Gdk.ModifierType.SHIFT_MASK:
                    mods.append("SHIFT")
                key = base_name.upper() if len(base_name) == 1 else base_name
                combo.set_text(" + ".join(mods + [key]))
                rec.set_active(False)
                return True
            keyc.connect("key-pressed", on_key)
            dlg.add_controller(keyc)
            rec.connect("toggled", lambda r: (r.add_css_class("recording") if r.get_active()
                                              else r.remove_css_class("recording")))

            def do_save(*_):
                c = combo.get_text().strip()
                d = desc.get_text().strip()
                try:
                    if new:
                        self.kbf.add(c, d or cmd.get_text().strip(), cmd.get_text().strip())
                    else:
                        if norm_combo(c) != norm_combo(b.combo):
                            self.kbf.set_combo(b, c)
                        if d != (b.description or ""):
                            self.kbf.set_description(b, d)
                    err = self.kbf.save()
                except Exception as e:  # noqa: BLE001 - surface any edit failure to the user
                    self.kbf.reload()
                    err = str(e)
                dlg.close()
                self._kb_fill()
                self.toast(f"Not saved, Hyprland reported: {err}" if err else "Shortcut saved and live", 5)
            save.connect("clicked", do_save)

            if not new:
                rm = Gtk.Button(label="Remove shortcut", css_classes=["destructive-action"],
                                margin_top=12, halign=Gtk.Align.START)

                def do_rm(*_):
                    self.kbf.remove(b)
                    err = self.kbf.save()
                    dlg.close()
                    self._kb_fill()
                    self.toast(f"Not removed: {err}" if err else "Shortcut removed (commented out in keybind.lua)")
                rm.connect("clicked", do_rm)
                rg = Adw.PreferencesGroup()
                rg.add(rm)
                page.add(rg)
            pointerize(dlg)
            dlg.present(self)

    class App(Adw.Application):
        def __init__(self):
            super().__init__(application_id="dev.hyprminimal.Settings",
                             flags=Gio.ApplicationFlags.HANDLES_COMMAND_LINE)
            self.win = None

        def do_startup(self):
            Adw.Application.do_startup(self)
            self.css = Gtk.CssProvider()
            Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), self.css,
                                                      Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
            esc = Gio.SimpleAction.new("close", None)
            esc.connect("activate", lambda *_: self.win and self.win.close())
            self.add_action(esc)
            # `hyprminimal-settings --page X` calls this over D-Bus when we're already running (~10 ms)
            # Super+I: hide the window when it's showing on the workspace you look at (whatever has the
            # keyboard: with follow_mouse focus wanders off it), else show it here, in front
            tog = Gio.SimpleAction.new("toggle", None)
            tog.connect("activate", lambda *_: self.toggle())
            self.add_action(tog)
            show = Gio.SimpleAction.new("show-page", GLib.VariantType.new("s"))
            show.connect("activate", lambda _a, v: self.present_page(v.get_string()))
            self.add_action(show)
            self._idle_quit = None
            self.shown = False                # on screen (not parked, not unmapped)
            self.preloaded = False
            self._preload_held = False
            self._held = False
            import panels
            self.panels = panels.PanelManager(self)
            self.restyle()
            pa = Gio.SimpleAction.new("panel", GLib.VariantType.new("s"))
            pa.connect("activate", lambda _a, v: self._safe_panel(v.get_string()))
            self.add_action(pa)
            # Hyprland (hypr/layerrules.lua): a click outside the popup or focus moving to a window
            pc = Gio.SimpleAction.new("panel-close", None)
            pc.connect("activate", lambda *_: self.panels.current and self.panels.close())
            self.add_action(pc)
            # Super+M (bin/hyprminimal-msg bar): the bar was just shown/hidden, save that
            bt = Gio.SimpleAction.new("bar-toggled", None)
            bt.connect("activate", lambda *_: self._bar_toggled())
            self.add_action(bt)
            quit_a = Gio.SimpleAction.new("quit", None)
            quit_a.connect("activate", lambda *_: self.quit())
            self.add_action(quit_a)

        def _bar_toggled(self):
            w = self.win
            s = w.s if w else load_settings()
            try:
                hidden = toggle_bar_hidden(s)
            except Exception as e:
                write_error("settings", f"{type(e).__name__}: {e}")
                return
            row = getattr(w, "hide_bar_row", None) if w else None
            if row and row.get_active() != hidden:   # sync the switch without its handler (= no bar restart)
                w._syncing_bar = True
                row.set_active(hidden)
                w._syncing_bar = False

        def restyle(self, s=None):
            """Theme the Settings window and the bar popups (live, no restart)."""
            g = (s or load_settings())["general"]
            th = themes.get(g.get("theme"))
            dark = themes.roles(th)["dark"]
            Adw.StyleManager.get_default().set_color_scheme(
                Adw.ColorScheme.FORCE_DARK if dark else Adw.ColorScheme.FORCE_LIGHT)
            self.css.load_from_string(themes.recolor(CSS, th).replace("%FONT%", g["font"]))
            self.panels.restyle(th)

        def preload(self):
            """Started hidden by `hyprminimal start`: build the window and the keybinds page now,
            stay resident, so Super+I / Super+H only have to show it."""
            self.preloaded = True
            self.hold()
            self._preload_held = True
            self.win = Win(self)
            self.win.connect("close-request", self._on_close)
            self.win._quiet = True          # prebuilding pages isn't the user visiting them
            self.win.show_page("keybinds")
            self.win._quiet = False
            self.win.show_page("last")
            self.panels.prewarm()           # bar popups too
            self._map_parked()

        def _map_parked(self):
            """Map the window straight onto the park workspace (a window rule matches PARK_TITLE), so even
            the first Super+I is a move. The real title comes back once Hyprland has placed it."""
            self.win.set_title(PARK_TITLE)
            self.win.present()
            tries = [40]                      # up to 2 s

            def placed():
                c = self._me()
                if c or tries[0] <= 0:
                    self.win.set_title("hyprminimal settings")
                    if c and not (c.get("workspace") or {}).get("name", "").startswith("special:"):   # rule missing
                        self.win.set_visible(False)
                    return False
                tries[0] -= 1
                return True
            GLib.timeout_add(50, placed)

        def _me(self):
            """Our window as Hyprland sees it (None while unmapped)."""
            pid = os.getpid()
            return next((c for c in hypr_json("clients") or []
                         if c.get("pid") == pid and c.get("class") == "dev.hyprminimal.Settings"), None)

        def toggle(self):
            c = self._me() if self.win and self.shown else None
            ws = (hypr_json("activeworkspace") or {}).get("name")   # special workspaces have no id: names
            if c and (c.get("workspace") or {}).get("name") == ws:
                self.hide()
            else:
                self.present_page("last")

        def hide(self):
            """Close the window: park it (Keep Settings ready on) or unmap it (off: cold start next time)."""
            if not self.win:
                return
            self.shown = False
            c = self._me() if self.preloaded else None
            if c:
                hypr(f'dispatch hl.dsp.window.move({{ workspace = "{PARK}", follow = false, window = "address:{c["address"]}" }})')
            else:
                self.win.set_visible(False)
                if not self.preloaded:
                    self._idle_quit = GLib.timeout_add_seconds(IDLE_QUIT_SECONDS, self._quit_idle)

        def panel_state(self, is_open):
            """Started for the Wi-Fi popup (not preloaded): quit a while after it and the window are closed."""
            if self._idle_quit:
                GLib.source_remove(self._idle_quit)
                self._idle_quit = None
            if not is_open and not self.preloaded and not self.shown:
                self._idle_quit = GLib.timeout_add_seconds(IDLE_QUIT_SECONDS, self._quit_idle)

        def _safe_panel(self, kind):
            """A bar popup that throws is reported and closed; Settings and the other popups keep working."""
            try:
                self.panels.toggle(kind)
            except Exception as e:
                import traceback
                traceback.print_exc()
                try:
                    self.panels.close()
                except Exception:
                    pass
                sh("notify-send", "-a", "hyprminimal", "-u", "critical", f"The {kind} popup failed",
                   f"{type(e).__name__}: {e}\nDetails: $XDG_RUNTIME_DIR/hyprminimal/settings.log")

        def present_page(self, page):
            if self.win is None:
                self.win = Win(self)
                self.win.connect("close-request", self._on_close)
            if self._idle_quit:
                GLib.source_remove(self._idle_quit)
                self._idle_quit = None
            self.win.show_page(page)
            self.win.update_error_banner()
            c = self._me() if self.win.get_visible() else None
            if c:   # mapped (parked, or open on another workspace): move it here and focus it, no remap
                ws = (hypr_json("activeworkspace") or {}).get("id")
                if ws is not None and (c.get("workspace") or {}).get("id") != ws:
                    hypr(f'dispatch hl.dsp.window.move({{ workspace = {ws}, follow = false, window = "address:{c["address"]}" }})')
                hypr(f'dispatch hl.dsp.focus({{ window = "address:{c["address"]}" }})')
                self.win.present()            # GTK side: keyboard focus to the window
            else:
                self.win.present()
            self.shown = True
            self.win.focus_search(page)
            GLib.timeout_add(100, self._wake_pointer)

        @staticmethod
        def _wake_pointer():
            """Hyprland gives a window that appears under a still cursor no pointer-enter, so GTK ignores the
            scroll wheel until the mouse moves. Nudge the cursor 1px and back (invisible) to send it."""
            try:
                x, y = (int(float(v)) for v in hypr("cursorpos").split(","))
            except ValueError:
                return False
            for px in (x + 1, x):
                hypr(f"dispatch hl.dsp.cursor.move({{ x = {px}, y = {y} }})")
            return False

        def _on_close(self, win):
            self.hide()                       # Esc, Super+Q, click outside: keep it warm, never destroy
            return True

        def set_preload(self, on):
            """Settings -> General -> Keep Settings ready, live: stay resident, or quit like a cold start
            (10 min after the window closes). `hyprminimal start` reads .generated/preload at the next login."""
            self.preloaded = on
            if on:
                if self._idle_quit:
                    GLib.source_remove(self._idle_quit)
                    self._idle_quit = None
                if not self._preload_held:
                    self.hold()
                    self._preload_held = True
            else:
                if self._preload_held:
                    self.release()
                    self._preload_held = False
                if self.win and not self.shown and self.win.get_visible():   # parked: let it go
                    self.win.set_visible(False)
                    self._idle_quit = GLib.timeout_add_seconds(IDLE_QUIT_SECONDS, self._quit_idle)

        def _quit_idle(self):
            self._idle_quit = None
            if self.panels.current or self.shown:
                return False                  # in use again: the next close re-arms the timer
            self.quit()
            return False
            self.set_accels_for_action("app.close", ["<Ctrl>w", "<Ctrl>q"])

        def do_command_line(self, cl):
            args = cl.get_arguments()[1:]
            if "--panel" in args and args.index("--panel") + 1 < len(args):
                if not self._held:
                    self.hold()                 # alive while the popup is open; panel_state() quits later
                    self._held = True
                self._safe_panel(args[args.index("--panel") + 1])
                return 0
            if "--preload" in args:
                if self.win is None:
                    self.preload()
                return 0
            page = args[args.index("--page") + 1] if "--page" in args and args.index("--page") + 1 < len(args) else "last"
            self.present_page(page)
            return 0

    return App().run(sys.argv)


def main():
    if "--toggle-bar" in sys.argv:
        toggle_bar_hidden(load_settings())
        return 0
    if "--apply" in sys.argv:
        s = load_settings()
        if not SETTINGS.exists():
            save_settings(s)
        render(s)
        gtk_theme(themes.roles(themes.get(s["general"].get("theme")))["dark"])
        return 0
    if "-h" in sys.argv or "--help" in sys.argv:
        print(__doc__.strip())
        return 0
    page = sys.argv[sys.argv.index("--page") + 1] if "--page" in sys.argv[:-1] else "last"
    return run_gui(page)


if __name__ == "__main__":
    sys.exit(main())
