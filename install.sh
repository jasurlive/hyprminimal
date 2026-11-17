#!/usr/bin/env bash
# hyprminimal installer: fresh Arch Linux -> ready-to-use Hyprland + hyprminimal desktop.
#
#   git clone <repo> ~/hyprminimal && cd ~/hyprminimal && ./install.sh
#
# Run it as your normal user (it asks for sudo). Safe to run again: it only adds what is missing.
#   --no-autostart don't start Hyprland when you log in on tty1 (start it yourself: start-hyprland)
#   --reset-hypr  replace ~/.config/hypr with hyprminimal's defaults again (old one is backed up)
#   --reset-kitty replace ~/.config/kitty/kitty.conf with hyprminimal's again (old one is backed up)
#   --no-gpu      don't install GPU drivers
#   -y, --yes     don't ask questions
set -euo pipefail

DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
LINK="$HOME/.local/share/hyprminimal"   # stable path every config uses, wherever the repo was cloned
HYPR="$HOME/.config/hypr"
WALL="$HOME/Pictures/Wallpapers/hyprminimal.png"
AUTOSTART=1 RESET_HYPR=0 RESET_KITTY=0 GPU=1 YES=0

for a in "$@"; do
  case $a in
    --no-autostart) AUTOSTART=0 ;;
    --reset-hypr) RESET_HYPR=1 ;;
    --reset-kitty) RESET_KITTY=1 ;;
    --no-gpu) GPU=0 ;;
    -y|--yes) YES=1 ;;
    -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $a (see --help)"; exit 2 ;;
  esac
done

c_b=$'\e[1;34m' c_y=$'\e[1;33m' c_r=$'\e[1;31m' c_0=$'\e[0m'
step() { printf '\n%s==>%s %s\n' "$c_b" "$c_0" "$*"; }
warn() { printf '%s warning:%s %s\n' "$c_y" "$c_0" "$*"; }
die()  { printf '%s error:%s %s\n' "$c_r" "$c_0" "$*" >&2; exit 1; }
ask()  { (( YES )) && return 0; local r; read -rp "$1 [Y/n] " r; [[ ${r,,} != n* ]]; }

# ------------------------------------------------------------------ checks
[[ $EUID != 0 ]] || die "run as your normal user, not root (it uses sudo where needed)"
command -v pacman >/dev/null || die "this installer is for Arch Linux (pacman not found)"
command -v sudo >/dev/null || die "sudo is missing: as root run 'pacman -S sudo' and add your user to wheel (EDITOR=nano visudo)"
step "Asking for sudo once (kept alive while installing)"
sudo -v || die "sudo failed: is $USER allowed to use sudo?"
while true; do sudo -n true; sleep 50; kill -0 "$$" 2>/dev/null || exit; done 2>/dev/null &
KEEPALIVE=$!
trap 'kill $KEEPALIVE 2>/dev/null' EXIT

# ------------------------------------------------------------------ packages
BASE=(
  # session plumbing
  xdg-desktop-portal-hyprland xdg-desktop-portal-gtk hyprpolkitagent polkit gnome-keyring
  qt5-wayland qt6-wayland xdg-user-dirs xdg-utils pciutils
  # audio
  pipewire pipewire-pulse pipewire-alsa wireplumber sof-firmware
  # bluetooth
  bluez
  # apps the default keybinds open
  thunar gvfs firefox
  # screenshots, clipboard, displays
  grim slurp hyprshot satty wl-clipboard wl-clip-persist nwg-displays
  # look
  noto-fonts-emoji ttf-jetbrains-mono-nerd adwaita-cursors adwaita-icon-theme gnome-themes-extra
)
mapfile -t SHELL_PKGS < <(awk '!/^[[:space:]]*(#|$)/ {print $1}' "$DIR/packages.txt")

