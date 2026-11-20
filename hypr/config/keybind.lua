local scrPath = (os.getenv("HOME") or "") .. "/.config/hypr/Scripts"
local mainMod = "SUPER"
local TERMINAL = "kitty"
local EDITOR = "code"
local EXPLORER = "thunar"
local BROWSER = "firefox"
local WORKSPACE = scrPath .. "/workspace.sh"
local SATTY = scrPath .. "/satty.sh"
local SHELLMSG = (os.getenv("HOME") or "") .. "/.local/share/hyprminimal/bin/hyprminimal-msg"

local KEY = {
	TERMINAL = ("%s + SPACE"):format(mainMod),
	TERMINAL_ALT = ("%s + RETURN"):format(mainMod),
	EDITOR = ("%s + C"):format(mainMod),
	EXPLORER = ("%s + E"):format(mainMod),
	BROWSER = ("%s + B"):format(mainMod),
	WORKSPACE = ("%s + TAB"):format(mainMod),
	LAUNCHER = ("%s + R"):format(mainMod),
	LOCK = ("%s + L"):format(mainMod),
	SETTINGS = ("%s + I"):format(mainMod),
	CHEATSHEET_TOGGLE = ("%s + H"):format(mainMod),
	CONTROL_CENTER = ("%s + S"):format(mainMod),
	WIFI = ("%s + W"):format(mainMod),
	CALENDAR = ("%s + D"):format(mainMod),
	BATTERY = ("%s + P"):format(mainMod),
	SESSION = ("%s + ESCAPE"):format(mainMod),

	CLOSE = ("%s + Q"):format(mainMod),
	FLOAT = ("%s + V"):format(mainMod),
	FULLSCREEN = ("%s + F"):format(mainMod),

	WORKSPACE_NEXT_MONITOR = ("%s + CTRL + tab"):format(mainMod),
	WORKSPACE_PREV_MONITOR = ("%s + SHIFT + tab"):format(mainMod),

	FOCUS_LEFT = ("%s + left"):format(mainMod),
	FOCUS_RIGHT = ("%s + right"):format(mainMod),
	FOCUS_UP = ("%s + up"):format(mainMod),
	FOCUS_DOWN = ("%s + down"):format(mainMod),

	RESIZE_RIGHT = ("%s + SHIFT + right"):format(mainMod),
	RESIZE_LEFT = ("%s + SHIFT + left"):format(mainMod),
	RESIZE_UP = ("%s + SHIFT + up"):format(mainMod),
	RESIZE_DOWN = ("%s + SHIFT + down"):format(mainMod),

	MOVE_SWAP_LEFT = ("%s + SHIFT + CTRL + left"):format(mainMod),
	MOVE_SWAP_RIGHT = ("%s + SHIFT + CTRL + right"):format(mainMod),
	MOVE_SWAP_UP = ("%s + SHIFT + CTRL + up"):format(mainMod),
	MOVE_SWAP_DOWN = ("%s + SHIFT + CTRL + down"):format(mainMod),

	WORKSPACE_NEXT = ("%s + CTRL + right"):format(mainMod),
	WORKSPACE_PREV = ("%s + CTRL + left"):format(mainMod),
	WORKSPACE_EMPTY = ("%s + CTRL + down"):format(mainMod),

	MOVE_TO_NEXT_WORKSPACE = ("%s + CTRL + ALT + right"):format(mainMod),
	MOVE_TO_PREV_WORKSPACE = ("%s + CTRL + ALT + left"):format(mainMod),

	SCROLL_WORKSPACE_NEXT = ("%s + mouse_down"):format(mainMod),
	SCROLL_WORKSPACE_PREV = ("%s + mouse_up"):format(mainMod),

	MOUSE_DRAG = ("%s + mouse:272"):format(mainMod),
	MOUSE_RESIZE = ("%s + mouse:273"):format(mainMod),
	RESIZE_MODE = ("%s + X"):format(mainMod),
}

-- 1. Applications
hl.bind(KEY.TERMINAL, hl.dsp.exec_cmd(TERMINAL), { description = "Terminal" })
hl.bind(KEY.TERMINAL_ALT, hl.dsp.exec_cmd(TERMINAL), { description = "Terminal" })
hl.bind(KEY.EDITOR, hl.dsp.exec_cmd(EDITOR), { description = "Code editor" })
hl.bind(KEY.EXPLORER, hl.dsp.exec_cmd(EXPLORER), { description = "File manager" })
hl.bind(KEY.BROWSER, hl.dsp.exec_cmd(BROWSER), { description = "Browser" })

