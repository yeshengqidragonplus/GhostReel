#!/bin/sh
# Builds the core static library and the alphaclip CLI for Android arm64-v8a with the NDK in the SDK.
# Run from Git Bash at the repo root:  sh core/scripts/build_core_android_arm64.sh
# Output: build/core-android-arm64/bin/alphaclip (a command-line executable for adb shell, not an APK)
#
# android-24 is an arbitrary value: the project's minimum API level is not decided yet.
# Static libc++ (c++_static) so there is no libc++_shared.so to ship. NDK r30 links with 16 KB
# LOAD-segment alignment by default; the script prints the segment alignment as a check.
set -eu

ROOT=/e/ToolsProject/GhostReel
NDK="${ANDROID_NDK:-E:/AndroidSDK/ndk/30.0.16248370}"
API="${ANDROID_API:-24}"
export PATH="/e/Tools/cmake-4.4.4-windows-x86_64/bin:/e/Tools/ninja-1.13.2:$PATH"

sh "$ROOT/core/scripts/build_libvpx_android_arm64.sh"

cd "$ROOT"
OUT=build/core-android-arm64
cmake -S core -B "$OUT" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DAC_VPX_LIB="E:/ToolsProject/GhostReel/build/libvpx-android-arm64/libvpx.a"
cmake --build "$OUT"

READELF="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe"
ls -l "$OUT/bin/alphaclip"
"$READELF" -h "$OUT/bin/alphaclip" | grep -E 'Machine|Type'
"$READELF" -lW "$OUT/bin/alphaclip" | grep -E 'LOAD|Type'
"$READELF" -dW "$OUT/bin/alphaclip" | grep NEEDED || true