GPU_PKGS=()
if (( GPU )); then
  gpus=$(lspci -nn 2>/dev/null | grep -Ei 'vga|3d|display' || true)
  [[ $gpus ]] || { sudo pacman -S --needed --noconfirm pciutils >/dev/null && gpus=$(lspci -nn | grep -Ei 'vga|3d|display' || true); }
  GPU_PKGS+=(mesa vulkan-icd-loader)
  grep -qi intel <<<"$gpus" && GPU_PKGS+=(vulkan-intel intel-media-driver)
  grep -qiE 'amd|ati|radeon' <<<"$gpus" && GPU_PKGS+=(vulkan-radeon)
  if grep -qi nvidia <<<"$gpus"; then
    GPU_PKGS+=(nvidia-open-dkms nvidia-utils egl-wayland libva-nvidia-driver)
    for pb in /usr/lib/modules/*/pkgbase; do [[ -f $pb ]] && GPU_PKGS+=("$(<"$pb")-headers"); done
    NVIDIA=1
  fi
fi

step "Updating the system and installing packages"
echo "  shell: ${#SHELL_PKGS[@]}  desktop: ${#BASE[@]}  gpu: ${GPU_PKGS[*]:-none}"
sudo pacman -Syu --needed --noconfirm "${SHELL_PKGS[@]}" "${BASE[@]}" "${GPU_PKGS[@]}" \
  || die "pacman failed (see above). Fix it and run ./install.sh again."
if [[ ${NVIDIA:-} ]]; then
  warn "NVIDIA: nvidia-open needs a GTX 16xx / RTX 20xx or newer. For older cards install the matching driver yourself."
fi

# ------------------------------------------------------------------ repo link + commands
step "Linking hyprminimal to $LINK"
mkdir -p "$(dirname "$LINK")" "$HOME/.local/bin"
if [[ $(readlink -f "$LINK") != "$DIR" ]]; then
  if [[ -e $LINK && ! -L $LINK ]]; then
    die "$LINK exists and is not a link to this repo. Move it away and run again."
  fi
  ln -sfn "$DIR" "$LINK"
fi
for b in "$DIR"/bin/*; do ln -sfn "$LINK/bin/${b##*/}" "$HOME/.local/bin/${b##*/}"; done
chmod +x "$DIR"/bin/* "$DIR"/scripts/*.sh "$DIR"/hypr/config/Scripts/*.sh

# ------------------------------------------------------------------ folders + wallpaper
step "Creating folders and the default wallpaper"
xdg-user-dirs-update 2>/dev/null || true
mkdir -p "$HOME/Pictures/Wallpapers" "$HOME/Pictures/Screenshots"
if [[ ! -f $WALL ]]; then
  # Tokyo Night Moon: soft glow in the middle, darker edges
  magick -size 4200x4200 radial-gradient:'#3b4261'-'#1b1d2b' -gravity center -crop 3840x2160+0+0 +repage \
    -depth 8 "$WALL"
fi

# ------------------------------------------------------------------ Hyprland config
step "Installing the Hyprland config to $HYPR"
install_hypr() {
  mkdir -p "$HYPR/Scripts"
  cp -r "$DIR"/hypr/config/. "$HYPR/"
  chmod +x "$HYPR"/Scripts/*.sh
  echo "installed from $DIR/hypr/config on $(date -I)" > "$HYPR/.hyprminimal"
}
if [[ -f $HYPR/.hyprminimal && $RESET_HYPR == 0 ]]; then
  # ours already: keep the user's edits, only add files that are missing
  cp -rn "$DIR"/hypr/config/. "$HYPR/"
  echo "  already installed, kept your edits (--reset-hypr to start over)"
elif [[ -d $HYPR && -n $(ls -A "$HYPR" 2>/dev/null) ]]; then
  bak="$HOME/.config/hypr.pre-hyprminimal-$(date +%Y%m%d-%H%M%S)"
  if ask "  $HYPR already has a config. Move it to $bak and use hyprminimal's?"; then
    mv "$HYPR" "$bak"; install_hypr; echo "  old config saved in $bak"
  else
    warn "kept your config. To use hyprminimal from it, add to hyprland.lua:"
    echo "      pcall(dofile, os.getenv(\"HOME\") .. \"/.local/share/hyprminimal/hypr/layerrules.lua\")"
    echo "    and start it at login: $LINK/bin/hyprminimal start   (see hypr/config/ for the full setup)"
  fi
else
  install_hypr
fi

# ------------------------------------------------------------------ terminal (kitty + bash)
step "Installing the kitty terminal config"
KITTY="$HOME/.config/kitty/kitty.conf"
mkdir -p "${KITTY%/*}"
if [[ -f $KITTY ]] && grep -q '^# hyprminimal kitty config' "$KITTY" && (( ! RESET_KITTY )); then
  echo "  already installed, kept your edits (--reset-kitty to start over)"
elif [[ -s $KITTY ]]; then
  bak="$KITTY.pre-hyprminimal-$(date +%Y%m%d-%H%M%S)"
  if (( RESET_KITTY )) || ask "  $KITTY already exists. Move it to $bak and use hyprminimal's?"; then
    mv "$KITTY" "$bak"; cp "$DIR/kitty/kitty.conf" "$KITTY"; echo "  old config saved in $bak"
  else
    warn "kept your kitty config. For hyprminimal colours add to it:"
    echo "      include ~/.local/share/hyprminimal/kitty/colors.conf"
  fi
else
  cp "$DIR/kitty/kitty.conf" "$KITTY"
fi
pgrep -x kitty >/dev/null && pkill -USR1 -x kitty || true   # live-reload open terminals

# ------------------------------------------------------------------ dark theme for GTK/Qt apps
mkdir -p "$HOME/.config/gtk-3.0" "$HOME/.config/gtk-4.0"
for v in 3.0 4.0; do
  f="$HOME/.config/gtk-$v/settings.ini"
  [[ -f $f ]] || printf '[Settings]\ngtk-application-prefer-dark-theme=1\ngtk-icon-theme-name=Papirus-Dark\ngtk-cursor-theme-name=Adwaita\n' > "$f"
done
# Qt apps: dark palette through qt6ct (startup.lua sets QT_QPA_PLATFORMTHEME=qt6ct)
mkdir -p "$HOME/.config/qt6ct"
f="$HOME/.config/qt6ct/qt6ct.conf"
grep -qs '^\[Appearance\]' "$f" || { printf '[Appearance]\ncolor_scheme_path=/usr/share/qt6ct/colors/darker.conf\ncustom_palette=true\nicon_theme=Papirus-Dark\nstandard_dialogs=default\nstyle=Fusion\n\n'; cat "$f" 2>/dev/null; } > "$f.new" && mv "$f.new" "$f"

# ------------------------------------------------------------------ settings render
step "Rendering hyprminimal settings"
"$DIR/bin/hyprminimal-settings" --apply || warn "settings render failed; it is retried at every start"

# ------------------------------------------------------------------ services
step "Enabling services"
# NetworkManager drives the Wi-Fi panel: hand networking over to it from the other managers on next boot
for s in systemd-networkd iwd dhcpcd netctl; do
  if systemctl is-enabled -q "$s" 2>/dev/null; then
    warn "$s is enabled; disabling it so NetworkManager manages the network after reboot"
    sudo systemctl disable "$s" 2>/dev/null || true
  fi
done
sudo systemctl enable NetworkManager.service
systemctl is-active -q NetworkManager || systemctl is-active -q systemd-networkd || systemctl is-active -q iwd \
  || systemctl is-active -q dhcpcd || sudo systemctl start NetworkManager.service || true
sudo systemctl enable bluetooth.service power-profiles-daemon.service
systemctl --user enable pipewire.socket pipewire-pulse.socket wireplumber.service 2>/dev/null || true

# ------------------------------------------------------------------ login: plain TTY, no display manager
# an old hyprminimal greetd login screen would take tty1: turn it off (the TTY login prompt comes back)
if [[ -d /usr/local/share/hyprminimal-greeter ]] || grep -qs hyprminimal /etc/greetd/config.toml; then
  step "Removing the old hyprminimal login screen (greetd)"
  sudo systemctl disable greetd.service 2>/dev/null || true
  sudo rm -rf /usr/local/share/hyprminimal-greeter /var/cache/hyprminimal-greeter
  b=/etc/greetd/config.toml.pre-hyprminimal-greeter
  [[ -f $b ]] && sudo mv "$b" /etc/greetd/config.toml
  rm -f "$DIR/.generated/greeter.css" "$DIR/.generated/greeter_wall"
fi
for dm in greetd gdm sddm lightdm ly lxdm; do
  systemctl is-enabled -q "$dm" 2>/dev/null && warn "$dm is enabled and takes tty1; for the TTY login: sudo systemctl disable $dm"
done
if (( AUTOSTART )); then
  step "Starting Hyprland at login on tty1 (~/.bash_profile)"
  f="$HOME/.bash_profile"
  if grep -qs '^# hyprminimal autostart' "$f"; then
    echo "  already there"
  else
    printf '\n# hyprminimal autostart: log in on tty1 -> Hyprland\nif [[ -z $WAYLAND_DISPLAY && $(tty) == /dev/tty1 ]]; then\n  exec start-hyprland\nfi\n' >> "$f"
  fi
  [[ ${SHELL##*/} == bash ]] || warn "your login shell is $SHELL, not bash: add 'exec start-hyprland' on tty1 to its profile yourself"
fi

# ------------------------------------------------------------------ done
step "Done"
cat <<EOF
  hyprminimal is installed. Reboot and log in on tty1:   sudo reboot
$( (( AUTOSTART )) || echo "  (no autostart: after logging in, run start-hyprland)")

  Keys:  Super+Space  terminal · Super+R launcher · Super+S control center · Super+I settings
         Super+H all keybinds · Super+L lock · Super+Q close · Super+Esc power menu
  Commands are linked in ~/.local/bin (hyprminimal, hyprminimal-settings, ...).
EOF
if [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} && -f $HYPR/.hyprminimal ]]; then
  ask "  Hyprland is running: reload it and start hyprminimal now?" && {
    hyprctl reload >/dev/null; "$DIR/bin/hyprminimal" start; }
fi
