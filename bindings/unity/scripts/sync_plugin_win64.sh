#!/bin/sh
# Copies the core DLL built by core/scripts/build_core_win64.sh into the Unity package, where the editor
# loads it as a native plugin (Editor + Windows x86_64).
# Run from Git Bash at the repo root:  sh bindings/unity/scripts/sync_plugin_win64.sh
# Close Unity first: the editor keeps a loaded plugin DLL locked, so the copy would fail or be skipped.
set -eu

ROOT=/e/ToolsProject/GhostReel
SRC="$ROOT/build/core-win64/bin/alphaclip.dll"
DST="$ROOT/bindings/unity/package/Runtime/Plugins/x86_64"

test -f "$SRC" || { echo "missing $SRC: run  sh core/scripts/build_core_win64.sh  first"; exit 1; }
mkdir -p "$DST"
cp "$SRC" "$DST/alphaclip.dll"
ls -l "$DST/alphaclip.dll"
