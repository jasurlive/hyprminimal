#!/usr/bin/env bash
# Bar network speed: shows whichever direction is busier right now (download or upload).
# Reads kernel byte counters every 2 s; no subprocesses in the loop.
# The C build (helpers/netspeed.c, built by `hyprminimal start`) does the same with no forks and only
# prints when the text changes; this loop is the fallback when gcc is missing.
NS=${HYPRMINIMAL_DIR:-}/.generated/bin/netspeed
[[ -x $NS ]] && exec "$NS"
trap 'exit 0' PIPE TERM HUP INT
DOWN=$'\U000f0120' UP=$'\U000f011d'
iface() {  # interface of the default route
  local i d _
  while read -r i d _; do [[ $d == 00000000 ]] && { echo "$i"; return; }; done < /proc/net/route
}
rate() {   # bytes/s -> "863 B/s", "1.2 kB/s", "4.7 MB/s"
  local b=$1
  if   (( b < 1000 ));    then printf '%d B/s' "$b"
  elif (( b < 1000000 )); then printf '%d.%d kB/s' $((b / 1000)) $((b % 1000 / 100))
  else                         printf '%d.%d MB/s' $((b / 1000000)) $((b % 1000000 / 100000))
  fi
}
prx=-1 ptx=-1 dev=
while :; do
  [[ -z $dev || ! -r /sys/class/net/$dev/statistics/rx_bytes ]] && { dev=$(iface); prx=-1; }
  if [[ -z $dev ]]; then
    printf '{"text":"%s 0 B/s","class":"offline","tooltip":"Offline"}\n' "$DOWN"
  else
    read -r rx < "/sys/class/net/$dev/statistics/rx_bytes"; read -r tx < "/sys/class/net/$dev/statistics/tx_bytes"
    if (( prx >= 0 )); then
      d=$(( (rx - prx) / 2 )) u=$(( (tx - ptx) / 2 ))
      if (( u > d )); then icon=$UP cls=up v=$u; else icon=$DOWN cls=down v=$d; fi
      pct=125; read -r pct < "$HYPRMINIMAL_DIR/.generated/icon_scale" 2>/dev/null   # icon size / text size
      printf '{"text":"<span size=\x27%s%%\x27 rise=\x27-1pt\x27>%s</span>  %s","class":"%s","tooltip":"Download %s\\nUpload %s"}\n' \
        "$pct" "$icon" "$(rate "$v")" "$cls" "$(rate "$d")" "$(rate "$u")"
    fi
    prx=$rx ptx=$tx
  fi
  sleep 2
done
