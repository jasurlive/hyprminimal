# hyprminimal

Hyprland theme for endless typing and coding.
No wallpaper, no blur, no animations. Official pacman packages only.

Surprise: the whole desktop runs on just 4 programs (hyprland, waybar, rofi and hyprlock). Popups, notifications and OSD are a small built-in C helper.

<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/748b03a2-a375-4b7c-89e7-4a4d3684be53" />

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

Logs: `$XDG_RUNTIME_DIR/hyprminimal/*.log`. Help: [GUIDE.md](GUIDE.md). Shortcuts: [KEYBINDINGS.md](KEYBINDINGS.md).

## Keys

The ones to learn first. **All shortcuts: [KEYBINDINGS.md](KEYBINDINGS.md)**, or press Super+H.

| Key | What |
|---|---|
| Super+Space | Terminal |
| Super+R | Launcher (`rm NAME` uninstalls) |
| Super+S | Control center |
| Super+I | Settings (type to search) |
| Super+H | Keybinds (view, search, edit) |
| Super+Q | Close window |
| Super+1…0 | Switch workspace |
| Super+Escape | Session menu |
| Super+L | Lock |
| Print | Screenshot a region |

## Parts

| Part | Program | Config |
|---|---|---|
| Bar | waybar | `waybar/` |
| Popups, control center, notifications, OSD, launcher, session menu | C helper (GTK4) | `helpers/panels.c`, `helpers/panels/`, `settings/panels.css` |
| Launcher, session menu fallback | rofi | `rofi/` |
| Lock | hyprlock | `hyprlock/` |
| Settings | hyprminimal-settings | `settings/`, `settings.json` |

Keybind edits are backed up to `~/.config/hypr/.pre-hyprminimal-backup/`.
