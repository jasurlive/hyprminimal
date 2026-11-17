#!/bin/sh

state="$HOME/.cache/hypr-display-mode"
mode=$(cat "$state" 2>/dev/null || echo laptop)

case "$mode" in
laptop)
    hyprctl eval 'hl.monitor({output="eDP-1", disabled=false})'
    hyprctl eval 'hl.monitor({output="HDMI-A-1", mode="1920x1080@60", position="0x0", scale=1.25})'
    echo hdmi > "$state"
    ;;
hdmi)
    hyprctl eval 'hl.monitor({output="eDP-1", mode="1920x1080@60.05", position="0x0", scale=1.25})'
    hyprctl eval 'hl.monitor({output="HDMI-A-1", mode="1920x1080@60", position="1536x0", scale=1.25})'
    echo extend > "$state"
    ;;
extend)
    hyprctl eval 'hl.monitor({output="eDP-1", mode="1920x1080@60.05", position="0x0", scale=1.25})'
    hyprctl eval 'hl.monitor({output="HDMI-A-1", mode="1920x1080@60", position="0x0", scale=1.25, mirror="eDP-1"})'
    echo mirror > "$state"
    ;;
mirror)
    hyprctl eval 'hl.monitor({output="eDP-1", mode="1920x1080@60.05", position="0x0", scale=1.25})'
    hyprctl eval 'hl.monitor({output="HDMI-A-1", disabled=true})'
    echo laptop > "$state"
    ;;
esac
