# hyprminimal
A Noctalia look-alike built only from official Arch packages (no AUR).

<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/eefb15bc-68b6-4ba6-a3c8-8cec93c29ac0" />

| Noctalia | hyprminimal | Config |
|---|---|---|
| Bar: workspace pills · `15 \| 17:04` · media capsule, bell, battery, ↓ speed | waybar | `waybar/`, `scripts/media.sh` |
| Notifications (top right) | swaync | `swaync/` |
| Control center (Super+S, or click clock/bell): Wi-Fi, network list, System and toggles on keys 1–7, session buttons, media (also a click on the media capsule), notifications | swaync | `swaync/` |
| Volume / brightness OSD | swayosd | `swayosd/` |
| Launcher (Super+R); `rm NAME` + Enter uninstalls an app (pacman -Rns + cache clean, asks first) | rofi | `rofi/theme.rasi`, `helpers/launcher.c`, `scripts/remove-app.sh` |
| Session panel (Lock, Lock & Suspend, Reboot, Shut Down, keys 1-4) | rofi | `rofi/menu.rasi`, `scripts/session.sh` |
| Lock screen (Super+L) | hyprlock | `hyprlock/` |
| Wallpaper | swaybg | Settings → Wallpaper |
| Glass blur | Hyprland layer rules | `hypr/layerrules.lua` |
| Settings (Super+I): bar, wallpaper, notifications, lock screen, night light, system | hyprminimal-settings | `settings/`, `settings.json` |
| Keybinds (Super+H): search, edit, record, add, remove shortcuts | hyprminimal-settings | edits `~/.config/hypr/keybind.lua` |
| Bar popups: click clock/date (calendar), battery (power mode), ↓ speed (network), CPU (system) | C helper (GTK4) | `helpers/panels.c` |
| Wi-Fi network list (control center ›) | hyprminimal-settings | `settings/panels.py` |

## Settings

`bin/hyprminimal-settings` (Super+I) is the one place to change hyprminimal. Every change is saved to
`settings.json`, rendered into `.generated/` and applied live. There is one theme, Tokyo Night Moon.

Super+H opens it on **Keybinds**. Each edit to `keybind.lua` is backed up to
`~/.config/hypr/.pre-hyprminimal-backup/`, Hyprland is reloaded, and the change is undone if Hyprland reports
an error. Removed shortcuts are commented out, not deleted. Shortcuts defined in a loop (Super+1…9) are
shown but must be edited in the file.

## Performance

The bar popups (calendar, battery, network, system) are one small resident C program (`helpers/panels.c`,
GTK4 with software rendering, ~22 MB of its own, 0 wakeups while hidden). It reads `/proc` and `/sys`
directly, gets the power mode from power-profiles-daemon over D-Bus, and the clock ticks on the second
boundary. The settings app is started (hidden) by `hyprminimal start` too, so Super+I, Super+H and bar
clicks are a D-Bus call: about 30-80 ms to show. Bar, notifications, OSD and wallpaper
are native programs (C++, Vala, Rust, C) idling near 0% CPU.
The bar's network speed and the watchdog are small C helpers (`helpers/`): no polling forks, the bar is only
redrawn when its text changes, and nothing animates while music plays (an animated bar is redrawn and re-blurred
every frame). CPU/RAM widgets update every 5 s.

## Install

On a fresh Arch Linux (any machine, as your normal user with sudo):

```bash
sudo pacman -S --needed git
git clone https://github.com/jasurlive/hyprminimal ~/hyprminimal
cd ~/hyprminimal && ./install.sh     # then: sudo reboot
```

It installs Hyprland, hyprminimal and a base desktop (PipeWire, NetworkManager, Bluetooth, GPU drivers,
Firefox, Thunar, kitty) from the official repos only, puts a default Hyprland config in `~/.config/hypr`
(an existing one is backed up first), and starts Hyprland when you log in on tty1 (plain TTY login, no display manager). Options:
`--no-autostart`, `--reset-hypr`, `--no-gpu`, `-y` (`./install.sh --help`). Re-running it is safe.

The repo can live anywhere: it is linked at `~/.local/share/hyprminimal`, and the commands in `~/.local/bin`.

```bash
hyprminimal start | stop | status
hyprminimal restart   # after editing a config
```

Logs: `$XDG_RUNTIME_DIR/hyprminimal/*.log`. hyprminimal starts at login.

Settings reference, crash recovery and troubleshooting: [GUIDE.md](GUIDE.md).

## Changes outside this folder

The default Hyprland config is in `hypr/config/`. Settings backs up `keybind.lua` edits in
`~/.config/hypr/.pre-hyprminimal-backup/`.
- `keybind.lua`, `Scripts/accel-step.sh`: call `bin/hyprminimal-msg`.
- `hyprland.lua`: last line loads `hypr/layerrules.lua` (blur).
