#!/usr/bin/env bash
# Builds Samus Returns for Linux and packs it as an AppImage: recompiled with the HD texture pack
# inside for your own use, or the public build (JIT, no textures) with "-" for both. Runs on
# Linux (or WSL) from the repository; needs gcc/g++ 11+, cmake, ninja, python3 with pip, curl,
# patchelf, and the X11/GL/audio headers a Qt app uses. Qt 6 is
# downloaded with aqtinstall and SDL2 and the rest are Azahar's bundled dependencies, so nothing
# is installed on the system.
#
# Usage: tools/msr/linux/build_appimage.sh <generated-dir> <textures-dir> <output-dir> [build-dir]
#   generated-dir  the recompiled code (tools/msr/recomp/generate.py output: aot_shard_*.cpp);
#                  generate it from your own dump on any system, it is plain C++. "-": none, the
#                  game runs on the JIT
#   textures-dir   staged packs (tools/msr/package_release.py textures), shipped in
#                  usr/share/samus-returns-recomp/textures (MSR::GameFolder::BuiltInTextures).
#                  A shaders/ folder beside it (package_release.py shaders) ships as the shader
#                  seed. "-": none
#   output-dir     where <ProjectName>-<Version>-<arch>.AppImage goes (x86_64 or aarch64, the
#                  build system's)
#   build-dir      default build-linux
#   MSR_JOBS       parallel compile jobs (default: nproc / 2; the generated code is large)
#
# Libraries Qt's xcb platform plugin needs that the system lacks are fetched with apt-get
# download (Debian/Ubuntu, no root) and bundled. The AppImage needs a glibc at least as new as
# the build system's. A recompiled one embeds code translated from the game: keep it for your
# own use. Only the JIT build without textures can be shared.
set -euo pipefail
cd "$(dirname "$0")/../../.."

generated="${1:?usage: build_appimage.sh <generated-dir> <textures-dir> <output-dir> [build-dir]}"
textures="${2:?textures-dir}"
if [ "$generated" = - ]; then
  recomp=(-DENABLE_MSR_RECOMP=OFF)
else
  generated="$(realpath "$generated")"
  recomp=(-DENABLE_MSR_RECOMP=ON -DMSR_RECOMP_DIR="$generated")
fi
[ "$textures" = - ] || textures="$(realpath "$textures")"
output="$(realpath "${3:?output-dir}")"
build_dir="$(realpath -m "${4:-build-linux}")"
jobs="${MSR_JOBS:-$(( $(nproc) / 2 ))}"
arch="$(uname -m)"
case "$arch" in
  x86_64) qt_host=linux; qt_arch=linux_gcc_64; qt_dir=gcc_64 ;;
  aarch64) qt_host=linux_arm64; qt_arch=linux_gcc_arm64; qt_dir=gcc_arm64 ;;
  *) echo "Unsupported architecture: $arch (x86_64 or aarch64)" >&2; exit 1 ;;
esac

name="$(sed -n 's/.*ProjectName\[\] = "\(.*\)";/\1/p' src/msr/version.h)"
version="$(sed -n 's/.*char Version\[\] = "\(.*\)";/\1/p' src/msr/version.h)"
tools="$build_dir/appimage-tools"
appdir="$build_dir/AppDir"
mkdir -p "$tools" "$output"

# Qt: Azahar takes the system's on Linux; this fetches the release Qt with aqtinstall (as
# Azahar's own build does on Windows), so the AppImage carries a current Qt.
qt_version=6.10.3
qt="$tools/qt/$qt_version/$qt_dir"
if [ ! -x "$qt/bin/qmake" ]; then
  python3 -m pip install --quiet --target "$tools/aqt" aqtinstall
  PYTHONPATH="$tools/aqt" python3 -m aqt install-qt --outputdir "$tools/qt" \
    "$qt_host" desktop "$qt_version" "$qt_arch" -m qtmultimedia
fi
[ -x "$qt/bin/qmake" ] || { echo "Qt $qt_version not installed in $qt" >&2; exit 1; }

cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_PREFIX_PATH="$qt" \
  -DUSE_SYSTEM_QT=ON -DUSE_SYSTEM_SDL2=OFF -DENABLE_LTO=OFF \
  -DENABLE_QT_TRANSLATION=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF \
  -DENABLE_MSR_GAME_FOLDER=ON \
  "${recomp[@]}" \
  -DENABLE_QT=ON -DENABLE_VULKAN=ON -DENABLE_TESTS=OFF \
  -DENABLE_LIBRETRO=OFF -DENABLE_ROOM_STANDALONE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF \
  -DENABLE_WEB_SERVICE=OFF
cmake --build "$build_dir" --parallel "$jobs" --target citra_meta

exe="$build_dir/bin/Release/SamusReturns"
[ -x "$exe" ] || exe="$build_dir/bin/SamusReturns"
qmake="$qt/bin/qmake"
qt_lib="$qt/lib"

# Runtime libraries for Qt's xcb plugin missing from the system, unpacked locally.
sysroot="$tools/sysroot"
missing=()
for pkg in libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 \
           libxcb-shape0 libxcb-xkb1 libxkbcommon-x11-0 libxcb-xinerama0 libxcb-util1 libsm6 \
           libice6; do
  dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
done
if [ ${#missing[@]} -gt 0 ]; then
  mkdir -p "$tools/debs" "$sysroot"
  (cd "$tools/debs" && apt-get download "${missing[@]}")
  for deb in "$tools"/debs/*.deb; do dpkg -x "$deb" "$sysroot"; done
fi

for tool in "linuxdeploy-$arch.AppImage" "linuxdeploy-plugin-qt-$arch.AppImage"; do
  if [ ! -x "$tools/$tool" ]; then
    project="${tool%-"$arch".AppImage}"
    curl -fsSL -o "$tools/$tool" \
      "https://github.com/linuxdeploy/$project/releases/download/continuous/$tool"
    chmod +x "$tools/$tool"
  fi
done

# The AppDir: the program, its texture packs if any, a desktop entry and the icon.
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/samus-returns-recomp" \
         "$appdir/usr/share/applications" "$appdir/usr/share/icons/hicolor/512x512/apps"
cp "$exe" "$appdir/usr/bin/SamusReturns"
strip -s "$appdir/usr/bin/SamusReturns"
if [ "$textures" != - ]; then
  cp -r "$textures" "$appdir/usr/share/samus-returns-recomp/textures"
  # The shader seed staged beside the packs (package_release.py shaders), personal build only:
  # it holds the game's shader programs (msr/shader_seed.h, GameFolder::BuiltInShaders).
  if [ -d "$(dirname "$textures")/shaders" ]; then
    cp -r "$(dirname "$textures")/shaders" "$appdir/usr/share/samus-returns-recomp/shaders"
  fi
fi
cp dist/msr/samus.png "$appdir/usr/share/icons/hicolor/512x512/apps/samus-returns-recomp.png"
cat > "$appdir/usr/share/applications/samus-returns-recomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Metroid: Samus Returns
Comment=Single-screen PC edition ($version)
Exec=SamusReturns
Icon=samus-returns-recomp
Categories=Game;
Terminal=false
EOF

# Qt Multimedia (the camera) runs on its FFmpeg backend. Its GStreamer plugin would need GStreamer
# on the build system and in the AppImage, and linuxdeploy stops when that is missing: the plugin
# is set aside while the AppImage is made (Qt's CMake files still list it).
gst="$qt/plugins/multimedia/libgstreamermediaplugin.so"
if [ -f "$gst" ]; then
  mv "$gst" "$qt/gstreamermediaplugin.aside"
  trap 'mv "$qt/gstreamermediaplugin.aside" "$gst"' EXIT
fi

export APPIMAGE_EXTRACT_AND_RUN=1 QMAKE="$qmake"
export LD_LIBRARY_PATH="$qt_lib:$sysroot/usr/lib/$arch-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export OUTPUT="$output/$name-$version-$arch.AppImage"
rm -f "$OUTPUT"
(cd "$tools" && "$tools/linuxdeploy-$arch.AppImage" \
  --appdir "$appdir" \
  --executable "$appdir/usr/bin/SamusReturns" \
  --desktop-file "$appdir/usr/share/applications/samus-returns-recomp.desktop" \
  --icon-file "$appdir/usr/share/icons/hicolor/512x512/apps/samus-returns-recomp.png" \
  --plugin qt --output appimage)
ls -la "$OUTPUT"
