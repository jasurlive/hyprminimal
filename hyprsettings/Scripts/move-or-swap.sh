#!/usr/bin/env bash
# Floating window: nudge it 30px. Tiled window: swap with the neighbour. Usage: move-or-swap.sh l|r|u|d
case $1 in l) dx=-30 dy=0 ;; r) dx=30 dy=0 ;; u) dx=0 dy=-30 ;; d) dx=0 dy=30 ;; *) exit 1 ;; esac
if [[ $(hyprctl activewindow -j | jq -r .floating) == true ]]; then
  hyprctl dispatch "hl.dsp.window.move({ x = $dx, y = $dy, relative = true })"
else
  hyprctl dispatch "hl.dsp.window.move({ direction = \"$1\" })"
fi
