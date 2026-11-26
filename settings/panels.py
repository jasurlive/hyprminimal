"""hyprminimal bar popups that still live in Python: the Wi-Fi network list (control center ›).

The everyday popups (calendar, battery, network, system), the clock click strip and the click-outside
catcher are a small C program now: helpers/panels.c. This process starts on demand for the Wi-Fi list
(or the Settings window) and quits once both have been closed for a while.
Close with Esc or a click outside.
"""
import subprocess
import threading
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, GLib, Gtk, Pango  # noqa: E402

try:
    gi.require_version("Gtk4LayerShell", "1.0")
    from gi.repository import Gtk4LayerShell as LS  # needs LD_PRELOAD=libgtk4-layer-shell.so
    if not LS.is_supported():
        LS = None
except (ValueError, ImportError):
    LS = None

POPUP_TOP = 8         # gap under the bar (its exclusive zone already reserves bar + margin)
POPUP_RIGHT = 4       # right edge in line with the bar (bar margin is 4)
CSS = (Path(__file__).resolve().parent / "panels.css").read_text(encoding="utf-8")


def run(*cmd, timeout=2):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def bg(work, done, owner=None, key=None):
    """Run work() off the UI thread, then done(result) on it, so panels never stutter.
    With owner+key it is single-flight: if the same job is still running, this call is skipped,
    so a slow D-Bus service can never pile up processes."""
    if owner is not None:
        busy = owner.__dict__.setdefault("_inflight", set())
        if key in busy:
            return
        busy.add(key)

    def t():
        try:
            r = work()
        except Exception:                   # noqa: BLE001 - a failed probe must not wedge the job
            r = None
        if owner is not None:
            owner._inflight.discard(key)
        GLib.idle_add(lambda: done(r) or False)
    threading.Thread(target=t, daemon=True).start()


