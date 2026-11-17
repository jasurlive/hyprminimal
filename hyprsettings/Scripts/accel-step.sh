#!/usr/bin/env bash
# Accelerated volume/brightness stepping for press-and-hold keys.
# Usage: accel-step.sh volume|brightness up|down
#
# Tap = 1%. While held (key repeat), the step grows: 1% -> 2% after 0.4s -> 3% after 0.8s -> 5% after 1.4s.
# Repeats that arrive while a previous step is still running are dropped,
# so the value stops changing as soon as the key is released.

kind=$1 dir=$2
run=${XDG_RUNTIME_DIR:-/tmp}
state=$run/accel-$kind-$dir

exec 9>"$state.lock"
flock -n 9 || exit 0

now=$(date +%s%3N)
start=$now last=0
[[ -r $state ]] && read -r start last < "$state"
# a gap over 400 ms means a new press, not a held repeat
(( now - last > 400 )) && start=$now
echo "$start $now" > "$state"

held=$(( now - start ))
if   (( held < 400 ));  then step=1
elif (( held < 800 ));  then step=2
elif (( held < 1400 )); then step=3
else                         step=5
fi

sign=$([[ $dir == up ]] && echo + || echo -)

case $kind in
  volume)     wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ "${step}%${sign}" ;;
  brightness) brightnessctl -q set "${step}%${sign}" ;;
esac
