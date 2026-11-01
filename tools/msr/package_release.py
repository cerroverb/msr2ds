"""Packages releases: the public one, and a personal one with the HD texture pack built in.

  python tools/msr/package_release.py public [build-dir]
      The public release: copies <build-dir>/bundle (default build-jit, a JIT build made with
      tools/msr/windows/build.sh build-jit) without user/, game/, scripting/ or any dump, adds a
      README.txt and license.txt, and zips it as
      build-release/public/<ProjectName>-<Version>-windows-x64.zip. No textures. It refuses a
      recompiled build.

  python tools/msr/package_release.py textures <pack-folder>
      Stages a Citra-style pack (textures in <pack-folder>/00040000001BB200, add-ons in
      <pack-folder>/Optional Addons/<name>, Readme.txt beside them) as build-release/textures/:
      "HD Texture Pack", "HD Texture Pack Addons/<name>" and the pack's readme, which credits
      its authors. The game lists these as packs included with it (GameFolder::BuiltInTextures).

  python tools/msr/package_release.py shaders <cache-folder>...
      Stages shader caches (the vulkan/transferable folders of user/shaders, e.g. from a copy of
      the game that has been played through) as build-release/shaders/: their shader configs and
      programs merged, without SPIR-V. The game adds the entries a player's cache lacks when it
      boots, so their pipelines build at boot instead of when an effect first appears
      (msr/shader_seed.h).

  python tools/msr/package_release.py windows [build-dir]
      Copies <build-dir>/bundle (default build-windows) without user/, game/, scripting/,
      shaders/ or any dump, adds build-release/textures, build-release/shaders (when staged),
      README.txt and license.txt, and zips it as
      build-release/<ProjectName>-<Version>-windows-x64.zip.

  python3 tools/msr/package_release.py macos [build-dir]
      The public macOS release: <build-dir>/bundle/SamusReturns.app (default build-macos, a JIT
      build made with tools/msr/macos/build.sh) with README.txt and license.txt, zipped with
      ditto (which keeps the app's symlinks and signature) as
      build-release/public/<ProjectName>-<Version>-macos-<arch>.zip. It refuses a recompiled
      build.

The Linux AppImage is made in Linux by tools/msr/linux/build_appimage.sh (public: no
generated code, no textures); on a Mac, by tools/msr/macos/build_linux.sh. Name and version come
from src/msr/version.h.

A recompiled build embeds code translated from your game dump, the shader caches hold the game's
shader programs, and the texture pack is its authors' work made from the game's textures: keep
the personal packages for your own machines. Only the public package can be shared.
"""
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
RELEASE = ROOT / 'build-release'
PUBLIC = RELEASE / 'public'
TEXTURES = RELEASE / 'textures'
SHADERS = RELEASE / 'shaders'
EXCLUDED_DIRS = {'user', 'game', 'scripting', 'shaders'}
DUMP_SUFFIXES = {'.3ds', '.cci', '.cxi', '.cia', '.app', '.zcci', '.zcxi'}
STORED_SUFFIXES = {'.png', '.dds', '.ktx', '.zip'}  # already compressed
MAIN_PACK = 'HD Texture Pack'
ADDONS = 'HD Texture Pack Addons'

README_INTRO = """{name} {version} ({label})
Metroid: Samus Returns as a single-screen PC game: the 3DS game's code recompiled to run
natively, on Azahar's emulation of the rest of the 3DS.

You need your own dump of Metroid: Samus Returns (USA, .3ds or .cci). The launcher asks for it
the first time and remembers where it is.
"""

README_PUBLIC_INTRO = """{name} {version}
Metroid: Samus Returns as a single-screen PC game, on the Azahar 3DS emulator.
https://github.com/gminustwo/msr2ds

You need your own decrypted dump of Metroid: Samus Returns (USA, .3ds or .cci): none is
included. The launcher asks for it the first time and remembers where it is.
"""

