#!/usr/bin/env bash
# Build Samus Returns (Azahar + src/msr) with MSYS2 UCRT64.
# Usage: tools/msr/windows/build.sh [build-dir]   (run from any shell; the
# script puts the UCRT64 toolchain first on PATH itself)
set -euo pipefail
cd "$(dirname "$0")/../../.."

MSYS_ROOT="${MSYS_ROOT:-/c/msys64}"
export MSYSTEM=UCRT64
export PATH="$MSYS_ROOT/ucrt64/bin:$MSYS_ROOT/usr/bin:$PATH"
export CCACHE_DIR="$PWD/.ccache-windows"
export CCACHE_MAXSIZE=4G
export CCACHE_COMPILERCHECK=content

build_dir="${1:-build-windows}"

cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DUSE_SYSTEM_QT=ON -DUSE_SYSTEM_SDL2=ON -DENABLE_LTO=OFF \
  -DCITRA_WARNINGS_AS_ERRORS=OFF \
  -DENABLE_MSR_GAME_FOLDER=ON \
  -DENABLE_QT=ON -DENABLE_VULKAN=ON -DENABLE_TESTS=OFF \
  -DENABLE_LIBRETRO=OFF -DENABLE_ROOM_STANDALONE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF \
  -DENABLE_WEB_SERVICE=OFF
cmake --build "$build_dir" --parallel "${MSR_JOBS:-$(nproc)}"
cmake --build "$build_dir" --target bundle
ccache -s | head -5
