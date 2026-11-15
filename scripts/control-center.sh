#!/usr/bin/env bash
# Super+S / bar clicks: toggle the swaync control center. Before opening, write live info into the tile
# labels (second line: Wi-Fi network and signal, CPU/RAM/battery) and reload swaync's config.
# swaync can't update button labels by itself, so the info is a snapshot taken each time the panel opens.
set -u
DIR="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
export PATH="$DIR/bin:$PATH" HYPRMINIMAL_DIR="$DIR"
# --auto-start=no: never D-Bus-activate the stock swaync (it would take the bus name from hyprminimal's)
call() { busctl --user --auto-start=no call org.erikreider.swaync.cc /org/erikreider/swaync/cc \
           org.erikreider.swaync.cc "$@" 2>/dev/null; }

v=$(call GetVisibility) || exit 0   # hyprminimal's swaync isn't running (the watchdog restarts it)
if [[ $v == "b true" ]]; then call SetVisibility b false; exit 0; fi

# the config file the running swaync was started with (.generated/swaync.json, or the last working copy)
conf=$(ps -o args= -C swaync | sed -n 's/.* -c \([^ ]*\).*/\1/p' | head -1)
[[ -f $conf ]] || { call SetVisibility b true; exit 0; }

tmp=${XDG_RUNTIME_DIR:-/tmp}/hyprminimal/cc; mkdir -p "$tmp"

# label centring helper: start it now, its Python/gi start-up overlaps the gathering below
coproc PAD { python3 -I "$DIR/scripts/center-lines.py" 2>/dev/null; }

# ---- gather (only Wi-Fi and System tiles carry info; the toggles show their state by colour)
{ # CPU: busy share over 150 ms
  read -r _ a b c d e f g _ </proc/stat; t1=$((a+b+c+d+e+f+g)) i1=$((d+e)); sleep 0.15
  read -r _ a b c d e f g _ </proc/stat; t2=$((a+b+c+d+e+f+g)) i2=$((d+e))
  echo $(( (100 * ((t2-t1) - (i2-i1))) / ((t2-t1) > 0 ? (t2-t1) : 1) )) >"$tmp/cpu"; } & p1=$!
{ if nmcli -t radio wifi 2>/dev/null | grep -q enabled; then
    w=$(nmcli -t -f ACTIVE,SSID,SIGNAL dev wifi list --rescan no 2>/dev/null | grep -m1 '^yes:')
    if [[ $w ]]; then s=${w##*:}; w=${w#yes:}; echo "${w%:*} · ${s}%"; else echo "Not connected"; fi
  else echo "Off"; fi >"$tmp/wifi"; } & p2=$!
mem=$(awk '/^MemTotal/ {t=$2} /^MemAvailable/ {a=$2} END {printf "%.0f", 100*(t-a)/t}' /proc/meminfo)
bat=""; for b in /sys/class/power_supply/BAT*; do [[ -r $b/capacity ]] && { bat=$(<"$b/capacity"); break; }; done
[[ $bat ]] && { [[ $(<"${b}/status") == Charging ]] && bat="   󰂄 ${bat}%" || bat="   󰁹 ${bat}%"; }
wait "$p1" "$p2"   # not a bare wait: that would also wait for the coprocess

wifi="󰖩   Wi-Fi"$'\t'"$(<"$tmp/wifi")"
sys="󰓅   System"$'\t'"CPU $(<"$tmp/cpu")%   RAM ${mem}%${bat}"
centred=""
if [[ ${PAD[1]:-} ]]; then
  printf '%s\n%s\n' "$wifi" "$sys" >&"${PAD[1]}"; exec {PAD[1]}>&-
  read -r centred <&"${PAD[0]}"
fi
[[ $centred == \[* ]] || centred=$(jq -cn --arg a "${wifi/$'\t'/$'\n'}" --arg b "${sys/$'\t'/$'\n'}" '[$a, $b]')   # helper failed

# top row in swaync/config.json (buttons-grid#info): 0 Wi-Fi, 1 the › to the network list, 2 System
jq --argjson c "$centred" '.["widget-config"]["buttons-grid#info"].actions |= [to_entries[]
     | .value.label = (if .key == 0 then $c[0] elif .key == 2 then $c[1] else .value.label end) | .value]' \
   <"$conf" >"$conf.tmp" && mv -f "$conf.tmp" "$conf"

call ReloadConfig
call SetVisibility b true
