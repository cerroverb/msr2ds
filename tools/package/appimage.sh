#!/bin/bash
# Builds the Linux release on a Linux system (here: WSL Ubuntu 24.04): dist/msr-native-<version>-x86_64.AppImage.
#   tools/package/appimage.sh [build folder] [SDL2 source folder] [zstd source folder]
# Needs: cmake, ninja, g++, the Vulkan headers and loader (libvulkan-dev), glslc or glslangValidator, libpng and zlib
# headers, and SDL2: the system's (libsdl2-dev), or built statically from a source folder (the second argument; the
# emulator-based build's externals/sdl2/SDL is one); zstd the same way (libzstd-dev, or the third argument: the
# emulator-based build's externals/zstd). LINUXDEPLOY: the linuxdeploy AppImage (it collects libpng and
# the others the program links; the system keeps glibc, the Vulkan loader, X11, Wayland and the sound servers).
# MSR_JOBS: parallel compiles (default 8). The AppImage holds the program, recomp/ (the recompiler: the launcher
# builds the native code from the player's dump with it) and the licences. No game code or data.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$HOME/msr-native-build}"
sdl="${2:-}"
zstd="${3:-}"
jobs="${MSR_JOBS:-8}"
linuxdeploy="${LINUXDEPLOY:-$HOME/samus-build/build/appimage-tools/linuxdeploy-x86_64.AppImage}"
version="$(sed -n 's/^set(MSR_NATIVE_VERSION "\([^"]*\)").*/\1/p' "$root/native/CMakeLists.txt" | tr -d '\r')"
[ -n "$version" ] || { echo "no MSR_NATIVE_VERSION in native/CMakeLists.txt" >&2; exit 1; }
[ -x "$linuxdeploy" ] || { echo "no linuxdeploy at $linuxdeploy (set LINUXDEPLOY)" >&2; exit 1; }

cmake -S "$root/native" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release ${sdl:+-DMSR_SDL2_SOURCE="$sdl"} \
      ${zstd:+-DMSR_ZSTD_SOURCE="$zstd"}
ninja -C "$build" -j "$jobs" msr-native

appdir="$build/AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/msr-native" "$appdir/usr/share/applications" \
         "$appdir/usr/share/icons/hicolor/512x512/apps" "$appdir/usr/share/doc/msr-native"
cp "$build/msr-native" "$appdir/usr/bin/msr-native"
strip -s "$appdir/usr/bin/msr-native"
# the recompiler, where the launcher looks for it: usr/share/msr-native/recomp/tools/msr/recomp/build_module.py
grep -v '^#' "$root/tools/msr/recomp/bundle_files.txt" | tr -d '\r' | while read -r file; do
  [ -n "$file" ] || continue
  mkdir -p "$appdir/usr/share/msr-native/recomp/$(dirname "$file")"
  cp "$root/$file" "$appdir/usr/share/msr-native/recomp/$file"
done
cp "$root/tools/package/README.txt" "$appdir/usr/share/doc/msr-native/README.txt"
cp "$root/native/LICENSE" "$appdir/usr/share/doc/msr-native/LICENSE-GPL-3.0.txt"
cp "$root/license.txt" "$appdir/usr/share/doc/msr-native/LICENSE-GPL-2.0.txt"
cp "$root/externals/msr-imgui/LICENSE.txt" "$appdir/usr/share/doc/msr-native/LICENSE-Dear-ImGui.txt"
cp "$root/native/runtime/shaders/fsr/LICENSE-AMD-FidelityFX.txt" "$appdir/usr/share/doc/msr-native/"
if [ -n "$zstd" ]; then cp "$zstd/LICENSE" "$appdir/usr/share/doc/msr-native/LICENSE-zstd.txt"   # zstd, BSD
elif [ -f /usr/share/doc/libzstd1/copyright ]; then cp /usr/share/doc/libzstd1/copyright "$appdir/usr/share/doc/msr-native/LICENSE-zstd.txt"; fi
cp "$root/tools/package/msr-native.png" "$appdir/usr/share/icons/hicolor/512x512/apps/msr-native.png"
cat > "$appdir/usr/share/applications/msr-native.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Metroid: Samus Returns
Comment=msr-native $version: the game's code built from your own dump, without an emulator
Exec=msr-native
Icon=msr-native
Categories=Game;
Terminal=false
EOF

mkdir -p "$root/dist"
export APPIMAGE_EXTRACT_AND_RUN=1 OUTPUT="$root/dist/msr-native-$version-x86_64.AppImage"
rm -f "$OUTPUT"
(cd "$build" && "$linuxdeploy" --appdir "$appdir" --executable "$appdir/usr/bin/msr-native" \
  --desktop-file "$appdir/usr/share/applications/msr-native.desktop" \
  --icon-file "$appdir/usr/share/icons/hicolor/512x512/apps/msr-native.png" \
  --exclude-library "libvulkan.so*" --output appimage)   # the system's Vulkan loader finds its own drivers
ls -la "$OUTPUT"
