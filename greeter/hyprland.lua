-- hyprminimal greeter compositor: a bare Hyprland that only runs greeter.py, then exits.
-- Started by greetd's [default_session] as the `greeter` user (see bin/hyprminimal-greeter).
local dir = "/usr/local/share/hyprminimal-greeter"

-- your monitor + input setup, copied here on install; falls back to auto
if not pcall(dofile, dir .. "/monitors.lua") then
    hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "auto" })
end

hl.config({
    input = {
        kb_layout = "us",
        numlock_by_default = true,
        touchpad = { natural_scroll = true, clickfinger_behavior = true },
    },
    general = { border_size = 0, gaps_in = 0, gaps_out = 0 },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = false },
    misc = {
        disable_hyprland_logo = true,
        disable_splash_rendering = true,
        force_default_wallpaper = 0,
    },
})

hl.on("hyprland.start", function()
    -- logged in: fast-exit.sh ends this compositor at once (a clean exit costs ~1 s before greetd starts
    -- your session). Otherwise (greeter crashed or quit): the normal clean exit, greetd shows it again.
    hl.exec_cmd([[sh -c 'python3 ]] .. dir .. [[/greeter.py && sh ]] .. dir .. [[/fast-exit.sh || hyprctl dispatch "hl.dsp.exit()"']])
end)
