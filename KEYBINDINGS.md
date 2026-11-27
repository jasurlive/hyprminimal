# Keybindings

Every shortcut hyprminimal sets, taken from [`hypr/config/keybind.lua`](hypr/config/keybind.lua).
**Super** is the Windows / Command key.

Press **Super+H** to see them on screen, search them, and change them. The editor backs up
`~/.config/hypr/keybind.lua` before every save and undoes the change if Hyprland reports an error.
See [GUIDE.md §3](GUIDE.md#3-keybinds).

## Apps

| Key | Opens |
|---|---|
| Super+Space | Terminal (kitty) |
| Super+Enter | Terminal (kitty) |
| Super+C | Code editor (`code`) |
| Super+E | File manager (thunar) |
| Super+B | Browser (firefox) |

## hyprminimal

| Key | Does |
|---|---|
| Super+R | Launcher (type `rm NAME` + Enter to uninstall an app) |
| Super+S | Control center |
| Super+I | Settings (press again to hide) |
| Super+H | Keybinds editor / cheatsheet |
| Super+W | Wi-Fi popup |
| Super+D | Calendar popup |
| Super+P | Battery / power mode popup |
| Super+M | Show / hide the bar |
| Super+Escape | Session menu (lock, log out, reboot, shut down) |
| Super+L | Lock screen |

## Windows

| Key | Does |
|---|---|
| Super+Q | Close window |
| Super+V | Toggle floating |
| Super+F | Toggle fullscreen |
| Super+←/→/↑/↓ | Move focus |
| Alt+Tab | Cycle windows |
| Super+Shift+←/→/↑/↓ | Resize by 30 px |
| Super+Shift+Ctrl+←/→/↑/↓ | Tiled: swap with neighbour. Floating: nudge 30 px |
| Super+X | Resize mode |
| Super+Left drag | Move window |
| Super+Right drag | Resize window |

## Workspaces

| Key | Does |
|---|---|
| Super+1 … Super+9, Super+0 | Go to workspace 1–10 |
| Super+Shift+1 … Super+Shift+0 | Move window to workspace 1–10 |
| Super+Ctrl+→ | Next workspace |
| Super+Ctrl+← | Previous workspace |
| Super+Ctrl+↓ | First empty workspace |
| Super+Ctrl+Alt+→ | Move window to next workspace |
| Super+Ctrl+Alt+← | Move window to previous workspace |
| Super+Tab | Next workspace on this monitor |
| Super+Ctrl+Tab | Next workspace on this monitor |
| Super+Shift+Tab | Previous workspace on this monitor |
| Super+Scroll down | Next workspace |
| Super+Scroll up | Previous workspace |

## Screenshots

Saved to `~/Pictures/Screenshots`.

| Key | Captures |
|---|---|
| Print | Region (select with the mouse) |
| Super+Print | A window |
| Alt+Print | A monitor (pick one) |
| Shift+Print | The active monitor, instantly |
| Super+A | Region, then annotate it in satty |

## Media, volume, brightness

These work on the lock screen too. Volume and brightness step 1% per tap and speed up
(2% → 3% → 5%) while the key is held.

| Key | Does |
|---|---|
| Play/Pause | Play or pause |
| Next / Previous | Next / previous track |
| Volume Up / Down | Volume ±, with OSD |
| Mute | Mute / unmute |
| Brightness Up / Down | Brightness ±, with OSD |

## Inside the control center (Super+S)

| Key | Does |
|---|---|
| 1–7 | System toggles |
| K | Play / pause |
| C | Clear all notifications |

## Inside the bar popups

Each key is shown as a keycap next to what it does. Esc closes any popup.

| Popup | Keys |
|---|---|
| Network (Super+W) | W Wi-Fi on/off · H hotspot on/off · N list of Wi-Fi networks |
| Wi-Fi networks (control center ›, or N) | R scan again · W Wi-Fi on/off |
| Battery (Super+P) | S Power Saver · B Balanced · P Performance |

## Mouse

Clicking anywhere outside an open bar popup closes it.
