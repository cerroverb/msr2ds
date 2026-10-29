#!/usr/bin/env bash
# Build the recompiled Samus Returns: Azahar + src/msr + the game's code recompiled ahead of
# time from your own dump (tools/msr/recomp). With MSYS2 UCRT64, like build.sh.
#
# Usage: tools/msr/windows/build_recomp.sh <dump.cci|.3ds> [build-dir]
#   build-dir   default build-windows: the recompiled build is the main one. build.sh in the
#               same folder keeps building it recompiled; use another folder for an
#               emulated-CPU build to share (tools/msr/windows/package.py).
#   MSR_PYTHON  Python 3 with capstone (python -m pip install capstone); default: python
#   MSR_JOBS    parallel compile jobs (default: nproc - 4; the generated code is large)
#
# Entry points the last run reached only through the JIT (<bundle>/user/msr/
# recomp-jit-entries-core*.txt, written by the recompiled build) are folded in automatically.
# The result embeds code translated from the game: keep it for your own use.
set -euo pipefail
cd "$(dirname "$0")/../../.."

dump="${1:?usage: build_recomp.sh <dump.cci> [build-dir]}"
build_dir="${2:-build-windows}"
python="${MSR_PYTHON:-python}"

MSYS_ROOT="${MSYS_ROOT:-/c/msys64}"
export MSYSTEM=UCRT64
export PATH="$MSYS_ROOT/ucrt64/bin:$MSYS_ROOT/usr/bin:$PATH"
export CCACHE_DIR="$PWD/.ccache-windows"
export CCACHE_MAXSIZE=8G
export CCACHE_COMPILERCHECK=content

work="$build_dir/msr-recomp"
"$python" tools/msr/recomp/ctr.py "$dump" "$work/input"
extra=()
for f in "$build_dir"/bundle/user/msr/recomp-jit-entries-core*.txt; do
  [ -f "$f" ] && extra+=(--extra "$f")
done
"$python" tools/msr/recomp/generate.py "$work/input" --out "$work/generated" "${extra[@]}"

cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DUSE_SYSTEM_QT=ON -DUSE_SYSTEM_SDL2=ON -DENABLE_LTO=OFF \
  -DCITRA_WARNINGS_AS_ERRORS=OFF \
  -DENABLE_MSR_GAME_FOLDER=ON \
  -DENABLE_MSR_RECOMP=ON -DMSR_RECOMP_DIR="$PWD/$work/generated" \
  -DENABLE_QT=ON -DENABLE_VULKAN=ON -DENABLE_TESTS=OFF \
  -DENABLE_LIBRETRO=OFF -DENABLE_ROOM_STANDALONE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF \
  -DENABLE_WEB_SERVICE=OFF
cmake --build "$build_dir" --parallel "${MSR_JOBS:-$(( $(nproc) > 8 ? $(nproc) - 4 : $(nproc) ))}"
cmake --build "$build_dir" --target bundle
ccache -s | head -5
