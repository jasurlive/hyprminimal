#!/usr/bin/env bash
# Bar error pill: red "⚠ Bar" (or "⚠ 2 errors") while a part of hyprminimal failed; hidden otherwise.
# No polling: bin/hyprminimal and Settings refresh it with `pkill -RTMIN+9 waybar` when an error appears or clears.
E=${XDG_RUNTIME_DIR:-/tmp}/hyprminimal/errors
declare -A NAME=([waybar]="Bar" [panels]="Popups"
                 [settings]="Settings" [build]="Build")
json() { local s=${1//\\/\\\\}; s=${s//\"/\\\"}; s=${s//$'\n'/\\n}; s=${s//$'\t'/ }; printf '%s' "$s"; }
n=0 first="" tip=""
for f in "$E"/*; do
  [[ -f $f ]] || continue
  c=${f##*/}; (( n++ )); [[ $first ]] || first=${NAME[$c]:-$c}
  tip+="${tip:+$'\n'}${NAME[$c]:-$c}: $(<"$f")"
done
if (( n == 0 )); then echo '{"text":""}'; exit 0; fi
(( n > 1 )) && label="$n errors" || label=$first
printf '{"text":"󰀦  %s","class":"error","tooltip":"%s\\n\\nClick to open Settings"}\n' "$(json "$label")" "$(json "$tip")"
