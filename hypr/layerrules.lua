-- hyprminimal: blur behind the bar and panels (Noctalia's "glass" look).
-- Loaded from ~/.config/hypr/hyprland.lua (dofile); harmless when hyprminimal isn't running.
local function glass(name, ns, extra)
    local spec = { name = "hyprminimal-" .. name, match = { namespace = ns }, blur = true, ignore_alpha = 0.2 }
    for k, v in pairs(extra or {}) do spec[k] = v end
    hl.layer_rule(spec)
end

glass("bar",    "^waybar$")
glass("cc",     "^swaync-control-center$",       { animation = "slide top" })
glass("notif",  "^swaync-notification-window$",  { animation = "slide right" })
glass("osd",    "^swayosd$",                     { animation = "fade" })
glass("rofi",   "^rofi$",                        { animation = "slide top" })
glass("panel",  "^hyprminimal-panel$",              { animation = "slide top" })
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
local function close_on_click()
    local panel = open_panel()
    if not panel then return end                        -- the usual case: costs one layer lookup
    local p = hl.get_cursor_pos()
    if not p or inside(panel, p) then return end
    for _, l in ipairs(hl.get_layers()) do
        if l.namespace == "waybar" and inside(l, p) then return end
    end
    hl.exec_cmd(CLOSE)
end
for _, button in ipairs({ "mouse:272", "mouse:273", "mouse:274" }) do
    hl.bind(button, close_on_click, { non_consuming = true, description = "Close bar popup on outside click" })
end

-- settings window: float and centre it, like Noctalia's settings panel
-- launcher "rm NAME": uninstall terminal (scripts/remove-app.sh)
hl.window_rule({ match = { class = "^hyprminimal-remove$" }, float = true, size = "820 520", center = 1 })
hl.window_rule({ match = { class = "^dev\\.hyprminimal\\.Settings$" }, float = true, size = "1120 760", center = 1, opaque = true })

return true
