#!/usr/bin/env bash
# hyprminimal uninstaller: undoes install.sh, so the next Hyprland start is stock Hyprland
# (its own default config) and ./install.sh can be tried again from scratch.
#
#   ./uninstall.sh            # asks once, then removes everything hyprminimal put on the system
#
# Run it as your normal user (it asks for sudo). Reboot afterwards.
#   --restore     put back the configs install.sh moved away (~/.config/hypr.pre-hyprminimal-*,
#                 kitty.conf.pre-hyprminimal-*) instead of starting from Hyprland's defaults
#   --packages    also remove the hyprminimal-only packages from packages.txt (pacman -Rnsu: only those
#                 nothing else needs; hyprland, kitty, NetworkManager and the build/runtime basics are kept)
#   --purge       also delete this repo folder (settings.json included)
#   -y, --yes     don't ask
set -euo pipefail

DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
LINK="$HOME/.local/share/hyprminimal"
HYPR="$HOME/.config/hypr"
KITTY="$HOME/.config/kitty/kitty.conf"
RESTORE=0 PACKAGES=0 PURGE=0 YES=0

for a in "$@"; do
  case $a in
    --restore) RESTORE=1 ;;
    --packages) PACKAGES=1 ;;
    --purge) PURGE=1 ;;
    -y|--yes) YES=1 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $a (see --help)"; exit 2 ;;
  esac
done

c_b=$'\e[1;34m' c_y=$'\e[1;33m' c_r=$'\e[1;31m' c_0=$'\e[0m'
step() { printf '\n%s==>%s %s\n' "$c_b" "$c_0" "$*"; }
warn() { printf '%s warning:%s %s\n' "$c_y" "$c_0" "$*"; }
die()  { printf '%s error:%s %s\n' "$c_r" "$c_0" "$*" >&2; exit 1; }
rmv()  { [[ -e $1 || -L $1 ]] || return 0; rm -rf -- "$1"; echo "  removed $1"; }
newest() { ls -1d "$@" 2>/dev/null | sort | tail -n1; }   # names end in a sortable date

[[ $EUID != 0 ]] || die "run as your normal user, not root (it uses sudo where needed)"

# ------------------------------------------------------------------ confirm
EXTRA=""
(( PACKAGES )) && EXTRA+="  - hyprminimal-only packages from packages.txt"$'\n'
(( PURGE )) && EXTRA+="  - the repo itself: $DIR"$'\n'
cat <<EOF
This removes hyprminimal from this system:
  - the login screen (greetd + cage) and the tty1 autostart in ~/.bash_profile
  - $HYPR $( ((RESTORE)) && echo "(your pre-hyprminimal backup is put back)" || echo "(Hyprland writes its default config on next start)")
  - kitty.conf, the font, GTK and Qt theme files, the Firefox file-dialog tweak
  - the hotspot helper and its polkit rule, the swaync mask
  - $LINK, ~/.local/bin/hyprminimal*, caches, state, $DIR/.generated and settings.json
