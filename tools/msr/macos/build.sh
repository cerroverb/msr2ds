#!/usr/bin/env bash
# Build Samus Returns (Azahar + src/msr) on macOS with Apple Clang. Qt and MoltenVK are
# downloaded by Azahar's CMake, and SDL2 is its bundled copy, so nothing is installed.
# Usage: tools/msr/macos/build.sh [build-dir [cmake options...]]
#   build-dir   default build-macos. If build_recomp.sh configured it, it stays recompiled.
#   MSR_JOBS    parallel compile jobs (default: the number of cores)
# The app is <build-dir>/bundle/SamusReturns.app; its user/ folder goes beside it.
set -euo pipefail
cd "$(dirname "$0")/../../.."

build_dir="${1:-build-macos}"
shift || true
launcher=()
if command -v ccache >/dev/null; then
  export CCACHE_DIR="$PWD/.ccache-macos" CCACHE_MAXSIZE=8G CCACHE_COMPILERCHECK=content
  launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release ${launcher[@]+"${launcher[@]}"} \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DUSE_SYSTEM_QT=OFF -DUSE_SYSTEM_SDL2=OFF -DENABLE_LTO=OFF \
  -DENABLE_QT_TRANSLATION=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF \
  -DENABLE_MSR_GAME_FOLDER=ON \
  -DENABLE_QT=ON -DENABLE_VULKAN=ON -DENABLE_TESTS=OFF \
  -DENABLE_LIBRETRO=OFF -DENABLE_ROOM_STANDALONE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF \
  -DENABLE_WEB_SERVICE=OFF "$@"
cmake --build "$build_dir" --parallel "${MSR_JOBS:-$(sysctl -n hw.ncpu)}"
cmake --build "$build_dir" --target bundle
