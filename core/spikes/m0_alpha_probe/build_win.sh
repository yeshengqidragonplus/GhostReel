#!/bin/sh
# M0 spike build (Windows host, llvm-mingw clang). Run from Git Bash:  sh build_win.sh
#
# Step 1: libvpx (VP9 *decoder only*, no x86 SIMD because nasm is not installed
#         yet) via its own configure/make, out-of-tree.
# Step 2: the alpha probe via CMake + Ninja.
#
# Tool locations are the ones installed for M0 (E:\Tools). Nothing is put on the
# system PATH; PATH is extended inside this script only.
set -eu

ROOT=/e/ToolsProject/GhostReel
LLVM=/e/Tools/llvm-mingw-20261020-ucrt-x86_64/bin
export PATH="$LLVM:/e/Tools/make-ndk-r23b:/e/Tools/cmake-4.4.4-windows-x86_64/bin:/e/Tools/ninja-1.13.2:$PATH"

# The libvpx Makefiles are driven by a *native* Windows make.exe (borrowed from a
# Unity-bundled NDK), which does not understand MSYS-style /e/... paths. Use the
# mixed form E:/... for everything configure bakes into the generated Makefiles.
VPX_SRC="E:/ToolsProject/GhostReel/core/third_party/libvpx"
VPX_BUILD="$ROOT/build/libvpx-win64"
PROBE_SRC="$ROOT/core/spikes/m0_alpha_probe"
PROBE_BUILD="$ROOT/build/alpha_probe-win64"

export CC=x86_64-w64-mingw32-clang CXX=x86_64-w64-mingw32-clang++
export LD=x86_64-w64-mingw32-clang AR=llvm-ar NM=llvm-nm STRIP=llvm-strip RANLIB=llvm-ranlib

mkdir -p "$VPX_BUILD"
cd "$VPX_BUILD"
if [ ! -f libvpx.a ]; then
  # No assembler is installed, and libvpx v1.17.0 on x86_64 needs one unless ALL x86
  # SIMD is off: (1) configure's assembler detection dies with "Neither yasm nor
  # nasm have been found" -- naming one with --as skips that check; (2) vpx_ports
  # builds emms_mmx.asm whenever MMX is on, regardless of --disable-x86_asm.
  # So: --disable-mmx (which also turns off every higher x86 SIMD extension) makes
  # this a plain-C build and nothing ever invokes the assembler.
  # Decode output is bit-exact with the SIMD build; only speed differs, so Windows
  # timings from this build are pessimistic (revisit once nasm is installed).
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

cd "$ROOT"
cmake -S "$PROBE_SRC" -B "$PROBE_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ \
  -DVPX_LIB="$VPX_BUILD/libvpx.a" -DVPX_ROOT="$VPX_SRC"
cmake --build "$PROBE_BUILD"
ls -l "$PROBE_BUILD/alpha_probe.exe"
