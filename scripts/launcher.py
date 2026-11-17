#!/usr/bin/env python3
"""hyprminimal launcher cache builder (run by scripts/launcher.sh only when apps change).

Writes one line per visible app, A-Z (numbers stay the same between runs):
    id \t desktop-file \t search-text \t display-markup \t rank-tail   (no icons: faster)
rank-tail: rofi ranks by the display text (markup included) and its fzf score favours matches late in the
text and short rows, so "man" put "Bluetooth Manager" above "Manage Printing". The tail is an invisible,
1/1024 pt copy of the app's first word at the very end of every row, left-padded so every row has the same
length (the score also costs each character before the match): an app whose name starts with the typed
text matches in that copy, at the same spot in every row, and outranks every match inside a name.
Python + Gio is the correct .desktop parser but costs ~150 ms, so it is kept off the keypress path.
"""
import sys
from html import escape
from pathlib import Path

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio  # noqa: E402


def clean(s):
    return s.replace("\t", " ").replace("\n", " ").replace("\0", "").replace("\x1f", "").replace("\x1e", "")


def main(out):
    seen, apps = set(), []
    for a in Gio.AppInfo.get_all():
        if a.should_show() and a.get_id() not in seen and a.get_filename():
            seen.add(a.get_id())
            apps.append(a)
    apps.sort(key=lambda a: a.get_display_name().casefold())
    width = len(str(len(apps)))
    rows = []
    for num, a in enumerate(apps, 1):
        name = clean(a.get_display_name())
        shown = f"<span alpha='45%' font_family='monospace'>{num:>{width}}</span>   {escape(name)}"
        rows.append((a, name, shown, escape(name.split(" ", 1)[0])))
    # every row this long; rofi skips scoring rows over 256 characters, so a huge name can't push all rows past
    # it (256 - the 47-character recent tag - the tail's 33 characters of markup)
    full = min(max((len(shown) + len(first) for _, _, shown, first in rows), default=0), 256 - 47 - 33)
    lines = []
    for num, (a, name, shown, first) in enumerate(rows, 1):
        tail = f"<span alpha='1%' size='1'>{first:>{full - len(shown)}}</span>"
        # search sees only the name + number ("rm" keeps the app listed while typing "rm fire")
        lines.append("\t".join([clean(a.get_id()), clean(a.get_filename()), f"{name} {num} rm", shown, tail]))
    tmp = Path(out + ".tmp")
    tmp.write_text("\n".join(lines) + "\n")
    tmp.replace(out)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
