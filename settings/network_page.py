"""Settings → Network: everyday NetworkManager jobs without nm-connection-editor.

Sections: Airplane mode · Wi-Fi (current network with password/details, nearby, saved) · Ethernet
(every wired adapter, incl. USB tethering: connect/disconnect, speed, IP details) · Hotspot (share
this computer's connection: name, password, band, connected devices) · VPN & other connections
(WireGuard/OpenVPN/mobile/Bluetooth: on/off, import, remove). Every connection has an Advanced page,
opened inside this window: name, priority, metered, IPv4/IPv6 (automatic, manual addresses, gateway, DNS),
MTU and MAC address.
All NetworkManager calls go through nmcli in a worker thread so the window never stutters.
"""
import secrets
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

SIGNAL_ICONS = ["network-wireless-signal-none-symbolic", "network-wireless-signal-weak-symbolic",
                "network-wireless-signal-ok-symbolic", "network-wireless-signal-good-symbolic",
                "network-wireless-signal-excellent-symbolic"]
OTHER_TYPES = {"vpn": "VPN", "wireguard": "WireGuard", "gsm": "Mobile broadband", "cdma": "Mobile broadband",
               "bluetooth": "Bluetooth tethering", "pppoe": "DSL (PPPoE)"}
BANDS = [("", "Automatic"), ("bg", "2.4 GHz"), ("a", "5 GHz")]
OPENVPN = Path("/usr/lib/NetworkManager/VPN/nm-openvpn-service.name")
IP4_METHODS = [("auto", "Automatic (DHCP)"), ("manual", "Manual"), ("link-local", "Link-local only"),
               ("shared", "Shared to other computers"), ("disabled", "Disabled")]
IP6_METHODS = [("auto", "Automatic"), ("dhcp", "Automatic, DHCP only"), ("manual", "Manual"),
               ("link-local", "Link-local only"), ("shared", "Shared to other computers"),
               ("ignore", "Ignore"), ("disabled", "Disabled")]
METERED = [("unknown", "Detect automatically"), ("yes", "Yes (limit background data)"), ("no", "No")]
MACS = [("", "Default"), ("permanent", "Permanent (the adapter's own)"), ("stable", "Stable (per network)"),
        ("random", "Random (each connection)"), ("preserve", "Keep current")]


def nmcli(*args, timeout=15):
    """(ok, stdout, stderr)."""
    try:
        r = subprocess.run(["nmcli", *args], capture_output=True, text=True, timeout=timeout)
        return r.returncode == 0, r.stdout.strip(), r.stderr.strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, "", str(e)


def split_terse(line):
    """nmcli -t fields: ':' separates, '\\:' and '\\\\' are escapes."""
    out, cur, esc = [], [], False
    for ch in line:
        if esc:
            cur.append(ch)
            esc = False
        elif ch == "\\":
            esc = True
        elif ch == ":":
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return out


