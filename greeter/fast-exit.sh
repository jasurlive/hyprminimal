#!/bin/sh
# hyprminimal greeter, after a successful login: end the greeter's Hyprland at once.
# A clean Hyprland exit takes ~1 s on real hardware (DRM teardown), and greetd starts your session only
# after the greeter session is gone: that second was pure waiting after pressing Enter.
# SIGKILL is the same as a Hyprland crash, which greetd handles every day: the kernel drops DRM master
# and logind revokes the input devices as the process dies; your Hyprland then sets the screen up anew
# (the login card simply stays on screen until it does).
# start-hyprland restarts a Hyprland that dies uncleanly, so it is frozen first and killed after.
# Exit 1 (nothing done) when this Hyprland can't be found: the caller then exits it the normal way.
sig=${HYPRLAND_INSTANCE_SIGNATURE:?}
h=$(hyprctl instances 2>/dev/null | awk -v s="instance $sig:" '$0 == s {f=1; next} f && $1 == "pid:" {print $2; exit}')
[ -n "$h" ] && [ -e "/proc/$h" ] || exit 1
w=$(awk '{print $4}' "/proc/$h/stat")                      # parent: start-hyprland (the watchdog), if any
[ "$(cat "/proc/$w/comm" 2>/dev/null)" = start-hyprland ] || w=""
[ -n "$w" ] && kill -STOP "$w"
kill -KILL "$h"
# gone once it is a zombie or reaped (its fds, DRM included, are closed by then); wait at most 2 s
i=0
while [ $i -lt 200 ] && [ -e "/proc/$h" ] && [ "$(awk '{print $3}' "/proc/$h/stat" 2>/dev/null)" != Z ]; do
  sleep 0.01; i=$((i + 1))
done
[ -n "$w" ] && kill -KILL "$w"
exit 0
