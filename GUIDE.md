# hyprminimal guide

How to run hyprminimal, change every setting, and recover when something breaks.
For what hyprminimal is and how it's built, see [README.md](README.md).

- [1. Everyday use](#1-everyday-use)
- [2. Settings, all of them](#2-settings-all-of-them) (and [customizing the look](#customizing-the-look))
- [3. Keybinds](#3-keybinds)
- [4. Where things live](#4-where-things-live)
- [5. Crashes and problems](#5-crashes-and-problems)
- [6. Reset, undo, uninstall](#6-reset-undo-uninstall)

---

## 1. Everyday use

### Commands

| Command | What it does |
|---|---|
| `hyprminimal start` | Starts hyprminimal (bar, notifications, OSD, desktop background). It refuses to start if a package from `packages.txt` is missing and prints the `pacman` line to install it. |
| `hyprminimal stop` | Stops hyprminimal, for this session only. |
| `hyprminimal restart` | Restarts everything, including the settings process. Run it after editing any file in this folder by hand. |
| `hyprminimal respawn NAME` | Restarts one component: `waybar`, `swaync` or `swayosd-server`. |
| `hyprminimal status` | Shows whether hyprminimal is active and which components are running. |
| `hyprminimal-settings` | Opens Settings. `--page NAME` opens a specific page, and `--apply` regenerates the configs without opening a window. |

All of these are in `~/www/hyprminimal/bin/`.

### hyprminimal as the default shell

`~/.config/hypr/startup.lua` starts hyprminimal at every login:

```lua
[[sh -c '"$HOME/www/hyprminimal/bin/hyprminimal" start']],
```

### Login screen

tty1 shows the lock screen as a login screen (greetd → cage → `greeter/greeter.c`, a small GTK4 program):
clock, date, the user (↑/↓ or the arrows switch users) and a password field, then Hyprland starts.
Colours, font and sizes come from your theme's `hyprlock.conf`: Settings rewrites the greeter's copy
(`/usr/local/share/hyprminimal-greeter/hyprlock.conf`, owned by you) on every theme change.

```sh
sudo ~/.local/share/hyprminimal/bin/hyprminimal-greeter install     # also after editing greeter.c
sudo ~/.local/share/hyprminimal/bin/hyprminimal-greeter uninstall   # back to the TTY login
~/.local/share/hyprminimal/bin/hyprminimal-greeter status
```

Preview without logging out: `gcc -o /tmp/g greeter/greeter.c $(pkg-config --cflags --libs gtk4 json-c) &&
GREETER_CONF=.generated/hyprlock.conf /tmp/g --test` (password `test`, Esc quits).
`./install.sh --tty-login` keeps the plain TTY login instead.

### Keys

| Key | Action |
|---|---|
| Super+R | Launcher. Apps are numbered and the most recently used come first. Type `rm NAME` (or `rm NUMBER`) + Enter to uninstall that app: a terminal shows what will go and asks first, then runs `pacman -Rns` (app, unneeded dependencies, its /etc config), `pacman -Sc` (cache) and offers to delete its folders in ~/.config, ~/.local/share, ~/.cache. Flatpaks use `flatpak uninstall --delete-data`. |
| Super+S | Control center: Wi-Fi, network list, System and toggles on keys 1–7, Lock / Reboot / Shut Down, media (K: play/pause), notifications (C: clear all) |
| Super+W | Wi-Fi popup |
| Super+D | Calendar popup |
| Super+P | Battery / power mode popup |
| Super+C | VS Code (`code`) |
| Super+I | Settings (press again to hide it) |
| Super+H | Keybinds editor |
| Super+L | Lock screen |
| Media, volume and brightness keys | Control playback, volume and brightness, with an OSD |

### Bar clicks

| Click | Opens |
|---|---|
| Clock or date | Calendar with the time, centred under the clock. Hovering shows it, moving away hides it; a click pins it open (click again, click outside or Esc to close). Right click: control center. |
| Battery | Power mode |
| ↓ speed | Network |
| Media capsule (square-dot equalizer while playing, flat dots while paused) | Control center (player) |
| Bell | Control center |

### Session menu (keys 1–4)

The menu is opened from the control center. Its entries:

1. Lock
2. Lock & Suspend
3. Reboot
4. Shut Down

### Control center keys (1–7)

While the control center is open, a number key presses a tile. Each tile shows its number.

1. **Wi-Fi** turns the radio on or off.
2. **›** (next to Wi-Fi) lists networks.
3. **System** opens CPU and memory details.
4. **Bluetooth**
5. **Caffeine** blocks sleep and lid-close until you turn it off.
6. **Night Light** turns `hyprsunset` on or off at the temperature set in Settings.
7. **Silent** hides notification popups. Notifications still collect in the control center.

Notifications sit under the buttons. The list opens collapsed to one card; click the
"N notifications" header to show them all.

---

## 2. Settings, all of them

Open Settings with **Super+I**. Each change is:

1. saved to `settings.json`,
2. rendered into `.generated/`, then
3. applied live, without a restart.

You can also edit `settings.json` by hand and then run `hyprminimal restart`.
Any key that is missing from the file falls back to its default.

### General
| Setting | Key | Default | Notes |
|---|---|---|---|
| Interface font | `general.font` | `Noto Sans` | Used by the bar, panels, notifications and lock screen. |
| Theme | General | Charcoal | Charcoal, Hygge, Spruce, Fjord (dark), Linen, Matte, Sage (light), all from the django-react template, or Tokyo Night. Bar, popups, notifications, OSD, launcher, kitty, lock screen, login screen, Hyprland borders and Settings follow it. See [Customizing the look](#customizing-the-look). |
| Keep Settings ready | `general.preload` | On | Settings starts at login, parked on a hidden workspace: Super+I only moves it in or out (~20 ms, no redraw; ~110 MB RAM). Super+I closes it whenever it shows on your workspace, wherever the keyboard focus is. Off: cold start on first use (~0.5 s), and it quits 10 min after you close it. Takes effect live. |

### Bar
| Setting | Key | Range / values | Default |
|---|---|---|---|
| Text size | `bar.font_size` | 11–24 px | 16 |
| Icon size | `bar.icon_size` | 12–32 px | 20 |
| Clock size | `bar.clock_size` | 11–32 px | 18 |
| Weight | `bar.font_weight` | 400 Regular, 500 Medium, 600 Semibold, 700 Bold | 500 |
| 24-hour clock | `bar.clock_24h` | true / false | true |

**Widgets** (`bar.widgets.*`, each one true or false):

| Side | Widgets |
|---|---|
| Left | `workspaces` |
| Center | `date`, `clock` |
| Right | `media`, `notifications`, `wifi`, `bluetooth`, `volume`, `tray`, `battery`, `network_speed` |

`tray` shows the icons of apps that offer one, such as Telegram, Discord or Steam.

### Desktop background
No wallpaper and no wallpaper program. Hyprland paints the theme's background colour (`misc.background_color`
in `.generated/hypr-theme.lua`), and the bar popup program (`helpers/panels.c`) draws an Arch logo and
a name (default "hyprminimal") on a background layer, once per monitor. Both follow the theme live.

| Setting | Key | Default | Notes |
|---|---|---|---|
| Name under the logo | `desktop.name` | `hyprminimal` | Any text; empty = no text. Saved with Enter or the ✓ button. |
| Logo | `desktop.logo` | On | Off with an empty name: a plain theme colour, nothing else. |

Both apply live (Settings writes `.generated/desktop`; the popup program watches it and redraws the desktop). The logo is
`.generated/arch-logo.png`, drawn by `arch_logo()` in `settings/hyprminimal_settings.py` from the Symbols
Nerd Font glyph without its ™ sign; change the wordmark in `settings/panels.css` (`.bg-word`).

### Notifications
| Setting | Key | Range / values | Default |
|---|---|---|---|
| Position | `notifications.position` | `top-` or `bottom-` + `left`/`center`/`right` | `top-right` |
| Show for | `notifications.timeout` | 1–60 s | 3 |
| Do not disturb | — | live toggle | off |

### Lock Screen
Plain theme colour behind the clock and the password card, like the desktop: no image, no blur
(`hyprlock/hyprlock.conf`, recoloured per theme into `.generated/hyprlock.conf`). The page has a Lock now button.

hyprminimal has no idle daemon: the screen never locks, turns off or suspends by itself. Lock with **Super+L**.

### Startup
Commands to run at login, stored in `startup.commands` as `{"command": "...", "enabled": true}`.
Anything you would type in a terminal works: an app (`telegram-desktop`), a script (`~/bin/sync.sh`,
or pick one with the folder button) or a shell line (`sleep 5 && discord`).
Each row has a switch (run at login or not), an editable command (press Enter or ✓ to save),
▶ to run it now, and a trash button to remove it.

`hyprminimal start` runs the enabled ones **once per login** (per Hyprland session, so also after a
log out and log in), from your home folder, after the bar is up. `hyprminimal restart`, or `stop` + `start`,
never starts them a second time in the same session.
Output goes to `$XDG_RUNTIME_DIR/hyprminimal/startup.log`.

### Network
- **Airplane mode**: turns Wi-Fi and mobile radios off together.
- **Wi-Fi**: on/off; the current network (signal, password show/copy/change, IP, gateway, DNS, MAC,
  auto-connect, disconnect, forget); nearby networks (connect, password prompt when needed); hidden networks;
  saved networks that are out of range.
- **Ethernet**: every wired adapter, including USB tethering from a phone: connect/disconnect, link speed,
  IP details, auto-connect.
- **Hotspot**: share this computer's internet over Wi-Fi. Name, password (copy button) and band (2.4/5 GHz)
  can be changed; while it's on, the connected devices are listed. If the adapter can join a network and
  host a hotspot at once (`iw list` shows `#{ managed }` and `#{ AP }` together), the hotspot runs on a second
  interface (`ap0`, hostapd + dnsmasq, run by `scripts/hotspot-iface`) on the network's channel and Wi-Fi stays
  connected, so it's shared too; the band then follows the network. Otherwise Wi-Fi disconnects while the hotspot
  is on. Needs `dnsmasq` (and `hostapd` for side by side); `install.sh` installs a root-owned copy of the helper
  in `/usr/local/lib/hyprminimal/` and a polkit rule so wheel users run it without a password.
- **VPN & other connections**: WireGuard, OpenVPN, mobile broadband and Bluetooth tethering profiles, each
  with an on/off switch and remove. **+** imports a WireGuard `.conf` (and `.ovpn` when
  `networkmanager-openvpn` is installed).
- **Advanced** (inside each connection, or the gear icon) opens in the same window: name, priority, metered,
  IPv4/IPv6 method, manual addresses, gateway, DNS servers, automatic DNS, MTU, MAC address (random/stable),
  hidden network. Save applies it and reconnects if the connection is in use.

### Night Light
| Setting | Key | Range | Default |
|---|---|---|---|
| Enabled | `nightlight.enabled` | true / false | false |
| Temperature | `nightlight.temperature` | 2500–6500 K | 4000 |

### Keybinds
See [section 3](#3-keybinds).

### System
- **Restart hyprminimal**: reloads bar, notifications and OSD (starts it if it isn't running).
- **Shortcuts to other tools**:

  | Button | Opens |
  |---|---|
  | Audio | `pavucontrol` |
  | Displays | `nwg-displays` |
  | Bluetooth | `blueman-manager` |

  If a tool isn't installed, Settings tells you so.

### About
Shows the version of each package and the paths of `settings.json` and `keybind.lua`.

### Customizing the look

**Pick a theme:** Settings → General → Theme. It applies at once: bar, notifications, control center, OSD,
launcher and session menu, bar popups, Settings, kitty, the lock screen and Hyprland's window borders.

**How it works.** The style files in the repo (`waybar/style.css`, `swaync/style.css`, `swayosd/style.css`,
`rofi/*.rasi`, `hyprlock/hyprlock.conf`, the CSS inside `settings/*.py`) are written in
Tokyo Night colours. `settings/themes.py` gives every one of those colours a *role* (`primary`, `surface`,
`outline`, `on_surface`, …) and, when you pick a theme, writes copies with each colour swapped for the theme's
colour in the same role into `.generated/`. So:

- **Change the layout or shape of something** (sizes, padding, corners): edit its repo file, then
  `hyprminimal restart`. Keep using the Tokyo Night colours from `colors/palette.css` in it, and the edit
  works in every theme.
- **Change a theme's colours**: edit its entry in `TEMPLATE` in `settings/themes.py`: `bg`, `bg_elev`,
  `surface`, `text`, `text_strong`, `muted`, `accent`, `accent_hover`, `danger`. The other roles are mixed
  from these in `roles()`. Then `hyprminimal restart`.
- **Add a theme**: add an entry to `TEMPLATE`, its name to `ORDER` and a line to `DESCRIPTION` in
  `settings/themes.py`. It shows up in Settings → General.
- **Terminal colours**: kitty includes `.generated/kitty-colors.conf` (from `kitty_conf()` in `themes.py`,
  ANSI colours in `ANSI_DARK` / `ANSI_TOKYO`). Running kittys reload on a theme change.
- **Window borders**: `.generated/hypr-theme.lua` (from `hypr_lua()`), loaded by `hypr/layerrules.lua`
  after your own `~/.config/hypr/theme.lua`, so it wins.
- **Solid surfaces**: blur, shadows and window opacity are off in `~/.config/hypr/theme.lua`, animations in
  `~/.config/hypr/animations.lua`, and every shell surface (bar, popups, notifications, OSD, launcher) has an
  opaque background. Translucency forces the compositor to redraw what's behind on every frame, blur even more.

**Calendar click area.** An invisible strip from the bar popup program sits over the date and clock
(`reload_zones()` in `helpers/panels.c`): a click toggles the calendar, a right click opens the control center.
Its size comes from `.generated/clock_zone.json`, recalculated whenever the bar's text, font or widgets
change. Nothing on the bar reacts to hovering: tooltips are off, info opens on click.

**Other apps.** GTK apps follow the theme's light or dark mode: on every theme change (and at the first start)
Settings sets `gsettings` `color-scheme` and `gtk-theme` (`adw-gtk3` / `adw-gtk3-dark`, so GTK3 apps like
blueman match the libadwaita ones) and the same keys in `~/.config/gtk-3.0` / `gtk-4.0/settings.ini`. Qt apps use qt6ct
(`QT_QPA_PLATFORMTHEME=qt6ct` in `startup.lua`, dark palette in `~/.config/qt6ct/qt6ct.conf`; change it with
the `qt6ct` app). Electron apps (VS Code, Discord, …) follow the dark setting through the desktop portal; VS
Code also needs `"window.titleBarStyle": "custom"` in its settings, or its menu bar stays light on Wayland.

| Generated file | Used by |
|---|---|
| `.generated/theme` | Current theme, dark/light |
| `.generated/palette.css` | Colour roles (GTK `@define-color`), read by `scripts/media.sh` |
| `.generated/waybar.css`, `swaync.css`, `swayosd.css` | Bar, notifications/control center, OSD |
| `.generated/rofi/*.rasi` | Launcher and session menu |
| `.generated/hyprlock.conf` | Lock screen |
| `.generated/kitty-colors.conf` | kitty |
| `.generated/hypr-theme.lua` | Hyprland border colours |
| `.generated/preload` | `on`/`off`: start Settings hidden at login |
| `.generated/clock_zone.json` | Calendar click area |

---

## 3. Keybinds

Super+H opens the editor. From it you can search,
edit, record a new key combo, add shortcuts and remove them.

Every save goes through these steps:
1. `~/.config/hypr/keybind.lua` is backed up to `~/.config/hypr/.pre-hyprminimal-backup/`.
2. Hyprland reloads.
3. If Hyprland reports an error, the change is undone automatically.

Some things the editor doesn't do:
- Removed shortcuts are commented out, not deleted, so you can bring them back by uncommenting them.
- Shortcuts defined in a loop, such as Super+1…9, are listed but have to be edited in the file.

Shell actions in `keybind.lua` call `bin/hyprminimal-msg ACTION`, which runs the action in hyprminimal.

| Action | Meaning |
|---|---|
| `launcher` | Toggle the launcher |
| `lock` | Lock the screen |
| `settings` | Open Settings |
| `cheatsheet` | Open the keybinds editor |
| `control-center` | Toggle the control center |
| `session` | Open the session menu |
| `media toggle\|next\|previous` | Control playback |
| `volume +5` / `volume -5` | Change the volume |
| `brightness +5` / `brightness -5` | Change the brightness |
| `caffeine` | Toggle Caffeine |
| `nightlight` | Toggle Night Light |
| `panel calendar\|battery\|network\|system` | Open a bar popup (`wifi`: the network list) |

---

## 4. Where things live

| What | Where |
|---|---|
| Your settings | `settings.json` |
| Generated configs (never edit these) | `.generated/`, rebuilt from `settings.json` |
| Hand-editable base configs | `waybar/`, `swaync/`, `swayosd/`, `rofi/`, `hyprlock/`, `colors/` (written in Tokyo Night colours; themed copies go to `.generated/`) |
| Themes | `settings/themes.py` |
| Logs | `$XDG_RUNTIME_DIR/hyprminimal/*.log` (usually `/run/user/1000/hyprminimal/`). The previous run of each log is kept as `NAME.log.1`. |
| "hyprminimal is active" flag | `$XDG_RUNTIME_DIR/hyprminimal/active` |
| Backups of Hyprland files | `~/.config/hypr/.pre-hyprminimal-backup/` |
| Hyprland files hyprminimal touches | `startup.lua` (autostart), `keybind.lua` (hyprminimal-msg), `hyprland.lua` (last line loads `hypr/layerrules.lua` for popup rules) |

Log files:

| Log | Component |
|---|---|
| `waybar.log` | Bar |
| `swaync.log` | Notifications and control center |
| `swayosd.log` | Volume/brightness OSD |
| `settings.log` | Settings app and the Wi-Fi list popup |
| `panels.log` | Bar popups (calendar, battery, network, system) and the clock click strip |
| `helpers-build.log` | Build of the C helpers (`helpers/*.c`, incl. the bar popups) |
| `watchdog.log` | Watchdog |
| `shim-build.log` | Build of the control center shim (cursor, tooltips, keys, live labels) |

---

## 5. Crashes and problems

### First steps for any problem
```bash
hyprminimal status                                   # what is running?
ls -lt $XDG_RUNTIME_DIR/hyprminimal/                 # which log changed last?
tail -50 $XDG_RUNTIME_DIR/hyprminimal/waybar.log     # replace waybar with the component
hyprminimal restart                                  # fixes most problems
```

### The watchdog (automatic crash recovery)
While hyprminimal is active, a watchdog restarts components that die. It does not poll: it sleeps in the kernel
(`helpers/waitpids.c`) until one of them exits, so it costs nothing while everything runs. Without gcc it falls back to
checking every 3 seconds.
- **A component dies.** The watchdog restarts it and sends a notification titled *"Bar stopped, restarted it"*, with the last error from the log, or `crashed (SIGSEGV)` if the program crashed.
- **It dies 3 times within 60 seconds.** The watchdog stops restarting it and sends a critical notification: *"… keeps crashing, stopped restarting it"*. The notification includes the reason, what to check and the log path. Fix the cause, then run `hyprminimal restart`.

To see the crash details, check its core dump: `coredumpctl list waybar`, then `coredumpctl info waybar`.

### Problems by symptom

**No bar or shell at all after login**
- Run `hyprminimal start` from a terminal and read its output. If it says packages are missing, install them:
  `sudo pacman -S --needed $(grep -v '^#' ~/www/hyprminimal/packages.txt)`
- Check that `startup.lua` still has the hyprminimal line (see [section 1](#hyprminimal-as-the-default-shell)).
- No terminal? Press Super+Enter, or whatever your terminal key is. If even that fails, switch to a TTY with **Ctrl+Alt+F2** and run `hyprminimal start` there, or fix the files from it.

**The bar keeps crashing**
- This usually means a bad edit to `waybar/config.jsonc` or `waybar/style.css`, or an unusual value in Settings → Bar.
- To find out which, set a value back in Settings, or undo the hand edit, then run `hyprminimal respawn waybar`.
- Look for the error in `waybar.log`. Waybar names the line it doesn't like.
- Bar changes are applied by restarting waybar, not by reloading it in place, because waybar 0.15 can segfault on an in-place reload.

**Notification "hyprminimal: settings could not be applied"**
- Rendering `settings.json` failed, so hyprminimal is using the last working generated files.
- Usually a value has the wrong type, for example `"height": "big"`. Fix it in Settings or in the file.
- Test it with `hyprminimal-settings --apply`, which prints the error.

**`settings.json` is broken (bad JSON)**
- If the file can't be parsed, hyprminimal uses the **defaults** for everything.
- ⚠️ If you change anything in Settings while the file is broken, Settings saves the defaults over it. Fix the file first.
- Find the problem with `jq . ~/www/hyprminimal/settings.json`. It prints the line with the error.

**Settings (Super+I) doesn't open, or shows stale pages**
- Settings stays running in the background for 10 minutes after you close it, so that it opens instantly. If that background process hangs:
  `pkill -f hyprminimal_settings.py`. Then press Super+I again.
- After you edit the Python files in `settings/`, run `hyprminimal restart`. The background process caches the old code.
- Errors are in `settings.log`.

**Bar popups (calendar, battery, network) don't appear or appear in the wrong place**
- They need `gtk4-layer-shell` (`/usr/lib/libgtk4-layer-shell.so`). Install it with `sudo pacman -S gtk4-layer-shell`, then run `hyprminimal restart`.

**Control center: no hand cursor, tooltips, key numbers or live Wi-Fi/System info** (a notification says the build failed)
- The small C shim in `swaync/cursor/pointer.c` couldn't be compiled.
- Install the build tools: `sudo pacman -S --needed gcc pkgconf gtk4`. Then run `hyprminimal restart`.
- The compiler output is in `shim-build.log`.

**Bar speed or watchdog use the slower shell fallback**
- `helpers/*.c` (network speed, watchdog wait) are built into `.generated/bin/` by `hyprminimal start`/`restart`.
  If gcc is missing, the shell versions run instead (they work, but wake the CPU more). Build errors are in `helpers-build.log`.

**Notifications look like stock swaync, or appear twice**
- Something started the stock swaync through D-Bus before hyprminimal's copy was up.
- Fix it with `systemctl --user stop swaync.service`, then `hyprminimal respawn swaync`.

**No volume or brightness OSD**
- Run `hyprminimal respawn swayosd-server` and check `swayosd.log`.
- Brightness also needs `brightnessctl`, and your user may need to be in the `video` group.

**The media capsule is empty or shows the wrong player**
- Check that `playerctld` is running: `pgrep playerctld || playerctld daemon`.
- To see which players are available, run `playerctl -l`.

**Plain background without the Arch logo**
- The bar popup program isn't running: `pgrep -f .generated/bin/panels || hyprminimal restart`.

**The lock screen is stuck or won't accept your password**
- Make sure Caps Lock is off and the keyboard layout is the one your password uses.
- From a TTY (**Ctrl+Alt+F2**, log in), you can force an unlock: `pkill -USR1 hyprlock`
- If hyprlock crashed and Hyprland shows its "lock screen died" screen, you can't get back to the session from a TTY. Run `loginctl terminate-user $USER` and log in again. This ends the session, so unsaved work is lost.

**Night Light won't turn off**
- Run `pkill -x hyprsunset`. Then set Settings → Night Light → Enabled to off, so it doesn't come back.

**A keybind edit broke a shortcut**
- The editor already undoes changes that Hyprland rejects. If a change was valid but did the wrong thing, restore the previous file:
  ```bash
  ls -t ~/.config/hypr/.pre-hyprminimal-backup/           # newest first
  cp ~/.config/hypr/.pre-hyprminimal-backup/keybind.lua ~/.config/hypr/keybind.lua
  hyprctl reload
  ```

**Hyprland shows a config error after an hyprminimal change**
- Run `hyprctl configerrors` to see the error. Then restore the affected file (`startup.lua`, `hyprland.lua` or `keybind.lua`) from `~/.config/hypr/.pre-hyprminimal-backup/`.
- To check a Lua file's syntax: `luac -p ~/.config/hypr/startup.lua`

**Logging in on tty1 doesn't start Hyprland**
- `~/.bash_profile` needs the `# hyprminimal autostart` block (`./install.sh` adds it). It only runs on tty1, in a login shell.
- If a display manager still holds tty1, turn it off: `sudo systemctl disable greetd` (or gdm, sddm, ...), then reboot.

**A component won't start and its log is empty**
- Start it in the foreground to see the error, for example:
  `waybar -c ~/www/hyprminimal/.generated/waybar.json -s ~/www/hyprminimal/.generated/waybar.css`

---

## 6. Reset, undo, uninstall

### Reset settings to the defaults
```bash
cd ~/www/hyprminimal
mv settings.json settings.json.old      # keep it, just in case
rm -rf .generated
hyprminimal restart                        # everything is rebuilt from the defaults
```

### Uninstall hyprminimal completely
1. In `~/.config/hypr/startup.lua`, remove the hyprminimal line (or restore `.pre-hyprminimal-backup/startup.lua`, after checking it).
2. Remove the `# hyprminimal autostart` block from `~/.bash_profile`.
3. Run `hyprminimal stop`.
4. Remove the last line of `~/.config/hypr/hyprland.lua` (the one that loads the hyprminimal layer rules).
5. In `keybind.lua`, point `SHELLMSG` at your new shell, or restore the file from `.pre-hyprminimal-backup/`.
6. Delete the folders:
   ```bash
   rm -rf ~/www/hyprminimal ~/.cache/hyprminimal ~/.local/state/hyprminimal
   ```
   The packages in `packages.txt` are ordinary Arch packages. Keep or remove them as you like.
