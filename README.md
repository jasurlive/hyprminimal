# hyprminimal

A Noctalia look-alike built only from official Arch packages (no AUR).
It copies your **active** Noctalia setup: Tokyo Night Moon colours, square panels, glass blur.

| Noctalia | hyprminimal | Config |
|---|---|---|
| Bar: workspace pills · `15 \| 17:04` · media capsule, bell, battery, ↓ speed | waybar | `waybar/`, `scripts/media.sh` |
| Notifications (top right) | swaync | `swaync/` |
| Control center (Super+S, or click clock/bell): media, Wi-Fi, Bluetooth, Caffeine, Night Light, DND, System, Session | swaync | `swaync/` |
| Volume / brightness OSD | swayosd | `swayosd/` |
| Launcher (Super+R) | rofi | `rofi/theme.rasi` |
| Session panel (Lock, Log Out, Lock & Suspend, Reboot, Shut Down, keys 1-5) | rofi | `rofi/menu.rasi`, `scripts/session.sh` |
| Lock screen (Super+L), lock before suspend | hyprlock + hypridle | `hyprlock/`, `hypridle/` |
| Wallpaper | swaybg | Settings → Wallpaper |
| Glass blur | Hyprland layer rules | `hypr/layerrules.lua` |
| Settings (Super+I): bar, wallpaper, notifications, lock & idle, night light, system | hyprminimal-settings | `settings/`, `settings.json` |
| Keybinds (Super+H): search, edit, record, add, remove shortcuts | hyprminimal-settings | edits `~/.config/hypr/keybind.lua` |
| Bar popups: click clock/date (calendar), battery (power mode), ↓ speed (network), media (player) | hyprminimal-settings | `settings/panels.py` |

## Settings

`bin/hyprminimal-settings` (Super+I) is the one place to change hyprminimal. Every change is saved to
`settings.json`, rendered into `.generated/` and applied live. There is one theme, Tokyo Night Moon.

Super+H opens it on **Keybinds**, which works on Noctalia too. Each edit to `keybind.lua` is backed up to
`~/.config/hypr/.pre-hyprminimal-backup/`, Hyprland is reloaded, and the change is undone if Hyprland reports
an error. Removed shortcuts are commented out, not deleted. Shortcuts defined in a loop (Super+1…9) are
shown but must be edited in the file.

## Performance

The settings app and bar popups run in one resident process started (hidden) by `hyprminimal start`, so
Super+I, Super+H and bar clicks are a D-Bus call: about 30-80 ms to show. Bar, notifications, OSD and wallpaper
are native programs (C++, Vala, Rust, C) idling near 0% CPU.
The bar's network speed and the watchdog are small C helpers (`helpers/`): no polling forks, the bar is only
redrawn when its text changes, and nothing animates while music plays (an animated bar is redrawn and re-blurred
every frame). CPU/RAM widgets update every 5 s.

## Try it

```bash
sudo pacman -S --needed $(grep -v '^#' ~/www/hyprminimal/packages.txt)
~/www/hyprminimal/bin/hyprminimal start     # Noctalia stops, hyprminimal starts
~/www/hyprminimal/bin/hyprminimal stop      # back to Noctalia
~/www/hyprminimal/bin/hyprminimal restart   # after editing a config
~/www/hyprminimal/bin/hyprminimal status
```

Logs: `$XDG_RUNTIME_DIR/hyprminimal/*.log`. hyprminimal starts at login (falls back to Noctalia if it can't).

Settings reference, crash recovery and troubleshooting: [GUIDE.md](GUIDE.md).

## Changes outside this folder

Originals are backed up in `~/.config/hypr/.pre-hyprminimal-backup/`.
- `keybind.lua`, `Scripts/accel-step.sh`: call `bin/hyprminimal-msg`, which forwards to Noctalia unless hyprminimal is active.
- `hyprland.lua`: last line loads `hypr/layerrules.lua` (blur).
