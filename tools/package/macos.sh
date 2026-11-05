#!/bin/bash
# Builds the macOS release on a Mac (Apple Silicon): dist/msr-native-<version>-macos-arm64.zip, holding msr-native.app.
#   tools/package/macos.sh [build folder]
# Needs: Xcode's command line tools, cmake, ninja, glslc or glslangValidator (Homebrew: cmake ninja shaderc), and these,
# built for macOS 13 (the app's minimum: Homebrew's own libraries are built for the Mac they are installed on):
#   MSR_SDL2_SOURCE  SDL 2's source (built statically; the emulator-based build's externals/sdl2/SDL is one)
#   MSR_ZSTD_SOURCE  zstd's source (its decoder built statically; the emulator-based build's externals/zstd)
#   MSR_PNG_SOURCE   libpng's source (built statically; zlib is the system's)
#   MOLTENVK         MoltenVK's release folder (KhronosGroup/MoltenVK's MoltenVK-macos.tar, unpacked: MoltenVK/), whose
#                    libMoltenVK.dylib the program links directly (it is the Vulkan driver; no loader)
#   SHADERC          libshaderc_shared.1.dylib (google/shaderc, built with CMAKE_OSX_DEPLOYMENT_TARGET=13.0): the
#                    runtime's vertex shader translation loads it (runtime/pica_vsgen.c); without it the GPU interpreter
#   SHADERC_LICENSE  shaderc's LICENSE (its source folder's)
# MSR_JOBS: parallel compiles (default: the number of cores). The app holds the program, the two libraries,
# Resources/recomp/ (the recompiler: the launcher builds the native code from the player's dump with it), README.txt and
# the licences. No game code or data. Ad-hoc signed, not notarized.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$root/build-macos-release}"
jobs="${MSR_JOBS:-$(sysctl -n hw.ncpu)}"
min=13.0
: "${MSR_SDL2_SOURCE:?set MSR_SDL2_SOURCE (SDL 2 source folder)}"
: "${MSR_ZSTD_SOURCE:?set MSR_ZSTD_SOURCE (zstd source folder)}"
: "${MSR_PNG_SOURCE:?set MSR_PNG_SOURCE (libpng source folder)}"
: "${MOLTENVK:?set MOLTENVK (MoltenVK release folder)}"
: "${SHADERC:?set SHADERC (libshaderc_shared.1.dylib built for macOS $min)}"
: "${SHADERC_LICENSE:?set SHADERC_LICENSE (the LICENSE file of shaderc)}"
version="$(sed -n 's/^set(MSR_NATIVE_VERSION "\([^"]*\)").*/\1/p' "$root/native/CMakeLists.txt" | tr -d '\r')"
[ -n "$version" ] || { echo "no MSR_NATIVE_VERSION in native/CMakeLists.txt" >&2; exit 1; }
mvk_lib="$MOLTENVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib"
mvk_include="$MOLTENVK/MoltenVK/include"
[ -f "$mvk_lib" ] && [ -d "$mvk_include/vulkan" ] || { echo "no MoltenVK release in $MOLTENVK" >&2; exit 1; }
[ -f "$SHADERC" ] || { echo "no $SHADERC" >&2; exit 1; }
# the shader compiler is a build tool (Homebrew's will do), found before Homebrew's prefix is left out of the search
glslc="$(command -v glslc || true)"; glslang=
[ -n "$glslc" ] || glslang="$(command -v glslangValidator || true)"
[ -n "$glslc$glslang" ] || { echo "no glslc or glslangValidator" >&2; exit 1; }

cmake -S "$root/native" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=$min \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DMSR_SDL2_SOURCE="$MSR_SDL2_SOURCE" -DMSR_ZSTD_SOURCE="$MSR_ZSTD_SOURCE" -DMSR_PNG_SOURCE="$MSR_PNG_SOURCE" \
      -DVulkan_LIBRARY="$mvk_lib" -DVulkan_INCLUDE_DIR="$mvk_include" -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
      ${glslc:+-DGLSLC="$glslc"} ${glslang:+-DGLSLANG="$glslang"}
ninja -C "$build" -j "$jobs" msr-native

app="$build/msr-native.app"
c="$app/Contents"
rm -rf "$app"
mkdir -p "$c/MacOS" "$c/Frameworks" "$c/Resources/doc"
cp "$build/msr-native" "$c/MacOS/msr-native"
strip -x "$c/MacOS/msr-native"
lipo "$mvk_lib" -thin arm64 -output "$c/Frameworks/libMoltenVK.dylib" 2>/dev/null || cp "$mvk_lib" "$c/Frameworks/libMoltenVK.dylib"
cp "$SHADERC" "$c/Frameworks/libshaderc_shared.1.dylib"
chmod u+w "$c/Frameworks"/*.dylib
install_name_tool -id @rpath/libshaderc_shared.1.dylib "$c/Frameworks/libshaderc_shared.1.dylib"
# the program finds MoltenVK in Frameworks (its install name is @rpath/libMoltenVK.dylib); no build folder paths
for r in $(otool -l "$c/MacOS/msr-native" | awk '/LC_RPATH/ {getline; getline; print $2}'); do
  install_name_tool -delete_rpath "$r" "$c/MacOS/msr-native"
done
install_name_tool -add_rpath @executable_path/../Frameworks "$c/MacOS/msr-native"
# nothing from Homebrew or the build folder, and nothing newer than the minimum
inside='^(@rpath/|/usr/lib/|/System/Library/)'   # (outside $(...): macOS's bash 3.2 misreads the parentheses there)
for f in "$c/MacOS/msr-native" "$c/Frameworks"/*.dylib; do
  outside="$(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -v -E "$inside" || true)"
  [ -z "$outside" ] || { echo "$f loads a library outside the app or the system: $outside" >&2; exit 1; }
  m="$(otool -l "$f" | awk '/LC_BUILD_VERSION/ {b=1} b && /minos/ {print $2; exit}')"
  newest="$(printf '%s\n%s\n' "$m" "$min" | sort -V | tail -1)"
  if [ "$newest" != "$min" ]; then echo "$f is built for macOS $m, newer than $min" >&2; exit 1; fi
done
# the recompiler, where the launcher looks for it: Resources/recomp/tools/msr/recomp/build_module.py
grep -v '^#' "$root/tools/msr/recomp/bundle_files.txt" | tr -d '\r' | while read -r file; do
  [ -n "$file" ] || continue
  mkdir -p "$c/Resources/recomp/$(dirname "$file")"
  cp "$root/$file" "$c/Resources/recomp/$file"
done
cp "$root/tools/package/README.txt" "$c/Resources/doc/README.txt"
cp "$root/native/LICENSE" "$c/Resources/doc/LICENSE-GPL-3.0.txt"
cp "$root/license.txt" "$c/Resources/doc/LICENSE-GPL-2.0.txt"
cp "$root/externals/msr-imgui/LICENSE.txt" "$c/Resources/doc/LICENSE-Dear-ImGui.txt"
cp "$root/native/runtime/shaders/fsr/LICENSE-AMD-FidelityFX.txt" "$c/Resources/doc/"
cp "$MSR_ZSTD_SOURCE/LICENSE" "$c/Resources/doc/LICENSE-zstd.txt"
cp "$MSR_PNG_SOURCE/LICENSE" "$c/Resources/doc/LICENSE-libpng.txt"
cp "$MSR_SDL2_SOURCE/LICENSE.txt" "$c/Resources/doc/LICENSE-SDL2.txt"
cp "$MOLTENVK/LICENSE" "$c/Resources/doc/LICENSE-MoltenVK.txt"
cp "$SHADERC_LICENSE" "$c/Resources/doc/LICENSE-shaderc.txt"
# the icon: tools/package/msr-native.png (512 x 512) at the sizes an .icns holds
iconset="$build/msr-native.iconset"
rm -rf "$iconset"; mkdir -p "$iconset"
for s in 16 32 64 128 256 512; do
  sips -z $s $s "$root/tools/package/msr-native.png" --out "$iconset/icon_${s}x${s}.png" >/dev/null
  [ $s -gt 16 ] && cp "$iconset/icon_${s}x${s}.png" "$iconset/icon_$((s / 2))x$((s / 2))@2x.png"
done
cp "$root/tools/package/msr-native.png" "$iconset/icon_512x512@2x.png"
iconutil -c icns "$iconset" -o "$c/Resources/msr-native.icns"
cat > "$c/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>msr-native</string>
  <key>CFBundleIconFile</key><string>msr-native</string>
  <key>CFBundleIdentifier</key><string>io.github.gminustwo.msrnative</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>msr-native</string>
  <key>CFBundleDisplayName</key><string>msr-native</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>LSMinimumSystemVersion</key><string>$min</string>
  <key>LSArchitecturePriority</key><array><string>arm64</string></array>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>msr-native $version: GPL 3.0 or later. Not affiliated with Nintendo.</string>
</dict>
</plist>
EOF
# ad-hoc signatures (Apple Silicon runs only signed code): the libraries, then the app; no hardened runtime, so the
# native code the launcher builds (ad-hoc signed by Zig's linker) loads
codesign --force --sign - "$c/Frameworks"/*.dylib
codesign --force --sign - "$app"
codesign --verify --strict --verbose=1 "$app"

mkdir -p "$root/dist"
out="$root/dist/msr-native-$version-macos-arm64.zip"
rm -f "$out"
ditto -c -k --keepParent "$app" "$out"
ls -la "$out"
