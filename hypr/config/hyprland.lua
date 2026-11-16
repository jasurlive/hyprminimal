-- hyprminimal default Hyprland config (installed to ~/.config/hypr by install.sh).
-- Everything here is yours to edit; install.sh only rewrites it with --reset-hypr.
local config_dir = (os.getenv("HOME") or "") .. "/.config/hypr"
package.path = table.concat({
    config_dir .. "/?.lua",
    config_dir .. "/?/init.lua",
    package.path,
}, ";")

-- re-run modules on reload so binds/rules re-register
for _, mod in ipairs({ "monitors", "startup", "inputs", "keybind", "windowrules", "animations", "theme" }) do
    package.loaded[mod] = nil
end

require("monitors")
require("startup")
require("inputs")
require("keybind")
require("windowrules")
require("animations")
require("theme")

hl.config({
    dwindle = { preserve_split = true },
    master = { new_status = "master" },
    misc = {
        disable_hyprland_logo = true,
        disable_splash_rendering = true,
        force_default_wallpaper = 0,
        anr_missed_pings = 5,
        allow_session_lock_restore = true,
    },
    xwayland = { force_zero_scaling = true },
    general = { snap = { enabled = true } },
})

-- hyprminimal blur rules and popup behaviour
pcall(dofile, (os.getenv("HOME") or "") .. "/.local/share/hyprminimal/hypr/layerrules.lua")