-- 2. Shell (hyprminimal)
hl.bind(KEY.LAUNCHER, hl.dsp.exec_cmd(SHELLMSG .. " launcher"), { description = "Toggle launcher" })
hl.bind(KEY.LOCK, hl.dsp.exec_cmd(SHELLMSG .. " lock"), { description = "Lock screen" })
hl.bind(KEY.SETTINGS, hl.dsp.exec_cmd(SHELLMSG .. " settings"), { description = "Toggle settings" })
hl.bind(KEY.CHEATSHEET_TOGGLE, hl.dsp.exec_cmd(SHELLMSG .. " cheatsheet"), { description = "Toggle cheatsheet" })
hl.bind(KEY.CONTROL_CENTER, hl.dsp.exec_cmd(SHELLMSG .. " control-center"), { description = "Control center" })
hl.bind(KEY.WIFI, hl.dsp.exec_cmd(SHELLMSG .. " panel network"), { description = "Toggle Wi-Fi popup" })
hl.bind(KEY.CALENDAR, hl.dsp.exec_cmd(SHELLMSG .. " panel calendar"), { description = "Toggle calendar popup" })
hl.bind(KEY.BATTERY, hl.dsp.exec_cmd(SHELLMSG .. " panel battery"), { description = "Toggle battery popup" })
hl.bind(KEY.SESSION, hl.dsp.exec_cmd(SHELLMSG .. " session"), { description = "Session menu" })

-- 3. Window Management
hl.bind(KEY.CLOSE, hl.dsp.window.close(), { description = "Close window" })
hl.bind(KEY.FLOAT, hl.dsp.window.float({ action = "toggle" }), { description = "Toggle floating" })
hl.bind(KEY.FULLSCREEN, hl.dsp.window.fullscreen(), { description = "Toggle fullscreen" })

-- 4. Window Navigation & Layout
hl.bind(KEY.WORKSPACE_NEXT_MONITOR, hl.dsp.exec_cmd(WORKSPACE), { description = "Next workspace on monitor" })
hl.bind(KEY.WORKSPACE_PREV_MONITOR, hl.dsp.focus({ workspace = "m-1" }), { description = "Previous workspace on monitor" })

hl.bind(KEY.FOCUS_LEFT, hl.dsp.focus({ direction = "left" }), { description = "Focus left" })
hl.bind(KEY.FOCUS_RIGHT, hl.dsp.focus({ direction = "right" }), { description = "Focus right" })
hl.bind(KEY.FOCUS_UP, hl.dsp.focus({ direction = "up" }), { description = "Focus up" })
hl.bind(KEY.FOCUS_DOWN, hl.dsp.focus({ direction = "down" }), { description = "Focus down" })

hl.bind("ALT + tab", hl.dsp.window.cycle_next(), { description = "Cycle windows" })

hl.bind(KEY.RESIZE_RIGHT, hl.dsp.window.resize({ x = 30, y = 0, relative = true }), { description = "Resize window right" })
hl.bind(KEY.RESIZE_LEFT, hl.dsp.window.resize({ x = -30, y = 0, relative = true }), { description = "Resize window left" })
hl.bind(KEY.RESIZE_UP, hl.dsp.window.resize({ x = 0, y = -30, relative = true }), { description = "Resize window up" })
hl.bind(KEY.RESIZE_DOWN, hl.dsp.window.resize({ x = 0, y = 30, relative = true }), { description = "Resize window down" })

hl.bind(KEY.MOVE_SWAP_LEFT, hl.dsp.exec_cmd(scrPath .. "/move-or-swap.sh l"), { description = "Move or swap window left" })
hl.bind(KEY.MOVE_SWAP_RIGHT, hl.dsp.exec_cmd(scrPath .. "/move-or-swap.sh r"), { description = "Move or swap window right" })
hl.bind(KEY.MOVE_SWAP_UP, hl.dsp.exec_cmd(scrPath .. "/move-or-swap.sh u"), { description = "Move or swap window up" })
hl.bind(KEY.MOVE_SWAP_DOWN, hl.dsp.exec_cmd(scrPath .. "/move-or-swap.sh d"), { description = "Move or swap window down" })

