-- Tokyo Night Moon window look (matches the hyprminimal shell)
hl.config({
    general = {
        gaps_in = 2,
        gaps_out = 6,
        border_size = 2,
        resize_on_border = true,
        allow_tearing = false,
        layout = "dwindle",
        col = {
            active_border = "rgb(7a88cf)",
            inactive_border = "rgb(1f2335)",
        },
    },
    group = {
        col = {
            border_active = "rgb(d7729f)",
            border_inactive = "rgb(1f2335)",
            border_locked_active = "rgb(f7768e)",
            border_locked_inactive = "rgb(1f2335)",
        },
        groupbar = {
            col = {
                active = "rgb(d7729f)",
                inactive = "rgb(1f2335)",
                locked_active = "rgb(f7768e)",
                locked_inactive = "rgb(1f2335)",
            },
            text_color = "rgb(1f2335)",
            text_color_inactive = "rgb(a9b1d6)",
        },
    },
    decoration = {
        rounding = 12,
        shadow = { enabled = true },
        active_opacity = 1.0,
        inactive_opacity = 0.98,
        fullscreen_opacity = 1.0,
        blur = {
            enabled = true,
            size = 1,
            passes = 3,
            new_optimizations = true,
            ignore_opacity = true,
            xray = false,
            special = true,
        },
    },
})
