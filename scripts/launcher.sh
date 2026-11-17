#!/usr/bin/env bash
# hyprminimal launcher: numbered app list in rofi.
# Apps are numbered A-Z (numbers stay the same between runs); the 5 apps launched last are pinned on
# top, newest first. Type a number + Enter to launch that app, or search by name.
# "rm NAME" (or "rm NUMBER") + Enter uninstalls the app instead, in a terminal that asks first.
# Fast path: the app list is cached; Python (scripts/launcher.py) only runs when apps change.
DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)"
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/hyprminimal/launcher-apps
STATE=${XDG_STATE_HOME:-$HOME/.local/state}/hyprminimal
RECENT=$STATE/launcher-recent   # app ids, newest first
KEEP=5

"$DIR/bin/hyprminimal-msg" catch rofi &   # click outside closes it (don't wait for it)

# rebuild the cache when an applications dir changed (install/remove) or the builder did
dirs=("${XDG_DATA_HOME:-$HOME/.local/share}/applications")
IFS=: read -ra dd <<< "${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
for d in "${dd[@]}"; do dirs+=("$d/applications"); done
if [[ ! -s $CACHE ]] || [[ -n $(find "${dirs[@]}" "$DIR/scripts/launcher.py" -maxdepth 1 -newer "$CACHE" -print -quit 2>/dev/null) ]]; then
  mkdir -p "${CACHE%/*}" && python3 -I "$DIR/scripts/launcher.py" "$CACHE" || exit 1
fi

mapfile -t ids < <(cut -f1 "$CACHE")
declare -A pos; for i in "${!ids[@]}"; do pos[${ids[i]}]=$i; done
recent_ids=(); [[ -r $RECENT ]] && read -d '' -ra recent_ids < "$RECENT"

order=(); declare -A pinned
for r in "${recent_ids[@]}"; do
  [[ -n ${pos[$r]+x} && -z ${pinned[$r]+x} ]] || continue   # uninstalled apps drop out
  order+=("${pos[$r]}"); pinned[$r]=1; (( ${#order[@]} < KEEP )) || break
done
nrecent=${#order[@]}
for i in "${!ids[@]}"; do [[ -z ${pinned[${ids[i]}]+x} ]] && order+=("$i"); done

mapfile -t lines < "$CACHE"
tag="<span alpha='40%' size='small'>   recent</span>"
notag="<span alpha='1%'  size='small'>         </span>"   # same length, invisible: rofi ranks by display text
extra=(); (( nrecent )) && extra=(-a "0-$((nrecent - 1))")

theme=$DIR/.generated/rofi/launcher.rasi; [[ -f $theme ]] || theme=$DIR/rofi/launcher.rasi
out=$(for n in "${!order[@]}"; do
        IFS=$'\t' read -r _ _ search shown tail <<< "${lines[order[n]]}"
        if (( n < nrecent )); then shown+=$tag; else shown+=$notag; fi
        shown+=$tail   # invisible: apps starting with the typed text rank first (launcher.py)
        printf '%s\036display\037%s\n' "$search" "$shown"
      done | tr '\036' '\000' |
      rofi -dmenu -i -markup-rows -no-show-icons -no-custom -format $'i\tf' -p $'\U000f0349' -theme "$theme" "${extra[@]}")
rc=$?
"$DIR/bin/hyprminimal-msg" catch "" &
[[ $rc -eq 0 && -n $out ]] || exit 0

idx=${out%%$'\t'*} typed=${out#*$'\t'}
typed=${typed#"${typed%%[![:space:]]*}"}; typed=${typed%"${typed##*[![:space:]]}"}   # trim
remove=0
if [[ $typed == rm || $typed == "rm "* ]]; then
  remove=1; typed=${typed#rm}; typed=${typed#"${typed%%[![:space:]]*}"}
  [[ -n $typed ]] || exit 0   # bare "rm" + Enter: never guess which app to remove
fi
if [[ $typed =~ ^[0-9]+$ ]] && (( 10#$typed >= 1 && 10#$typed <= ${#ids[@]} )); then
  app=$((10#$typed - 1))   # "12" + Enter = app number 12
elif [[ $idx =~ ^[0-9]+$ ]] && (( idx < ${#order[@]} )); then
  app=${order[idx]}
else
  exit 0
fi
IFS=$'\t' read -r id file name _ <<< "${lines[app]}"
name=${name% * rm}

if (( remove )); then
  setsid -f kitty --class hyprminimal-remove --title "Remove $name" \
    "$DIR/scripts/remove-app.sh" "$file" "$name" >/dev/null 2>&1 </dev/null
  exit 0
fi

mkdir -p "$STATE"
{ printf '%s\n' "$id"; for r in "${recent_ids[@]}"; do [[ $r != "$id" ]] && printf '%s\n' "$r"; done; } | head -n "$KEEP" > "$RECENT.tmp" \
  && mv "$RECENT.tmp" "$RECENT"
exec setsid -f gio launch "$file" >/dev/null 2>&1 </dev/null
