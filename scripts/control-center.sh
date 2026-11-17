#!/usr/bin/env bash
# Super+S / bar clicks: toggle the swaync control center. The panel is shown first (instantly); the live
# tile info (Wi-Fi network and signal, CPU/RAM, notification count) follows a moment later: written to
# $XDG_RUNTIME_DIR/hyprminimal/cc/labels, which the swaync shim (swaync/cursor/pointer.c) puts into
# the open panel. No config reload, no helpers. The notification list opens collapsed every time.
set -u
# --auto-start=no: never D-Bus-activate the stock swaync (it would take the bus name from hyprminimal's)
call() { busctl --user --auto-start=no call org.erikreider.swaync.cc /org/erikreider/swaync/cc \
           org.erikreider.swaync.cc "$@" 2>/dev/null; }

v=$(call GetVisibility) || exit 0   # hyprminimal's swaync isn't running (the watchdog restarts it)
if [[ $v == "b true" ]]; then call SetVisibility b false; exit 0; fi
tmp=${XDG_RUNTIME_DIR:-/tmp}/hyprminimal/cc; mkdir -p "$tmp"
rm -f "$tmp/expanded"
call SetVisibility b true

# ---- live info, after the panel is up
count=$(call NotificationCount); count=${count#u }
case $count in 0) n="󰂚   No notifications" ;; 1) n="󰂚   1 notification   󰅀" ;; *) n="󰂚   $count notifications   󰅀" ;; esac
read -r _ a b c d e f g _ </proc/stat; t1=$((a+b+c+d+e+f+g)) i1=$((d+e))
w="Off"
if [[ $(nmcli -t -f WIFI g 2>/dev/null) == enabled ]]; then   # the nmcli calls double as the CPU sample window
  w=$(nmcli -t -f ACTIVE,SSID,SIGNAL dev wifi list --rescan no 2>/dev/null | grep -m1 '^yes:')
  if [[ $w ]]; then s=${w##*:}; w=${w#yes:}; w=${w%:*}; (( ${#w} > 12 )) && w="${w:0:11}…"; w="$w · $s%"
  else w="Not connected"; fi
fi
read -r _ a b c d e f g _ </proc/stat; t2=$((a+b+c+d+e+f+g)) i2=$((d+e))
cpu=$(( (100 * ((t2-t1) - (i2-i1))) / ((t2-t1) > 0 ? (t2-t1) : 1) ))
while read -r k val _; do case $k in MemTotal:) mt=$val ;; MemAvailable:) ma=$val; break ;; esac; done </proc/meminfo
printf '%s\t%s\n' "Wi-Fi" "󰖩   Wi-Fi\\n$w" "System" "󰓅   System\\nCPU $cpu% · RAM $(( 100 * (mt - ma) / mt ))%" \
  "otification" "$n" >"$tmp/labels.new" && mv -f "$tmp/labels.new" "$tmp/labels"
