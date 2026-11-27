"""NetworkManager helpers without GTK: nmcli calls, Wi-Fi/hotspot state, hotspot on/off.

Shared by Settings → Network (network_page.py) and the C network
popup (helpers/panels.c), which runs this file as a small CLI:
  python3 -I settings/nm.py hotspot-status   key=value lines (supported, on, ssid, clients, together, station)
  python3 -I settings/nm.py hotspot on|off     exit 0 on success, the reason on stderr otherwise
"""
import re
import os
import secrets
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

SIGNAL_ICONS = ["network-wireless-signal-none-symbolic", "network-wireless-signal-weak-symbolic",
                "network-wireless-signal-ok-symbolic", "network-wireless-signal-good-symbolic",
                "network-wireless-signal-excellent-symbolic"]
OTHER_TYPES = {"vpn": "VPN", "wireguard": "WireGuard", "gsm": "Mobile broadband", "cdma": "Mobile broadband",
               "bluetooth": "Bluetooth tethering", "pppoe": "DSL (PPPoE)"}
SAVED_SHOWN = 5                               # "Saved networks" rows before "Show all"
BANDS = [("", "Automatic"), ("bg", "2.4 GHz"), ("a", "5 GHz")]
OPENVPN = Path("/usr/lib/NetworkManager/VPN/nm-openvpn-service.name")
# hotspot next to the Wi-Fi connection: hostapd on a second, access-point interface, run by this root helper
# (scripts/hotspot-iface, installed by install.sh); its state lives in HELPER_RUN
AP_IF = "ap0"
IFACE_HELPER = "/usr/local/lib/hyprminimal/hotspot-iface"
HELPER_RUN = Path("/run/hyprminimal-hotspot")
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


_concurrent = {}


def sta_ap_concurrent(dev):
    """Can this adapter stay on a Wi-Fi network and run an access point at the same time?
    From `iw phy … info`: a combination allowing managed and AP together, e.g.
    `#{ managed } <= 1, #{ AP } <= 1, total <= 2, #channels <= 1` (then both share one channel)."""
    if dev in _concurrent:
        return _concurrent[dev]
    ok = False
    try:
        phy = Path("/sys/class/net", dev, "phy80211").resolve().name
        out = subprocess.run(["iw", "phy", phy, "info"], capture_output=True, text=True, timeout=3).stdout
        text = out.split("valid interface combinations:", 1)[1]
        for combo in re.findall(r"\*\s*(#\{.*?total <= \d+)", text, re.S):
            groups = [([n.strip() for n in g.split(",")], int(lim))
                      for g, lim in re.findall(r"#\{([^}]*)\}\s*<=\s*(\d+)", combo)]
            total = int(re.search(r"total <= (\d+)", combo).group(1))
            sta = any("managed" in names for names, _ in groups)
            ap = any("AP" in names for names, _ in groups)
            shared = any("managed" in names and "AP" in names and lim < 2 for names, lim in groups)
            if sta and ap and total >= 2 and not shared:
                ok = True
                break
    except (OSError, IndexError, AttributeError, subprocess.TimeoutExpired):
        pass
    _concurrent[dev] = ok
    return ok


def station_channel(dev):
    """(channel, MHz) the Wi-Fi connection is on, or (0, 0)."""
    try:
        out = subprocess.run(["iw", "dev", dev, "info"], capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.TimeoutExpired):
        return 0, 0
    m = re.search(r"channel (\d+) \((\d+) MHz\)", out)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def iface_helper(*args, stdin=None):
    """Run the root helper that starts/stops the side-by-side hotspot (no password: polkit rule)."""
    try:
        r = subprocess.run(["pkexec", IFACE_HELPER, *args], input=stdin, capture_output=True, text=True, timeout=90)
        return r.returncode == 0, r.stdout.strip(), r.stderr.strip() or "the hotspot helper failed"
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, "", str(e)


def helper_running():
    """Is the helper's hostapd (hotspot next to the Wi-Fi connection) up?"""
    try:
        pid = int((HELPER_RUN / "hostapd.pid").read_text())
        return Path(f"/proc/{pid}/comm").read_text().strip() == "hostapd"
    except (OSError, ValueError):
        return False


def hotspot_clients(dev):
    """[(name, ip)] of devices on the hotspot: dnsmasq leases, else the neighbour table."""
    out = []
    leases = HELPER_RUN / "dnsmasq.leases" if dev == AP_IF else Path(f"/var/lib/NetworkManager/dnsmasq-{dev}.leases")
    try:
        for line in leases.read_text().splitlines():
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


