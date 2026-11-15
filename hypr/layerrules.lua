-- hyprminimal: blur behind the bar and panels (Noctalia's "glass" look).
-- Loaded from ~/.config/hypr/hyprland.lua; harmless when hyprminimal isn't running.
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

-- settings window: float and centre it, like Noctalia's settings panel
hl.window_rule({ match = { class = "^dev\\.hyprminimal\\.Settings$" }, float = true, size = "1120 760", center = 1, opaque = true })

return true
