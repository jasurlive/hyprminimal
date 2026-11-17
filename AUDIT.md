# hyprminimal audit (phase 1, no code changes)

Scope: every file tracked in git at `d85e74a`, plus the working tree as it was during the audit
(9 files carry uncommitted edits, see "Open questions"). Line numbers refer to the working tree.

Legend for **action**: keep · remove · optional (move to `packages-optional.txt`) · **ask** (the
change touches a feature on the must-preserve list, so it waits for your decision).

---

## 1. Packages

Sources: `packages.txt` (34 entries, checked by `hyprminimal start`) and the `BASE` array in
`install.sh:51-67` (33 entries, *not* checked anywhere). One package (`wireplumber`) is in both.
GPU packages (`install.sh:70-82`) are picked by `lspci` and are out of scope here (keep).

### packages.txt

| package | used by (file:line) | essential? | replacement | action |
|---|---|---|---|---|
| hyprland | `hypr/config/*.lua`, `greeter/hyprland.lua`, `hyprctl` in most scripts | yes | – | keep |
| waybar | `bin/hyprminimal:54` | yes (bar) | – | keep |
| rofi | `scripts/launcher.py:67`, `scripts/session.sh:9` | yes (launcher, session menu) | – | keep |
| swaync | `bin/hyprminimal:62` | yes (notifications, control center) | – | keep |
| swayosd | `bin/hyprminimal:69`, `bin/hyprminimal-msg` `volume`/`brightness` | yes (OSD) | – | keep |
| swaybg | `bin/hyprminimal:72` | yes (wallpaper) | – | keep |
| hyprlock | `bin/hyprminimal-msg` `lock` | yes (lock screen) | – | keep |
| hypridle | `bin/hyprminimal:77` | yes (idle) | – | keep |
| hyprsunset | `bin/hyprminimal:238`, `bin/hyprminimal-msg` `nightlight` | yes (Night Light tile) | – | keep |
| python | settings, greeter, launcher | yes | – | keep |
| python-gobject | same | yes | – | keep |
| gtk4 | settings, greeter, panels; headers for `swaync/cursor/pointer.c` | yes | – | keep |
| libadwaita | `settings/hyprminimal_settings.py` (Adw widgets) | yes | – | keep |
| gtk4-layer-shell | `bin/hyprminimal-settings:17` (bar popups) | yes | – | keep |
| gcc | `bin/hyprminimal:58,100` builds `helpers/*.c` + `swaync/cursor/pointer.c` | build-only, **221 MiB** (largest package after firefox) | build at install time, see Q2 | **ask** |
| pkgconf | `bin/hyprminimal:58` (gtk4 flags for the shim) | build-only | goes with gcc | **ask** |
| imagemagick | `install.sh:110` (default wallpaper), `bin/hyprminimal-greeter:43` (blurred greeter background) | yes, at install / greeter-install time only (22 MiB) | GTK4 CSS `filter: blur()` in the greeter would change the look and cost GPU at every login | keep |
| playerctl | `scripts/media.sh:25`, `settings/panels.py:723`, `bin/hyprminimal:234` (playerctld) | yes (media controls) | – | keep |
| jq | `scripts/control-center.sh:47`, `hypr/config/Scripts/move-or-swap.sh:4`, `workspace.sh`; also a hard dependency of `hyprshot` | yes | – | keep |
| brightnessctl | **nothing** (only the dead `hyprsettings/Scripts/accel-step.sh`). swayosd sets brightness itself (its Arch deps don't include brightnessctl) | no | swayosd-client (already used) | **remove** |
| libnotify | `bin/hyprminimal:40`, `settings/hyprminimal_settings.py:1222,1689` | yes | – | keep |
| pavucontrol | `settings/hyprminimal_settings.py:1384`, `waybar/config.jsonc:27` (optional widget) | yes (only output/input device picker) | – | keep |
| wireplumber | `hypr/config/keybind.lua:145` (`wpctl`); also listed in `BASE` | yes | – | keep, list once |
| power-profiles-daemon | `settings/panels.py:300,348` (battery popup), `install.sh:185` | yes | – | keep |
| kitty | `hypr/config/keybind.lua:3`, `scripts/launcher.py:91` | yes | – | keep |
| networkmanager | `nmcli` in `settings/network_page.py`, `panels.py`, `control-center.sh`, `swaync/config.json:57` | yes | – | keep |
| network-manager-applet | **nothing launches it**: `nm-applet` is never started, `nm-connection-editor` is never run (Settings → Network replaced it, `settings/network_page.py:1`). Only a window rule remains (`hypr/config/windowrules.lua:4`) | no | Settings → Network | **ask** (touches networking) → remove |
| bluez-utils | `bin/hyprminimal-msg` `bluetooth`, `bluetooth-state` | yes | – | keep |
| blueman | `waybar/config.jsonc:39`, `settings/hyprminimal_settings.py:1386` | yes (the only pairing UI) | – | keep |
| noto-fonts | Noto Sans in every stylesheet, `kitty/kitty.conf:8` | yes | – | keep |
| ttf-nerd-fonts-symbols | "Symbols Nerd Font" in `waybar/style.css:4`, `swaync/style.css:4`, `scripts/session.sh:6` | yes | – | keep |
| papirus-icon-theme | `rofi/theme.rasi:22`, `install.sh:162`, `startup.lua:35` | yes (launcher icons) | – | keep |
| qt6ct | `hypr/config/startup.lua:8`, `install.sh:164-167` | only for Qt apps, and **no Qt app is installed by default**. Pulls `qt6-base` (68 MiB) | – | **ask** → optional |

### install.sh `BASE`

| package | used by (file:line) | essential? | replacement | action |
|---|---|---|---|---|
| xdg-desktop-portal-hyprland | D-Bus activated (screen share, portals) | yes | – | keep |
| xdg-desktop-portal-gtk | file chooser portal | yes | – | keep |
| hyprpolkitagent | `hypr/config/startup.lua:31`, `windowrules.lua:6` | yes | – | keep |
| polkit | dependency of the agent | yes (transitive) | – | keep |
| gnome-keyring | `hypr/config/startup.lua:32` | yes (browser/app secrets) | – | keep |
| qt5-wayland, qt6-wayland | no Qt app installed by default | no | – | **ask** → optional (with qt6ct) |
| xdg-user-dirs | `install.sh:106` | yes | – | keep |
| xdg-utils | `xdg-open` for apps | yes | – | keep |
| pciutils | `install.sh:72` (GPU detection) | install-time | – | keep |
| pipewire, pipewire-pulse, pipewire-alsa | audio | yes | – | keep |
| wireplumber | duplicate of packages.txt | – | – | remove duplicate |
| sof-firmware | laptop audio DSPs | hardware | – | keep |
| bluez | `bluetooth.service` (`install.sh:185`) | yes | – | keep |
| greetd | greeter / autologin | yes | – | keep |
| thunar, gvfs | `hypr/config/keybind.lua:5` (Super+E) | yes | – | keep |
| firefox | `hypr/config/keybind.lua:6` (Super+B) | **302 MiB**, the largest package installed | any browser | **ask** → optional? |
| grim, slurp | `hypr/config/Scripts/satty.sh:5-6`, hyprshot deps | yes | – | keep |
| hyprshot | `hypr/config/keybind.lua:104-107` | yes | – | keep |
| satty | `hypr/config/Scripts/satty.sh:7` (Super+A) | yes | – | keep |
| wl-clipboard | hyprshot dependency | yes | – | keep |
| wl-clip-persist | `hypr/config/startup.lua:37`, resident daemon | nice-to-have (clipboard survives closing the app) | – | keep |
| nwg-displays | `settings/hyprminimal_settings.py:1385` (Settings → Displays) | yes | – | keep |
| noto-fonts-emoji | emoji in notifications/apps | yes | – | keep |
| ttf-jetbrains-mono-nerd | **nothing** (no "JetBrains" anywhere outside the dead `hyprsettings/`) | no | – | **remove** |
| adwaita-cursors | `startup.lua:13` `XCURSOR_THEME` | yes | – | keep |
| adwaita-icon-theme | GTK fallback icons | yes | – | keep |
| gnome-themes-extra | `startup.lua:34` (`Adwaita-dark` for GTK3 apps) | yes | – | keep |

### Binaries used but not installed by anything

| binary | where | note |
|---|---|---|
| `code` | `hypr/config/keybind.lua:4` (Super+C) | VS Code isn't installed: the bind fails silently on a fresh install. Keep the bind, list `code` in optional extras. |
| `sudo`, `git` | `install.sh:43`, README | not in a `base` install. `install.sh` stops with a clear message for sudo; git is needed to clone. Prerequisites, documented. |

Everything else invoked (`busctl`, `coredumpctl`, `systemd-inhibit`, `gdbus`, `gsettings`, `flock`,
`setsid`, `timeout`, `awk`, `pgrep`, `swaync-client`, `swayosd-client`, `playerctld`, `start-hyprland`)
comes from `base`, systemd, glib2, util-linux, or the package of its daemon.

---

## 2. Processes started per session

| process | started by | lifetime | wakeups / polling |
|---|---|---|---|
| `dbus-update-activation-environment --all` | `startup.lua:30` | one-shot | – |
| hyprpolkitagent (user unit) | `startup.lua:31` | resident | event |
| gnome-keyring-daemon | `startup.lua:32` | resident | event |
| `gsettings set …` ×4 | `startup.lua:33-36` | one-shot, every login | – (persistent in dconf: they only need to run once) |
| wl-clip-persist | `startup.lua:37` | resident | event |
| `hyprctl keyword gesture …` ×3 | `inputs.lua:19-25` | one-shot **on every config reload** | – |
| `bin/hyprminimal start` | `startup.lua:39` | one-shot | runs **one `pacman -Qq` per line of packages.txt** (`bin/hyprminimal:267-269`, about 34 forks at every login) |
| `hyprminimal-settings --apply` | `bin/hyprminimal:218` | one-shot Python render | – |
| swaybg | `bin/hyprminimal:72` | resident | none |
| swaync (+ `pointer.so` preload) | `bin/hyprminimal:62` | resident | the start waits on a `busctl list` loop, 25 × 0.1 s (`:65`) |
| waybar | `bin/hyprminimal:54` | resident | clock (per minute), battery `interval: 30` (`waybar/config.jsonc:102`); cpu/memory 5 s, off by default |
| ↳ `scripts/media.sh` + `playerctl --follow` | waybar `custom/media` | resident | event |
| ↳ `.generated/bin/netspeed` | waybar `custom/net` via `netspeed.sh` | resident | **2 s timer**: justified, a byte-rate needs two samples; it prints only on change and has 50 ms timer slack |
| ↳ `swaync-client -swb` | waybar `custom/notification` | resident | event |
| ↳ `scripts/errors.sh` | waybar `custom/errors` | one-shot on SIGRTMIN+9 | event |
| swayosd-server | `bin/hyprminimal:69` | resident | event |
| hypridle | `bin/hyprminimal:77` | resident | event |
| playerctld | `bin/hyprminimal:234` | resident | event |
| `.generated/bin/workspace-watch` | `bin/hyprminimal:236` | resident | event (socket2) |
| hyprsunset | `bin/hyprminimal:238` when Night Light is on | resident | – |
| **`hyprminimal_settings.py --preload`** | `bin/hyprminimal:240` | **resident, never quits** (`preloaded` disables the 10-min idle quit, `settings/hyprminimal_settings.py:1668,1723`) | idle, but **≈95 MB RSS** (largest hyprminimal process) |
| `hyprminimal watchdog` (bash) + `waitpids` | `bin/hyprminimal:245` | resident | blocks on pidfds. Timeout 6 s while a config is unproven, 30 s while a component has given up, **`sleep 3` polling** while anything is missing (`:199`), `sleep 1` after every exit |
| user startup commands | `bin/hyprminimal:252-262` | user-defined | – |
| hyprlock (autologin only) | `startup.lua:48` | until unlock | – |

No hyprminimal systemd user units or timers exist. hyprminimal masks `swaync.service`
(`bin/hyprminimal:226`) and enables `pipewire.socket`, `pipewire-pulse.socket` and
`wireplumber.service` (`install.sh:186`).

Snapshot of this machine's live session (`ps`, one sample, **not** a bench.sh measurement):
settings 95 MB · swaync 72 MB · swayosd 65 MB · waybar 62 MB · swaybg 16 MB · playerctl 8 MB.

On-demand (per keypress or click), no idle cost: `scripts/launcher.py` (Python + Gio per Super+R),
`scripts/control-center.sh` (spawns **Python + Pango** `center-lines.py` per open, plus nmcli, jq and
a 150 ms CPU sample), `Scripts/workspace.sh` (`sleep 0.05` + 4 hyprctl + 6 jq per Super+Tab),
`accel-step.sh`, `move-or-swap.sh`, `session.sh`.

Other sleep loops, each justified or short: `hyprminimal-msg lock` retries hyprlock after a crash
(0.5 s, at most 20 tries); `logout` checks the session 25 × 0.2 s; `stop_one` 20 × 0.05 s;
`restart` 30 × 0.1 s; `install.sh` keeps sudo alive with a 50 s loop (install time only).
The swaync wait (`bin/hyprminimal:65`) can become event-driven: `gdbus wait --session --timeout 3 org.erikreider.swaync.cc`.

---

## 3. Python scripts

| file | lines | what it does | replace? |
|---|---|---|---|
| `settings/hyprminimal_settings.py` | 1767 | Settings app (GTK4/Adw), keybind editor, `--apply` renderer for `.generated/`, resident D-Bus service for Settings and popups | **keep**: rewriting it would cost far more than it saves |
| `settings/panels.py` | 1279 | bar popups (calendar, battery, network, media, system, wifi) inside the settings process | keep |
| `settings/network_page.py` | 850 | Settings → Network over nmcli | keep |
| `settings/themes.py` | 291 | colour roles + `recolor()`, the single theme source | keep |
| `greeter/greeter.py` | 214 | greetd greeter (GTK4) | keep (gtkgreet/regreet look different) |
| `scripts/launcher.py` | 105 | numbered rofi app list, recent apps, `rm NAME` | keep (Gio's .desktop parsing is the correct way; costs nothing while idle) |
| `scripts/center-lines.py` | 42 | pads the Wi-Fi and System tile labels with spaces so their two lines look centred. Starts **Python + gi + Pango on every control-center open** | **ask** → replace: the swaync shim is already preloaded into swaync (`pointer.c`). One `gtk_label_set_justify(…, GTK_JUSTIFY_CENTER)` on button labels in its map hook centres the lines exactly, so this script and its jq fallback go away |
| `scripts/workspace-watch.py` | 30 | fallback for `helpers/workspace-watch.c` when gcc is missing | **remove**: gcc is required, so this path only runs if the build fails, and install should fail loudly instead |

---

## 4. Duplicated logic

| what | where | fix |
|---|---|---|
| package list in two places, checked in one | `packages.txt`, `install.sh:51-67` `BASE`, `bin/hyprminimal:266-274` (packages.txt only), `settings/hyprminimal_settings.py:1407` | `packages.txt` + `packages-optional.txt` as the only lists. `install.sh` and `hyprminimal start` read them, and start uses one `pacman -T` call |
| whole installer twice | `app.sh` is an older/variant copy of `install.sh` (diff: `-Syu` vs `pacman -T`, qt6ct block) | delete `app.sh` |
| `gdbus call --dest dev.hyprminimal.Settings … org.gtk.Actions.Activate` | `bin/hyprminimal:283,290`, `bin/hyprminimal-msg` ×3, `bin/hyprminimal-settings` ×2 | one shared bash helper |
| `${XDG_RUNTIME_DIR:-/tmp}/hyprminimal` | `bin/hyprminimal:14`, `bin/hyprminimal-msg` ×4, `scripts/errors.sh:3`, `control-center.sh:19`, settings | define once in the shared helper |
| "`.generated/X` if present, else the repo's `X`" | hyprlock conf ×3 (`hyprminimal-msg lock`, `startup.lua:44`, `hyprminimal-greeter:41`), rofi themes (`session.sh:3`, `launcher.py:64`), swayosd css (`bin/hyprminimal:69`) | one `gen_or_repo` helper in bash |
| JSON string escaping | `scripts/errors.sh:6`, `scripts/media.sh:4` | shared helper |
| swaync D-Bus call (`busctl --auto-start=no … swaync.cc`) | `control-center.sh:9`, `bin/hyprminimal:39,65`, `helpers/workspace-watch.c:19` | the bash copies share the helper (C keeps its own) |
| colours written outside the palette | `hypr/config/theme.lua`, `hyprlock/hyprlock.conf:2-8`, `waybar/config.jsonc:108` (`#e8d5a6`), `install.sh:110` gradient, `bin/hyprminimal:73` `#222436` | by design: `themes.recolor()` maps these Tokyo Night values to the active theme. The two literals outside rendered files (`install.sh:110`, `bin/hyprminimal:73`) can read `colors/palette.css` |
| hard-coded `~/.config/hypr` (XDG_CONFIG_HOME ignored) | `install.sh:17`, `hypr/config/hyprland.lua:3`, `keybind.lua:1`, `settings/hyprminimal_settings.py:62`, `hyprminimal-greeter:42`, kitty/gtk/qt6ct paths in install.sh | `${XDG_CONFIG_HOME:-$HOME/.config}` in bash/Python, `os.getenv("XDG_CONFIG_HOME")` in Lua |
| hard-coded `~/.local/share/hyprminimal` | keybind.lua, startup.lua, accel-step.sh, hypridle.conf, kitty.conf, layerrules.lua | stays: the stable link is the single source by design. Respect `XDG_DATA_HOME` in install.sh and its readers |

---

## 5. Dead files

| file | evidence | action |
|---|---|---|
| `app.sh` | referenced nowhere; a variant copy of `install.sh` | remove |
| `hyprsettings/` (31 files) | referenced nowhere. A personal backup of an older `~/.config/hypr` (`hyprsettings/restore`: "cp -a ~/hyprsettings ~/.config/hypr"), with machine-specific nwg-displays output (`monitors.conf`, `workspaces.*`) | **ask**: remove from the repo (it stays in git history), or move it outside the repo? |
| `scripts/workspace-watch.py` | fallback only, see §3 | remove |
| `scripts/netspeed.sh` lines 7-38 | bash fallback loop, only reached without gcc | reduce to the exec of the built helper (or point waybar at it directly) |
| `bin/hyprminimal:209` `pkill … workspace-watch.py` | follows from the above | remove |
| `hypr/config/windowrules.lua:4` `nm-connection-editor` rule | its app is no longer launched | remove with network-manager-applet |
| `.gitignore: settings.json.tmp` | `save_settings` writes `settings.tmp` (`hyprminimal_settings.py:145`, `with_suffix`) | fix the entry |

---

## 6. Install / uninstall

* **There is no `uninstall.sh`.** Today uninstalling means `sudo hyprminimal-greeter uninstall`,
  `sudo hyprminimal-autologin disable`, and the six manual steps in `GUIDE.md` §6. Plan: add
  `uninstall.sh`, which reverses exactly what install.sh did:
  * the link and `~/.local/bin` links
  * the greeter and greetd config (restores the pre-hyprminimal backup)
  * autologin
  * the swaync mask
  * `~/.config/hypr` and kitty.conf: restores the backups install.sh made, and only touches files that carry the hyprminimal marker
  * the GTK and qt6ct files install.sh created
  * optionally, after a prompt, removes the packages that `install.sh` itself installed (recorded at install time; packages you already had are never removed)
* `install.sh` is mostly idempotent already. Gaps:
  * it doesn't record what it installed
  * several steps swallow errors with `|| true`: user units (`:186`), settings render (`:171`, only a warning)
  * the C helpers are built later, at first login, so a build failure only shows up in the session
  * it uses `pacman -Syu` (correct on Arch, but every re-run is a full system upgrade)
* `bin/hyprminimal-autologin:328` copies the config before its root check, so without sudo it
  fails with a `cp` error instead of "run with sudo".

---

## 7. Proposed changes by phase (pending your answers)

2. **Dependencies.** `packages.txt` (essential) + `packages-optional.txt`; drop `BASE`. Remove
   brightnessctl, ttf-jetbrains-mono-nerd and the wireplumber duplicate. Install with
   `pacman -S --needed`, fail loudly, record what was installed, build the C helpers at install
   time, add `uninstall.sh`.
3. **Processes.**
   * One `pacman -T` instead of ~34 `pacman -Qq` at every login.
   * `gdbus wait` instead of the swaync poll loop.
   * Watchdog: replace the `sleep 3` polling with a waitpids timeout that is armed only while a component is down (keeps the crash-fallback behaviour).
   * `center-lines.py` → shim (if approved).
   * The settings process start policy (Q3).
4. **Code.**
   * Shared bash helper (`bin/hyprminimal-lib`) for the runtime dir, gdbus/Settings calls, gen-or-repo paths and JSON escaping.
   * Delete the dead files.
   * `XDG_CONFIG_HOME` / `XDG_DATA_HOME` everywhere.
5. **Verify.** `bash -n` and shellcheck (**not installed here**: `pacman -S shellcheck` needed, or
   I'll run it from a temporary location), `gcc -Wall -Wextra -O2`, `py_compile`, `Hyprland
   --verify-config` (available here), the binary-vs-package grep. **No docker/podman on this
   machine**, so the `archlinux:latest` dry run can't run here unless you install one.
6. **Docs.** README dependency list, optional extras, and a before/after table filled only with
   `bench.sh` numbers.

## Open questions

1. **Uncommitted work.** 9 files have edits that aren't committed (`GUIDE.md`, `bin/hyprminimal-greeter`,
   `bin/hyprminimal-msg`, `greeter/*`, `scripts/session.sh`, `settings/*.py`, `swaync/config.json`).
   Phases 2-4 edit several of them. Commit them first as your own baseline commit, or stash them?
2. **gcc (221 MiB).** (a) keep it as a runtime dependency, as now; or (b) install gcc+pkgconf
   only for the build in install.sh, then remove them. A `git pull` that changes a `.c` file would
   then need `./install.sh` again, and until then `hyprminimal start` keeps the old binary.
3. **Resident settings process (~95 MB idle).** (a) keep it preloaded: popups open in ~30-80 ms;
   or (b) start it on first use and quit it after 10 idle minutes (the code for this already
   exists). Expected: −95 MB idle, but the first popup or Super+I after login/idle takes about
   0.5-1 s.
4. **Removals that touch must-preserve features**:
   * network-manager-applet (networking): unused, remove?
   * `center-lines.py` → shim centring (control center look)?
   * qt6ct + qt5/qt6-wayland → optional (theme for Qt apps; none are installed by default)?
   * firefox → optional (Super+B opens nothing until you install a browser)?
   * delete `hyprsettings/`?
