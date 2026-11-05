#!/bin/sh
# Builds the core static library and the alphaclip CLI for Windows x86_64 (llvm-mingw clang).
# Run from Git Bash:  sh core/scripts/build_core_win64.sh
# Output: build/core-win64/bin/alphaclip.exe
set -eu

ROOT=/e/ToolsProject/GhostReel
export PATH="/e/Tools/llvm-mingw-20261020-ucrt-x86_64/bin:/e/Tools/cmake-4.4.4-windows-x86_64/bin:/e/Tools/ninja-1.13.2:$PATH"

sh "$ROOT/core/scripts/build_libvpx_win64.sh"

cd "$ROOT"
cmake -S core -B build/core-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ \
  -DAC_VPX_LIB="E:/ToolsProject/GhostReel/build/libvpx-win64/libvpx.a"
cmake --build build/core-win64
ls -l build/core-win64/bin/alphaclip.exe