README_BODY = """
To skip the launcher and boot straight into the game with your saved settings, run
SamusReturns-Direct.exe on Windows (a Steam shortcut can point at it), start the AppImage with
--play on Linux, or run "open SamusReturns.app --args --play" on macOS. The settings stay in the
in-game menu (F1 or Select).

Your settings and saves:
  Windows: the user folder beside {exe}
  Linux (AppImage): ~/.local/share/samus-returns-recomp, or a folder named user beside the
  AppImage file if you create one
  macOS: ~/Library/Application Support/samus-returns-recomp, or a folder named user beside
  SamusReturns.app if you create one
The launcher's Game tab imports saves from Azahar or Citra.

Game mods: mods made for Citra, Azahar or Luma3DS (romfs, exefs, code.ips...) work as they are.
Put each mod, as its own folder, in the mods/game folder of your user folder (the launcher's
Mods tab opens it), then turn mods on and order them there or in the in-game menu.

{textures}
Controls: see the launcher's Controls tab. A gamepad is set up when it is connected; every
connected gamepad controls the game, Steam Input included.

{footer}"""

README_MACOS = """
macOS: Apple Silicon, macOS 13.4 or newer. The app is not notarized, so the first time macOS
refuses to open it: choose Open Anyway in System Settings, Privacy & Security (or right-click
the app, Open). Metal compiles each new shader the first time it is used, so effects pause the
game briefly the first time they appear; macOS keeps what it compiled.
"""

README_TEXTURES = """HD textures: the HD Texture Pack and its add-ons are included and turned on. Turn them off or
reorder them in the launcher's Textures tab (or the in-game menu, F1 or Select). Credits in
textures/{readme}: Triforce141 dumped and upscaled the textures and made the add-ons;
TheHatedGravity upscaled the rest.
"""

README_PUBLIC_TEXTURES = """Texture packs: none are included. Citra and Azahar packs for this game work as they are: put
each pack, as its own folder, in the mods/textures folder of your user folder (the launcher's
Textures tab opens it), then turn packs on and order them there or in the in-game menu (F1 or
Select).
"""

README_FOOTER = """This build is for your own use: its code is translated from your copy of the game, and the
texture pack is its authors' work. Do not share it.

License: GPLv2 or later (license.txt), as Azahar.
"""

README_PUBLIC_FOOTER = """License: GPLv2 or later (license.txt), as Azahar, which it is built on. Metroid: Samus Returns
belongs to Nintendo; this project is not affiliated with Nintendo and includes nothing from the
game.
"""


def version():
    text = (ROOT / 'src/msr/version.h').read_text()
    values = dict(re.findall(r'(\w+)\[\] = "([^"]*)"', text))
    return values['ProjectName'], values['Version'], values['VersionLabel']


def copy_tree(source, target):
    shutil.copytree(source, target, dirs_exist_ok=True)


def stage_textures(pack):
    pack = pathlib.Path(pack)
    main = pack / '00040000001BB200'
    if not main.is_dir():
        sys.exit(f'{main} not found: point at the folder holding 00040000001BB200')
    if TEXTURES.exists():
        shutil.rmtree(TEXTURES)
    copy_tree(main, TEXTURES / MAIN_PACK)
    addons = pack / 'Optional Addons'
    if addons.is_dir():
        for addon in sorted(p for p in addons.iterdir() if p.is_dir()):
            copy_tree(addon, TEXTURES / ADDONS / addon.name)
    readme = pack / 'Readme.txt'
    if readme.is_file():
        shutil.copy2(readme, TEXTURES / f'{MAIN_PACK} - Readme.txt')
    files = sum(1 for p in TEXTURES.rglob('*') if p.is_file())
    print(f'{files} files staged in {TEXTURES}')


