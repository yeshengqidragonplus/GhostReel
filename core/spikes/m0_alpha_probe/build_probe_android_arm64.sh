#!/bin/sh
# M0 spike: build alpha_probe for Android arm64 against the libvpx built by
# build_libvpx_android_arm64.sh (run that first).
# Run from the repo root in Git Bash:  sh core/spikes/m0_alpha_probe/build_probe_android_arm64.sh
set -eu

NDK="${ANDROID_NDK:-E:/AndroidSDK/ndk/30.0.16248370}"
API="${ANDROID_API:-24}"          # arbitrary smoke value: minimum API level is not decided yet
export PATH="/e/Tools/cmake-4.4.4-windows-x86_64/bin:/e/Tools/ninja-1.13.2:$PATH"

OUT="build/alpha_probe-android-arm64"
VPX_LIB="E:/ToolsProject/GhostReel/build/libvpx-android-arm64/libvpx.a"
test -f "$VPX_LIB" || { echo "missing $VPX_LIB: run build_libvpx_android_arm64.sh first"; exit 1; }

cmake -S core/spikes/m0_alpha_probe -B "$OUT" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DVPX_LIB="$VPX_LIB"
cmake --build "$OUT"

READELF="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe"
echo "--- $OUT/alpha_probe ---"
ls -l "$OUT/alpha_probe"
"$READELF" -lW "$OUT/alpha_probe" | grep -E 'LOAD|Type'
"$READELF" -dW "$OUT/alpha_probe" | grep NEEDED || true
