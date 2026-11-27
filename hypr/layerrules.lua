-- hyprminimal: shell layers (bar, popups, notifications, OSD, launcher). No blur: every surface is solid.
-- Animations are off globally (animations.lua); no_anim keeps popups instant even if you turn them back on.
-- Loaded from ~/.config/hypr/hyprland.lua (dofile); harmless when hyprminimal isn't running.
for name, ns in pairs({ notif = "^hyprminimal-notifications$", osd = "^hyprminimal-osd$",
                        rofi = "^rofi$", panel = "^hyprminimal-panel$" }) do
    hl.layer_rule({ name = "hyprminimal-" .. name, match = { namespace = ns }, no_anim = true })
end
hl.layer_rule({ name = "hyprminimal-catcher", match = { namespace = "^hyprminimal-catcher$" }, no_anim = true })

-- bar popups close on a click anywhere but the popup or the bar (non_consuming: the click still
-- reaches the window). Not on focus changes: follow_mouse moves focus on mere hover.
-- Bar icons are left alone: they toggle or switch popups themselves.
-- this file's repo, wherever it was cloned (fallback: the link install.sh makes)
local HM = (os.getenv("HOME") or "") .. "/.local/share/hyprminimal"
do
    local ok, info = pcall(function() return debug.getinfo(1, "S") end)
    local src = ok and info and info.source or ""
    local dir = src:match("^@(.*)/hypr/layerrules%.lua$")
    if dir then HM = dir end
end
local CLOSE = HM .. "/bin/hyprminimal-msg panel-close"
local function inside(l, p) return p.x >= l.x and p.x < l.x + l.w and p.y >= l.y and p.y < l.y + l.h end
local function open_panel()
    for _, l in ipairs(hl.get_layers() or {}) do
        if l.namespace == "hyprminimal-panel" and l.mapped ~= false then return l end
    end
end
local CLOSE_SETTINGS = HM .. "/bin/hyprminimal-msg settings-close"
local function win_inside(w, p)
    return p.x >= w.at.x and p.x < w.at.x + w.size.x and p.y >= w.at.y and p.y < w.at.y + w.size.y
end
-- the Settings window, when it's showing on the active workspace
local function open_settings()
    local ws = hl.get_active_workspace()
    for _, w in ipairs(hl.get_windows() or {}) do
        if w.class == "dev.hyprminimal.Settings" and w.mapped ~= false and not w.hidden
            and (not ws or not w.workspace or w.workspace.id == ws.id) then return w end
    end
end
-- Settings closes like a popup on a click outside it. Not on clicks in its own dialogs (file and
-- font pickers are separate windows of the same process), the bar, or hyprminimal's popups.
local SHELL_LAYERS = { waybar = true, ["hyprminimal-panel"] = true, rofi = true,
                       ["hyprminimal-notifications"] = true }
local function settings_click(p)
    local s = open_settings()
    if not s or win_inside(s, p) then return end
    for _, w in ipairs(hl.get_windows()) do
        if w.pid == s.pid and w.mapped ~= false and not w.hidden and win_inside(w, p) then return end
    end
    for _, l in ipairs(hl.get_layers()) do
        if SHELL_LAYERS[l.namespace] and l.mapped ~= false and inside(l, p) then return end
    end
    hl.exec_cmd(CLOSE_SETTINGS)
end
local function close_on_click()
    local p = hl.get_cursor_pos()
    if not p then return end
    settings_click(p)
    local panel = open_panel()
    if not panel then return end                        -- the usual case: costs one layer lookup
    if inside(panel, p) then return end
    for _, l in ipairs(hl.get_layers()) do
        if l.namespace == "waybar" and inside(l, p) then return end
    end
    hl.exec_cmd(CLOSE)
end
for _, button in ipairs({ "mouse:272", "mouse:273", "mouse:274" }) do
    hl.bind(button, close_on_click, { non_consuming = true, description = "Close bar popup on outside click" })
end

-- settings window: float and centre it, like Noctalia's settings panel; no open/close animation so Super+I is instant
-- launcher "rm NAME": uninstall terminal (scripts/remove-app.sh)
hl.window_rule({ match = { class = "^hyprminimal-remove$" }, float = true, size = "820 520", center = 1 })
-- theme from Settings -> General (window border colours)
pcall(dofile, HM .. "/.generated/hypr-theme.lua")
local settings_rule = { match = { class = "^dev\\.hyprminimal\\.Settings$" }, float = true, center = 1,
                        size = "min(1120,monitor_w*0.92) min(760,monitor_h*0.9)",  -- fits scaled laptops (1280x720 at 1.5x)
                        no_anim = true, opaque = true }
hl.window_rule(settings_rule)
-- Keep Settings ready: the preloaded window maps straight onto a special workspace nothing shows (by its
-- temporary title); Super+I then only moves it in and out (settings/hyprminimal_settings.py, PARK)
hl.window_rule({ match = { class = "^dev\\.hyprminimal\\.Settings$", title = "^hyprminimal settings \\(parked\\)$" },
                 workspace = "special:hyprminimal-settings silent" })

return true
