#!/bin/sh
# M0 spike: build the empty C++17 library for Android arm64-v8a with the NDK installed in the SDK.
# Run from the repo root in Git Bash:  sh core/spikes/m0_empty_lib/build_android_arm64.sh
# Nothing is added to the system PATH; the tools are only put on PATH inside this process.
set -eu

NDK="${ANDROID_NDK:-E:/AndroidSDK/ndk/30.0.16248370}"
# PATH entries must use Git Bash's /e/... form (E:/... only works for arguments, not PATH).
CMAKE_BIN="/e/Tools/cmake-4.4.4-windows-x86_64/bin"
NINJA_BIN="/e/Tools/ninja-1.13.2"
# android-24 is an arbitrary smoke-test value: the project's minimum API level is not decided yet.
API="${ANDROID_API:-24}"

export PATH="$CMAKE_BIN:$NINJA_BIN:$PATH"

SRC="core/spikes/m0_empty_lib"
OUT="build/empty_lib-android-arm64"

cmake -S "$SRC" -B "$OUT" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT"

READELF="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe"
SO="$OUT/libac_empty.so"
echo "--- $SO ---"
ls -l "$SO"
echo "--- program headers (LOAD segments; Align must be 0x4000 or larger for 16 KB pages) ---"
"$READELF" -lW "$SO" | grep -E 'LOAD|Type'
echo "--- dynamic section: NEEDED entries ---"
"$READELF" -dW "$SO" | grep NEEDED || true
echo "--- exported ac_* symbols ---"
"$READELF" --dyn-syms -W "$SO" | grep ' ac_' || true
