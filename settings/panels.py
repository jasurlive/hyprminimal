"""hyprminimal bar panels: small Noctalia-style popups opened by clicking bar widgets.

They live inside the resident hyprminimal-settings process, so opening one is a D-Bus call (~10 ms).
Panels: calendar (clock/date), battery, network (download speed), media.
Close with Esc, a click outside, or clicking the same widget again.
"""
import glob
import math
import os
import subprocess
import threading
import time
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, GdkPixbuf, GLib, Gtk, Pango  # noqa: E402

try:
    gi.require_version("Gtk4LayerShell", "1.0")
    from gi.repository import Gtk4LayerShell as LS  # needs LD_PRELOAD=libgtk4-layer-shell.so
    if not LS.is_supported():
        LS = None
except (ValueError, ImportError):
    LS = None

POPUP_TOP = 8         # gap under the bar (its exclusive zone already reserves bar + margin)
POPUP_RIGHT = 4       # right edge in line with the bar (bar margin is 4)
CSS = """
window.hyprminimal-panel { background: transparent; }
window.hyprminimal-catcher { background: rgba(0, 0, 0, 0.01); }   /* needs real pixels or no buffer is attached */
.panel {
  background: rgba(31, 35, 53, 0.96); border: 1px solid #4b517a; padding: 18px 20px;
  color: #c0caf5; box-shadow: 0 6px 18px rgba(24, 27, 42, 0.55);
}
.panel .big   { font-size: 40px; font-weight: 800; color: #c0caf5; }
.panel .mid   { font-size: 18px; font-weight: 700; color: #c0caf5; }
.panel .muted { color: #a9b1d6; }
.panel .accent { color: #7a88cf; }
.panel .pink  { color: #d7729f; }
.panel .icon  { font-family: "Symbols Nerd Font"; font-size: 26px; color: #7a88cf; }
.panel .kv-key { color: #a9b1d6; }
.panel .kv-val { color: #c0caf5; font-weight: 700; }
.panel .card { background: rgba(44, 49, 74, 0.6); border: 1px solid #4b517a; border-radius: 0; padding: 12px 14px; }
.panel button.tile { background: rgba(44, 49, 74, 0.6); border: 1px solid #4b517a; border-radius: 0; padding: 8px 12px; }
.panel button.tile:hover { background: rgba(122, 136, 207, 0.35); }
.panel button.tile.on { background: #7a88cf; color: #1f2335; }
.panel button.round { border-radius: 999px; min-width: 42px; min-height: 42px; padding: 0; font-family: "Symbols Nerd Font"; font-size: 20px; }
.panel button.round.main { background: #7a88cf; color: #1f2335; min-width: 52px; min-height: 52px; font-size: 24px; }
.panel levelbar trough { background: #2c314a; min-height: 8px; border-radius: 0; }
.panel levelbar block.filled { background: #7a88cf; border-radius: 0; }
.panel levelbar block.filled.low { background: #cf8f9a; }   /* dusty rose */
.panel levelbar.high block.filled { background: #cf8f9a; }
.panel calendar { background: transparent; border: none; color: #c0caf5; }
.panel calendar > grid > label.today { background: #d7729f; color: #1f2335; border-radius: 0; }
.panel calendar > grid > label:selected { background: #7a88cf; color: #1f2335; border-radius: 0; }
.panel calendar > header { border: none; }
.panel .art { border: 1px solid #4b517a; }
.panel .icon-sm { font-family: "Symbols Nerd Font"; font-size: 18px; color: #a9b1d6; }
.panel .caption { font-size: 12px; }
.panel scale.timeline trough { background: #2c314a; min-height: 6px; border-radius: 0; }
.panel scale.timeline highlight { background: #7a88cf; min-height: 6px; border-radius: 0; }
.panel scale.timeline slider { background: #c0caf5; min-width: 14px; min-height: 14px; border-radius: 999px; margin: -4px; box-shadow: none; }
.panel button.round.flat-icon { background: transparent; min-width: 36px; min-height: 36px; font-size: 18px; color: #a9b1d6; }
.panel button.round.flat-icon.on { color: #d7729f; }
.panel.compact { padding: 14px 16px; }
.panel .card.tight { padding: 6px 6px 6px 12px; }
.panel .pw { font-family: monospace; letter-spacing: 1px; }
.panel .card.speed-tile { padding: 10px 12px; }
.panel .speed { font-size: 16px; font-weight: 700; font-feature-settings: "tnum"; }
.panel .speed-tile .icon-sm { font-size: 22px; }
.panel button.link-btn { background: transparent; border: none; padding: 2px 0; color: #7a88cf; }
.panel button.link-btn:hover { color: #c0caf5; }
.panel switch { background: #2c314a; border: 1px solid #4b517a; }
.panel switch:checked { background: #7a88cf; }
.panel switch slider { background: #c0caf5; }
.panel list.wifi-list { background: transparent; }
.panel list.wifi-list > row { padding: 9px 10px; margin: 2px 0; border-radius: 10px; }
.panel list.wifi-list > row:hover { background: rgba(122, 136, 207, 0.18); }
.panel list.wifi-list > row.on { background: rgba(122, 136, 207, 0.30); }
.panel .sig { font-family: "Symbols Nerd Font"; font-size: 19px; color: #7a88cf; }
.panel button.scan-btn:disabled { color: #7a88cf; opacity: 1; font-family: "Noto Sans"; font-size: 16px; font-weight: 800; }
.panel .net-name { font-size: 15px; font-weight: 600; }
.panel .lock { font-size: 14px; }
.panel .wifi-pw { margin-top: 8px; background: #2c314a; border: 1px solid #4b517a; border-radius: 8px; padding: 4px 10px; }
"""


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


