#!/usr/bin/env python3
"""hyprminimal greeter: the lock-screen look (clock, date, login card) as a greetd greeter, plus user selection.

Runs as the `greeter` user inside greeter/hyprland.lua. Talks to greetd over $GREETD_SOCK.
  greeter.py           real greeter
  greeter.py --test    windowed preview against a fake greetd (password "test"); Esc quits
"""
import json, os, pwd, socket, struct, sys, threading

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk

HERE = os.path.dirname(os.path.abspath(__file__))
STATE = "/var/cache/hyprminimal-greeter/last-user"   # writable by greeter (made by hyprminimal-greeter install)
SESSION = ["start-hyprland"]
TEST = "--test" in sys.argv


# ---------- greetd IPC: 4-byte native-endian length + JSON, both ways ----------
class Greetd:
    def __init__(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(os.environ["GREETD_SOCK"])

    def call(self, req):
        data = json.dumps(req).encode()
        self.sock.sendall(struct.pack("=I", len(data)) + data)
        n = struct.unpack("=I", self._read(4))[0]
        return json.loads(self._read(n))

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("greetd closed the connection")
            buf += chunk
        return buf

    def login(self, user, password):
        """Returns None once the session is queued to start, else an error string."""
        r = self.call({"type": "create_session", "username": user})
        sent = False
        while r["type"] == "auth_message":
            if r["auth_message_type"] in ("secret", "visible"):
                if sent:   # a second prompt (e.g. expired password) we can't answer here
                    self.call({"type": "cancel_session"})
                    return r.get("auth_message", "Unexpected prompt").strip()
                r = self.call({"type": "post_auth_message_response", "response": password})
                sent = True
            else:          # info / error messages need an empty reply
                r = self.call({"type": "post_auth_message_response"})
        if r["type"] == "success":
            r = self.call({"type": "start_session", "cmd": SESSION, "env": []})
            if r["type"] == "success":
                return None
        self.call({"type": "cancel_session"})
        if r.get("error_type") == "auth_error":
            return "Wrong password"
        return r.get("description") or "Login failed"


class FakeGreetd:
    def login(self, user, password):
        import time; time.sleep(0.6)
        return None if password == "test" else "Wrong password"


# ---------- users ----------
def users():
    out = []
    for p in pwd.getpwall():
        if 1000 <= p.pw_uid < 60000 and not p.pw_shell.endswith(("nologin", "false")):
            out.append((p.pw_name, p.pw_gecos.split(",")[0] or p.pw_name))
    return sorted(out) or [("root", "root")]


def last_user():
    try:
        with open(STATE) as f:
            return f.read().strip()
    except OSError:
        return ""


def save_user(name):
    try:
        with open(STATE, "w") as f:
            f.write(name)
    except OSError:
        pass


# ---------- UI ----------
class Greeter(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="dev.hyprminimal.greeter")
        self.greetd = FakeGreetd() if TEST else Greetd()
        self.users = users()
        names = [u for u, _ in self.users]
        self.idx = names.index(last_user()) if last_user() in names else 0
        self.busy = False

    def do_activate(self):
        css = Gtk.CssProvider()
        css.load_from_path(os.path.join(HERE, "style.css"))
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_USER)

        monitors = Gdk.Display.get_default().get_monitors()
        main = None
        for i in range(monitors.get_n_items()):
            win = self.window(with_ui=(i == 0))
            if TEST:
                win.set_default_size(1280, 720)
            else:
                win.fullscreen_on_monitor(monitors.get_item(i))
            win.present()
            main = main or win
            if TEST:
                break
        self.entry.grab_focus()
        GLib.timeout_add_seconds(1, self.tick)
        self.tick()

    def window(self, with_ui):
        win = Gtk.ApplicationWindow(application=self, decorated=False)
        overlay = Gtk.Overlay()
        bg = os.path.join(HERE, "background.png")
        if os.path.exists(bg):
            pic = Gtk.Picture.new_for_filename(bg)
            pic.set_content_fit(Gtk.ContentFit.COVER)
            overlay.set_child(pic)
        else:
            overlay.set_child(Gtk.Box(css_classes=["fallback-bg"]))
        win.set_child(overlay)
        if with_ui:
            overlay.add_overlay(self.ui())
            keys = Gtk.EventControllerKey()
            keys.connect("key-pressed", self.on_key)
            win.add_controller(keys)
        return win

    def ui(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, halign=Gtk.Align.CENTER, valign=Gtk.Align.CENTER)
        self.clock = Gtk.Label(css_classes=["clock"])
        self.date = Gtk.Label(css_classes=["date"])
        box.append(self.clock)
        box.append(self.date)

        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, css_classes=["card"], halign=Gtk.Align.CENTER)
        row = Gtk.Box(halign=Gtk.Align.CENTER, css_classes=["user-row"])
        multi = len(self.users) > 1
        self.prev = Gtk.Button(label="󰅁", css_classes=["arrow"], visible=multi, can_focus=False)
        self.next = Gtk.Button(label="󰅂", css_classes=["arrow"], visible=multi, can_focus=False)
        self.prev.connect("clicked", lambda *_: self.switch(-1))
        self.next.connect("clicked", lambda *_: self.switch(1))
        self.user_label = Gtk.Label(css_classes=["user"])
        row.append(self.prev)
        row.append(self.user_label)
        row.append(self.next)
        card.append(row)

        self.entry = Gtk.PasswordEntry(css_classes=["password"], halign=Gtk.Align.CENTER)
        self.entry.set_property("placeholder-text", "Password…")
        self.entry.connect("activate", self.submit)
        self.entry.connect("changed", lambda *_: self.entry.remove_css_class("fail"))
        card.append(self.entry)
        box.append(card)
        self.show_user()
        return box

    def tick(self):
        now = GLib.DateTime.new_now_local()
        self.clock.set_label(now.format("%H:%M"))
        self.date.set_label(now.format("%A, %d %B"))
        caps = Gdk.Display.get_default().get_default_seat().get_keyboard()
        if caps is not None:
            (self.entry.add_css_class if caps.get_caps_lock_state() else self.entry.remove_css_class)("caps")
        return True

    def show_user(self):
        name, full = self.users[self.idx]
        self.user_label.set_label(f"󰀄   {full}" if full != name else f"󰀄   {name}")

    def switch(self, step):
        if self.busy or len(self.users) < 2:
            return
        self.idx = (self.idx + step) % len(self.users)
        self.show_user()
        self.entry.set_text("")
        self.entry.grab_focus()

    def on_key(self, _ctl, keyval, _code, _state):
        if keyval in (Gdk.KEY_Up, Gdk.KEY_Page_Up):
            self.switch(-1); return True
        if keyval in (Gdk.KEY_Down, Gdk.KEY_Page_Down):
            self.switch(1); return True
        if TEST and keyval == Gdk.KEY_Escape:
            self.quit(); return True
        return False

    def submit(self, *_):
        password = self.entry.get_text()
        if self.busy or not password:
            return
        self.busy = True
        self.entry.set_sensitive(False)
        self.entry.set_property("placeholder-text", "Logging in…")
        self.entry.set_text("")
        user = self.users[self.idx][0]

        def work():
            try:
                err = self.greetd.login(user, password)
            except Exception as e:   # greetd gone or protocol error: show it, don't crash to a black screen
                err = str(e)
            GLib.idle_add(self.done, user, err)
        threading.Thread(target=work, daemon=True).start()

    def done(self, user, err):
        if err is None:
            save_user(user)
            self.quit()   # greetd starts the session once the greeter exits
            return False
        self.busy = False
        self.entry.set_sensitive(True)
        self.entry.add_css_class("fail")
        self.entry.set_property("placeholder-text", err)
        self.entry.grab_focus()
        return False


if __name__ == "__main__":
    Greeter().run([sys.argv[0]])
