#!/usr/bin/env bash
# Media capsule for waybar: "<icon> <title>" or "Nothing Playing", like Noctalia.
# One long-lived playerctl, no per-event subprocesses; playerctl dies with us.
# While playing, the icon is a tiny equalizer drawn in braille: 5 slim dotted bars in 3 characters
# (each holds 2 columns; the last column stays empty), spaced evenly (⣠⣴⡆ …). FPS frames a second from a table built once; bash builtins only, no forks per frame,
# and nothing runs at all when paused/stopped.
MAX=26   # characters of the title shown in the capsule
FPS=4
# EQ[a*5+b]: one braille character, left column a dots high, right column b (0-4, from the bottom)
L=(0 64 68 70 71) R=(0 128 160 176 184)   # dot bits: left 7,3,2,1  right 8,6,5,4
EQ=()
for a in 0 1 2 3 4; do for b in 0 1 2 3 4; do printf -v hex '%04x' $(( 0x2800 + L[a] + R[b] )); printf -v 'EQ[a*5+b]' "\\u$hex"; done; done
H=(2 4 3 1 3)   # current bar heights, 1-4
GAP=-2900   # pango units (1024 = 1pt): pulls the characters together so all bars are evenly spaced

json() { local s=${1//\\/\\\\}; s=${s//\"/\\\"}; s=${s//$'\t'/ }; printf -v "$2" '%s' "$s"; }
pango() { local s=${1//&/\&amp;}; s=${s//</\&lt;}; s=${s//>/\&gt;}; printf -v "$2" '%s' "$s"; }   # \& : bash 5.2 treats & as "match"

PCT=135; read -r PCT < "$HYPRMINIMAL_DIR/.generated/icon_scale" 2>/dev/null   # icon size / text size
# the playing icon takes the theme's accent colour (waybar CSS can't colour part of a label)
ACCENT=$(sed -n 's/^@define-color primary *\(#[0-9a-fA-F]\{6\}\);.*/\1/p' "$HYPRMINIMAL_DIR/.generated/palette.css" "$HYPRMINIMAL_DIR/colors/palette.css" 2>/dev/null | head -1)

# set on every metadata change: CLS, TEXT (pango-escaped title), TIP (json-escaped)
update() {  # status title artist
  local text=$2
  case $1 in
    Playing) CLS=playing ;;
    Paused)  CLS=paused ;;
    *)       CLS=stopped; text= ;;
  esac
  [[ -z $text ]] && text="Nothing Playing"
  (( ${#text} > MAX )) && text="${text:0:MAX-1}…"
  pango "$text" TEXT
  json "${3:+$3 — }${2:-Nothing Playing}" TIP
}

# one line of JSON; while playing, the bars move one step each call
emit() {
  local icon shown out
  case $CLS in
    playing)
      # each bar jumps to a new height 1-4, but never the same as before: always moving, never flat
      for i in 0 1 2 3 4; do H[i]=$(( (H[i] + RANDOM % 3) % 4 + 1 )); done
      icon="<span${ACCENT:+ foreground='$ACCENT'} font_family='DejaVu Sans' size='125%' rise='-1pt' letter_spacing='$GAP'>${EQ[H[0]*5+H[1]]}${EQ[H[2]*5+H[3]]}${EQ[H[4]*5]}</span>" ;;
    paused)  icon="<span size='${PCT}%' rise='-1pt'>󰏤</span>" ;;
    *)       icon="<span size='${PCT}%' rise='-1pt'>󰝛</span>" ;;
  esac
  local sp="  "; [[ $CLS == playing ]] && sp=" "   # the equalizer's empty 6th column is already a gap
  json "$icon$sp$TEXT" shown
  printf '{"text":"%s","class":"%s","tooltip":"%s"}\n' "$shown" "$CLS" "$TIP"
}

coproc PC { exec playerctl --follow metadata --format '{{status}}	{{title}}	{{artist}}' 2>/dev/null; }
trap 'kill "$PC_PID" 2>/dev/null' EXIT
trap 'exit 0' TERM HUP INT PIPE
TICK=$(( 1000 / FPS )); TICK=0.$(printf '%03d' "$TICK")
update Stopped; emit
while :; do
  if [[ $CLS == playing ]]; then
    IFS=$'\t' read -r -t "$TICK" st ti ar <&"${PC[0]}"; rc=$?
  else
    IFS=$'\t' read -r st ti ar <&"${PC[0]}"; rc=$?   # idle: block, zero wakeups
  fi
  if (( rc == 0 )); then update "$st" "$ti" "$ar"
  elif (( rc <= 128 )); then break; fi                # EOF: playerctl ended
  emit || exit 0
done
sleep 2   # playerctl ended: waybar restarts us
