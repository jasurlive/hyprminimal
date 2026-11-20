-- animations off: frames appear at once, nothing to render in between (snappier, less GPU/battery).
-- To bring them back: enabled = true, then add hl.curve()/hl.animation() lines (see the Hyprland wiki).
hl.config({
    animations = {
        enabled = false,
    },
})