${EXTRA}Kept: system packages$( ((PACKAGES)) && echo " other than the ones above"), enabled services (NetworkManager, bluetooth, ...) and
any ~/.config/*.pre-hyprminimal-* backups.
EOF
if (( ! YES )); then read -rp "Continue? [y/N] " r; [[ ${r,,} == y* ]] || { echo "nothing changed"; exit 0; }; fi

step "Asking for sudo"
sudo -v || die "sudo failed"

# ------------------------------------------------------------------ stop the running shell
step "Stopping hyprminimal"
[[ -x $DIR/bin/hyprminimal ]] && "$DIR/bin/hyprminimal" stop 2>/dev/null || true
pkill -f "$DIR/(\.generated|helpers|settings|scripts|bin)/" 2>/dev/null || true   # helpers still running from the repo
systemctl --user unmask -q swaync.service 2>/dev/null || true

# ------------------------------------------------------------------ login
step "Login: back to the plain TTY login on tty1"
if [[ -x $DIR/bin/hyprminimal-greeter ]]; then
  sudo "$DIR/bin/hyprminimal-greeter" uninstall
else   # repo gone already: same steps by hand
  sudo systemctl disable greetd.service 2>/dev/null || true
  [[ -f /etc/greetd/config.toml.pre-hyprminimal-greeter ]] && sudo mv /etc/greetd/config.toml.pre-hyprminimal-greeter /etc/greetd/config.toml
  sudo rm -rf /usr/local/share/hyprminimal-greeter /var/cache/hyprminimal-greeter
fi
f="$HOME/.bash_profile"
if grep -qs '^# hyprminimal autostart' "$f"; then
  sed -i '/^# hyprminimal autostart/,/^fi$/d' "$f"
  echo "  removed the tty1 autostart from $f"
fi

# ------------------------------------------------------------------ Hyprland config
step "Hyprland config"
bak=$(newest "$HOME"/.config/hypr.pre-hyprminimal-*)
if [[ -d $HYPR && ! -f $HYPR/.hyprminimal ]]; then
  warn "$HYPR was not installed by hyprminimal (no .hyprminimal marker): left alone."
  warn "if it loads hyprminimal, remove the layerrules.lua line and the 'hyprminimal start' line yourself"
else
  rmv "$HYPR"
  if (( RESTORE )) && [[ -n $bak ]]; then
    mv "$bak" "$HYPR"; echo "  restored $bak -> $HYPR"
  else
    echo "  Hyprland writes its default config there on next start"
  fi
fi

# ------------------------------------------------------------------ kitty
step "kitty config"
if [[ -f $KITTY ]] && grep -qE '^include .*hyprminimal/kitty/defaults.conf|^# hyprminimal kitty config' "$KITTY"; then
  rmv "$KITTY"
  kbak=$(newest "$KITTY".pre-hyprminimal-*)
  if (( RESTORE )) && [[ -n $kbak ]]; then mv "$kbak" "$KITTY"; echo "  restored $kbak"; fi
  rmdir "${KITTY%/*}" 2>/dev/null || true
  pgrep -x kitty >/dev/null && pkill -USR1 -x kitty || true
else
  echo "  not hyprminimal's, left alone"
fi

# ------------------------------------------------------------------ fonts, GTK, Qt, Firefox
step "Font and theme files"
rmv "$HOME/.config/fontconfig/conf.d/50-hyprminimal.conf"
rmdir "$HOME/.config/fontconfig/conf.d" "$HOME/.config/fontconfig" 2>/dev/null || true
fc-cache -f >/dev/null 2>&1 || true
# GTK settings.ini: drop the keys install.sh / Settings write; the file goes when nothing else is left
for v in 3.0 4.0; do
  ini="$HOME/.config/gtk-$v/settings.ini"
  [[ -f $ini ]] || continue
  sed -i -E '/^(gtk-application-prefer-dark-theme|gtk-icon-theme-name|gtk-cursor-theme-name|gtk-theme-name|gtk-font-name)=/d' "$ini"
  if ! grep -qvE '^\s*(\[Settings\])?\s*$' "$ini"; then rmv "$ini"; rmdir "${ini%/*}" 2>/dev/null || true
  else echo "  cleaned $ini (kept your other settings)"; fi
done
if command -v gsettings >/dev/null; then
  for k in color-scheme gtk-theme font-name document-font-name monospace-font-name; do
    gsettings reset org.gnome.desktop.interface "$k" 2>/dev/null || true
  done
  echo "  reset the GNOME interface theme and fonts (gsettings)"
fi
rmv "$HOME/.config/qt6ct"
for d in "$HOME"/.mozilla/firefox/*.default* "$HOME"/.config/mozilla/firefox/*.default*; do
  [[ -f $d/user.js ]] || continue
  sed -i '/widget.use-xdg-desktop-portal.file-picker/d' "$d/user.js"
  [[ -s $d/user.js ]] || rm -f "$d/user.js"
done
pol=/etc/firefox/policies/policies.json
if [[ -f $pol ]] && [[ $(tr -d ' \n' <"$pol") == '{"policies":{"Preferences":{"widget.use-xdg-desktop-portal.file-picker":{"Value":1,"Status":"default"}}}}' ]]; then
  sudo rm -f "$pol"; sudo rmdir /etc/firefox/policies /etc/firefox 2>/dev/null || true; echo "  removed $pol"
fi

# ------------------------------------------------------------------ hotspot helper
step "Hotspot helper"
sudo rm -rf /usr/local/lib/hyprminimal /etc/polkit-1/rules.d/50-hyprminimal-hotspot.rules
echo "  removed /usr/local/lib/hyprminimal and its polkit rule"

# ------------------------------------------------------------------ links, caches, state
step "Commands, caches and state"
for b in "$HOME"/.local/bin/hyprminimal*; do
  [[ -L $b && $(readlink "$b") == "$LINK"/* ]] && rmv "$b"
done
[[ -L $LINK ]] && rmv "$LINK"
# the hyprlock.conf link Settings drops in ~/.config/hypr (left behind when that dir was not ours)
lk="$HYPR/hyprlock.conf"
[[ -L $lk && $(readlink "$lk") == "$DIR"/* ]] && rmv "$lk"
rmv "$HOME/.cache/hyprminimal"
rmv "${XDG_STATE_HOME:-$HOME/.local/state}/hyprminimal"
rmv "${XDG_RUNTIME_DIR:-/tmp}/hyprminimal"
rmv "$DIR/.generated"
rmv "$DIR/settings.json"

# ------------------------------------------------------------------ packages (opt-in)
if (( PACKAGES )); then
  step "Removing hyprminimal-only packages"
  # needed by stock Hyprland, the network, or to run ./install.sh again. python: hyprminimal no longer
  # installs it (older versions did), but much other software depends on it, so it is never removed here
  keep=" hyprland kitty networkmanager wireplumber python gcc pkgconf gtk4 json-c noto-fonts "
  mapfile -t pk < <(awk '!/^[[:space:]]*(#|$)/ {print $1}' "$DIR/packages.txt")
  rm_pk=()
  for p in "${pk[@]}"; do [[ $keep == *" $p "* ]] || ! pacman -Qq "$p" >/dev/null 2>&1 || rm_pk+=("$p"); done
  if (( ${#rm_pk[@]} )); then
    # -u: skips any package something else still depends on, instead of failing
    sudo pacman -Rnsu --noconfirm "${rm_pk[@]}" || warn "pacman could not remove some packages (see above)"
  else
    echo "  none installed"
  fi
fi

# ------------------------------------------------------------------ repo (opt-in, last)
if (( PURGE )); then
  step "Deleting the repo"
  cd "$HOME"
  rm -rf -- "$DIR"; echo "  removed $DIR"
fi

step "Done"
cat <<EOF
  hyprminimal is removed. Reboot:   sudo reboot
  tty1 is the plain TTY login now: log in, then run  start-hyprland  (stock Hyprland).
$( ((PURGE)) && echo "  To install again: git clone <repo> && cd hyprminimal && ./install.sh" || echo "  To install again: cd $DIR && ./install.sh")
EOF
[[ -n $bak && ! -e $HYPR ]] && echo "  Your pre-hyprminimal config is still in $bak (./uninstall.sh --restore would have put it back)."
exit 0