def label(text="", cls=None, xalign=0, **kw):
    lb = Gtk.Label(label=text, xalign=xalign, **kw)
    for c in (cls or "").split():
        lb.add_css_class(c)
    return lb


def kv_grid(rows):
    g = Gtk.Grid(column_spacing=18, row_spacing=6)
    vals = {}
    for i, (k, key) in enumerate(rows):
        g.attach(label(k, "kv-key"), 0, i, 1, 1)
        v = label("…", "kv-val", xalign=1, hexpand=True, ellipsize=Pango.EllipsizeMode.END, max_width_chars=26)
        g.attach(v, 1, i, 1, 1)
        vals[key] = v
    return g, vals


def wifi_signal(dev):
    """(percent, dBm) from the kernel, instant. nmcli can stall seconds while it rescans."""
    try:
        for line in Path("/proc/net/wireless").read_text().splitlines()[2:]:
            name, rest = line.split(":", 1)
            if name.strip() == dev:
                f = rest.split()
                link, level = float(f[1].rstrip(".")), float(f[2].rstrip("."))
                return min(100, round(link * 100 / 70)), int(level)
    except (OSError, ValueError, IndexError):
        pass
    return None


WIFI_ICONS = ["\U000f092f", "\U000f091f", "\U000f0922", "\U000f0925", "\U000f0928"]   # 0..4 bars


def signal_words(pct):
    return "excellent" if pct >= 80 else "good" if pct >= 60 else "fair" if pct >= 40 else "weak"


def human_rate(bps):
    for unit in ("B/s", "kB/s", "MB/s", "GB/s"):
        if bps < 1000:
            return f"{bps:.0f} {unit}" if unit == "B/s" else f"{bps:.1f} {unit}"
        bps /= 1000
    return f"{bps:.1f} TB/s"


def human_bytes(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024:
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} PB"


# --------------------------------------------------------------------------- base
class Panel(Gtk.Window):
    width = 360
    bottom = False        # True: anchor bottom right (opened from the control center's bottom row)

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
            LS.set_keyboard_mode(self, LS.KeyboardMode.ON_DEMAND)
            edge = LS.Edge.BOTTOM if self.bottom else LS.Edge.TOP
            LS.set_anchor(self, edge, True)
            LS.set_margin(self, edge, POPUP_TOP)
            LS.set_anchor(self, LS.Edge.RIGHT, True)        # every popup sits on the right
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
        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", lambda _c, kv, *_: (self.manager.close() or True) if kv == Gdk.KEY_Escape else False)
        self.add_controller(keys)

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


# --------------------------------------------------------------------------- calendar
class CalendarPanel(Panel):
    width = 340

    def build(self, box):
        self.time = label("", "big", xalign=0.5)
        self.date = label("", "muted", xalign=0.5)
        box.append(self.time)
        box.append(self.date)
        self.cal = Gtk.Calendar(show_week_numbers=False)
        box.append(self.cal)

    def interval(self):
        return 1

    def opened(self):
        today = GLib.DateTime.new_now_local()
        self.cal.select_day(today)
        super().opened()

    def refresh(self):
        now = time.localtime()
        self.time.set_label(time.strftime("%H:%M:%S", now))
        self.date.set_label(time.strftime("%A, %d %B %Y", now))


