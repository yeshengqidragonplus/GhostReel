#!/bin/sh
# M0 spike: cross-build the decoder-only libvpx for Android arm64 (NEON) with the NDK in the SDK.
# Run from the repo root in Git Bash:  sh core/spikes/m0_alpha_probe/build_libvpx_android_arm64.sh
#
# libvpx has no CMake build and its "standalone NDK toolchain" mode expects tool names ending in
# "clang" (configure.sh matches `*clang)` to add `-c` to the assembler flags). The NDK's own
# per-target wrappers on Windows are .cmd files, so we generate tiny sh wrappers instead.
# They live under build/ (git-ignored). Nothing is put on the system PATH.
set -eu

ROOT=/e/ToolsProject/GhostReel
NDK="${ANDROID_NDK:-/e/AndroidSDK/ndk/30.0.16248370}"
API="${ANDROID_API:-24}"          # arbitrary smoke value: minimum API level is not decided yet
TRIPLE="aarch64-linux-android${API}"
LLVM="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"

export PATH="$LLVM:/e/Tools/make-ndk-r23b:$PATH"

# The Makefiles are driven by a native Windows make.exe; it needs E:/... style source paths.
VPX_SRC="E:/ToolsProject/GhostReel/core/third_party/libvpx"
OUT="$ROOT/build/libvpx-android-arm64"
WRAP="$ROOT/build/ndk-wrap-arm64"

mkdir -p "$WRAP" "$OUT"
for pair in "clang:clang" "clang++:clang++"; do
    name="${pair%%:*}"
    printf '#!/bin/sh\nexec "%s/%s.exe" --target=%s "$@"\n' "$LLVM" "$name" "$TRIPLE" \
        > "$WRAP/$TRIPLE-$name"
    chmod +x "$WRAP/$TRIPLE-$name"
done

cd "$OUT"
CC="$WRAP/$TRIPLE-clang" \
CXX="$WRAP/$TRIPLE-clang++" \
AS="$WRAP/$TRIPLE-clang" \
LD="$WRAP/$TRIPLE-clang++" \
AR="$LLVM/llvm-ar.exe" \
STRIP="$LLVM/llvm-strip.exe" \
sh "$VPX_SRC/configure" \
    --target=arm64-android-gcc \
    --disable-vp8 --enable-vp9 --disable-vp9-encoder \
    --disable-webm-io --disable-libyuv \
    --disable-examples --disable-tools --disable-docs --disable-unit-tests \
    --disable-install-bins --disable-install-docs \
    --enable-pic --enable-static --disable-shared

make -j8
ls -l "$OUT/libvpx.a"
