#!/usr/bin/env python3
# Close the swaync control center (Super+S) when the workspace or focused monitor changes, e.g. Super+2.
# swaync is a layer surface, so Hyprland leaves it open across workspaces by itself.
# Listens on Hyprland's event socket; started and stopped by bin/hyprminimal.
import os, socket, subprocess, sys, time

sig = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
if not sig:
    sys.exit("workspace-watch: not running inside Hyprland")
path = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "hypr", sig, ".socket2.sock")

# --auto-start=no: never D-Bus-activate the stock swaync
HIDE = ["busctl", "--user", "--auto-start=no", "call", "org.erikreider.swaync.cc",
        "/org/erikreider/swaync/cc", "org.erikreider.swaync.cc", "SetVisibility", "b", "false"]

def watch():
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(path)
        buf = b""
        while chunk := s.recv(4096):
            buf += chunk
            *lines, buf = buf.split(b"\n")
            if any(l.startswith((b"workspace>>", b"focusedmon>>")) for l in lines):
                subprocess.run(HIDE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

while os.path.exists(path):   # Hyprland gone = socket gone: exit
    try:
        watch()
    except OSError:
        pass
    time.sleep(1)
