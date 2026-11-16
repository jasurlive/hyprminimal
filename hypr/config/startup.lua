local HM = (os.getenv("HOME") or "") .. "/.local/share/hyprminimal"

local env = {
    XDG_CURRENT_DESKTOP = "Hyprland",
    XDG_SESSION_TYPE = "wayland",
    XDG_SESSION_DESKTOP = "Hyprland",
    QT_QPA_PLATFORM = "wayland;xcb",
    QT_AUTO_SCREEN_SCALE_FACTOR = "1",
    QT_WAYLAND_DISABLE_WINDOWDECORATION = "1",
    ELECTRON_OZONE_PLATFORM_HINT = "auto",
    MOZ_ENABLE_WAYLAND = "1",
    XCURSOR_THEME = "Adwaita",
    XCURSOR_SIZE = "24",
    HYPRCURSOR_SIZE = "24",
}

-- NVIDIA proprietary driver loaded: the usual Wayland variables
local nv = io.open("/proc/driver/nvidia/version", "r")
if nv then
    nv:close()
    env.LIBVA_DRIVER_NAME = "nvidia"
    env.__GLX_VENDOR_LIBRARY_NAME = "nvidia"
    env.NVD_BACKEND = "direct"
end

for k, v in pairs(env) do hl.env(k, v) end

local exec_once = {
    "dbus-update-activation-environment --systemd --all",
    "systemctl --user start hyprpolkitagent",
    "gnome-keyring-daemon --start --components=secrets",
    "gsettings set org.gnome.desktop.interface color-scheme prefer-dark",
    "gsettings set org.gnome.desktop.interface gtk-theme Adwaita-dark",
    "gsettings set org.gnome.desktop.interface icon-theme Papirus-Dark",
    "gsettings set org.gnome.desktop.interface cursor-theme Adwaita",
    "wl-clip-persist --clipboard regular",
    -- hyprminimal shell
    HM .. "/bin/hyprminimal start",
}

-- autologin (sudo hyprminimal-autologin enable): greetd starts Hyprland with HYPRMINIMAL_AUTOLOGIN=1 and no
-- password, so the lock screen comes up first and acts as the login screen.
local AUTOLOGIN_LOCK = ([[sh -c 'c="%s/.generated/hyprlock.conf"; [ -f "$c" ] || c="%s/hyprlock/hyprlock.conf"; exec hyprlock -c "$c"']])
    :format(HM, HM)

hl.on("hyprland.start", function()
    if os.getenv("HYPRMINIMAL_AUTOLOGIN") == "1" then
        hl.exec_cmd(AUTOLOGIN_LOCK)
    end
    for _, cmd in ipairs(exec_once) do
        hl.exec_cmd(cmd)
    end
end)

return true
