#!/usr/bin/env bash
# Load Technicolor's patched hyprwater.
#
# Upstream hyprwater caches a glass LAYER surface's sampled backdrop and only
# refreshes it when a window behind moves / focus / workspace changes, or the
# layer itself animates — never on a wallpaper frame. So the alt-tab pie's
# liquid glass froze the (animated) wallpaper it captured when it mapped. The
# one-line patch (src/GlassLayerSurface.cpp, kForceLiveLayer) forces glass
# layers to re-sample every frame.
#
# The BSD-3 source is vendored next to this script (see ./LICENSE) and built
# locally — no hyprpm, no fork. Called from the hyprland.lua startup handler.
#
# ABI SAFETY: never dlopen a .so built against a different Hyprland — a stale
# plugin segfaults the compositor during dlopen and drops the session into
# safe mode (that is exactly what the 0.55.4 -> 0.56.0 update did to
# Hypr-DarkWindow). ../plugin-abi.sh rebuilds on any Hyprland/hypr*-library
# change BEFORE loading, and refuses to load if that build fails — you lose the
# glass effect, never the session.
set -u
DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
SO="$DIR/hyprwater.so"
STAMP="$DIR/.abi-stamp"

if [ -r "$DIR/../plugin-abi.sh" ]; then
    . "$DIR/../plugin-abi.sh"
    tc_plugin_guard "$DIR" "$SO" "$STAMP" "Liquid glass (hyprwater)" || exit 0
else
    # No guard available: still never load a possibly-stale binary — rebuild it.
    make -s -B -C "$DIR" >/dev/null 2>&1 && [ -f "$SO" ] || exit 0
fi

# Drop any hyprpm-managed UPSTREAM hyprglass that is already loaded (this fork
# replaces it, and the two would fight over the same decorations). That name
# stays 'hyprglass' on purpose: it refers to the other project, not to this one.
# the user or the system store) so ours is the one in effect.
for p in \
    "$HOME"/.local/share/hyprpm/*/hyprglass.so \
    "$HOME"/.local/share/hyprpm/*/*/hyprglass.so \
    /var/cache/hyprpm/*/*/hyprglass.so; do
    [ -f "$p" ] && [ "$p" != "$SO" ] && hyprctl plugin unload "$p" >/dev/null 2>&1
done
hyprctl plugin unload "$SO" >/dev/null 2>&1   # idempotent on re-run
hyprctl plugin load   "$SO" >/dev/null 2>&1
