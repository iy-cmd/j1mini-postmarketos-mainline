#!/bin/sh
# SXMO device profile for Samsung Galaxy J1 Mini 3G (SM-J105H, 480x800)

export GDK_SCALE=1
export GDK_DPI_SCALE=1
export QT_SCALE_FACTOR=1
export QT_AUTO_SCREEN_SCALE_FACTOR=0

# Compact keyboard height for 480x800 screen
export KEYBOARD_ARGS="-g 480x280+0+520"

# Compact bemenu/dmenu options for 480px width
export BEMENU_OPTS="--ab #222222 --af #bbbbbb --bdr #005577 --border 2 --cb #222222 --center --cf #bbbbbb --fb #222222 --fbb #eeeeee --fbf #222222 --ff #bbbbbb --fixed-height --fn Sxmo\ 10 --hb #005577 --hf #eeeeee --line-height 18 --list 14 --margin 10 --nb #222222 --nf #bbbbbb --no-overlap --no-spacing --sb #323232 --scb #005577 --scf #eeeeee --scrollbar autohide --tb #005577 --tf #eeeeee --wrap"

# Keep idle/suspend safe until panel power-off is wired
mkdir -p "${XDG_CACHE_HOME:-$HOME/.cache}/sxmo" 2>/dev/null || true
touch "${XDG_CACHE_HOME:-$HOME/.cache}/sxmo/sxmo.nosuspend" 2>/dev/null || true
touch "${XDG_CACHE_HOME:-$HOME/.cache}/sxmo/sxmo.noidle" 2>/dev/null || true
export GSK_RENDERER=cairo
export WEBKIT_DISABLE_DMABUF_RENDERER=1
export GDK_DISABLE=gl,vulkan
export WEBKIT_DISABLE_COMPOSITING_MODE=1
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
