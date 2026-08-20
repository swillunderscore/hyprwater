#!/usr/bin/env bash
# Rebuild and reload without ever writing over a mapped plugin.
# Linking straight onto hyprwater.so while Hyprland has it dlopen'd is a way to
# lose the session: the compositor is executing from that file. Unload first,
# build second, load third.
set -u
DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
SO="$DIR/hyprwater.so"
hyprctl plugin unload "$SO" >/dev/null 2>&1
# ALWAYS a full rebuild. Two compositor crashes (free(): invalid pointer in
# commitPendingPresets at plugin init) were traced to FRANKENBUILDS: deploys
# that copied build directories around left mixed-generation object files
# whose struct layouts disagreed, and make linked them without complaint.
# Thirty seconds of rebuild is cheaper than a session.
make -C "$DIR" clean >/dev/null
make -C "$DIR" "$@" || exit 1
hyprctl plugin load "$SO"
# A plugin load SEVERS the binding between runtime `hl.config` evals and the
# plugin's config values — Settings sliders and toggles silently stop reaching
# the plugin until the next full config reload re-links them (proven with a
# nested A/B: evals dead after load, alive again after reload; load.sh at
# session start always did this, which is why fresh sessions behaved).
hyprctl reload >/dev/null 2>&1
