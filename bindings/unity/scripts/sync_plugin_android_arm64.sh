#!/bin/sh
# Copies the stripped Android core library built by core/scripts/build_core_android_arm64.sh into the Unity package,
# where AlphaClipPluginSettings marks it as an Android arm64 plugin.
# Run from Git Bash at the repo root:  sh bindings/unity/scripts/sync_plugin_android_arm64.sh
# The stripped copy is the one to ship (0.7 MB; the unstripped build is 5.5 MB of symbols).
set -eu

ROOT=/e/ToolsProject/GhostReel
SRC="$ROOT/build/core-android-arm64/libalphaclip.stripped.so"
DST="$ROOT/bindings/unity/package/Runtime/Plugins/Android/arm64-v8a"

test -f "$SRC" || { echo "missing $SRC: run  sh core/scripts/build_core_android_arm64.sh  first"; exit 1; }
mkdir -p "$DST"
cp "$SRC" "$DST/libalphaclip.so"
ls -l "$DST/libalphaclip.so"
