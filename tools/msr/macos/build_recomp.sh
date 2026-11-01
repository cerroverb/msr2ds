#!/usr/bin/env bash
# Build the recompiled Samus Returns on macOS (Apple Silicon or Intel): Azahar + src/msr + the
# game's code recompiled ahead of time from your own dump (tools/msr/recomp), like
# tools/msr/windows/build_recomp.sh.
#
# Usage: tools/msr/macos/build_recomp.sh <dump.cci|.3ds> [build-dir]
#   build-dir   default build-macos. build.sh in the same folder keeps building it recompiled.
#   MSR_PYTHON  Python 3 with capstone; default: a venv in build-macos-venv, made on first use
#   MSR_JOBS    parallel compile jobs (default: the number of cores)
#
# Entry points the last run reached only through the JIT (<build-dir>/bundle/user/msr/
# recomp-jit-entries-core*.txt) are folded in automatically.
# The result embeds code translated from the game: keep it for your own use.
set -euo pipefail
cd "$(dirname "$0")/../../.."

dump="${1:?usage: build_recomp.sh <dump.cci> [build-dir]}"
build_dir="${2:-build-macos}"
python="${MSR_PYTHON:-}"
if [ -z "$python" ]; then
  python=build-macos-venv/bin/python
  if [ ! -x "$python" ]; then
    python3 -m venv build-macos-venv
    "$python" -m pip install --quiet capstone
  fi
fi

work="$build_dir/msr-recomp"
"$python" tools/msr/recomp/ctr.py "$dump" "$work/input"
extra=()
for f in "$build_dir"/bundle/user/msr/recomp-jit-entries-core*.txt; do
  [ -f "$f" ] && extra+=(--extra "$f")
done
"$python" tools/msr/recomp/generate.py "$work/input" --out "$work/generated" "${extra[@]+"${extra[@]}"}"

bash tools/msr/macos/build.sh "$build_dir" \
  -DENABLE_MSR_RECOMP=ON -DMSR_RECOMP_DIR="$PWD/$work/generated"
