#!/usr/bin/env bash
# Session panel: Lock, Log Out, Lock & Suspend, Reboot, Shut Down (keys 1-5, like Noctalia)
D=$HYPRMINIMAL_DIR
THEME=$D/.generated/rofi/menu.rasi; [[ -f $THEME ]] || THEME=$D/rofi/menu.rasi   # themed copy from Settings
# one line each: icon, label, dim key number
item() { printf '<span font_family="Symbols Nerd Font">%s</span>   %s<span alpha="40%%">   %s</span>' "$1" "$2" "$3"; }
entries="$(item 󰌾 Lock 1)|$(item 󰗽 'Log Out' 2)|$(item 󰏤 'Lock &amp; Suspend' 3)|$(item 󰑓 Reboot 4)|$(item 󰐥 'Shut Down' 5)"
hyprminimal-msg catch rofi   # a click outside closes the menu, like the other popups
choice=$(printf '%s' "$entries" |
  rofi -dmenu -sep '|' -markup-rows -no-custom -p '' -format i -u 4 -theme "$THEME" \
       -kb-custom-1 "" -kb-custom-2 "" -kb-custom-3 "" -kb-custom-4 "" -kb-custom-5 "" \
       -kb-select-1 1 -kb-select-2 2 -kb-select-3 3 -kb-select-4 4 -kb-select-5 5)
hyprminimal-msg catch ""
case $choice in
  0) hyprminimal-msg lock ;;
  1) hyprctl dispatch 'hl.dsp.exit()' ;;
  2) hyprminimal-msg lock & sleep 1; systemctl suspend ;;
  3) systemctl reboot ;;
  4) systemctl poweroff ;;
esac
