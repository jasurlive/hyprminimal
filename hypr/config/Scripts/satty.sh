#!/usr/bin/env bash
# Select a region, then annotate it in satty; saved to ~/Pictures/Screenshots.
mkdir -p "$HOME/Pictures/Screenshots"
out="$HOME/Pictures/Screenshots/Screenshot-$(date '+%Y%m%d-%H%M%S').png"
region=$(slurp) || exit 0
grim -g "$region" - | satty --filename - --output-filename "$out"