# --------------------------------------------------------------------------- battery
class BatteryPanel(Panel):
    def build(self, box):
        top = Gtk.Box(spacing=14)
        self.icon = label("", "icon")
        top.append(self.icon)
        col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.pct = label("", "big")
        self.state = label("", "muted")
        col.append(self.pct)
        col.append(self.state)
        top.append(col)
        box.append(top)
        self.level = Gtk.LevelBar(min_value=0, max_value=100)
        self.level.add_offset_value("low", 20)
        box.append(self.level)
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, css_classes=["card"])
        grid, self.v = kv_grid([("Time left", "left"), ("Power draw", "power"), ("Health", "health"),
                                ("Charge cycles", "cycles")])
        card.append(grid)
        box.append(card)
        box.append(label("POWER MODE", "pink", margin_top=4))
        self.modes = Gtk.Box(spacing=6, homogeneous=True)
        self.mode_btns = {}
        for mode, text in (("power-saver", "Saver"), ("balanced", "Balanced"), ("performance", "Performance")):
            b = Gtk.Button(label=text, css_classes=["tile"])
            b.connect("clicked", lambda _b, m=mode: bg(lambda: run("powerprofilesctl", "set", m),
                                                       lambda _r: self.refresh()))
            self.modes.append(b)
            self.mode_btns[mode] = b
        box.append(self.modes)

    def interval(self):
        return 5

    def refresh(self):
        bat = next(iter(sorted(glob.glob("/sys/class/power_supply/BAT*"))), None)
        if not bat:
            self.pct.set_label("No battery")
            return
        u = {}
        try:
            for line in Path(bat, "uevent").read_text().splitlines():
                k, _, v = line.partition("=")
                u[k.replace("POWER_SUPPLY_", "")] = v
        except OSError:
            return
        cap = int(u.get("CAPACITY", 0))
        status = u.get("STATUS", "Unknown")
        self.pct.set_label(f"{cap}%")
        self.state.set_label(status)
        self.level.set_value(cap)
        icons = ["", "", "", "", ""]
        self.icon.set_label("\U000f0084" if status == "Charging" else icons[min(4, cap // 21)])
        now = float(u.get("CHARGE_NOW") or u.get("ENERGY_NOW") or 0)
        full = float(u.get("CHARGE_FULL") or u.get("ENERGY_FULL") or 0)
        design = float(u.get("CHARGE_FULL_DESIGN") or u.get("ENERGY_FULL_DESIGN") or 0)
        rate = float(u.get("CURRENT_NOW") or u.get("POWER_NOW") or 0)
        volts = float(u.get("VOLTAGE_NOW") or 0) / 1e6
        watts = (rate * volts / 1e6) if "CURRENT_NOW" in u else rate / 1e6
        if rate > 0 and status == "Discharging":
            hours = now / rate
        elif rate > 0 and status == "Charging":
            hours = (full - now) / rate
        else:
            hours = None
        self.v["left"].set_label(f"{int(hours)} h {int(hours * 60) % 60:02d} min" if hours else "—")
        self.v["power"].set_label(f"{watts:.1f} W" if watts else "—")
        self.v["health"].set_label(f"{100 * full / design:.0f}%" if design else "—")
        self.v["cycles"].set_label(u.get("CYCLE_COUNT", "—"))

        def done(cur):
            for m, b in self.mode_btns.items():
                (b.add_css_class if m == cur else b.remove_css_class)("on")
        bg(lambda: run("powerprofilesctl", "get"), done, self, "profile")


# --------------------------------------------------------------------------- network
class NetworkPanel(Panel):
    """Small Wi-Fi card: network, signal, live speed, password (hidden, show/copy), IP, radio switch."""
    width = 360

    def build(self, box):
        box.add_css_class("compact")
        top = Gtk.Box(spacing=12)
        self.icon = label("\U000f0928", "icon")
        top.append(self.icon)
        col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, hexpand=True, valign=Gtk.Align.CENTER)
        self.ssid = label("", "mid", ellipsize=Pango.EllipsizeMode.END, max_width_chars=18)
        self.sub = label("", "muted caption")
        col.append(self.ssid)
        col.append(self.sub)
        top.append(col)
        self.switch = Gtk.Switch(valign=Gtk.Align.CENTER, tooltip_text="Wi-Fi on/off")
        self._sw_sync = False
        self.switch.connect("state-set", self._toggle_wifi)
        top.append(self.switch)
        box.append(top)

        # live speed: two small tiles, fixed-width digits so they don't wobble
        speeds = Gtk.Box(spacing=8, homogeneous=True)
        self.down = label("…", "speed accent", xalign=0)
        self.up = label("…", "speed pink", xalign=0)
        for lb, glyph, title, cls in ((self.down, "\U000f0045", "Download", "accent"), (self.up, "\U000f005d", "Upload", "pink")):
            tile = Gtk.Box(spacing=10, css_classes=["card", "speed-tile"])
            tile.append(label(glyph, f"icon-sm {cls}", valign=Gtk.Align.CENTER))
            col2 = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            col2.append(label(title, "muted caption"))
            col2.append(lb)
            tile.append(col2)
            speeds.append(tile)
        box.append(speeds)

        # password: hidden until the eye is clicked; copy works either way
        self.pw_card = Gtk.Box(spacing=6, css_classes=["card", "tight"])
        pcol = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, hexpand=True, valign=Gtk.Align.CENTER)
        pcol.append(label("Password", "kv-key caption"))
        self.pw = label("", "kv-val pw", selectable=False)
        pcol.append(self.pw)
        self.pw_card.append(pcol)
        self.eye = Gtk.Button(label="\U000f0208", css_classes=["round", "flat-icon"], valign=Gtk.Align.CENTER,
                              tooltip_text="Show password")
        self.eye.connect("clicked", self._reveal)
        self.copy = Gtk.Button(label="\U000f018f", css_classes=["round", "flat-icon"], valign=Gtk.Align.CENTER,
                               tooltip_text="Copy password")
        self.copy.connect("clicked", self._copy)
        self.pw_card.append(self.eye)
        self.pw_card.append(self.copy)
        box.append(self.pw_card)

        grid, self.v = kv_grid([("IP address", "ip"), ("Gateway", "gw")])
        box.append(grid)

        s = Gtk.Button(label="Network settings…", css_classes=["link-btn"])
        s.connect("clicked", lambda *_: (self.manager.close(), self.manager.app.present_page("network")))
        box.append(s)
        self._last = None
        self._dev = None
        self._psk = ""
        self._shown = False

    def interval(self):
        return 1

    # --- password
    def _show_pw(self):
        if not self._psk:
            self.pw.set_label("—")
        else:
            self.pw.set_label(self._psk if self._shown else "•" * min(len(self._psk), 14))
        self.eye.set_label("\U000f0209" if self._shown else "\U000f0208")      # eye-off / eye
        self.eye.set_tooltip_text("Hide password" if self._shown else "Show password")

    def _reveal(self, *_):
        self._shown = not self._shown
        self._show_pw()

    def _copy(self, *_):
        if self._psk:
            self.get_clipboard().set(self._psk)
            self.copy.set_label("\U000f012c")                                   # check mark
            GLib.timeout_add(1200, lambda: (self.copy.set_label("\U000f018f"), False)[1])

    # --- radio
    def _toggle_wifi(self, _sw, on):
        if self._sw_sync:
            return False
        bg(lambda: run("nmcli", "radio", "wifi", "on" if on else "off", timeout=5),
           lambda _r: GLib.timeout_add(1500, lambda: (self._info(), False)[1]))
        return False

    def opened(self):
        self._last = None
        self._shown = False
        self._info()
        super().opened()
        GLib.timeout_add(400, lambda: (self.refresh(), False)[1])   # first speed after 0.4 s

    def closed(self):
        super().closed()
        self._shown = False
        self._psk = ""                     # don't keep the password around while hidden

    def _info(self):
        def work():
            # interface from the kernel routing table (instant, never stalls)
            dev = None
            try:
                for line in Path("/proc/net/route").read_text().splitlines()[1:]:
                    f = line.split()
                    if f[1] == "00000000":
                        dev = f[0]
                        break
            except (OSError, IndexError):
                pass
            typ = None
            if dev:
                typ = "wifi" if Path("/sys/class/net", dev, "wireless").exists() else "ethernet"
            info = {"dev": dev, "type": typ, "conn": None, "psk": "", "band": "", "sec": "",
                    "radio": run("nmcli", "radio", "wifi", timeout=6) != "disabled"}
            if dev:   # name and addresses from NetworkManager
                show = run("nmcli", "-t", "-f", "GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY",
                           "dev", "show", dev, timeout=6)
                for line in show.splitlines():
                    k, _, v = line.partition(":")
                    if k == "GENERAL.CONNECTION":
                        info["conn"] = v
                    elif k.startswith("IP4.ADDRESS") and "ip" not in info:
                        info["ip"] = v.split("/")[0]
                    elif k == "IP4.GATEWAY":
                        info["gw"] = v
            if typ == "wifi" and info["conn"]:
                info["psk"] = run("nmcli", "-s", "-g", "802-11-wireless-security.psk",
                                  "connection", "show", info["conn"], timeout=4)
                # cached scan only (--rescan no): never stalls
                for line in run("nmcli", "-t", "-f", "IN-USE,FREQ,SECURITY", "dev", "wifi", "list",
                                "--rescan", "no", timeout=4).splitlines():
                    f = line.split(":")
                    if f[0] == "*" and len(f) >= 3:
                        mhz = int(f[1].split()[0]) if f[1].split() and f[1].split()[0].isdigit() else 0
                        info["band"] = "6 GHz" if mhz >= 5925 else "5 GHz" if mhz >= 4900 else "2.4 GHz" if mhz else ""
                        info["sec"] = f[2].replace(" ", "/") if f[2] else "Open"
                        break
            return info

        def done(info):
            if not info:
                return
            first = self._dev is None or self._dev != info["dev"]
            self._dev = info["dev"]
            if first and info["dev"]:          # baseline now, first speed 0.4 s later
                self._last = None
                self.refresh()
                GLib.timeout_add(400, lambda: (self.refresh(), False)[1])
            self._sw_sync = True
            self.switch.set_active(info["radio"])
            self._sw_sync = False
            self._type = info["type"]
            self._extra = " · ".join(x for x in (info["band"], info["sec"]) if x)
            self._psk = info["psk"]
            self.pw_card.set_visible(info["type"] == "wifi" and bool(info["psk"]))
            self._show_pw()
            if not info["dev"]:
                self.ssid.set_label("Wi-Fi off" if not info["radio"] else "Disconnected")
                self.sub.set_label("")
                self.icon.set_label("\U000f092e")
                for k in ("ip", "gw"):
                    self.v[k].set_label("—")
                self.down.set_label("—")
                self.up.set_label("—")
                return
            self.ssid.set_label(info["conn"] or info["dev"])
            if info["type"] == "wifi":
                self._show_signal()
            else:
                self.sub.set_label("Ethernet")
                self.icon.set_label("\U000f0200")
            self.v["ip"].set_label(info.get("ip", "—"))
            self.v["gw"].set_label(info.get("gw", "—"))
        bg(work, done, self, "info")

    def _show_signal(self):
        sig = wifi_signal(self._dev)
        extra = getattr(self, "_extra", "")
        if sig:
            pct, _dbm = sig
            self.sub.set_label(" · ".join(x for x in (f"{signal_words(pct).capitalize()} {pct}%", extra) if x))
            self.icon.set_label(WIFI_ICONS[min(4, pct // 20)])
        else:
            self.sub.set_label(extra or "Wi-Fi")

    def refresh(self):
        if not self._dev:
            return
        if getattr(self, "_type", None) == "wifi":
            self._show_signal()                 # live, every second
        base = Path("/sys/class/net", self._dev, "statistics")
        try:
            rx = int((base / "rx_bytes").read_text())
            tx = int((base / "tx_bytes").read_text())
        except OSError:
            return
        t = time.monotonic()
        if self._last:
            lt, lrx, ltx = self._last
            dt = max(t - lt, 0.001)
            self.down.set_label(human_rate((rx - lrx) / dt))
            self.up.set_label(human_rate((tx - ltx) / dt))
        else:
            self.down.set_label("…")
            self.up.set_label("…")
        self._last = (t, rx, tx)


# --------------------------------------------------------------------------- media
class Equalizer(Gtk.DrawingArea):
    """Big animated equalizer. There is no audio data, so it plays a beat-like pattern:
    a kick every ~0.47 s lifts the low bars, the rest flicker, everything eases down.
    Animates only while visible and playing; settles flat and stops when paused."""

    BARS = 56
    BAR_W = 3.0                 # px; thin rounded pills
    COLOR = (0x7a / 255, 0x88 / 255, 0xcf / 255)   # @primary, same blue as the bar's equalizer

    def __init__(self):
        super().__init__(content_height=78, hexpand=True)
        import random
        self.rand = random.Random()
        self.h = [0.06] * self.BARS
        # bell-shaped envelope: the middle reaches highest, the edges stay low
        self.env = [0.25 + 0.75 * math.sin(math.pi * (i + 0.5) / self.BARS) ** 1.5 for i in range(self.BARS)]
        self.target = [0.06] * self.BARS
        self.playing = False
        self._tick = None
        self._last_beat = 0.0
        self._last_flick = 0.0
        self.set_draw_func(self._draw)
        self.connect("map", lambda *_: self._ensure_tick())
        self.connect("unmap", lambda *_: self._stop())

    FPS = 30                    # timer-driven, not per display frame (120 Hz panels would redraw 4x as often)

    def set_playing(self, playing):
        self.playing = playing
        self._ensure_tick()

    def _ensure_tick(self):
        if self._tick is None and self.get_mapped():
            self._tick = GLib.timeout_add(1000 // self.FPS, self._on_tick)

    def _stop(self):
        if self._tick is not None:
            GLib.source_remove(self._tick)
            self._tick = None

    def _on_tick(self):
        root = self.get_root()
        if not self.get_mapped() or root is None or not root.get_visible():   # never animate while hidden
            self._tick = None
            return False
        t = GLib.get_monotonic_time() / 1e6
        r = self.rand
        if self.playing:
            if t - self._last_beat > 0.47:                 # kick
                self._last_beat = t
                for i in range(self.BARS):
                    self.target[i] = min(1.0, self.env[i] * r.uniform(0.55, 1.0))
            elif t - self._last_flick > 0.09:              # hats / melody
                self._last_flick = t
                for i in range(self.BARS):
                    if r.random() < 0.45:
                        self.target[i] = max(self.target[i] * 0.7, self.env[i] * r.uniform(0.15, 0.8))
            for i in range(self.BARS):
                self.target[i] = max(0.06, self.target[i] * 0.912)   # decay between hits (0.955/frame at 60 fps)
        else:
            self.target = [0.04] * self.BARS
        moving = False
        for i in range(self.BARS):
            k = 0.70 if self.target[i] > self.h[i] else 0.33        # fast attack, slow release (30 fps)
            nh = self.h[i] + (self.target[i] - self.h[i]) * k
            moving |= abs(nh - self.h[i]) > 0.002
            self.h[i] = nh
        self.queue_draw()
        if not self.playing and not moving:
            self._tick = None
            return False
        return True

    def _draw(self, _a, cr, w, h):
        n, bw = self.BARS, self.BAR_W
        gap = (w - n * bw) / max(1, n - 1)
        hv = self.h
        r, g, b = self.COLOR
        cr.set_line_width(bw)
        cr.set_line_cap(1)                              # cairo.LINE_CAP_ROUND
        mid = h / 2
        for i in range(n):
            # light smoothing with the neighbours: reads as one soft wave, not noise
            v = 0.5 * hv[i] + 0.25 * hv[max(0, i - 1)] + 0.25 * hv[min(n - 1, i + 1)]
            half = max(0.0, v * (h - bw) / 2)           # 0 = a round dot at rest
            x = i * (bw + gap) + bw / 2
            cr.set_source_rgba(r, g, b, 0.45 + 0.55 * min(1.0, v * 1.4))
            cr.move_to(x, mid - half)
            cr.line_to(x, mid + half)
            cr.stroke()


class MediaPanel(Panel):
    width = 400

    def build(self, box):
        top = Gtk.Box(spacing=16)
        self.art = Gtk.Image(pixel_size=96, css_classes=["art"], valign=Gtk.Align.START)   # fixed 96px square
        top.append(self.art)
        col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.CENTER, spacing=4, hexpand=True)
        self.title = label("", "mid", wrap=True, lines=2, ellipsize=Pango.EllipsizeMode.END, max_width_chars=22)
        self.title.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.artist = label("", "accent", ellipsize=Pango.EllipsizeMode.END, max_width_chars=26)
        self.player = label("", "muted caption")
        for w in (self.title, self.artist, self.player):
            col.append(w)
        top.append(col)
        box.append(top)

        self.eq = Equalizer()
        self.eq.set_content_height(56)
        box.append(self.eq)

        # timeline: only when the player reports a length (drag to seek)
        self.seek = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 1, 1)
        self.seek.set_draw_value(False)
        self.seek.add_css_class("timeline")
        self._dragging = False
        self.seek.connect("change-value", self._on_seek)
        g = Gtk.GestureClick()
        g.connect("pressed", lambda *_: setattr(self, "_dragging", True))
        g.connect("stopped", lambda *_: setattr(self, "_dragging", False))
        g.connect("released", lambda *_: setattr(self, "_dragging", False))
        self.seek.add_controller(g)
        box.append(self.seek)
        times = Gtk.Box()
        self.times_box = times
        self.t_pos = label("0:00", "muted caption")
        self.t_len = label("0:00", "muted caption", xalign=1, hexpand=True)
        times.append(self.t_pos)
        times.append(self.t_len)
        box.append(times)

        ctl = Gtk.Box(spacing=18, halign=Gtk.Align.CENTER)
        prev = Gtk.Button(label="\U000f04ae", css_classes=["round"], tooltip_text="Previous", valign=Gtk.Align.CENTER)
        prev.connect("clicked", lambda *_: self._cmd("previous"))
        self.play = Gtk.Button(label="\U000f040a", css_classes=["round", "main"], tooltip_text="Play / pause",
                               valign=Gtk.Align.CENTER)
        self.play.connect("clicked", lambda *_: self._cmd("play-pause"))
        nxt = Gtk.Button(label="\U000f04ad", css_classes=["round"], tooltip_text="Next", valign=Gtk.Align.CENTER)
        nxt.connect("clicked", lambda *_: self._cmd("next"))
        for b_ in (prev, self.play, nxt):
            ctl.append(b_)
        box.append(ctl)
        self._art_url = None
        self._len = 0

    def interval(self):
        return 0.5

    def _cmd(self, *args):
        def after(_r):
            for ms in (120, 450):
                GLib.timeout_add(ms, lambda: (self.refresh(), False)[1])
        bg(lambda: run("playerctl", *args), after)

    def _on_seek(self, _s, _scroll, value):
        if self._len:
            v = max(0, min(value, self._len))
            bg(lambda: run("playerctl", "position", f"{v:.1f}"), lambda _r: None)
        return False

    def refresh(self):
        fmt = ("{{status}}\t{{title}}\t{{artist}}\t{{album}}\t{{mpris:artUrl}}\t{{position}}\t"
               "{{mpris:length}}\t{{playerName}}")
        bg(lambda: run("playerctl", "metadata", "--format", fmt), self._show, self, "meta")

    @staticmethod
    def _mmss(sec):
        sec = int(sec)
        return f"{sec // 3600}:{sec // 60 % 60:02d}:{sec % 60:02d}" if sec >= 3600 else f"{sec // 60}:{sec % 60:02d}"

    def _show(self, out):
        out = out or ""
        f = (out.split("\t") + [""] * 8)[:8] if out else [""] * 8
        status, title, artist, album, art, pos, length, player = f
        playing = status == "Playing"
        self.eq.set_playing(playing)
        if not status:
            self.title.set_label("Nothing Playing")
            for w in (self.artist, self.player):
                w.set_label("")
            self.t_pos.set_label("0:00")
            self.t_len.set_label("0:00")
            self.seek.set_range(0, 1)
            self.seek.set_value(0)
            self.art.set_from_paintable(None)
            self.art.set_from_icon_name("audio-x-generic-symbolic")
            self.play.set_label("\U000f040a")
            self.seek.set_visible(False)
            self.times_box.set_visible(False)
            return
        self.title.set_label(title or "Unknown title")
        self.artist.set_label(" · ".join(x for x in (artist, album) if x))
        self.player.set_label(player.replace("_", " ").title())
        self.play.set_label("\U000f03e4" if playing else "\U000f040a")
        try:
            p, ln = int(pos or 0) / 1e6, int(length or 0) / 1e6
        except ValueError:
            p, ln = 0, 0
        self._len = ln
        if ln:
            self.seek.set_sensitive(True)
            self.seek.set_range(0, ln)
            if not self._dragging:
                self.seek.set_value(p)
            self.t_pos.set_label(self._mmss(p))
            self.t_len.set_label("-" + self._mmss(max(0, ln - p)))
        self.seek.set_visible(bool(ln))         # some players (e.g. Firefox) report no length
        self.times_box.set_visible(bool(ln))
        if art != self._art_url:
            self._art_url = art
            if art.startswith("file://"):
                path = GLib.filename_from_uri(art)[0]

                def load():
                    try:                     # centre-crop to a square, any thumbnail shape fits
                        pb = GdkPixbuf.Pixbuf.new_from_file(path)
                        side = min(pb.get_width(), pb.get_height())
                        pb = pb.new_subpixbuf((pb.get_width() - side) // 2, (pb.get_height() - side) // 2, side, side)
                        pb = pb.scale_simple(192, 192, GdkPixbuf.InterpType.BILINEAR)   # 96px @2x
                        return Gdk.Texture.new_for_pixbuf(pb)
                    except GLib.Error:
                        return None
                bg(load, lambda t: self.art.set_from_paintable(t) if t else
                   self.art.set_from_icon_name("audio-x-generic-symbolic"))
            else:
                self.art.set_from_icon_name("audio-x-generic-symbolic")

    def closed(self):
        super().closed()
        self.eq.playing = False
        self.eq._stop()               # hidden: no animation timer left behind



# --------------------------------------------------------------------------- system
class SystemPanel(Panel):
    """Plain-language system overview: no jargon, numbers people read at a glance."""
    width = 380

    def build(self, box):
        top = Gtk.Box(spacing=14)
        top.append(label("\U000f0379", "icon"))
        col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.host = label("", "mid")
        self.up = label("", "muted")
        col.append(self.host)
        col.append(self.up)
        top.append(col)
        box.append(top)
        self.rows = {}
        for key, title in (("cpu", "Processor"), ("temp", "Temperature"), ("mem", "Memory"),
                           ("disk", "Storage"), ("bat", "Battery")):
            card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, css_classes=["card"])
            head = Gtk.Box()
            head.append(label(title, "kv-key"))
            val = label("…", "kv-val", xalign=1, hexpand=True)
            head.append(val)
            card.append(head)
            bar = Gtk.LevelBar(min_value=0, max_value=1)
            bar.add_offset_value("low", 0.0)
            card.append(bar)
            box.append(card)
            self.rows[key] = (val, bar, card)
        self._cpu_last = None

    def interval(self):
        return 2

    @staticmethod
    def _cpu_times():
        f = Path("/proc/stat").read_text().split("\n", 1)[0].split()[1:]
        v = list(map(int, f))
        idle = v[3] + (v[4] if len(v) > 4 else 0)
        return idle, sum(v)

    def opened(self):
        self._cpu_last = self._cpu_times()
        self.host.set_label(os.uname().nodename)
        super().opened()

    def refresh(self):
        # uptime
        secs = float(Path("/proc/uptime").read_text().split()[0])
        d, h, m = int(secs // 86400), int(secs // 3600 % 24), int(secs // 60 % 60)
        self.up.set_label("On for " + (f"{d} day{'s' if d != 1 else ''}, " if d else "") +
                          (f"{h} hour{'s' if h != 1 else ''} " if h or d else "") + f"{m} min")
        # cpu
        idle, total = self._cpu_times()
        li, lt = self._cpu_last or (idle, total)
        used = 1 - (idle - li) / max(1, total - lt)
        self._cpu_last = (idle, total)
        self._set("cpu", used, f"{used * 100:.0f}% busy")
        # temperature (hottest CPU-ish sensor)
        temps = []
        for z in glob.glob("/sys/class/thermal/thermal_zone*/temp") + glob.glob("/sys/class/hwmon/hwmon*/temp*_input"):
            try:
                t = int(Path(z).read_text()) / 1000
                if 10 < t < 120:
                    temps.append(t)
            except (OSError, ValueError):
                pass
        if temps:
            t = max(temps)
            word = "cool" if t < 60 else "warm" if t < 80 else "hot"
            self._set("temp", min(1, t / 100), f"{t:.0f} °C, {word}")
        else:
            self.rows["temp"][2].set_visible(False)
        # memory
        mi = {}
        for line in Path("/proc/meminfo").read_text().splitlines():
            k, v = line.split(":", 1)
            mi[k] = int(v.split()[0]) * 1024
        tot, avail = mi.get("MemTotal", 1), mi.get("MemAvailable", 0)
        self._set("mem", (tot - avail) / tot, f"{human_bytes(tot - avail)} of {human_bytes(tot)} used")
        # disk (/)
        st = os.statvfs("/")
        dt, df = st.f_blocks * st.f_frsize, st.f_bavail * st.f_frsize
        self._set("disk", (dt - df) / dt, f"{human_bytes(df)} free of {human_bytes(dt)}")
        # battery
        bat = next(iter(sorted(glob.glob("/sys/class/power_supply/BAT*"))), None)
        if bat:
            try:
                cap = int(Path(bat, "capacity").read_text())
                stat = Path(bat, "status").read_text().strip().lower()
                self._set("bat", cap / 100, f"{cap}%, {stat}")
            except OSError:
                pass
        else:
            self.rows["bat"][2].set_visible(False)

    def _set(self, key, frac, text):
        val, bar, _card = self.rows[key]
        val.set_label(text)
        bar.set_value(max(0.0, min(1.0, frac)))
        (bar.add_css_class if frac > 0.85 else bar.remove_css_class)("high")


# --------------------------------------------------------------------------- manager
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
                               tooltip_text="Scan again")
        self.scan.add_css_class("scan-btn")
        self.scan.connect("clicked", lambda *_: self._load(rescan=True))
        self._dots = None                              # animation timer while a scan runs
        head.append(self.scan)
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
            self.scan.set_tooltip_text("Scan again")
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
    KINDS = {"calendar": CalendarPanel, "battery": BatteryPanel, "network": NetworkPanel, "media": MediaPanel,
             "system": SystemPanel, "wifi": WifiPanel}

    def __init__(self, app):
        self.app = app
        self.panels = {}
        self.current = None
        self.catcher = None
        self.external = None    # a popup run as its own program (rofi): closed with pkill
        prov = Gtk.CssProvider()
        prov.load_from_string(CSS)
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), prov,
                                                  Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 1)

    def _make_catcher(self):
        """Invisible full-screen layer under the panel: a click anywhere outside closes it."""
        w = Gtk.Window(application=self.app, decorated=False)
        w.add_css_class("hyprminimal-catcher")
        if LS:
            LS.init_for_window(w)
            LS.set_namespace(w, "hyprminimal-catcher")
            LS.set_layer(w, LS.Layer.TOP)
            LS.set_exclusive_zone(w, -1)
            for e in (LS.Edge.TOP, LS.Edge.BOTTOM, LS.Edge.LEFT, LS.Edge.RIGHT):
                LS.set_anchor(w, e, True)
        w.set_child(Gtk.Box(hexpand=True, vexpand=True))
        click = Gtk.GestureClick(button=0)
        click.connect("pressed", lambda *_: self.close())
        w.add_controller(click)
        return w

    def prewarm(self):
        """Build every panel hidden (called on preload) so the first click is instant."""
        for kind in self.KINDS:
            self._get(kind)
        if LS and self.catcher is None:
            self.catcher = self._make_catcher()

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
        # no outside-click catcher for panels (it swallowed the click): Hyprland closes the popup on a
        # click outside it instead (hypr/layerrules.lua), and the click still reaches the window
        self.close()
        p = self._get(kind)
        p.opened()
        p.present()
        self.current = kind

    def _show_catcher(self):
        if LS:
            if self.catcher is None:
                self.catcher = self._make_catcher()
            self.catcher.set_visible(True)

    def catch(self, prog):
        """Outside clicks close another program's popup too (launcher, session menu): `prog` is its
        process name, shown while it runs; "" = it closed by itself, just drop the catcher."""
        if not prog:
            if self.external and not self.current and self.catcher:
                self.catcher.set_visible(False)
            self.external = None
            return
        self.close()
        self.external = prog
        self._show_catcher()

    def close(self):
        if self.external:
            subprocess.run(["pkill", "-x", self.external])
            self.external = None
        if self.current:
            p = self.panels[self.current]
            p.closed()
            p.set_visible(False)
            self.current = None
        if self.catcher:
            self.catcher.set_visible(False)
