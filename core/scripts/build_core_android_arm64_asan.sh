#!/bin/sh
# AddressSanitizer + UndefinedBehaviorSanitizer build of the core and CLI for Android arm64,
# libvpx included. For memory-safety runs on a device (see core/tests/adb_cli.py).
# Run from Git Bash at the repo root:  sh core/scripts/build_core_android_arm64_asan.sh
# Output: build/core-android-arm64-asan/bin/alphaclip. On the device it also needs the NDK's
# runtimes next to it (printed at the end) and LD_LIBRARY_PATH pointing at that folder.
set -eu

ROOT=/e/ToolsProject/GhostReel
NDK="${ANDROID_NDK:-E:/AndroidSDK/ndk/30.0.16248370}"
API="${ANDROID_API:-24}"
export PATH="/e/Tools/cmake-4.4.4-windows-x86_64/bin:/e/Tools/ninja-1.13.2:$PATH"

AC_VPX_ASAN=1 sh "$ROOT/core/scripts/build_libvpx_android_arm64.sh"

cd "$ROOT"
OUT=build/core-android-arm64-asan
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g"
cmake -S core -B "$OUT" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="$SAN" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DAC_VPX_LIB="E:/ToolsProject/GhostReel/build/libvpx-android-arm64-asan/libvpx.a"
cmake --build "$OUT"
ls -l "$OUT/bin/alphaclip"

RT="$NDK/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/21/lib/linux"
echo "runtimes to push next to the binary:"
ls -l "$RT/libclang_rt.asan-aarch64-android.so" "$RT/libclang_rt.ubsan_standalone-aarch64-android.so"
