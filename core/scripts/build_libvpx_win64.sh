#!/bin/sh
# Builds libvpx (VP9 decoder only) for Windows x86_64 with llvm-mingw clang, out-of-tree into
# build/libvpx-win64. Idempotent: does nothing when libvpx.a already exists.
# Run from Git Bash:  sh core/scripts/build_libvpx_win64.sh
#
# No assembler is installed, and libvpx v1.17.0 on x86_64 needs one unless ALL x86 SIMD is off:
# (1) configure's assembler detection dies with "Neither yasm nor nasm have been found" - naming
# one with --as skips that check; (2) vpx_ports builds emms_mmx.asm whenever MMX is on, regardless
# of --disable-x86_asm. So --disable-mmx (which also turns off every higher x86 SIMD extension)
# gives a plain-C build and nothing ever invokes the assembler. The output is bit-exact with the
# SIMD build; only speed differs, so Windows timings are pessimistic until nasm is installed.
set -eu

ROOT=/e/ToolsProject/GhostReel
LLVM=/e/Tools/llvm-mingw-20261020-ucrt-x86_64/bin
# PATH entries must be /e/... (E:/... only works inside arguments).
export PATH="$LLVM:/e/Tools/make-ndk-r23b:$PATH"

# libvpx's Makefiles are driven by a native Windows make.exe (borrowed from a Unity-bundled NDK),
# which does not understand MSYS-style /e/... paths: pass the mixed form E:/... to configure.
VPX_SRC="E:/ToolsProject/GhostReel/core/third_party/libvpx"
OUT="$ROOT/build/libvpx-win64"

export CC=x86_64-w64-mingw32-clang CXX=x86_64-w64-mingw32-clang++
export LD=x86_64-w64-mingw32-clang AR=llvm-ar NM=llvm-nm STRIP=llvm-strip RANLIB=llvm-ranlib

mkdir -p "$OUT"
cd "$OUT"
if [ ! -f libvpx.a ]; then
  sh "$VPX_SRC/configure" \
    --target=x86_64-win64-gcc \
    --disable-vp8 --enable-vp9 --disable-vp9-encoder \
    --disable-mmx --disable-x86_asm --as=yasm \
    --disable-webm-io --disable-libyuv \
    --disable-examples --disable-tools --disable-docs --disable-unit-tests \
    --disable-install-bins --disable-install-docs \
    --enable-static --disable-shared
  make -j"${NUMBER_OF_PROCESSORS:-4}"
fi
ls -l libvpx.a
