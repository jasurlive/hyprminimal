# hyprminimal

Hyprland theme for endless typing and coding.
No wallpaper, no blur, no animations. Official Arch packages only.

<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/62f3eec3-b9be-4a82-b8e4-9e8d4b430a98" />

## Install

```bash
sudo pacman -S --needed git
git clone https://github.com/jasurlive/hyprminimal ~/hyprminimal
cd ~/hyprminimal && ./install.sh     # then: sudo reboot
```

Options: `--no-autostart`, `--reset-hypr`, `--no-gpu`, `-y`.

If you hate this theme clean it up:

Uninstall: `./uninstall.sh` (back to stock Hyprland; `--purge` also deletes the repo). See [GUIDE.md §6](GUIDE.md#6-reset-undo-uninstall).

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
