local HM = (os.getenv("HOME") or "") .. "/.local/share/hyprminimal"

local env = {
    XDG_CURRENT_DESKTOP = "Hyprland",
    XDG_SESSION_TYPE = "wayland",
    XDG_SESSION_DESKTOP = "Hyprland",
    QT_QPA_PLATFORM = "wayland;xcb",
    QT_QPA_PLATFORMTHEME = "qt6ct",   -- Qt apps: dark palette from ~/.config/qt6ct (set by hyprminimal)
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
    "gsettings set org.gnome.desktop.interface icon-theme Papirus-Dark",
    "gsettings set org.gnome.desktop.interface cursor-theme Adwaita",
    "wl-clip-persist --clipboard regular",
    -- hyprminimal shell
    HM .. "/bin/hyprminimal start",
}

hl.on("hyprland.start", function()
    for _, cmd in ipairs(exec_once) do
        hl.exec_cmd(cmd)
    end
end)

return true