-- 5. Screenshots
hl.bind("PRINT", hl.dsp.exec_cmd("HYPRSHOT_DIR=~/Pictures/Screenshots hyprshot -m region"), { description = "Screenshot region" })
hl.bind("SUPER + PRINT", hl.dsp.exec_cmd("HYPRSHOT_DIR=~/Pictures/Screenshots hyprshot -m window"), { description = "Screenshot window" })
hl.bind("ALT + PRINT", hl.dsp.exec_cmd("HYPRSHOT_DIR=~/Pictures/Screenshots hyprshot -m output"), { description = "Screenshot monitor" })
hl.bind("SHIFT + PRINT", hl.dsp.exec_cmd("HYPRSHOT_DIR=~/Pictures/Screenshots hyprshot -m output -m active"), { description = "Screenshot whole screen, instantly" })
hl.bind("SUPER + A", hl.dsp.exec_cmd(SATTY), { description = "Screenshot annotate" })

-- 6. Workspace Switching
for i = 1, 9 do
	local focusKey = ("%s + %d"):format(mainMod, i)
	local moveKey = ("%s + SHIFT + %d"):format(mainMod, i)
	hl.bind(focusKey, hl.dsp.focus({ workspace = i }), { description = "Workspace " .. i })
	hl.bind(moveKey, hl.dsp.window.move({ workspace = i }), { description = "Move to workspace " .. i })
end
hl.bind(("%s + 0"):format(mainMod), hl.dsp.focus({ workspace = 10 }), { description = "Workspace 10" })
hl.bind(("%s + SHIFT + 0"):format(mainMod), hl.dsp.window.move({ workspace = 10 }), { description = "Move to workspace 10" })

-- 7. Workspace Navigation
hl.bind(KEY.WORKSPACE_NEXT, hl.dsp.focus({ workspace = "r+1" }), { description = "Next workspace" })
hl.bind(KEY.WORKSPACE_PREV, hl.dsp.focus({ workspace = "r-1" }), { description = "Previous workspace" })
hl.bind(KEY.WORKSPACE_EMPTY, hl.dsp.focus({ workspace = "empty" }), { description = "Open empty workspace" })
hl.bind(KEY.WORKSPACE, hl.dsp.exec_cmd(WORKSPACE), { description = "Workspace tabbing" })

-- 8. Move Window Across Workspaces
hl.bind(KEY.MOVE_TO_NEXT_WORKSPACE, hl.dsp.window.move({ workspace = "r+1" }), { description = "Move to next workspace" })
hl.bind(KEY.MOVE_TO_PREV_WORKSPACE, hl.dsp.window.move({ workspace = "r-1" }), { description = "Move to previous workspace" })

-- 9. Mouse Workspace Navigation
hl.bind(KEY.SCROLL_WORKSPACE_NEXT, hl.dsp.focus({ workspace = "e+1" }), { description = "Scroll to next workspace" })
hl.bind(KEY.SCROLL_WORKSPACE_PREV, hl.dsp.focus({ workspace = "e-1" }), { description = "Scroll to previous workspace" })

-- 10. Mouse Window Management
hl.bind(KEY.MOUSE_DRAG, hl.dsp.window.drag(), { mouse = true, description = "Drag window" })
hl.bind(KEY.MOUSE_RESIZE, hl.dsp.window.resize(), { mouse = true, description = "Resize window" })
hl.bind(KEY.RESIZE_MODE, hl.dsp.window.resize(), { description = "Resize mode" })

-- 11. Media
hl.bind("XF86AudioPlay", hl.dsp.exec_cmd(SHELLMSG .. " media toggle"), { locked = true, description = "Play or pause" })
hl.bind("XF86AudioNext", hl.dsp.exec_cmd(SHELLMSG .. " media next"), { locked = true, description = "Next track" })
hl.bind("XF86AudioPrev", hl.dsp.exec_cmd(SHELLMSG .. " media previous"), { locked = true, description = "Previous track" })
hl.bind("XF86AudioRaiseVolume", hl.dsp.exec_cmd(scrPath .. "/accel-step.sh volume up"), { locked = true, repeating = true, description = "Volume up (hold to accelerate)" })
hl.bind("XF86AudioLowerVolume", hl.dsp.exec_cmd(scrPath .. "/accel-step.sh volume down"), { locked = true, repeating = true, description = "Volume down (hold to accelerate)" })
hl.bind("XF86AudioMute", hl.dsp.exec_cmd("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"), { locked = true, description = "Volume mute" })

-- 12. Brightness
hl.bind("XF86MonBrightnessDown", hl.dsp.exec_cmd(scrPath .. "/accel-step.sh brightness down"), { locked = true, repeating = true, description = "Brightness down (hold to accelerate)" })
hl.bind("XF86MonBrightnessUp", hl.dsp.exec_cmd(scrPath .. "/accel-step.sh brightness up"), { locked = true, repeating = true, description = "Brightness up (hold to accelerate)" })

return true
