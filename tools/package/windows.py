"""Packs the Windows release: dist/msr-native-<version>-windows-x64.zip.

  python tools/package/windows.py [build folder]      (default: build; MSYS2 UCRT64 in C:/msys64)

The zip holds msr-native.exe, msr-native-direct.exe (the game without the launcher), the MSYS2 libraries it needs
(SDL2, libpng, zlib, the GCC runtime, shaderc: the vertex shader compiler), recomp/ (the recompiler that builds the native code from the player's dump:
tools/msr/recomp/bundle_files.txt, same layout), README.txt and the licences. No game code or data: the player's dump, the code made from it, saves and settings all
go in the user folder the program makes beside itself.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
MSYS = pathlib.Path(os.environ.get('MSYS2_ROOT', r'C:\msys64'))


def main():
    build = ROOT / (sys.argv[1] if len(sys.argv) > 1 else 'build')
    exe = build / 'msr-native.exe'
    if not exe.is_file():
        sys.exit(f'{exe}: build it first (cmake -S native -B build -G Ninja; cmake --build build -j 8)')
    version = re.search(r'set\(MSR_NATIVE_VERSION "([^"]+)"\)', (ROOT / 'native/CMakeLists.txt').read_text()).group(1)
    name = f'msr-native-{version}-windows-x64'
    out = ROOT / 'dist' / name
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    # the program, stripped, and the libraries it loads from MSYS2 (ldd lists them; Windows' own and the Vulkan
    # loader, which comes with the graphics driver, stay out)
    shutil.copy2(exe, out / exe.name)
    env = dict(os.environ, PATH=f'{MSYS / "ucrt64/bin"};{MSYS / "usr/bin"};' + os.environ.get('PATH', ''))
    subprocess.run([str(MSYS / 'ucrt64/bin/strip.exe'), '-s', str(out / exe.name)], check=True, env=env)
    ldd = subprocess.run([str(MSYS / 'usr/bin/ldd.exe'), str(exe)], capture_output=True, text=True, check=True, env=env).stdout
    dlls = sorted({m.group(1) for m in re.finditer(r'=> /ucrt64/bin/(\S+\.dll)', ldd, re.I)})
    if not {'SDL2.dll', 'libstdc++-6.dll'} <= set(dlls):
        sys.exit(f'unexpected libraries: {dlls}')
    for dll in dlls:
        shutil.copy2(MSYS / 'ucrt64/bin' / dll, out / dll)
    # shaderc, which runtime/pica_vsgen.c loads by name (not a link-time library, so ldd does not list it) to compile the
    # game's vertex shaders; without it they run on the GPU's interpreter. MSYS2's needs only Windows' own libraries.
    shaderc = MSYS / 'ucrt64/bin/libshaderc_shared.dll'
    if not shaderc.is_file():
        sys.exit(f'{shaderc}: missing (pacman -S mingw-w64-ucrt-x86_64-shaderc)')
    shutil.copy2(shaderc, out / shaderc.name)
    subprocess.run([str(MSYS / 'ucrt64/bin/strip.exe'), '-s', str(out / shaderc.name)], check=True, env=env)

    # msr-native-direct.exe (runtime/win/direct.c, static and stripped already): in the zip from the start, so a
    # shortcut, Steam or a frontend can point at it before the first build (until then it opens the launcher). The
    # launcher writes the copy inside msr-native.exe (resource 200) when the file differs: the same bytes, or it would
    # replace this one on every first start
    direct = build / 'direct' / 'msr-native-direct.exe'
    if not direct.is_file():
        sys.exit(f'{direct}: missing (it is built with msr-native.exe)')
    if direct.read_bytes() not in exe.read_bytes():
        sys.exit(f'{direct}: not the one inside {exe.name} (rebuild: cmake --build {build.name})')
    shutil.copy2(direct, out / direct.name)

    # the recompiler, as the launcher looks for it: recomp/tools/msr/recomp/build_module.py
    bundle = ROOT / 'tools/msr/recomp/bundle_files.txt'
    files = [l.strip() for l in bundle.read_text().splitlines() if l.strip() and not l.startswith('#')]
    for f in files:
        d = out / 'recomp' / f
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / f, d)

    # the player's notes and the licences
    shutil.copy2(ROOT / 'tools/package/README.txt', out / 'README.txt')
    shutil.copy2(ROOT / 'native/LICENSE', out / 'LICENSE-GPL-3.0.txt')
    shutil.copy2(ROOT / 'license.txt', out / 'LICENSE-GPL-2.0.txt')
    shutil.copy2(ROOT / 'externals/msr-imgui/LICENSE.txt', out / 'LICENSE-Dear-ImGui.txt')
    shutil.copy2(ROOT / 'native/runtime/shaders/fsr/LICENSE-AMD-FidelityFX.txt', out / 'LICENSE-AMD-FidelityFX.txt')
    if 'libzstd.dll' in dlls:   # zstd (compressed game files), BSD: MSYS2 keeps its licence beside the package's files
        shutil.copy2(MSYS / 'ucrt64/share/licenses/zstd/LICENSE', out / 'LICENSE-zstd.txt')
    shutil.copy2(MSYS / 'ucrt64/share/licenses/shaderc/LICENSE', out / 'LICENSE-shaderc.txt')   # Apache 2.0

    archive = ROOT / 'dist' / f'{name}.zip'
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(out.rglob('*')):
            if p.is_file():
                z.write(p, pathlib.Path(name) / p.relative_to(out))
    print(f'{archive} ({archive.stat().st_size:,} bytes): {len(dlls)} libraries, {len(files)} recompiler files')


if __name__ == '__main__':
    main()