def cache_entries(path):
    """Entries of an Azahar Vulkan shader cache file (.vkch) as (type, id, flags, payload): each
    is a 16-byte header (version 0x42, flags, type, size, id), the payload and a 16-byte footer
    (version 0x24 and the entry's index, size)."""
    data = path.read_bytes()
    pos = 0
    while pos < len(data):
        version, flags, kind, size, ident = struct.unpack_from('<BBHIQ', data, pos)
        if version != 0x42 or size < 32 or pos + size > len(data):
            sys.exit(f'{path} is damaged at byte {pos}')
        yield kind, ident, flags, data[pos + 16:pos + size - 16]
        pos += size


def stage_shaders(folders):
    title = '00040000001BB200'
    file_info, spirv = 0, {3, 5, 7}  # entry types: FILE_INFO; VS_, FS_ and GS_SPIRV
    if SHADERS.exists():
        shutil.rmtree(SHADERS)
    SHADERS.mkdir(parents=True)
    for kind in ('vs', 'fs', 'gs', 'pl'):
        info, entries, seen = None, [], set()
        for folder in folders:
            path = pathlib.Path(folder) / f'{title}_{kind}.vkch'
            if not path.is_file():
                continue
            items = list(cache_entries(path))
            if not items or items[0][0] != file_info or items[0][2] != 0:
                sys.exit(f'{path} does not start with an uncompressed info entry')
            # Info: magic, file version, config structure hash, file type (20 bytes), then the
            # shader generator's source hash (64 bytes), build name and driver profile.
            head = items[0][3]
            if info is None:
                info = head
            elif head[:20] != info[:20]:
                sys.exit(f'{path} was written for other config structures than the first cache')
            for item in items[1:]:
                if item[0] not in spirv and item[:2] not in seen:
                    seen.add(item[:2])
                    entries.append(item)
        if info is None:
            continue
        # A blank source hash: the game regenerates the shaders for the machine it runs on.
        blank = info[:20] + bytes(64) + info[84:]
        target = SHADERS / f'{title}_{kind}.vkch'
        with open(target, 'wb') as out:
            for index, (kind_id, ident, flags, payload) in enumerate([(file_info, 0, 0, blank)] +
                                                                      entries):
                size = len(payload) + 32
                out.write(struct.pack('<BBHIQ', 0x42, flags, kind_id, size, ident))
                out.write(payload)
                out.write(struct.pack('<IIQ', 0x24 | index << 8, size, 0))
        print(f'{target.name}: {len(entries)} entries, {target.stat().st_size // 1024} KiB')


def write_readme(target, exe, public=False, notes=''):
    name, ver, label = version()
    fields = dict(name=name, version=ver, label=label, exe=exe,
                  readme=f'{MAIN_PACK} - Readme.txt')
    intro, textures, footer = ((README_PUBLIC_INTRO, README_PUBLIC_TEXTURES, README_PUBLIC_FOOTER)
                               if public else (README_INTRO, README_TEXTURES, README_FOOTER))
    text = intro.format(**fields) + notes + README_BODY.format(
        textures=textures.format(**fields), footer=footer, **fields)
    target.write_text(text, encoding='utf-8', newline='\r\n' if exe.endswith('.exe') else '\n')


