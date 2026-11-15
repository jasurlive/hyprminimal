#!/usr/bin/env bash
# Uninstall an app picked in the launcher ("rm NAME" + Enter). Runs in a terminal, asks before every step.
#   remove-app.sh DESKTOP_FILE "Display name"
# pacman package: pacman -Rns (app + its now-unneeded dependencies + its /etc config), then pacman -Sc
# (drop cached package files of everything no longer installed). Flatpak: flatpak uninstall --delete-data.
# Finally offers to delete the app's own folders in ~/.config, ~/.local/share, ~/.cache, ~/.local/state.
set -u
desktop=$1 name=$2
id=$(basename "$desktop" .desktop)

bold=$'\e[1m' red=$'\e[31m' dim=$'\e[2m' off=$'\e[0m'
ask() { local a; read -r -p "$1 [y/N] " a; [[ $a == [yY]* ]]; }
done_wait() { echo; read -r -s -n1 -p "${dim}Press any key to close${off}"; exit "${1:-0}"; }

echo "${bold}Remove ${name}${off}  ${dim}(${desktop})${off}"
echo

kind="" pkg=""
if [[ $desktop == */flatpak/exports/* ]]; then
  kind=flatpak pkg=$id
elif pkg=$(pacman -Qoq "$desktop" 2>/dev/null); then
  kind=pacman
else
  echo "${red}$desktop does not belong to any pacman package or Flatpak.${off}"
  echo "It is a local shortcut; delete it by hand if you want it gone."
  done_wait 1
fi

# folder names the app may use for its own data (exact names only, checked case-insensitively)
names=("$pkg" "$id" "${id##*.}" "$name")
dirs=()
for base in "$HOME/.config" "$HOME/.local/share" "$HOME/.cache" "$HOME/.local/state" "$HOME/.var/app"; do
  [[ -d $base ]] || continue
  for n in "${names[@]}"; do
    [[ -n $n && $n != */* ]] || continue
    while IFS= read -r d; do
      [[ " ${dirs[*]} " == *" $d "* ]] || dirs+=("$d")
    done < <(find "$base" -mindepth 1 -maxdepth 1 -type d -iname "$n" 2>/dev/null)
  done
done

if [[ $kind == pacman ]]; then
  echo "Package: ${bold}$pkg${off} $(pacman -Q "$pkg" | cut -d' ' -f2)"
  echo "Will also remove dependencies nothing else needs. pacman lists everything before it asks."
else
  echo "Flatpak: ${bold}$pkg${off}"
fi
echo
ask "Uninstall ${name}?" || { echo "Cancelled."; done_wait; }
echo

if [[ $kind == pacman ]]; then
  sudo pacman -Rns "$pkg" || { echo "${red}pacman did not remove $pkg.${off}"; done_wait 1; }
  echo
  echo "${bold}Cleaning the package cache${off} (cached files of packages that are no longer installed)"
  sudo pacman -Sc --noconfirm
else
  flatpak uninstall --delete-data -y "$pkg" || { echo "${red}flatpak did not remove $pkg.${off}"; done_wait 1; }
  flatpak uninstall --unused -y
fi

if (( ${#dirs[@]} )); then
  echo
  echo "${bold}Leftover user data:${off}"
  for d in "${dirs[@]}"; do printf '  %s  %s\n' "$(du -sh "$d" 2>/dev/null | cut -f1)" "$d"; done
  if ask "Delete these folders too?"; then rm -rf -- "${dirs[@]}" && echo "Deleted."; else echo "Kept."; fi
fi

echo
echo "${bold}${name} removed.${off}"
done_wait
