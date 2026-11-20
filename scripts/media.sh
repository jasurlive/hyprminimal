#!/usr/bin/env bash
# Media capsule for waybar: the title; empty (waybar hides the module) when nothing is playing.
# The icon is a square-dot equalizer image in the bar CSS, picked by class (.generated/media-*.png, drawn
# by hyprminimal-settings). While playing, the class steps through the frames f0..f3, FPS times a second:
# only the class changes, so waybar repaints just that small icon (solid bar, no blur behind it).
# Paused or stopped: one flat row of dots and no timer at all; the loop blocks on playerctl.
MAX=26   # characters of the title shown in the capsule
FPS=3
FRAMES=4   # = len(EQ_FRAMES) in settings/hyprminimal_settings.py

json() { local s=${1//\\/\\\\}; s=${s//\"/\\\"}; s=${s//$'\t'/ }; printf -v "$2" '%s' "$s"; }
pango() { local s=${1//&/\&amp;}; s=${s//</\&lt;}; s=${s//>/\&gt;}; printf -v "$2" '%s' "$s"; }   # \& : bash 5.2 treats & as "match"

# on a playerctl line: CLS, TEXT (json-escaped title for the label), TIP
update() {  # status title artist
  local text=$2
  case $1 in
    Playing) CLS=playing ;;
    Paused)  CLS=paused ;;
    *)       CLS=stopped; return ;;
  esac
  [[ -z $text ]] && text="Nothing Playing"
  (( ${#text} > MAX )) && text="${text:0:MAX-1}…"
  pango "$text" text
  json "$text" TEXT
  json "${3:+$3 — }${2:-Nothing Playing}" TIP
}

F=0
emit() {
  case $CLS in
    stopped) printf '{"text":"","class":"stopped"}\n' ;;   # empty text: waybar hides the capsule
    playing) F=$(( (F + 1) % FRAMES ))
             printf '{"text":"%s","class":["playing","f%d"],"tooltip":"%s"}\n' "$TEXT" "$F" "$TIP" ;;
    *)       printf '{"text":"%s","class":"%s","tooltip":"%s"}\n' "$TEXT" "$CLS" "$TIP" ;;
  esac
}

coproc PC { exec playerctl --follow metadata --format '{{status}}	{{title}}	{{artist}}' 2>/dev/null; }
trap 'kill "$PC_PID" 2>/dev/null' EXIT
trap 'exit 0' TERM HUP INT PIPE
TICK=0.$(printf '%03d' $(( 1000 / FPS )))
CLS=stopped; emit
while :; do
  if [[ $CLS == playing ]]; then
    IFS=$'\t' read -r -t "$TICK" st ti ar <&"${PC[0]}"; rc=$?   # next frame after TICK, or sooner on a change
  else
    IFS=$'\t' read -r st ti ar <&"${PC[0]}"; rc=$?              # paused/stopped: block, zero wakeups
  fi
  if (( rc == 0 )); then update "$st" "$ti" "$ar"
  elif (( rc <= 128 )); then break; fi                          # EOF: playerctl ended
  emit || exit 0
done
sleep 2   # playerctl ended: waybar restarts us