def base_state():
    """Radio, Wi-Fi device and what it can do, saved/active Wi-Fi profiles and the hotspot: everything
    but the (slow) scan. Shared by Settings → Network and the bar's Wi-Fi popup. Returns (d, devs, conns)."""
    d = {"radio": nmcli("radio", "wifi")[1] == "enabled", "airplane": False, "wdev": None, "active": None,
         "saved": [], "nearby": [], "eth": [], "other": [], "hotspot": None, "ap": False, "five": False,
         "concurrent": False}
    d["airplane"] = not d["radio"] and nmcli("radio", "wwan")[1] != "enabled"
    devs = []
    for line in nmcli("-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device")[1].splitlines():
        f = split_terse(line)
        if len(f) >= 4:
            devs.append({"dev": f[0], "type": f[1], "state": f[2], "conn": "" if f[3] == "--" else f[3]})
    wdev = next((x for x in devs if x["type"] == "wifi" and x["dev"] != AP_IF), None)
    d["wdev"] = wdev and wdev["dev"]
    if d["wdev"]:
        props = nmcli("-t", "-f", "WIFI-PROPERTIES.AP,WIFI-PROPERTIES.5GHZ", "device", "show", d["wdev"])[1]
        d["ap"] = "AP:yes" in props
        d["five"] = "5GHZ:yes" in props
        d["concurrent"] = (d["ap"] and os.access(IFACE_HELPER, os.X_OK) and bool(shutil.which("hostapd"))
                           and sta_ap_concurrent(d["wdev"]))

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
                       "connection", "show", *[c["uuid"] for c in wifi])[1].split("\n")
        # 4 lines per profile, but an empty value (no band set) is an empty line too, so blank-line
        # splitting misreads the next profile: anchor on each profile's UUID instead
        info, uuids = {}, {c["uuid"] for c in wifi}
        for i, line in enumerate(blocks):
            if line in uuids:
                info[line] = tuple((blocks[i + 1:i + 4] + ["", "", ""])[:3])
        for c in wifi:
            c["mode"], c["ssid"], c["band"] = info.get(c["uuid"], ("", c["name"], ""))
    hotspots = [c for c in wifi if c.get("mode") == "ap"]
    if hotspots:
        hs = next((c for c in hotspots if c["active"]), max(hotspots, key=lambda c: c["ts"]))
        hs["psk"] = nmcli("-s", "-g", "802-11-wireless-security.psk", "connection", "show", hs["uuid"])[1]
        if not hs["active"] and helper_running():     # the profile only holds its name and password
            hs.update(active=True, dev=AP_IF, alongside=True)
        if hs["active"]:
            hs["clients"] = hotspot_clients(hs["dev"] or d["wdev"])
        d["hotspot"] = hs
    for c in wifi:
        if c.get("mode") != "ap":
            d["saved"].append(c)
            if c["active"]:
                d["active"] = c
    d["saved"].sort(key=lambda c: -c["ts"])
    return d, devs, conns


def hotspot_up(d, hs, cur):
    # NetworkManager's shared mode hands out addresses with dnsmasq; without it the access point
    # comes up, fails at the IP step and takes the current Wi-Fi connection down with it
    if not shutil.which("dnsmasq", path="/usr/bin:/usr/sbin"):
        return False, "", "dnsmasq is not installed (sudo pacman -S dnsmasq)"
    if not d["radio"]:
        nmcli("radio", "wifi", "on")
        time.sleep(1.5)
    if d["concurrent"] and d["active"]:
        # stay on the Wi-Fi network: the helper runs the hotspot on a second interface, on the
        # network's channel; the profile keeps its name and password (made here the first time)
        if not hs:
            res = nmcli("connection", "add", "type", "wifi", "ifname", d["wdev"], "con-name", "Hotspot",
                        "autoconnect", "no", "ssid", cur["ssid"], "802-11-wireless.mode", "ap",
                        "ipv4.method", "shared", "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", cur["psk"])
            if not res[0]:
                return res
        return iface_helper("start", d["wdev"], stdin=f"{cur['ssid']}\n{cur['psk']}\n")
    if helper_running():
        iface_helper("stop")
    if hs:
        res = nmcli("connection", "modify", hs["uuid"], "connection.interface-name", d["wdev"],
                    "802-11-wireless.channel", "0")
        return nmcli("connection", "up", hs["uuid"], timeout=40) if res[0] else res
    args = ["device", "wifi", "hotspot", "ifname", d["wdev"], "con-name", "Hotspot",
            "ssid", cur["ssid"], "password", cur["psk"]]
    if cur.get("band"):
        args += ["band", cur["band"]]
    return nmcli(*args, timeout=40)


def hotspot_down(hs):
    if hs.get("alongside"):
        return iface_helper("stop")
    return nmcli("connection", "down", hs["uuid"])


def hotspot_set(on):
    """Turn the hotspot on/off from its saved profile (the bar's Wi-Fi popup). (ok, out, err)."""
    d, _devs, _conns = base_state()
    hs = d["hotspot"]
    if not d["wdev"] or not d["ap"]:
        return False, "", "this Wi-Fi adapter can't host a hotspot"
    if not on:
        return hotspot_down(hs) if hs and hs["active"] else (True, "", "")
    if hs and hs["active"]:
        return True, "", ""
    cur = ({"ssid": hs.get("ssid") or hs["name"], "psk": hs.get("psk", ""), "band": hs.get("band", "")} if hs else
           {"ssid": socket.gethostname(), "psk": secrets.token_urlsafe(9)[:12], "band": ""})
    return hotspot_up(d, hs, cur)


def hotspot_status():
    d, _devs, _conns = base_state()
    hs = d["hotspot"]
    return {"supported": bool(d["wdev"] and d["ap"]), "on": bool(hs and hs["active"]),
            "ssid": (hs.get("ssid") or hs["name"]) if hs else "",
            "clients": len(hs.get("clients", [])) if hs else 0,
            "together": d["concurrent"], "station": bool(d["active"])}


def main(argv):
    if argv[:1] == ["hotspot-status"]:
        for k, v in hotspot_status().items():
            v = int(v) if isinstance(v, bool) else v
            print(f"{k}={str(v).replace(chr(10), ' ')}")
        return 0
    if len(argv) == 2 and argv[0] == "hotspot" and argv[1] in ("on", "off"):
        ok, _out, err = hotspot_set(argv[1] == "on")
        if not ok:
            print(err_text(err), file=sys.stderr)
        return 0 if ok else 1
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