def hide_control_center():
    """Hide the swaync control center (Super+S) without waiting. --auto-start=no: never start stock swaync."""
    try:
        subprocess.Popen(["busctl", "--user", "--auto-start=no", "call", "org.erikreider.swaync.cc",
                          "/org/erikreider/swaync/cc", "org.erikreider.swaync.cc", "SetVisibility", "b", "false"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    except OSError:
        pass


def close_c_panels():
    """Close a popup of helpers/panels.c (calendar, battery, network, system) without waiting."""
    try:
        subprocess.Popen(["gdbus", "call", "--session", "--dest", "dev.hyprminimal.Panels", "--object-path",
                          "/dev/hyprminimal/Panels", "--method", "org.gtk.Actions.Activate", "panel-close", "[]", "{}"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    except OSError:
        pass


def label(text="", cls=None, xalign=0, **kw):
    lb = Gtk.Label(label=text, xalign=xalign, **kw)
    for c in (cls or "").split():
        lb.add_css_class(c)
    return lb


# --------------------------------------------------------------------------- base
class Panel(Gtk.Window):
    width = 360
    bottom = False        # True: anchor bottom right (opened from the control center's bottom row)
    center = False        # True: centred under the bar (the calendar, under the clock)

    def __init__(self, app, manager):
        super().__init__(application=app, decorated=False, resizable=False)
        self.manager = manager
        self.add_css_class("hyprminimal-panel")
        self.set_default_size(self.width, -1)
        self._tick = None
        if LS:
            LS.init_for_window(self)
            LS.set_namespace(self, "hyprminimal-panel")
            LS.set_layer(self, LS.Layer.OVERLAY)
            LS.set_keyboard_mode(self, LS.KeyboardMode.EXCLUSIVE)
            edge = LS.Edge.BOTTOM if self.bottom else LS.Edge.TOP
            LS.set_anchor(self, edge, True)
            LS.set_margin(self, edge, POPUP_TOP)
            if not self.center:
                LS.set_anchor(self, LS.Edge.RIGHT, True)    # popups sit on the right, the calendar centred
                LS.set_margin(self, LS.Edge.RIGHT, POPUP_RIGHT)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14, css_classes=["panel"])
        box.set_size_request(self.width, -1)
        self.body = box
        # fixed width: the scroller caps the natural width at self.width (never scrolls), and every
        # one-line label may ellipsize, so changing text can neither widen nor shrink the popup
        clamp = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, vscrollbar_policy=Gtk.PolicyType.NEVER,
                                   propagate_natural_width=True, propagate_natural_height=True,
                                   min_content_width=self.width, max_content_width=self.width)
        clamp.set_child(box)
        self.set_child(clamp)
        self.build(box)
        self._fit_labels(box)
        import hyprminimal_settings
        hyprminimal_settings.pointerize(box)          # hand pointer on every button/slider
        # capture phase: Esc closes even when a focused child (entry, switch, list) would take it
        keys = Gtk.EventControllerKey(propagation_phase=Gtk.PropagationPhase.CAPTURE)
        keys.connect("key-pressed", self._on_key)
        self.add_controller(keys)

    def _on_key(self, _c, kv, _code, _mods):
        if kv == Gdk.KEY_Escape:
            self.manager.close()
            return True
        if isinstance(self.get_focus(), Gtk.Editable):
            return False      # typing a password: letters are text, not panel shortcuts
        return self.key(Gdk.keyval_to_lower(kv))

    @staticmethod
    def _fit_labels(root):
        w = root.get_first_child()
        while w:
            if isinstance(w, Gtk.Label) and not w.get_wrap() and w.get_ellipsize() == Pango.EllipsizeMode.NONE:
                w.set_ellipsize(Pango.EllipsizeMode.END)
            Panel._fit_labels(w)
            w = w.get_next_sibling()

    def build(self, box):
        raise NotImplementedError

    def refresh(self):
        pass

    def key(self, keyval):
        return False  # panel shortcuts; True = handled

    def interval(self):
        return 0      # seconds between refreshes while open (0 = never)

    def opened(self):
        if self._tick:                      # never stack timers (e.g. two opens in a row)
            GLib.source_remove(self._tick)
            self._tick = None
        self.refresh()
        sec = self.interval()
        if sec:
            self._tick = GLib.timeout_add(int(sec * 1000), lambda: (self.refresh(), True)[1])

    def closed(self):
        if self._tick:
            GLib.source_remove(self._tick)
            self._tick = None


# --------------------------------------------------------------------------- Wi-Fi networks
def netpage():
    """settings/network_page.py (nmcli helpers). python -I leaves this folder off sys.path."""
    import sys
    here = str(Path(__file__).resolve().parent)
    if here not in sys.path:
        sys.path.insert(0, here)
    import network_page
    return network_page


SIGNAL_GLYPHS = ["\U000f092f", "\U000f091f", "\U000f0922", "\U000f0925", "\U000f0928"]   # 󰤯 󰤟 󰤢 󰤥 󰤨


class WifiPanel(Panel):
    """Nearby Wi-Fi networks, opened by the › next to the control center's Wi-Fi tile.
    Click a saved or open network to join it; a new secured one asks for its password inline."""
    width = 380
    bottom = True

    def build(self, box):
        box.add_css_class("compact")
        head = Gtk.Box(spacing=10)
        head.append(label("\U000f05a9", "icon-sm accent", valign=Gtk.Align.CENTER))
        head.append(label("Wi-Fi networks", "mid", hexpand=True))
        self.scan = Gtk.Button(label="\U000f0450", css_classes=["round", "flat-icon"], valign=Gtk.Align.CENTER,
                               tooltip_text="Scan again (R)")
        self.scan.add_css_class("scan-btn")
        self.scan.connect("clicked", lambda *_: self._load(rescan=True))
        self._dots = None                              # animation timer while a scan runs
        head.append(self.scan)
        head.append(label("R", "key-hint", 0.5, valign=Gtk.Align.CENTER))   # keycap like Super+S's
        self.switch = Gtk.Switch(valign=Gtk.Align.CENTER, tooltip_text="Wi-Fi on/off")
        self._sync = False
        self.switch.connect("state-set", self._toggle)
        head.append(self.switch)
        box.append(head)
        self.status = label("", "muted caption", wrap=True)
        box.append(self.status)
        self.list = Gtk.ListBox(css_classes=["wifi-list"], selection_mode=Gtk.SelectionMode.NONE)
        self.list.connect("row-activated", self._activate)
        box.append(Gtk.ScrolledWindow(child=self.list, hscrollbar_policy=Gtk.PolicyType.NEVER,
                                      propagate_natural_height=True, max_content_height=400))
        s = Gtk.Button(label="Network settings…", css_classes=["link-btn"])
        s.connect("clicked", lambda *_: (self.manager.close(), self.manager.app.present_page("network")))
        box.append(s)
        self._busy = False

    def key(self, keyval):
        if keyval == Gdk.KEY_r:                        # R = scan again (ignored while typing a password)
            self._load(rescan=True)
            return True
        return False

    # --- data (worker thread)
    @staticmethod
    def _scan(rescan):
        npg = netpage()
        nm, split = npg.nmcli, npg.split_terse
        radio = nm("radio", "wifi")[1] == "enabled"
        saved = {}                                     # SSID -> connection uuid
        uuids = [f[1] for f in map(split, nm("-t", "-f", "NAME,UUID,TYPE", "connection", "show")[1].splitlines())
                 if len(f) >= 3 and f[2] == "802-11-wireless"]
        if uuids:
            for block in nm("-t", "-g", "connection.uuid,802-11-wireless.ssid,802-11-wireless.mode",
                            "connection", "show", *uuids)[1].split("\n\n"):
                ln = (block.split("\n") + ["", "", ""])[:3]
                if ln[0] and ln[2] != "ap":            # hotspot profiles aren't networks to join
                    saved.setdefault(ln[1], ln[0])
        nets = {}
        if radio:
            out = nm("-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list",
                     "--rescan", "yes" if rescan else "auto", timeout=25)[1]
            for f in map(split, out.splitlines()):
                if len(f) < 4 or not f[1]:
                    continue
                n = {"ssid": f[1], "signal": int(f[2] or 0), "secure": f[3].strip() not in ("", "--"),
                     "inuse": f[0] == "*", "uuid": saved.get(f[1])}
                old = nets.get(n["ssid"])
                if not old or n["inuse"] or (n["signal"] > old["signal"] and not old["inuse"]):
                    nets[n["ssid"]] = n
        return radio, sorted(nets.values(), key=lambda n: (not n["inuse"], -n["signal"]))

    def _load(self, rescan=False):
        if not rescan:
            bg(lambda: self._scan(False), self._show, owner=self, key=("scan", False))
            return
        if self._dots:                                 # already scanning: the button is busy, ignore
            return
        self._scanning(True)
        bg(lambda: self._scan(True), lambda r: (self._scanning(False), self._show(r)),
           owner=self, key=("scan", True))

    def _scanning(self, on):
        """Scan button: animated dots (and not clickable) while a scan runs, the refresh icon after."""
        if self._dots:
            GLib.source_remove(self._dots)
            self._dots = None
        self.scan.set_sensitive(not on)
        if not on:
            self.scan.set_label("\U000f0450")
            self.scan.set_tooltip_text("Scan again (R)")
            return
        self.scan.set_tooltip_text("Scanning…")
        frames = ["\u2022\u2009\u00b7\u2009\u00b7", "\u00b7\u2009\u2022\u2009\u00b7", "\u00b7\u2009\u00b7\u2009\u2022"]
        step = [0]

        def tick():
            self.scan.set_label(frames[step[0] % 3])
            step[0] += 1
            return True
        tick()
        self._dots = GLib.timeout_add(280, tick)

    def _show(self, res):
        if res is None:
            self.status.set_label("Couldn't read networks from NetworkManager.")
            return
        radio, nets = res
        self._sync = True
        self.switch.set_active(radio)
        self._sync = False
        if not self._busy:
            self.status.set_label("" if radio and nets else ("Wi-Fi is off" if not radio else "No networks found"))
            self.status.set_visible(bool(self.status.get_label()))
        while (row := self.list.get_row_at_index(0)) is not None:
            self.list.remove(row)
        for n in nets[:15]:
            self.list.append(self._row(n))
        self.list.set_visible(bool(nets))

    def _row(self, n):
        row = Gtk.ListBoxRow()
        row.net = n
        col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        top = Gtk.Box(spacing=12)
        top.append(label(SIGNAL_GLYPHS[min(4, n["signal"] // 20)], "sig", valign=Gtk.Align.CENTER))
        top.append(label(n["ssid"], "net-name", hexpand=True, ellipsize=Pango.EllipsizeMode.END))
        tag = "Connected" if n["inuse"] else ("Saved" if n["uuid"] else "")
        if tag:
            top.append(label(tag, "muted caption", valign=Gtk.Align.CENTER))
        if n["secure"]:
            top.append(label("\U000f033e", "icon-sm lock", valign=Gtk.Align.CENTER))   # 󰌾
        col.append(top)
        if n["secure"] and not n["uuid"] and not n["inuse"]:
            pw = Gtk.PasswordEntry(show_peek_icon=True, placeholder_text="Password, then Enter",
                                   css_classes=["wifi-pw"])
            pw.connect("activate", lambda e: self._connect(n, e.get_text()) if e.get_text() else None)
            row.reveal = Gtk.Revealer(child=pw, transition_type=Gtk.RevealerTransitionType.SLIDE_DOWN)
            row.pw = pw
            col.append(row.reveal)
        if n["inuse"]:
            row.add_css_class("on")
        row.set_child(col)
        return row

    # --- actions
    def _activate(self, _lb, row):
        n = row.net
        if n["inuse"] or self._busy:
            return
        if hasattr(row, "reveal"):                     # new secured network: ask for the password
            row.reveal.set_reveal_child(not row.reveal.get_reveal_child())
            if row.reveal.get_reveal_child():
                row.pw.grab_focus()
            return
        self._connect(n, None)

    def _connect(self, n, password):
        npg = netpage()
        if n["uuid"] and password is None:
            args = ("connection", "up", n["uuid"])
        elif password:
            args = ("device", "wifi", "connect", n["ssid"], "password", password)
        else:
            args = ("device", "wifi", "connect", n["ssid"])
        self._busy = True
        self.status.set_visible(True)
        self.status.set_label(f"Connecting to {n['ssid']}…")

        def done(r):
            self._busy = False
            ok, _out, err = r if r else (False, "", "")
            self.status.set_label(f"Connected to {n['ssid']}" if ok else
                                  f"Couldn't connect: {(err.splitlines() or ['unknown error'])[-1][:120]}")
            self._load()
        bg(lambda: npg.nmcli(*args, timeout=40), done)

    def _toggle(self, _sw, on):
        if self._sync:
            return False
        bg(lambda: run("nmcli", "radio", "wifi", "on" if on else "off", timeout=5),
           lambda _r: GLib.timeout_add(1500, lambda: (self._load(), False)[1]))
        return False

    def closed(self):
        super().closed()
        self._scanning(False)

    def opened(self):
        super().opened()
        self._busy = False
        self.status.set_visible(False)
        self._load()                                   # cached list right away
        GLib.timeout_add(300, lambda: (self._load(rescan=True), False)[1])   # then a fresh scan


class PanelManager:
    KINDS = {"wifi": WifiPanel}

    def __init__(self, app):
        self.app = app
        self.panels = {}
        self.current = None
        self.css = Gtk.CssProvider()
        self.css.load_from_string(CSS)
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), self.css,
                                                  Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 1)

    def restyle(self, theme):
        """Recolour the popups for a hyprminimal theme (see themes.py)."""
        import themes
        self.css.load_from_string(themes.recolor(CSS, theme))

    def prewarm(self):
        """Build every panel hidden (called on preload) so the first click is instant."""
        for kind in self.KINDS:
            self._get(kind)

    def _get(self, kind):
        if kind not in self.panels:
            self.panels[kind] = self.KINDS[kind](self.app, self)
        return self.panels[kind]

    def toggle(self, kind):
        if kind not in self.KINDS:
            return
        if self.current == kind:
            self.close()
            return
        self.open(kind)

    def open(self, kind, monitor=None):
        self.close()
        hide_control_center()   # one popup at a time: never on top of the Super+S control center
        close_c_panels()        # nor on top of a C popup (helpers/panels.c)
        p = self._get(kind)
        if LS:
            # exclusive like the Super+S control center: with follow_mouse an on-demand popup loses the
            # keyboard as soon as the pointer leaves it, and Esc went to the window underneath
            LS.set_keyboard_mode(p, LS.KeyboardMode.EXCLUSIVE)
            LS.set_monitor(p, monitor)
        p.opened()
        p.present()
        self.current = kind
        self.app.panel_state(True)

    def close(self):
        if self.current:
            p = self.panels[self.current]
            p.closed()
            p.set_visible(False)
            self.current = None
            self.app.panel_state(False)
