#!/usr/bin/env bash
# Builds the Linux AppImage from Windows, in WSL: copies this repository and the generated
# recompiled code (from the Windows build) to ~/samus-build, where ext4 builds far faster than
# /mnt, and runs build_appimage.sh there. The AppImage lands in build-release/.
#
# Usage, from PowerShell or MSYS2:  wsl -d Ubuntu -- bash tools/msr/linux/build_in_wsl.sh
#   FRESH=1   reconfigure from scratch;  MSR_JOBS=N   parallel jobs (default 16)
#   PUBLIC=1  the public build instead: JIT, no textures, in ~/samus-build/build-jit, landing in
#             build-release/public/
# Needs build-windows/msr-recomp/generated (tools/msr/windows/build_recomp.sh) and
# build-release/textures (tools/msr/package_release.py textures <pack>), except PUBLIC=1.
set -euo pipefail
# WSL appends Windows' PATH: CMake would find MSYS2's Windows Qt through it.
PATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd:)"
export PATH
repo="$(cd "$(dirname "$0")/../../.." && pwd)"
work="$HOME/samus-build"
mkdir -p "$work/recomp"
rsync -a --delete --exclude '/build-*' --exclude '/.ccache-windows' --exclude '/.git' \
      "$repo/" "$work/src/"
# Submodules point at the repository's .git, which is not copied: build them as plain sources.
find "$work/src/externals" -maxdepth 3 -name .git -type f -delete
if [ "${PUBLIC:-0}" = 1 ]; then
    args=(- - "$repo/build-release/public" "$work/build-jit")
else
    rsync -a --delete "$repo/build-windows/msr-recomp/generated/" "$work/recomp/generated/"
    args=("$work/recomp/generated" "$repo/build-release/textures" "$repo/build-release"
          "$work/build")
fi
if [ "${FRESH:-0}" = 1 ]; then
    rm -rf "${args[3]}/CMakeCache.txt" "${args[3]}/CMakeFiles"
fi
cd "$work/src"
MSR_JOBS="${MSR_JOBS:-16}" bash tools/msr/linux/build_appimage.sh "${args[@]}"
