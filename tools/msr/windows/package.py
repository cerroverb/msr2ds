"""Packages the Windows build to run on another PC, without the game or anyone's data.

Copies <build-dir>/bundle to <build-dir>/package/SamusReturns and zips it as
<build-dir>/SamusReturns-windows.zip. Left out: user/ (saves, settings, the remembered game
location), game/ and any game dump, and the emulator's scripting sample. The launcher asks for
the dump on first start.

build-windows holds the recompiled main build, which embeds code translated from the game: to
share, build without it first (bash tools/msr/windows/build.sh build-jit) and package that.

Run from the repository root: python tools/msr/windows/package.py [build-dir]
"""
import os
import pathlib
import shutil
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
EXCLUDED_DIRS = {'user', 'game', 'scripting'}
DUMP_SUFFIXES = {'.3ds', '.cci', '.cxi', '.cia', '.app', '.zcci', '.zcxi'}


def main(build_dir='build-windows'):
    bundle = ROOT / build_dir / 'bundle'
    if not (bundle / 'SamusReturns.exe').is_file():
        sys.exit(f'{bundle} has no SamusReturns.exe: run tools/msr/windows/build.sh first')
    cache = ROOT / build_dir / 'CMakeCache.txt'
    if cache.exists() and 'ENABLE_MSR_RECOMP:BOOL=ON' in cache.read_text(errors='replace'):
        print('WARNING: this is a recompiled build. SamusReturns.exe contains code translated '
              'from your game dump; keep the package for your own machines.')

    out = ROOT / build_dir / 'package' / 'SamusReturns'
    if out.exists():
        shutil.rmtree(out)
    skipped = []
    for folder, dirs, files in os.walk(bundle):
        here = pathlib.Path(folder)
        if here == bundle:
            skipped += [d + '/' for d in dirs if d in EXCLUDED_DIRS]
            dirs[:] = [d for d in dirs if d not in EXCLUDED_DIRS]
        for name in files:
            source = here / name
            if source.suffix.lower() in DUMP_SUFFIXES:
                skipped.append(str(source.relative_to(bundle)))
                continue
            target = out / source.relative_to(bundle)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    shutil.copy2(ROOT / 'license.txt', out / 'license.txt')

    archive = ROOT / build_dir / 'SamusReturns-windows.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(out.rglob('*')):
            if path.is_file():
                zf.write(path, pathlib.Path('SamusReturns') / path.relative_to(out))

    files = sum(1 for p in out.rglob('*') if p.is_file())
    print(f'{files} files in {out}')
    print(f'{archive} ({archive.stat().st_size / 2**20:.1f} MiB)')
    if skipped:
        print('Left out: ' + ', '.join(skipped))


if __name__ == '__main__':
    main(*sys.argv[1:])
