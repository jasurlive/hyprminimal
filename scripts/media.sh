#!/usr/bin/env bash
# Media capsule for waybar: "<icon> <title>" or "Nothing Playing", like Noctalia.
# One long-lived playerctl, no per-event subprocesses; playerctl dies with us.
MAX=14
json() { local s=${1//\\/\\\\}; s=${s//\"/\\\"}; s=${s//$'\t'/ }; printf '%s' "$s"; }
pango() { local s=${1//&/\&amp;}; s=${s//</\&lt;}; s=${s//>/\&gt;}; printf '%s' "$s"; }   # \& : bash 5.2 treats & as "match"
emit() {  # status title artist
  local icon cls text=$2
  case $1 in
    Playing) icon=""; cls=playing ;;      # waybar CSS draws still equalizer bars here
    Paused)  icon="󰏤"; cls=paused ;;
    *)       icon="󰝛"; cls=stopped; text= ;;
  esac
  [[ -z $text ]] && text="Nothing Playing"
  (( ${#text} > MAX )) && text="${text:0:MAX-1}…"
  local tip=${3:+$3 — }${2:-Nothing Playing}
  text=$(pango "$text")
  local pct=135; read -r pct < "$HYPRMINIMAL_DIR/.generated/icon_scale" 2>/dev/null   # icon size / text size
  local open="<span size='${pct}%' rise='-1pt'>" shown=$text
  [[ -n $icon ]] && shown="$open$icon</span>  $text"
  printf '{"text":"%s","class":"%s","tooltip":"%s"}\n' "$(json "$shown")" "$cls" "$(json "$tip")"
}
coproc PC { exec playerctl --follow metadata --format '{{status}}	{{title}}	{{artist}}' 2>/dev/null; }
trap 'kill "$PC_PID" 2>/dev/null' EXIT
trap 'exit 0' TERM HUP INT PIPE
emit Stopped
while IFS=$'\t' read -r st ti ar <&"${PC[0]}"; do emit "$st" "$ti" "$ar" || exit 0; done
sleep 2   # playerctl ended: waybar restarts us