def ago(ts):
    if not ts:
        return "never used"
    d = time.time() - ts
    for unit, sec in (("year", 31536000), ("month", 2592000), ("day", 86400), ("hour", 3600), ("minute", 60)):
        if d >= sec:
            n = int(d // sec)
            return f"{n} {unit}{'s' if n > 1 else ''} ago"
    return "just now"


def band(mhz):
    return "6 GHz" if mhz >= 5925 else "5 GHz" if mhz >= 4900 else "2.4 GHz" if mhz else ""


def err_text(err):
    for line in err.splitlines():
        if line.startswith("Error:"):
            return line[6:].strip().rstrip(".")
    return err.splitlines()[-1] if err else "failed"


def details(dev):
    """{'IP address': ..., 'Gateway': ..., 'DNS': ..., 'IPv6': ..., 'MAC address': ...} of a device."""
    d = {}
    for line in nmcli("-t", "-f", "GENERAL.HWADDR,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS,IP6.ADDRESS",
                      "device", "show", dev)[1].splitlines():
        key, _, val = line.partition(":")
        key = key.split("[")[0]
        name = {"IP4.ADDRESS": "IP address", "IP4.GATEWAY": "Gateway", "IP4.DNS": "DNS",
                "IP6.ADDRESS": "IPv6", "GENERAL.HWADDR": "MAC address"}.get(key)
        if not name or not val or val == "--" or (name == "IPv6" and "IPv6" in d):
            continue
        d[name] = f"{d[name]}, {val}" if name in d else val
    return {k: d[k] for k in ("IP address", "Gateway", "DNS", "IPv6", "MAC address") if k in d}


def hotspot_clients(dev):
    """[(name, ip)] of devices on the hotspot: dnsmasq leases, else the neighbour table."""
    out = []
    try:
        for line in Path(f"/var/lib/NetworkManager/dnsmasq-{dev}.leases").read_text().splitlines():
            f = line.split()
            if len(f) >= 4:
                out.append((f[3] if f[3] != "*" else f[1], f[2]))
    except OSError:
        try:
            r = subprocess.run(["ip", "-4", "neigh", "show", "dev", dev], capture_output=True, text=True, timeout=3)
            for line in r.stdout.splitlines():
                f = line.split()
                if f and "lladdr" in f and f[-1] in ("REACHABLE", "STALE", "DELAY"):
                    out.append((f[f.index("lladdr") + 1], f[0]))
        except (OSError, subprocess.TimeoutExpired):
            pass
    return out


def copy_button(win, text, what):
    b = Gtk.Button(icon_name="edit-copy-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                   tooltip_text=f"Copy {what.lower()}")
    b.connect("clicked", lambda *_: (b.get_clipboard().set(text), win.toast(f"{what} copied")))
    return b


class NetworkPage:
    def __init__(self, win):
        self.win = win
        self.page = Adw.PreferencesPage()
        # Advanced pages slide in over this one, inside the Settings window (back arrow / swipe returns)
        self.nav = Adw.NavigationView()
        self.nav.add(Adw.NavigationPage(title="Network", tag="main", child=self.page))
        self._busy = self._again = False
        self._groups = []
        self._hs_draft = {"ssid": socket.gethostname(), "psk": secrets.token_urlsafe(9)[:12], "band": ""}
        self._loading = Adw.PreferencesGroup()
        self._loading.add(Adw.ActionRow(title="Loading networks…"))
        self.page.add(self._loading)
        self.page.connect("map", lambda *_: self.reload())

    # ---- plumbing
    def _bg(self, work, done):
        def t():
            r = work()
            GLib.idle_add(lambda: done(r) or False)
        threading.Thread(target=t, daemon=True).start()

    def _act(self, msg, work, ok_msg=None):
        """Run an nmcli action, toast the outcome, reload."""
        self.win.toast(msg)

        def done(r):
            ok, _out, err = r
            self.win.toast(ok_msg if ok and ok_msg else ("Done" if ok else f"Failed: {err_text(err)}"))
            GLib.timeout_add(600, lambda: (self.reload(), False)[1])
        self._bg(work, done)

    # ---- data
    def _fetch(self, rescan):
        d = {"radio": nmcli("radio", "wifi")[1] == "enabled", "airplane": False, "wdev": None, "active": None,
             "saved": [], "nearby": [], "eth": [], "other": [], "hotspot": None, "ap": False, "five": False}
        d["airplane"] = not d["radio"] and nmcli("radio", "wwan")[1] != "enabled"
        devs = []
        for line in nmcli("-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device")[1].splitlines():
            f = split_terse(line)
            if len(f) >= 4:
                devs.append({"dev": f[0], "type": f[1], "state": f[2], "conn": "" if f[3] == "--" else f[3]})
        wdev = next((x for x in devs if x["type"] == "wifi"), None)
        d["wdev"] = wdev and wdev["dev"]
        if d["wdev"]:
            props = nmcli("-t", "-f", "WIFI-PROPERTIES.AP,WIFI-PROPERTIES.5GHZ", "device", "show", d["wdev"])[1]
            d["ap"] = "AP:yes" in props
            d["five"] = "5GHZ:yes" in props

        conns, wifi = [], []
        for line in nmcli("-t", "-f", "NAME,UUID,TYPE,TIMESTAMP,AUTOCONNECT,ACTIVE,DEVICE",
                          "connection", "show")[1].splitlines():
            f = split_terse(line)
            if len(f) >= 7:
                c = {"name": f[0], "uuid": f[1], "type": f[2], "ts": int(f[3] or 0), "auto": f[4] == "yes",
                     "active": f[5] == "yes", "dev": "" if f[6] == "--" else f[6]}
                conns.append(c)
                if c["type"] == "802-11-wireless":
                    wifi.append(c)
        # one call for every Wi-Fi profile's mode: access-point ones are hotspots, not networks to join
        if wifi:
            blocks = nmcli("-t", "-g", "connection.uuid,802-11-wireless.mode,802-11-wireless.ssid,802-11-wireless.band",
                           "connection", "show", *[c["uuid"] for c in wifi])[1].split("\n\n")
            info = {}
            for b in blocks:
                ln = b.split("\n")
                if ln and ln[0]:
                    info[ln[0]] = (ln + ["", "", ""])[1:4]
            for c in wifi:
                c["mode"], c["ssid"], c["band"] = info.get(c["uuid"], ("", c["name"], ""))
        hotspots = [c for c in wifi if c.get("mode") == "ap"]
        if hotspots:
            hs = next((c for c in hotspots if c["active"]), max(hotspots, key=lambda c: c["ts"]))
            hs["psk"] = nmcli("-s", "-g", "802-11-wireless-security.psk", "connection", "show", hs["uuid"])[1]
            if hs["active"]:
                hs["clients"] = hotspot_clients(hs["dev"] or d["wdev"])
            d["hotspot"] = hs
        for c in wifi:
            if c.get("mode") != "ap":
                d["saved"].append(c)
                if c["active"]:
                    d["active"] = c
        d["saved"].sort(key=lambda c: -c["ts"])

        if d["radio"] and d["wdev"] and not (d["hotspot"] and d["hotspot"]["active"]):
            seen = {}
            for line in nmcli("-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY,FREQ", "device", "wifi", "list",
                              "--rescan", "yes" if rescan else "auto", timeout=25)[1].splitlines():
                f = split_terse(line)
                if len(f) < 5 or not f[1]:
                    continue
                mhz = int(f[4].split()[0]) if f[4].split() and f[4].split()[0].isdigit() else 0
                n = {"ssid": f[1], "signal": int(f[2] or 0), "sec": f[3].strip(), "band": band(mhz), "inuse": f[0] == "*"}
                if n["ssid"] not in seen or n["signal"] > seen[n["ssid"]]["signal"]:
                    seen[n["ssid"]] = n
            d["nearby"] = sorted(seen.values(), key=lambda n: -n["signal"])
        if d["active"]:
            a = d["active"]
            a["psk"] = nmcli("-s", "-g", "802-11-wireless-security.psk", "connection", "show", a["uuid"])[1]
            a["here"] = next((n for n in d["nearby"] if n["ssid"] == a.get("ssid", a["name"]) or n["inuse"]), None)
            a["details"] = details(a["dev"] or d["wdev"])

        # wired adapters (USB tethering from a phone shows up here too)
        for x in devs:
            if x["type"] != "ethernet":
                continue
            try:
                speed = int(Path("/sys/class/net", x["dev"], "speed").read_text())
            except (OSError, ValueError):
                speed = -1
            x["speed"] = speed if speed > 0 else 0
            x["usb"] = "usb" in str(Path("/sys/class/net", x["dev"], "device").resolve())
            x["profile"] = next((c for c in conns if c["active"] and c["dev"] == x["dev"]), None)
            x["details"] = details(x["dev"]) if x["state"] == "connected" else {}
            d["eth"].append(x)

        d["other"] = sorted((c for c in conns if c["type"] in OTHER_TYPES), key=lambda c: (not c["active"], c["name"].lower()))
        return d

    def reload(self, rescan=False):
        if self._busy:
            self._again = True
            return
        self._busy = True

        def done(d):
            self._busy = False
            self._render(d)
            if self._again:
                self._again = False
                self.reload()
        self._bg(lambda: self._fetch(rescan), done)

    # ---- UI helpers
    def _clear(self):
        for g in self._groups + [self._loading]:
            if g.get_parent():
                self.page.remove(g)
        self._groups = []

    def _group(self, title="", desc=""):
        g = Adw.PreferencesGroup(title=title, description=desc)
        self.page.add(g)
        self._groups.append(g)
        return g

    def _button(self, label, cb, css="flat"):
        b = Gtk.Button(label=label, valign=Gtk.Align.CENTER, css_classes=[css])
        b.connect("clicked", lambda *_: cb())
        return b

    def _details_rows(self, parent, info):
        """Read-only rows (IP, gateway, DNS, MAC) with copy buttons, added to an ExpanderRow or group."""
        add = parent.add_row if isinstance(parent, Adw.ExpanderRow) else parent.add
        for k, v in info.items():
            r = Adw.ActionRow(title=k, subtitle=GLib.markup_escape_text(v), subtitle_selectable=True,
                              css_classes=["property"])
            r.add_suffix(copy_button(self.win, v, k))
            add(r)

    # ---- render
    def _render(self, d):
        self._clear()
        g = self._group()
        air = Adw.SwitchRow(title="Airplane mode", subtitle="Turn off Wi-Fi and mobile radios", active=d["airplane"])
        air.connect("notify::active", lambda r, _p: self._act(
            "Airplane mode " + ("on…" if r.get_active() else "off…"),
            lambda: nmcli("radio", "all", "off" if r.get_active() else "on")))
        g.add(air)
        refresh = Gtk.Button(icon_name="view-refresh-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                             tooltip_text="Refresh")
        refresh.connect("clicked", lambda *_: self.reload())
        g.set_header_suffix(refresh)

        self._render_wifi(d)
        self._render_ethernet(d)
        if d["wdev"]:
            self._render_hotspot(d)
        self._render_other(d)


    def _render_wifi(self, d):
        g = self._group("Wi-Fi")
        radio = Adw.SwitchRow(title="Wi-Fi", subtitle=d["wdev"] or "No Wi-Fi adapter", active=d["radio"],
                              sensitive=bool(d["wdev"]))
        radio.connect("notify::active", lambda r, _p: self._act(
            "Turning Wi-Fi " + ("on…" if r.get_active() else "off…"),
            lambda: nmcli("radio", "wifi", "on" if r.get_active() else "off")))
        g.add(radio)
        if not d["radio"] or not d["wdev"]:
            return
        hs = d["hotspot"]
        if hs and hs["active"]:
            g.add(Adw.ActionRow(title="Wi-Fi is sharing a hotspot", subtitle="Turn the hotspot off to join networks",
                                css_classes=["dim-label"]))
            return

        a = d["active"]
        if a:
            here = a.get("here") or {}
            sub = " · ".join(x for x in (f"{here['signal']}%" if here else "", here.get("band", ""),
                                         here.get("sec", "")) if x)
            row = Adw.ExpanderRow(title=GLib.markup_escape_text(a["name"]), subtitle=sub or "Connected")
            row.add_prefix(Gtk.Image(icon_name=SIGNAL_ICONS[min(4, here.get("signal", 0) // 20)]))
            row.add_suffix(self._button("Disconnect", lambda: self._act(
                "Disconnecting…", lambda: nmcli("connection", "down", a["uuid"]))))
            if a.get("psk"):
                row.add_row(self._password_row(a))
            self._details_rows(row, a.get("details", {}))
            row.add_row(self._auto_row(a))
            row.add_row(self._advanced_row(a))
            row.add_row(self._forget_row(a))
            g.add(row)

        known = {c.get("ssid") or c["name"]: c for c in d["saved"]}
        g = self._group("Nearby networks")
        scan = Gtk.Button(icon_name="view-refresh-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                          tooltip_text="Scan again")
        scan.connect("clicked", lambda *_: (self.win.toast("Scanning…"), self.reload(rescan=True)))
        g.set_header_suffix(scan)
        nearby = [n for n in d["nearby"] if not (a and n["ssid"] == a.get("ssid", a["name"]))]
        if not nearby:
            g.add(Adw.ActionRow(title="No other networks found", css_classes=["dim-label"]))
        for n in nearby[:12]:
            g.add(self._nearby_row(n, known.get(n["ssid"])))
        g.add(self._hidden_row())

        in_range = {n["ssid"] for n in d["nearby"]}
        saved = [c for c in d["saved"] if not c["active"] and (c.get("ssid") or c["name"]) not in in_range]
        if saved:
            g = self._group("Saved networks", "Not in range right now")
            for c in saved:
                g.add(self._saved_row(c))

    def _render_ethernet(self, d):
        g = self._group("Ethernet")
        if not d["eth"]:
            g.add(Adw.ActionRow(title="No wired adapter", subtitle="Plug in a cable, a USB adapter or a phone with USB tethering",
                                css_classes=["dim-label"]))
            return
        for x in d["eth"]:
            st = x["state"]
            if st == "connected":
                sub = " · ".join(s for s in ("Connected", f"{x['speed']} Mb/s" if x["speed"] else "",
                                             x["profile"]["name"] if x["profile"] else "") if s)
            elif st == "unavailable":
                sub = "Cable unplugged"
            elif st.startswith("connecting"):
                sub = "Connecting…"
            elif st == "unmanaged":
                sub = "Not managed by NetworkManager"
            else:
                sub = "Disconnected"
            title = ("USB tethering" if x["usb"] else "Ethernet") + f" ({x['dev']})"
            row = Adw.ExpanderRow(title=title, subtitle=sub, enable_expansion=st == "connected")
            row.add_prefix(Gtk.Image(icon_name="network-wired-symbolic"))
            if st == "connected" or st.startswith("connecting"):
                row.add_suffix(self._button("Disconnect", lambda dev=x["dev"]: self._act(
                    "Disconnecting…", lambda: nmcli("device", "disconnect", dev))))
            elif st == "disconnected":
                row.add_suffix(self._button("Connect", lambda dev=x["dev"]: self._act(
                    "Connecting…", lambda: nmcli("device", "connect", dev, timeout=40), "Connected")))
            self._details_rows(row, x["details"])
            if x["profile"]:
                row.add_row(self._auto_row(x["profile"]))
                row.add_row(self._advanced_row(x["profile"]))
            g.add(row)

    def _render_hotspot(self, d):
        hs = d["hotspot"]
        on = bool(hs and hs["active"])
        if not d["ap"]:
            g = self._group("Hotspot")
            g.add(Adw.ActionRow(title="Hotspot not supported", subtitle="This Wi-Fi adapter can't act as an access point",
                                css_classes=["dim-label"]))
            return
        g = self._group("Hotspot", "Share this computer's internet over Wi-Fi."
                        + ("" if on or not d["active"] else " Your Wi-Fi network disconnects while it's on."))
        cur = {"ssid": hs.get("ssid") or hs["name"], "psk": hs.get("psk", ""), "band": hs.get("band", "")} if hs else self._hs_draft

        sw = Adw.SwitchRow(title="Hotspot", subtitle=f"Sharing as “{GLib.markup_escape_text(cur['ssid'])}”" if on else "Off",
                           active=on)

        def toggle(r, _p):
            if r.get_active():
                self._act("Starting hotspot…", lambda: self._hotspot_up(d, hs, dict(cur)), "Hotspot on")
            else:
                self._act("Stopping hotspot…", lambda: nmcli("connection", "down", hs["uuid"]), "Hotspot off")
        sw.connect("notify::active", toggle)
        g.add(sw)

        name = Adw.EntryRow(title="Network name", text=cur["ssid"], show_apply_button=True)
        pw = Adw.PasswordEntryRow(title="Password", text=cur["psk"], show_apply_button=True)
        pw.add_suffix(copy_button(self.win, cur["psk"], "Password"))

        def save(key, value):
            if key == "ssid" and not 1 <= len(value.encode()) <= 32:
                self.win.toast("Name must be 1 to 32 characters")
                return
            if key == "psk" and not 8 <= len(value) <= 63:
                self.win.toast("Password must be 8 to 63 characters")
                return
            cur[key] = value
            if not hs:              # nothing saved yet: used the first time the hotspot starts
                self._hs_draft[key] = value
                self.win.toast("Saved")
                return
            prop = {"ssid": "802-11-wireless.ssid", "psk": "802-11-wireless-security.psk", "band": "802-11-wireless.band"}[key]

            def work():
                res = nmcli("connection", "modify", hs["uuid"], prop, value)
                if res[0] and hs["active"]:
                    res = nmcli("connection", "up", hs["uuid"], timeout=40)
                return res
            self._act("Saving…" + (" the hotspot restarts" if hs["active"] else ""), work, "Saved")
        if hs:
            gear = Gtk.Button(icon_name="emblem-system-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                              tooltip_text="Advanced (address range, MTU…)")
            gear.connect("clicked", lambda *_: self.open_advanced(hs))
            g.set_header_suffix(gear)
        name.connect("apply", lambda r: save("ssid", r.get_text().strip()))
        pw.connect("apply", lambda r: save("psk", r.get_text()))
        g.add(name)
        g.add(pw)

        if d["five"]:
            bands = Adw.ComboRow(title="Band", subtitle="5 GHz is faster, 2.4 GHz reaches further and suits older devices",
                                 model=Gtk.StringList.new([b[1] for b in BANDS]))
            bands.set_selected(next((i for i, b in enumerate(BANDS) if b[0] == cur["band"]), 0))
            bands.connect("notify::selected", lambda r, _p: save("band", BANDS[r.get_selected()][0]))
            g.add(bands)

        if on:
            clients = hs.get("clients", [])
            cl = Adw.ExpanderRow(title="Connected devices", subtitle=str(len(clients)) if clients else "None yet",
                                 enable_expansion=bool(clients))
            for host, ip in clients:
                cl.add_row(Adw.ActionRow(title=GLib.markup_escape_text(host), subtitle=ip, subtitle_selectable=True))
            g.add(cl)

    def _hotspot_up(self, d, hs, cur):
        if not d["radio"]:
            nmcli("radio", "wifi", "on")
            time.sleep(1.5)
        if hs:
            return nmcli("connection", "up", hs["uuid"], timeout=40)
        args = ["device", "wifi", "hotspot", "ifname", d["wdev"], "con-name", "Hotspot",
                "ssid", cur["ssid"], "password", cur["psk"]]
        if cur.get("band"):
            args += ["band", cur["band"]]
        return nmcli(*args, timeout=40)

    def _render_other(self, d):
        g = self._group("VPN &amp; other connections")
        imp = Gtk.Button(icon_name="list-add-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                         tooltip_text="Import a WireGuard (.conf)" + (" or OpenVPN (.ovpn)" if OPENVPN.exists() else "") + " file")
        imp.connect("clicked", lambda *_: self._import())
        g.set_header_suffix(imp)
        if not d["other"]:
            g.add(Adw.ActionRow(title="None set up", subtitle="Add one with + (WireGuard config"
                                + (" or OpenVPN profile)" if OPENVPN.exists() else ")"), css_classes=["dim-label"]))
        for c in d["other"]:
            row = Adw.SwitchRow(title=GLib.markup_escape_text(c["name"]), active=c["active"],
                                subtitle=OTHER_TYPES[c["type"]] + (f" · {c['dev']}" if c["active"] and c["dev"] else ""))
            row.connect("notify::active", lambda r, _p, c=c: self._act(
                ("Connecting " if r.get_active() else "Disconnecting ") + c["name"] + "…",
                lambda: nmcli("connection", "up" if r.get_active() else "down", c["uuid"], timeout=60)))
            gear = Gtk.Button(icon_name="emblem-system-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                              tooltip_text="Advanced")
            gear.connect("clicked", lambda *_, c=c: self.open_advanced(c))
            row.add_suffix(gear)
            rm = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                            tooltip_text="Remove")
            rm.connect("clicked", lambda *_, c=c: self._confirm_forget(c, "Remove", "Its settings and keys are deleted."))
            row.add_suffix(rm)
            g.add(row)

    def _import(self):
        f = Gtk.FileFilter(name="VPN configuration")
        for pat in ("*.conf", "*.ovpn") if OPENVPN.exists() else ("*.conf",):
            f.add_pattern(pat)
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(f)
        dlg = Gtk.FileDialog(title="Import VPN", filters=filters)

        def picked(_d, res):
            try:
                path = dlg.open_finish(res).get_path()
            except GLib.Error:
                return
            kind = "openvpn" if path.endswith(".ovpn") else "wireguard"
            self._act("Importing…", lambda: nmcli("connection", "import", "type", kind, "file", path), "Imported")
        dlg.open(self.win, None, picked)

    # ---- Wi-Fi rows
    def _password_row(self, c):
        """Saved password: hidden (peek icon shows it), copy button, Enter / ✓ saves a new one."""
        row = Adw.PasswordEntryRow(title="Password", show_apply_button=True)
        row.set_text(c["psk"])
        copy = Gtk.Button(icon_name="edit-copy-symbolic", valign=Gtk.Align.CENTER, css_classes=["flat"],
                          tooltip_text="Copy password")

        def do_copy(*_):
            row.get_clipboard().set(c["psk"])
            self.win.toast("Password copied")
        copy.connect("clicked", do_copy)
        row.add_suffix(copy)

        def apply(r):
            pw = r.get_text()
            if len(pw) < 8:
                self.win.toast("Wi-Fi passwords are at least 8 characters")
                return

            def work():
                res = nmcli("connection", "modify", c["uuid"], "802-11-wireless-security.psk", pw)
                if res[0] and c.get("active"):          # reconnect so the new password is used now
                    res = nmcli("connection", "up", c["uuid"], timeout=40)
                return res
            c["psk"] = pw
            self._act("Saving password…", work, "Password saved")
        row.connect("apply", apply)
        return row

    def _auto_row(self, c):
        r = Adw.SwitchRow(title="Connect automatically", active=c["auto"])
        r.connect("notify::active", lambda s, _p: self._bg(
            lambda: nmcli("connection", "modify", c["uuid"], "connection.autoconnect", "yes" if s.get_active() else "no"),
            lambda res: res[0] or self.win.toast(f"Failed: {err_text(res[2])}")))
        return r

    def _forget_row(self, c):
        r = Adw.ButtonRow(title="Forget this network", css_classes=["destructive-action"])
        r.connect("activated", lambda *_: self._confirm_forget(c))
        return r

    def _confirm_forget(self, c, verb="Forget", body="Its saved password is deleted. You'll need it again to reconnect."):
        dlg = Adw.AlertDialog(heading=f"{verb} {c['name']}?", body=body)
        dlg.add_response("cancel", "Cancel")
        dlg.add_response("forget", verb)
        dlg.set_response_appearance("forget", Adw.ResponseAppearance.DESTRUCTIVE)
        dlg.connect("response", lambda _d, resp: resp == "forget" and self._act(
            f"{verb.rstrip('e')}ing {c['name']}…", lambda: nmcli("connection", "delete", c["uuid"]), "Done"))
        dlg.present(self.win)

    def _connect_row(self, title, sub, icon, connect):
        """Unknown secured network: expand to type the password; connect(password) runs in the worker."""
        row = Adw.ExpanderRow(title=title, subtitle=sub)
        if icon:
            row.add_prefix(icon)
        pw = Adw.PasswordEntryRow(title="Password", show_apply_button=True)

        def go(r):
            if r.get_text():
                connect(r.get_text())
        pw.connect("apply", go)
        pw.connect("entry-activated", go)
        row.add_row(pw)
        return row

    def _nearby_row(self, n, saved):
        title = GLib.markup_escape_text(n["ssid"])
        secured = bool(n["sec"]) and n["sec"] != "--"
        sub = " · ".join(x for x in (n["band"], n["sec"] if secured else "Open", "Saved" if saved else "") if x)
        icon = Gtk.Image(icon_name=SIGNAL_ICONS[min(4, n["signal"] // 20)])
        ok = f"Connected to {n['ssid']}"
        if saved or not secured:
            row = Adw.ActionRow(title=title, subtitle=sub, activatable=True)
            row.add_prefix(icon)
            row.add_suffix(Gtk.Image(icon_name="go-next-symbolic", css_classes=["dim-label"]))
            args = ("connection", "up", saved["uuid"]) if saved else ("device", "wifi", "connect", n["ssid"])
            row.connect("activated", lambda *_: self._act(f"Connecting to {n['ssid']}…",
                                                          lambda: nmcli(*args, timeout=40), ok))
            return row
        return self._connect_row(title, sub, icon, lambda p: self._act(
            f"Connecting to {n['ssid']}…", lambda: nmcli("device", "wifi", "connect", n["ssid"], "password", p, timeout=40), ok))

    def _hidden_row(self):
        row = Adw.ExpanderRow(title="Hidden network…", subtitle="Join a network that doesn't broadcast its name")
        name = Adw.EntryRow(title="Network name")
        pw = Adw.PasswordEntryRow(title="Password (empty if open)", show_apply_button=True)

        def go(*_):
            ssid = name.get_text().strip()
            if not ssid:
                self.win.toast("Type the network name")
                return
            args = ["device", "wifi", "connect", ssid, "hidden", "yes"]
            if pw.get_text():
                args += ["password", pw.get_text()]
            self._act(f"Connecting to {ssid}…", lambda: nmcli(*args, timeout=40), f"Connected to {ssid}")
        pw.connect("apply", go)
        pw.connect("entry-activated", go)
        row.add_row(name)
        row.add_row(pw)
        return row

    def _saved_row(self, c):
        row = Adw.ExpanderRow(title=GLib.markup_escape_text(c["name"]), subtitle=ago(c["ts"]))
        loaded = {"done": False}

        def on_expand(r, _p):
            if not r.get_expanded() or loaded["done"]:
                return
            loaded["done"] = True

            def got(res):
                c["psk"] = res[1]
                if c["psk"]:
                    row.add_row(self._password_row(c))
                row.add_row(self._auto_row(c))
                row.add_row(self._advanced_row(c))
                row.add_row(self._forget_row(c))
            self._bg(lambda: nmcli("-s", "-g", "802-11-wireless-security.psk", "connection", "show", c["uuid"]), got)
        row.connect("notify::expanded", on_expand)
        return row


    # ---- Advanced (in-window editor for one connection profile)
    def _advanced_row(self, c):
        r = Adw.ActionRow(title="Advanced", subtitle="IP address, DNS, MTU, MAC, metered…", activatable=True)
        r.add_suffix(Gtk.Image(icon_name="go-next-symbolic", css_classes=["dim-label"]))
        r.connect("activated", lambda *_: self.open_advanced(c))
        return r

    def open_advanced(self, c):
        self._bg(lambda: self._load_props(c), lambda props: self._push_advanced(c, props))

    @staticmethod
    def _load_props(c):
        kind = {"802-11-wireless": "802-11-wireless", "802-3-ethernet": "802-3-ethernet"}.get(c["type"])
        fields = ["connection.id", "connection.autoconnect-priority", "connection.metered",
                  "ipv4.method", "ipv4.addresses", "ipv4.gateway", "ipv4.dns", "ipv4.ignore-auto-dns",
                  "ipv6.method", "ipv6.addresses", "ipv6.gateway", "ipv6.dns", "ipv6.ignore-auto-dns"]
        if kind:
            fields += [f"{kind}.mtu", f"{kind}.cloned-mac-address"]
        if kind == "802-11-wireless":
            fields.append("802-11-wireless.hidden")
        ok, out, err = nmcli("-t", "-f", ",".join(fields), "connection", "show", c["uuid"])
        if not ok:
            return {"_error": err_text(err)}
        props = {"_kind": kind}
        for line in out.splitlines():
            key, _, val = line.partition(":")
            props[key] = val.replace("\\:", ":").replace("\\\\", "\\")
        return props

    def _push_advanced(self, c, props):
        if "_error" in props:
            self.win.toast(f"Can't read {c['name']}: {props['_error']}")
            return
        kind = props["_kind"]
        new = {}                       # property -> value, only what the user changed
        page = Adw.PreferencesPage()

        def track(prop):
            return lambda v: new.__setitem__(prop, v) if v != props.get(prop, "") else new.pop(prop, None)

        def entry(group, title, prop, hint=""):
            e = Adw.EntryRow(title=title, text=props.get(prop, ""))
            if hint:
                e.set_tooltip_text(hint)
            e.connect("changed", lambda w: track(prop)(w.get_text().strip()))
            group.add(e)
            return e

        def combo(group, title, prop, options, sub=""):
            cur = props.get(prop, "")
            opts = list(options)
            if cur not in [o[0] for o in opts]:        # an explicit value (e.g. a MAC address) we don't list
                opts.append((cur, cur))
            row = Adw.ComboRow(title=title, subtitle=sub, model=Gtk.StringList.new([o[1] for o in opts]))
            row.set_selected([o[0] for o in opts].index(cur))
            row.connect("notify::selected", lambda w, _p: track(prop)(opts[w.get_selected()][0]))
            group.add(row)
            return row

        def switch(group, title, prop, invert=False, sub=""):
            on = (props.get(prop) == "yes") != invert
            row = Adw.SwitchRow(title=title, subtitle=sub, active=on)
            row.connect("notify::active", lambda w, _p: track(prop)("yes" if w.get_active() != invert else "no"))
            group.add(row)

        g = Adw.PreferencesGroup(title="General")
        page.add(g)
        entry(g, "Name", "connection.id")
        prio = Adw.SpinRow.new_with_range(-999, 999, 1)
        prio.set_title("Priority")
        prio.set_subtitle("Higher connects first when several are available")
        prio.set_value(int(props.get("connection.autoconnect-priority") or 0))
        prio.connect("notify::value", lambda w, _p: track("connection.autoconnect-priority")(str(int(w.get_value()))))
        g.add(prio)
        combo(g, "Metered", "connection.metered", METERED, "Metered connections avoid big background downloads")
        if kind == "802-11-wireless":
            switch(g, "Hidden network", "802-11-wireless.hidden", sub="Connect even when it doesn't broadcast its name")

        for ver, methods in (("4", IP4_METHODS), ("6", IP6_METHODS)):
            g = Adw.PreferencesGroup(title=f"IPv{ver}")
            page.add(g)
            m = combo(g, "Method", f"ipv{ver}.method", methods)
            ex = "192.168.1.50/24" if ver == "4" else "2001:db8::50/64"
            rows = [entry(g, "Addresses", f"ipv{ver}.addresses", f"Comma separated, with prefix: {ex}"),
                    entry(g, "Gateway", f"ipv{ver}.gateway")]
            dns = entry(g, "DNS servers", f"ipv{ver}.dns",
                        "Comma separated, e.g. 1.1.1.1, 9.9.9.9" if ver == "4" else "Comma separated")
            switch(g, "Automatic DNS", f"ipv{ver}.ignore-auto-dns", invert=True,
                   sub="Off: use only the DNS servers above")

            def sync(_w=None, _p=None, m=m, rows=rows, dns=dns, methods=methods):
                manual = methods[m.get_selected()][0] == "manual" if m.get_selected() < len(methods) else True
                off = methods[m.get_selected()][0] in ("disabled", "ignore") if m.get_selected() < len(methods) else False
                for w in rows:
                    w.set_sensitive(manual)
                dns.set_sensitive(not off)
            m.connect("notify::selected", sync)
            sync()

        if kind:
            g = Adw.PreferencesGroup(title="Hardware")
            page.add(g)
            entry(g, "MTU", f"{kind}.mtu", "auto, or bytes (e.g. 1500)")
            combo(g, "MAC address", f"{kind}.cloned-mac-address", MACS,
                  "Random or stable addresses make this computer harder to track")

        g = Adw.PreferencesGroup()
        page.add(g)
        save = Adw.ButtonRow(title="Save", css_classes=["suggested-action"])

        def do_save(*_):
            if not new:
                self.nav.pop()
                return
            args = []
            for k, v in new.items():
                if k.endswith(".mtu") and v not in ("", "auto") and not v.isdigit():
                    self.win.toast("MTU must be a number or auto")
                    return
                args += [k, "0" if k.endswith(".mtu") and v in ("", "auto") else v]   # 0 = automatic

            def work():
                res = nmcli("connection", "modify", c["uuid"], *args)
                if res[0] and c.get("active"):          # apply now
                    res = nmcli("connection", "up", c["uuid"], timeout=40)
                return res

            def done(res):
                if res[0]:
                    self.win.toast("Saved" + (" and reconnected" if c.get("active") else ""))
                    self.nav.pop()
                else:
                    self.win.toast(f"Not saved: {err_text(res[2])}", )
            self.win.toast("Saving…")
            self._bg(work, done)
        save.connect("activated", do_save)
        g.add(save)

        view = Adw.ToolbarView(content=page)
        view.add_top_bar(Adw.HeaderBar(show_end_title_buttons=False, show_start_title_buttons=False))
        self.nav.push(Adw.NavigationPage(title=c["name"], child=view))
        main = sys.modules.get("__main__")
        if hasattr(main, "pointerize"):          # hand cursor on clickable rows, like the other pages
            main.pointerize(view)


def build(win):
    return NetworkPage(win).nav
