#!/usr/bin/env bash
# Builds the Linux AppImage on a Mac, in a Lima virtual machine (brew install lima): Ubuntu 24.04
# with the Containerfile's packages, on the Mac's own architecture, so Apple Silicon makes an
# aarch64 AppImage. The VM copies this repository to its own disk, where it builds far faster
# than on the shared folder, keeps the build there, and runs build_appimage.sh. The AppImage lands
# in build-release/.
#
# Usage: tools/msr/macos/build_linux.sh [generated-dir]
#   generated-dir  the recompiled code (tools/msr/recomp/generate.py output), default
#                  build-macos/msr-recomp/generated (build_recomp.sh). Embeds code translated from
#                  the game: keep the AppImage for your own use
#   PUBLIC=1       the public build instead: JIT, no textures, landing in build-release/public/
#   FRESH=1        reconfigure from scratch;  MSR_JOBS=N  parallel jobs (default: half the VM's
#                  CPUs, as build_appimage.sh: the generated code is large)
#   LIMA_CPUS, LIMA_MEMORY (GiB)  for a new VM; default all cores and half the memory
# The HD texture pack goes in when build-release/textures exists (package_release.py textures).
# The VM, samus-build, stays for the next build: limactl stop samus-build, or limactl delete.
set -euo pipefail
cd "$(dirname "$0")/../../.."
repo="$PWD"
vm=samus-build
command -v limactl >/dev/null || { echo "Needs Lima: brew install lima" >&2; exit 1; }

if [ "${PUBLIC:-0}" = 1 ]; then
  generated=-
  textures=-
  output="$repo/build-release/public"
  build=build-jit
else
  generated="$(cd "${1:-build-macos/msr-recomp/generated}" && pwd)"
  case "$generated" in "$repo"/*) ;; *) echo "$generated: not in $repo, the VM's only shared folder" >&2; exit 1 ;; esac
  textures=-
  [ -d build-release/textures ] && textures="$repo/build-release/textures"
  output="$repo/build-release"
  build=build
fi
mkdir -p "$output"

if ! limactl list -q 2>/dev/null | grep -qx "$vm"; then
  limactl start --name="$vm" --vm-type=vz --cpus="${LIMA_CPUS:-$(sysctl -n hw.ncpu)}" \
    --memory="${LIMA_MEMORY:-$(( $(sysctl -n hw.memsize) / 2147483648 ))}" --disk=100 \
    --containerd=none --mount-only "$repo:w" --mount-type=virtiofs -y template:ubuntu-24.04
elif [ "$(limactl list -f '{{.Status}}' "$vm")" != Running ]; then
  limactl start -y "$vm"
fi

# The Containerfile's apt-get install list.
packages="$(sed -n '/apt-get install/,/&&/p' tools/msr/linux/Containerfile | tr -d '\\' |
            sed -e 's/.*--no-install-recommends//' -e 's/&&.*//' | xargs)"
limactl shell --workdir / "$vm" bash -s -- "$repo" "$generated" "$textures" "$output" "$build" \
  "$packages" "${FRESH:-0}" "${MSR_JOBS:-}" <<'EOF'
set -euo pipefail
repo=$1 generated=$2 textures=$3 output=$4 build=$5 packages=$6 fresh=$7 jobs=$8
if [ "$(cat ~/.samus-packages 2>/dev/null)" != "$packages" ]; then
  sudo DEBIAN_FRONTEND=noninteractive apt-get update -q
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends $packages
  printf '%s' "$packages" > ~/.samus-packages
fi
work=~/samus-build
mkdir -p "$work/recomp"
rsync -a --delete --exclude '/build-*' --exclude '/.ccache-*' --exclude '/.git' \
      "$repo/" "$work/src/"
# Submodules point at the repository's .git, which is not copied: build them as plain sources.
find "$work/src/externals" -maxdepth 3 -name .git -type f -delete
if [ "$generated" != - ]; then
  rsync -a --delete "$generated/" "$work/recomp/generated/"
  generated="$work/recomp/generated"
fi
if [ "$fresh" = 1 ]; then
  rm -rf "$work/$build/CMakeCache.txt" "$work/$build/CMakeFiles"
fi
cd "$work/src"
[ -z "$jobs" ] || export MSR_JOBS="$jobs"
PIP_BREAK_SYSTEM_PACKAGES=1 bash tools/msr/linux/build_appimage.sh "$generated" "$textures" "$output" "$work/$build"
EOF