def package_windows(build_dir='build-windows', public=False):
    name, ver, _ = version()
    bundle = ROOT / build_dir / 'bundle'
    if not (bundle / 'SamusReturns.exe').is_file():
        sys.exit(f'{bundle} has no SamusReturns.exe: build the bundle target first')
    if public:
        cache = ROOT / build_dir / 'CMakeCache.txt'
        if not cache.is_file() or 'ENABLE_MSR_RECOMP:BOOL=OFF' not in cache.read_text(
                errors='replace'):
            sys.exit(f'{build_dir} is not a JIT build: the public package must not hold code '
                     'translated from the game (tools/msr/windows/build.sh build-jit)')
    elif not TEXTURES.is_dir():
        sys.exit(f'{TEXTURES} is missing: run "package_release.py textures <pack>" first')

    folder = f'{name}-{ver}-windows-x64'
    out = (PUBLIC if public else RELEASE) / folder
    if out.exists():
        shutil.rmtree(out)
    skipped = []
    for current, dirs, files in os.walk(bundle):
        here = pathlib.Path(current)
        if here == bundle:
            skipped += [d + '/' for d in dirs if d in EXCLUDED_DIRS]
            dirs[:] = [d for d in dirs if d not in EXCLUDED_DIRS]
        for file in files:
            source = here / file
            if source.suffix.lower() in DUMP_SUFFIXES:
                skipped.append(str(source.relative_to(bundle)))
                continue
            target = out / source.relative_to(bundle)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    if not public:
        copy_tree(TEXTURES, out / 'textures')
        if SHADERS.is_dir():
            copy_tree(SHADERS, out / 'shaders')
        else:
            print(f'no shaders: {SHADERS} is not staged ("package_release.py shaders <folder>")')
    shutil.copy2(ROOT / 'license.txt', out / 'license.txt')
    write_readme(out / 'README.txt', 'SamusReturns.exe', public)

    archive = out.parent / f'{folder}.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for path in sorted(out.rglob('*')):
            if path.is_file():
                stored = path.suffix.lower() in STORED_SUFFIXES
                zf.write(path, pathlib.Path(folder) / path.relative_to(out),
                         compress_type=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED)

    files = sum(1 for p in out.rglob('*') if p.is_file())
    print(f'{files} files in {out}')
    print(f'left out: {", ".join(skipped) or "nothing"}')
    print(f'{archive} ({archive.stat().st_size / 2**30:.2f} GiB)')


def package_macos(build_dir='build-macos'):
    name, ver, _ = version()
    app = ROOT / build_dir / 'bundle' / 'SamusReturns.app'
    exe = app / 'Contents' / 'MacOS' / 'SamusReturns'
    if not exe.is_file():
        sys.exit(f'{app} is missing: build it first (tools/msr/macos/build.sh {build_dir})')
    cache = ROOT / build_dir / 'CMakeCache.txt'
    if not cache.is_file() or 'ENABLE_MSR_RECOMP:BOOL=ON' in cache.read_text(errors='replace'):
        sys.exit(f'{build_dir} is not a JIT build: the public package must not hold code '
                 'translated from the game (tools/msr/macos/build.sh)')
    arch = subprocess.run(['lipo', '-archs', str(exe)], capture_output=True, text=True,
                          check=True).stdout.split()
    folder = f'{name}-{ver}-macos-{"universal" if len(arch) > 1 else arch[0]}'
    out = PUBLIC / folder
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    subprocess.run(['ditto', str(app), str(out / app.name)], check=True)
    shutil.copy2(ROOT / 'license.txt', out / 'license.txt')
    write_readme(out / 'README.txt', app.name, True, README_MACOS)

    archive = out.parent / f'{folder}.zip'
    archive.unlink(missing_ok=True)
    subprocess.run(['ditto', '-c', '-k', '--keepParent', str(out), str(archive)], check=True)
    print(f'{archive} ({archive.stat().st_size / 2**20:.0f} MiB)')


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('textures', 'shaders', 'windows', 'public',
                                                'readme', 'macos'):
        sys.exit(__doc__)
    if sys.argv[1] == 'textures':
        if len(sys.argv) < 3:
            sys.exit('usage: package_release.py textures <pack-folder>')
        stage_textures(sys.argv[2])
    elif sys.argv[1] == 'shaders':
        if len(sys.argv) < 3:
            sys.exit('usage: package_release.py shaders <cache-folder>...')
        stage_shaders(sys.argv[2:])
    elif sys.argv[1] == 'readme':
        # For the AppImage script: writes README.txt for Linux to the given path.
        write_readme(pathlib.Path(sys.argv[2]), 'the AppImage')
    elif sys.argv[1] == 'public':
        package_windows(*(sys.argv[2:3] or ['build-jit']), public=True)
    elif sys.argv[1] == 'macos':
        package_macos(*sys.argv[2:3])
    else:
        package_windows(*sys.argv[2:3])


if __name__ == '__main__':
    main()
