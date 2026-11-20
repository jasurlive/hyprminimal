# hyprminimal

Hyprland for typing and coding. Not for gamers.
No wallpaper, no blur, no animations. Official Arch packages only.

<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/eefb15bc-68b6-4ba6-a3c8-8cec93c29ac0" />

## Install

```bash
sudo pacman -S --needed git
git clone https://github.com/jasurlive/hyprminimal ~/hyprminimal
cd ~/hyprminimal && ./install.sh     # then: sudo reboot
```

Options: `--no-autostart`, `--reset-hypr`, `--no-gpu`, `-y`.

```bash
hyprminimal start | stop | status | restart
```

Logs: `$XDG_RUNTIME_DIR/hyprminimal/*.log`. Help: [GUIDE.md](GUIDE.md).

## Keys

| Key | What |
|---|---|
| Super+R | Launcher (`rm NAME` uninstalls) |
| Super+S | Control center |
| Super+I | Settings (type to search) |
| Super+H | Keybinds |
| Super+L | Lock |

## Parts

| Part | Program | Config |
|---|---|---|
| Bar | waybar | `waybar/` |
| Notifications | swaync | `swaync/` |
| OSD | swayosd | `swayosd/` |
| Launcher, session menu | rofi | `rofi/` |
| Lock | hyprlock | `hyprlock/` |
| Bar popups | C helper | `helpers/panels.c` |
| Settings | hyprminimal-settings | `settings/`, `settings.json` |

Keybind edits are backed up to `~/.config/hypr/.pre-hyprminimal-backup/`.
