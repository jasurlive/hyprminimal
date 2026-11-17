-- Ignore maximize requests from apps (kitty asks to be maximized on open, which stops it tiling)
hl.window_rule({ match = { class = ".*" }, suppress_event = "maximize" })

-- Float rules
hl.window_rule({ match = { class = "^org\\.pulseaudio\\.pavucontrol$" }, float = true, size = "1100 620" })
hl.window_rule({ match = { class = "^blueman-manager$" },                 float = true, size = "700 450" })
hl.window_rule({ match = { class = "^nm-connection-editor$" },            float = true })
hl.window_rule({ match = { class = "^nwg-displays$" },                    float = true })
hl.window_rule({ match = { class = "^hyprpolkitagent$" },                 float = true })
hl.window_rule({ match = { title = "^File Operation Progress$" },         float = true, size = "600 50" })
hl.window_rule({ match = { title = "^(Open File|Save File|All Files)$" }, float = true, size = "900 600", center = 1 })

-- Picture-in-picture
hl.window_rule({ match = { title = "^([Pp]icture[-\\s]?[Ii]n[-\\s]?[Pp]icture).*$" }, tag = "picture-in-picture" })
hl.window_rule({ match = { tag = "picture-in-picture" }, float = true, pin = true, keep_aspect_ratio = true, move = "73% 72%", size = "25% 25%" })

return true
