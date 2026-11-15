#!/usr/bin/env python3
"""hyprminimal launcher: numbered app list in rofi.

Apps are numbered alphabetically (numbers stay the same between runs). The 5 apps you
launched last are pinned on top as well, newest first. Type a number + Enter to launch that app, or search by name.
"""
import os
import subprocess
import sys
from html import escape
from pathlib import Path

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio  # noqa: E402

DIR = Path(__file__).resolve().parent.parent
STATE = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state") / "hyprminimal"
RECENT = STATE / "launcher-recent"   # app ids, newest first
KEEP = 5


def apps():
    seen, out = set(), []
    for a in Gio.AppInfo.get_all():
        if a.should_show() and a.get_id() not in seen:
            seen.add(a.get_id())
            out.append(a)
    out.sort(key=lambda a: a.get_display_name().casefold())
    return out


def row(num, app, width, tag=""):
    # search sees only "12 Firefox"; the styled text goes in `display` (rofi would match markup)
    name = app.get_display_name()
    shown = f"<span alpha='45%' font_family='monospace'>{num:>{width}}</span>   {escape(name)}{tag}"
    opts = [f"display\x1f{shown}"]
    icon = app.get_icon()
    if icon:
        opts.append(f"icon\x1f{icon.to_string()}")
    words = [f"{num} {name}"]
    if hasattr(app, "get_keywords"):   # .desktop apps: also find them by generic name / keywords ("browser")
        words += filter(None, [app.get_generic_name(), *(app.get_keywords() or [])])
    search = " ".join(words).replace("\n", " ").replace("\0", "").replace("\x1f", "")
    return search + "\0" + "\x1f".join(opts)


def main():
    lst = apps()
    width = len(str(len(lst)))
    try:
        recent_ids = RECENT.read_text().split()
    except OSError:
        recent_ids = []
    pos = {a.get_id(): i for i, a in enumerate(lst)}
    recent = [pos[r] for r in recent_ids if r in pos][:KEEP]   # uninstalled apps drop out

    tag = "<span alpha='40%' size='small'>   recent</span>"
    # recent apps sit on top instead of in the A-Z list (each app shown once, numbers never change)
    order = recent + [i for i in range(len(lst)) if i not in recent]   # rofi row -> app index
    rows = [row(i + 1, lst[i], width, tag if n < len(recent) else "") for n, i in enumerate(order)]
    extra = ["-a", f"0-{len(recent) - 1}"] if recent else []

    subprocess.run([str(DIR / "bin/hyprminimal-msg"), "catch", "rofi"])   # click outside closes it
    res = subprocess.run(
        ["rofi", "-dmenu", "-i", "-markup-rows", "-show-icons", "-no-custom",
         "-format", "i\tf", "-p", "\U000f0349", "-theme", str(DIR / "rofi/launcher.rasi"), *extra],
        input="\n".join(rows), capture_output=True, text=True)
    subprocess.run([str(DIR / "bin/hyprminimal-msg"), "catch", ""])
    if res.returncode != 0 or not res.stdout.strip("\n"):
        return
    idx, _, typed = res.stdout.rstrip("\n").partition("\t")
    typed = typed.strip()
    if typed.isdigit() and 1 <= int(typed) <= len(lst):   # "12" + Enter = app number 12
        app = lst[int(typed) - 1]
    elif idx.lstrip("-").isdigit() and 0 <= int(idx) < len(order):
        app = lst[order[int(idx)]]
    else:
        return

    STATE.mkdir(parents=True, exist_ok=True)
    ids = [app.get_id()] + [r for r in recent_ids if r != app.get_id()]
    RECENT.write_text("\n".join(ids[:KEEP]) + "\n")
    app.launch([], Gio.AppLaunchContext())


if __name__ == "__main__":
    sys.exit(main())
