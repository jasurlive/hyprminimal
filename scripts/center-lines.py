#!/usr/bin/env python3
"""Centre two-line control-center tile labels. GTK labels left-align their lines and swaync gives no way
to change that, so the shorter line is padded with spaces on both sides to the longer one's width,
measured with Pango in the tile font (swaync/style.css: Noto Sans + Symbols Nerd Font, 600, 14px).

stdin: one tile per line, "first line<TAB>second line". stdout: a JSON array of the centred labels.
Started early by scripts/control-center.sh (as a coprocess), so the gi import overlaps its other work."""
import json
import sys

import gi
gi.require_version("Pango", "1.0")
gi.require_version("PangoCairo", "1.0")
from gi.repository import Pango, PangoCairo  # noqa: E402

layout = Pango.Layout.new(PangoCairo.FontMap.get_default().create_context())
layout.set_font_description(Pango.FontDescription.from_string("Noto Sans, Symbols Nerd Font Semi-Bold 14px"))


def width(text):
    layout.set_text(text, -1)
    return layout.get_extents()[1].width / Pango.SCALE   # logical width, sub-pixel


SPACE = width("a a") - width("aa")


def centre(a, b):
    wa, wb = width(a), width(b)
    pad = " " * round(abs(wa - wb) / 2 / SPACE)
    if wa < wb:
        a = pad + a + pad
    else:
        b = pad + b + pad
    return a + "\n" + b


out = []
for line in sys.stdin:
    first, _, second = line.rstrip("\n").partition("\t")
    out.append(centre(first, second) if second else first)
print(json.dumps(out, ensure_ascii=False))
