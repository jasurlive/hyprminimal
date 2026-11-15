# hyprminimal
A Noctalia look-alike built only from official Arch packages (no AUR).

<img width="60%" alt="image" src="https://github.com/user-attachments/assets/77092ad3-4b8d-40e1-b797-7e412b52f5ae" />
<img width="60%" alt="image" src="https://github.com/user-attachments/assets/6446f98f-2204-4486-8a6f-7316f1f0f23a" />
<img width="60%" alt="image" src="https://github.com/user-attachments/assets/54e8cbe3-6b15-4021-9689-34908ea93cd9" />
<img width="60%" alt="image" src="https://github.com/user-attachments/assets/73bcae76-5dfd-48e7-9324-5ec7e22e90a1" />
<img width="60%" alt="image" src="https://github.com/user-attachments/assets/ce25edcb-fce8-47e2-8cde-30835dd718b3" />
<img width="60%" alt="image" src="https://github.com/user-attachments/assets/6fb4a0a4-c44a-4506-af12-f2c4f04f3041" />
<img width="60%" alt="image" src="https://github.com/user-attachments/assets/fd4fcf6e-e3dd-4ea6-aa37-a491769895e3" />

| Noctalia | hyprminimal | Config |
|---|---|---|
| Bar: workspace pills · `15 \| 17:04` · media capsule, bell, battery, ↓ speed | waybar | `waybar/`, `scripts/media.sh` |
| Notifications (top right) | swaync | `swaync/` |
| Control center (Super+S, or click clock/bell): media, Wi-Fi, Bluetooth, Caffeine, Night Light, DND, System, Session | swaync | `swaync/` |
| Volume / brightness OSD | swayosd | `swayosd/` |
| Launcher (Super+R); `rm NAME` + Enter uninstalls an app (pacman -Rns + cache clean, asks first) | rofi | `rofi/theme.rasi`, `scripts/launcher.py`, `scripts/remove-app.sh` |
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

Super+H opens it on **Keybinds**. Each edit to `keybind.lua` is backed up to
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
~/www/hyprminimal/bin/hyprminimal start
~/www/hyprminimal/bin/hyprminimal stop
~/www/hyprminimal/bin/hyprminimal restart   # after editing a config
~/www/hyprminimal/bin/hyprminimal status
```

Logs: `$XDG_RUNTIME_DIR/hyprminimal/*.log`. hyprminimal starts at login.

Settings reference, crash recovery and troubleshooting: [GUIDE.md](GUIDE.md).

## Changes outside this folder

Originals are backed up in `~/.config/hypr/.pre-hyprminimal-backup/`.
- `keybind.lua`, `Scripts/accel-step.sh`: call `bin/hyprminimal-msg`.
- `hyprland.lua`: last line loads `hypr/layerrules.lua` (blur).
