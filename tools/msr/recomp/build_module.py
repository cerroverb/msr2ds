"""Builds the recompiled code from the player's own dump into a library the game loads.

The launcher runs this (Game tab, "Recompiled code"): any build of the port, the public one
included, can then run Samus Returns' code natively instead of on the JIT, while the package
itself holds no code from the game. The steps, like Woohoo! Flight School's pipeline:

  1. check    the dump: a decrypted Samus Returns (USA) whose game code has the SHA-256 the
              recompiler's tables were made for (patchable.txt, extra_entries.txt, the port's
              patches); anything else is refused
  2. extract  the decompressed ExeFS .code and the ExHeader (ctr.py)
  3. analyze  find the code and recompile it to C++ (analyze.py, generate.py), with the timing
              table built from the emulator's own ARM11 cycle model (ticks.cpp)
  4. compile  the generated C++ in parallel, then link msr_aot.dll / libmsr_aot.so
  5. install  into <user>/msr/recomp/ with module.json; the game loads it at the next boot
              (src/core/arm/msr_aot/arm_aot.cpp) and checks it against its own aot_runtime.h
              and the program it runs

Usage:
  python build_module.py --dump <dump.3ds|.cci|.cxi> --user <user folder> [options]
  python build_module.py --setup-kit <kit folder> --zig-archive <zig .zip/.tar.xz>
                         --wheel <capstone .whl>

Options:
  --kit DIR        the build tools the launcher downloaded: zig/ (compiler), site/ (capstone)
  --cxx CMD        another C++ compiler instead of the kit's Zig, e.g. g++ (MSYS2) or clang++
  --jobs N         parallel compiles (default: logical processors)
  --extra FILE     more entry points (the game writes user/msr/recomp-jit-entries-core*.txt;
                   the launcher passes them on a rebuild)
  --port-version   recorded in module.json, so the launcher knows when to offer a rebuild
  --keep-work      keep the generated code and objects (<user>/msr/recomp-build/) for faster
                   rebuilds; by default only the library stays
  --target T       host (default: this PC's msr-native), or android-arm64: libmsr_aot.so for the
                   Android app, cross-compiled with the kit's Zig into <user>/msr/recomp-android/
                   (the generated code is the same; aot_runtime.h's SSE intrinsics come from
                   src/core/arm/msr_aot/a64/ there, on AArch64's FPCR)

Output protocol for the launcher (stdout, one per line): "@step <n> <count> <text>",
"@progress <done> <total>", "@done <json>", "@error <text>". Everything else is log.

Everything made from the dump is game code and stays in the player's user folder.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import queue
import shutil
import struct
import subprocess
import sys
import tarfile
import threading
import time
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]  # the source tree, or the package's recomp/ folder (same layout)
RUNTIME = REPO / 'src' / 'core' / 'arm' / 'msr_aot'
A64 = RUNTIME / 'a64'  # <emmintrin.h>/<xmmintrin.h> for AArch64 (msr_sse_a64.h)
sys.path.insert(0, str(HERE))

PROGRAM_ID = '00040000001BB200'
# Metroid: Samus Returns (USA), the decompressed ExeFS .code.
CODE_SHA256 = '669090e6caeef405aaf4d2b884749460f8f0b868ecd158c7f1943ac235c102c6'
STEPS = 5
WINDOWS = os.name == 'nt'
LIBRARY = 'msr_aot.dll' if WINDOWS else 'libmsr_aot.so'
NO_WINDOW = 0x08000000 if WINDOWS else 0  # CREATE_NO_WINDOW
ANDROID = 'android-arm64'
# The C library functions the Android library may call, for the stand-in libc.so it links against
# (Android's own libc.so provides them when the app loads it).
ANDROID_LIBC = ['memcpy', 'memmove', 'memset', 'memcmp', 'strlen', 'strncmp', 'strcmp', 'malloc', 'calloc',
                'realloc', 'free', 'abort', '__cxa_atexit', '__cxa_finalize', '__stack_chk_fail', '__stack_chk_guard']


class BuildError(Exception):
    pass


def emit(*parts):
    print('@' + ' '.join(str(p) for p in parts), flush=True)


def log(text):
    print(text, flush=True)


def runtime_sha256(target='host'):
    """aot_runtime.h without carriage returns, as src/core/CMakeLists.txt hashes it; for Android
    followed by a64/msr_sse_a64.h, as native/CMakeLists.txt hashes them there."""
    text = (RUNTIME / 'aot_runtime.h').read_bytes().replace(b'\r', b'')
    if target == ANDROID:
        text += (A64 / 'msr_sse_a64.h').read_bytes().replace(b'\r', b'')
    return hashlib.sha256(text).hexdigest()


def recipe_sha256(target='host'):
    """Everything besides the dump that decides the library's contents."""
    digest = hashlib.sha256()
    files = sorted(HERE.glob('*.py')) + sorted(HERE.glob('*.txt')) + [HERE / 'ticks.cpp'] + \
        sorted(RUNTIME.glob('aot_*')) + (sorted(A64.glob('*.h')) if target == ANDROID else [])
    for path in files:
        digest.update(path.name.encode() + b'\0' + path.read_bytes().replace(b'\r', b''))
    return digest.hexdigest()


# -- the compiler -------------------------------------------------------------------------------

class Compiler:
    """Zig from the kit (Clang), or a C++ compiler the player has."""

    def __init__(self, args):
        self.zig = None
        self.cache = None
        self.android = getattr(args, 'target', 'host') == ANDROID
        # Each parallel compile gets its own Zig cache: many at once in one cache sometimes
        # failed with exit code 5 (access denied) and no message on Windows.
        self.slots = queue.Queue()
        if args.cxx:
            self.command = args.cxx.split()
        elif os.environ.get('MSR_CXX'):
            self.command = os.environ['MSR_CXX'].split()
        else:
            zig = kit_zig(args.kit)
            if zig:
                self.zig = zig
                self.command = [str(zig), 'c++']
            else:
                found = shutil.which('g++') or shutil.which('clang++')
                if not found:
                    raise BuildError('No C++ compiler: download the build tools in the launcher.')
                self.command = [found]
        self.name = 'zig' if self.zig else pathlib.Path(self.command[0]).stem
        if self.zig:
            # Kept with the kit, so nothing lands in the player's profile folder.
            self.cache = pathlib.Path(args.kit) / 'cache'
            os.environ['ZIG_GLOBAL_CACHE_DIR'] = str(self.cache / 'global')
            os.environ['ZIG_LOCAL_CACHE_DIR'] = str(self.cache / 'local')
            # A fixed target, not the build machine's CPU: the library also runs where the
            # user folder is copied (the exe itself needs SSE4.2).
            self.target = ['-target', 'x86_64-windows-gnu' if WINDOWS else 'x86_64-linux-gnu.2.28',
                           '-mcpu=x86_64_v2']
        else:
            self.target = ['-march=x86-64-v2']
        if self.android:
            # Any 64-bit ARM phone (ARMv8.0). musl's headers stand in for Android's C library: the
            # library calls only memcpy-like functions, linked against a stand-in libc.so (link).
            if not self.zig:
                raise BuildError('The Android build needs the kit\'s Zig: download the build tools in the launcher.')
            self.target = ['-target', 'aarch64-linux-musl', '-mcpu=generic']

    def version(self):
        try:
            out = subprocess.run(self.command + ['--version'], capture_output=True, text=True,
                                 creationflags=NO_WINDOW, timeout=120)
            return (out.stdout or out.stderr).strip().splitlines()[0]
        except (OSError, IndexError, subprocess.TimeoutExpired):
            return self.name

    def flags(self):
        flags = self.target + (['-I', str(A64)] if self.android else []) + \
            ['-std=c++20', '-O1', '-g0', '-w', '-ffp-contract=off', '-fno-strict-aliasing',
             '-fno-exceptions', '-fno-rtti', '-fvisibility=hidden',
             '-DMSR_AOT_SHARED', '-I', str(RUNTIME)]
        if self.android:
            flags += ['-fno-threadsafe-statics', '-fno-stack-protector']   # no C++ runtime there
        if not WINDOWS or self.android:
            flags.append('-fPIC')
        return flags

    def compile(self, source, obj, extra=()):
        """None when it compiled, else what went wrong."""
        cmd = self.command + self.flags() + list(extra) + ['-c', str(source), '-o', str(obj)]
        env = None
        slot = None
        if self.cache:
            try:
                slot = self.slots.get_nowait()
            except queue.Empty:
                slot = 'x%d' % threading.get_ident()
            env = dict(os.environ, ZIG_LOCAL_CACHE_DIR=str(self.cache / 'local' / str(slot)))
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, creationflags=NO_WINDOW, env=env)
        finally:
            if slot is not None:
                self.slots.put(slot)
        if result.returncode == 0:
            return None
        if obj.exists():
            obj.unlink()
        return 'exit code %d: %s' % (result.returncode, (result.stderr or result.stdout).strip()[-4000:])

    def link(self, objects, out, cwd):
        rsp = cwd / 'link.rsp'
        rsp.write_text('\n'.join('"%s"' % o.replace('\\', '/') for o in objects) + '\n')
        cmd = self.command + self.target + ['-shared', '-s', '-o', str(out), '@link.rsp']
        if self.android:
            # No C or C++ library linked in: the functions it calls come from Android's libc.so
            # (DT_NEEDED on a stand-in made here). 16 KB pages (Android 15), and nothing undefined.
            stub = cwd / 'android-libc'
            stub.mkdir(exist_ok=True)
            (stub / 'libc.c').write_text(''.join('void %s(void) {}\n' % name for name in ANDROID_LIBC
                                                 if name != '__stack_chk_guard') + 'long __stack_chk_guard;\n')
            result = subprocess.run(self.command[:-1] + ['cc'] + self.target +
                                    ['-shared', '-nostdlib', '-Wl,-soname,libc.so', '-o', str(stub / 'libc.so'),
                                     str(stub / 'libc.c')], capture_output=True, text=True, creationflags=NO_WINDOW)
            if result.returncode != 0:
                raise BuildError('The stand-in libc.so failed:\n%s' % (result.stderr or result.stdout)[-4000:])
            cmd += ['-nostdlib', '-Wl,-z,max-page-size=16384', '-Wl,-z,defs', '-Wl,-soname,libmsr_aot.so',
                    str(stub / 'libc.so')]
        if not self.zig and WINDOWS:
            cmd += ['-static-libgcc', '-static-libstdc++']
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=str(cwd), creationflags=NO_WINDOW)
        if result.returncode != 0:
            raise BuildError('Linking failed:\n%s' % (result.stderr or result.stdout)[-4000:])
        # Zig also writes an import library and debug symbols beside it: not needed.
        for leftover in list(out.parent.glob('*.lib')) + list(out.parent.glob('*.pdb')):
            leftover.unlink()

    def ticks_command(self):
        """For generate.instruction_ticks: compiles the timing helper for this machine."""
        return self.command + (['-target', 'x86_64-windows-gnu' if WINDOWS else 'x86_64-linux-gnu.2.28']
                               if self.zig else [])


def kit_zig(kit):
    if not kit:
        return None
    path = pathlib.Path(kit) / 'zig' / ('zig.exe' if WINDOWS else 'zig')
    return path if path.is_file() else None


# -- the steps ----------------------------------------------------------------------------------

def check_dump(dump):
    import ctr
    try:
        code, exheader, layout = ctr.read_game(dump)
    except FileNotFoundError:
        raise BuildError('The dump was not found: %s' % dump)
    except ValueError as error:
        if 'encrypted' in str(error):
            raise BuildError('This dump is encrypted. Decrypt it first (the game itself runs '
                             'from it, but the recompiler reads its code directly).')
        raise BuildError('Not a 3DS game dump (%s).' % error)
    except (OSError, struct.error) as error:
        raise BuildError('The dump could not be read (%s).' % error)
    if layout['program_id'] != PROGRAM_ID:
        raise BuildError('This is not Metroid: Samus Returns (USA): program %s.' % layout['program_id'])
    digest = hashlib.sha256(code).hexdigest()
    if digest != CODE_SHA256:
        raise BuildError('This dump\'s game code is not the version the recompiler supports '
                         '(SHA-256 %s...). A modified or damaged dump?' % digest[:16])
    return code, exheader, layout


def write_if_changed(path, data):
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def generate_code(work, code, exheader, layout, extras, compiler):
    import analyze
    import generate
    inputs = work / 'input'
    inputs.mkdir(parents=True, exist_ok=True)
    write_if_changed(inputs / 'code.bin', code)
    write_if_changed(inputs / 'exheader.bin', exheader)
    write_if_changed(inputs / 'layout.json', json.dumps(layout, indent=2).encode())
    an = analyze.Analyzer(code, layout)
    an.extra_files = [str(p) for p in extras]
    log('Analyzing the code...')
    an.run()
    log('Building the timing table...')
    generate.TICKS_COMMAND = compiler.ticks_command()
    an.ticks = generate.instruction_ticks(an, work / 'ticks')
    an.patchable = generate.patchable_instructions(an)
    log('Translating...')
    regions = generate.all_regions(an)
    text = code[:layout['text']['size']]
    return generate.generate(an, regions, work / 'generated', 6000, hashlib.sha256(text).digest(), 0x4000)


def compile_all(work, compiler, jobs):
    generated = work / 'generated'
    objects = work / 'obj'
    objects.mkdir(parents=True, exist_ok=True)
    runtime_hash = runtime_sha256(ANDROID if compiler.android else 'host')
    sources = sorted(generated.glob('aot_*.cpp')) + [RUNTIME / 'aot_module.cpp']
    # Objects are reused when their source and the flags are unchanged.
    stamp = json.dumps({'cmd': compiler.command, 'flags': compiler.flags(), 'runtime': runtime_hash,
                        'module': hashlib.sha256((RUNTIME / 'aot_module.cpp').read_bytes()).hexdigest()})
    stamp_file = objects / 'flags.json'
    if not stamp_file.exists() or stamp_file.read_text() != stamp:
        for old in objects.glob('*.o'):
            old.unlink()
    stamp_file.write_text(stamp)
    keep = {s.stem + '.o' for s in sources}
    for old in objects.glob('*.o'):
        if old.name not in keep:
            old.unlink()
    todo = []
    for source in sources:
        obj = objects / (source.stem + '.o')
        if not obj.exists() or obj.stat().st_mtime < source.stat().st_mtime:
            extra = ['-DMSR_AOT_RUNTIME_SHA256="%s"' % runtime_hash] if source.name == 'aot_module.cpp' else []
            todo.append((source, obj, extra))
    # Largest first: the long ones start early and the last minute is not one big file.
    todo.sort(key=lambda item: -item[0].stat().st_size)
    total = len(todo)
    log('Compiling %d of %d files with %d jobs' % (total, len(sources), jobs))
    emit('progress', 0, total)
    done = 0
    failed = []
    for slot in range(jobs):
        compiler.slots.put(slot)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(compiler.compile, *item): item for item in todo}
        try:
            for future in concurrent.futures.as_completed(futures):
                problem = future.result()
                if problem:
                    log('%s: %s (trying again later)' % (futures[future][0].name, problem))
                    failed.append(futures[future])
                    continue
                done += 1
                emit('progress', done, total)
        except BaseException:
            for future in futures:
                future.cancel()
            raise
    # Zig sometimes fails under load on Windows with exit code 5 (access denied, likely a
    # virus scanner holding a fresh file) and no message: those get more tries on their own.
    for source, obj, extra in failed:
        for attempt in range(3):
            problem = compiler.compile(source, obj, extra)
            if not problem:
                break
            time.sleep(1 + attempt)
        if problem:
            raise BuildError('%s failed to compile (%s)' % (source.name, problem))
        done += 1
        emit('progress', done, total)
    return [s.stem + '.o' for s in sources]


def install(work, user, compiler, objects, stats, args, started):
    out_dir = user / 'msr' / ('recomp-android' if compiler.android else 'recomp')
    library = 'libmsr_aot.so' if compiler.android else LIBRARY
    out_dir.mkdir(parents=True, exist_ok=True)
    temporary = work / ('new-' + library)
    if temporary.exists():
        temporary.unlink()
    compiler.link(objects, temporary, work / 'obj')
    target = out_dir / library
    try:
        os.replace(temporary, target)
    except PermissionError:
        raise BuildError('The game is still using the recompiled code: stop the game and build again.')
    kind = ANDROID if compiler.android else 'host'
    info = {
        'port_version': args.port_version or '',
        'target': kind,
        'runtime_sha256': runtime_sha256(kind),
        'recipe_sha256': recipe_sha256(kind),
        'code_sha256': CODE_SHA256,
        'compiler': compiler.version(),
        'built': time.strftime('%Y-%m-%d %H:%M:%S'),
        'seconds': round(time.time() - started),
        'library_bytes': target.stat().st_size,
        'functions': stats.get('regions'),
        'instructions': stats.get('unique_instructions'),
        'interpreted_instructions': stats.get('interpreted_instructions'),
        'extra_entry_files': [str(p) for p in args.extra],
    }
    (out_dir / 'module.json').write_text(json.dumps(info, indent=1))
    return info


def raise_fd_limit():
    """Zig's linker keeps every object open while it links the library (372 of them), and macOS starts an app
    opened from the Finder with a limit of 256 open files: the compiler and linker inherit a higher one from
    here (macOS refuses an unlimited one). Windows has no such limit."""
    if WINDOWS:
        return
    import resource
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = 10240 if hard == resource.RLIM_INFINITY else min(hard, 10240)
    if soft != resource.RLIM_INFINITY and soft < want:
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))
        except (ValueError, OSError):
            pass


def build(args):
    started = time.time()
    raise_fd_limit()
    user = pathlib.Path(args.user).resolve()
    work = user / 'msr' / ('recomp-build-android' if args.target == ANDROID else 'recomp-build')
    compiler = Compiler(args)
    log('Compiler: %s' % ' '.join(compiler.command))
    extras = [pathlib.Path(p) for p in args.extra if pathlib.Path(p).is_file()]

    emit('step', 1, STEPS, 'Checking the dump')
    code, exheader, layout = check_dump(args.dump)
    emit('step', 2, STEPS, 'Reading the game code')
    emit('step', 3, STEPS, 'Recompiling the game code')
    stats = generate_code(work, code, exheader, layout, extras, compiler)
    log(json.dumps({k: v for k, v in stats.items() if k != 'interpreted_by_reason'}))
    emit('step', 4, STEPS, 'Compiling')
    objects = compile_all(work, compiler, args.jobs)
    emit('step', 5, STEPS, 'Installing')
    info = install(work, user, compiler, objects, stats, args, started)
    if compiler.cache:
        # Zig keeps a copy of every object there: hundreds of MB that a rebuild does not use.
        shutil.rmtree(compiler.cache / 'local', ignore_errors=True)
    if not args.keep_work:
        shutil.rmtree(work / 'generated', ignore_errors=True)
        shutil.rmtree(work / 'obj', ignore_errors=True)
        shutil.rmtree(work / 'input', ignore_errors=True)
    emit('done', json.dumps(info))


# -- the kit ------------------------------------------------------------------------------------

# Zig's parts for systems other than this one (C library sources and headers, sanitizers).
ZIG_TRIM_COMMON = ['doc', 'lib/libtsan', 'lib/libc/musl', 'lib/libc/wasi', 'lib/libc/darwin',
                   'lib/libc/freebsd', 'lib/libc/netbsd', 'lib/libc/openbsd']
ZIG_TRIM = ZIG_TRIM_COMMON + (['lib/libc/glibc'] if WINDOWS else ['lib/libc/mingw'])
# The C library headers kept (lib/libc/include/<target>): this system's, and the Android library's
# (--target android-arm64 compiles against musl's AArch64 headers; about 7 MB).
ZIG_HEADERS_ANDROID = {'any-linux-any', 'generic-musl', 'aarch64-linux-any', 'aarch64-linux-musl'}
ZIG_HEADERS_KEEP = ({'any-windows-any'} if WINDOWS else
                    {'any-linux-any', 'generic-glibc', 'x86-linux-any', 'x86-linux-gnu'}) | ZIG_HEADERS_ANDROID


def setup_kit(args):
    """Unpacks the downloaded Zig and capstone into the kit folder (the launcher downloads them
    and checks their SHA-256 first)."""
    kit = pathlib.Path(args.setup_kit)
    kit.mkdir(parents=True, exist_ok=True)
    emit('step', 1, 2, 'Unpacking Zig')
    zig = kit / 'zig'
    staging = kit / 'zig-unpack'
    shutil.rmtree(staging, ignore_errors=True)
    shutil.rmtree(zig, ignore_errors=True)
    archive = args.zig_archive
    if archive.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            z.extractall(staging)
    else:
        with tarfile.open(archive) as t:
            t.extractall(staging, **({'filter': 'tar'} if sys.version_info >= (3, 12) else {}))
    top = [p for p in staging.iterdir() if p.is_dir()]
    if len(top) != 1:
        raise BuildError('Unexpected Zig archive layout')
    top[0].rename(zig)
    shutil.rmtree(staging, ignore_errors=True)
    for part in ZIG_TRIM:
        shutil.rmtree(zig / part, ignore_errors=True)
    headers = zig / 'lib' / 'libc' / 'include'
    if headers.is_dir():
        for folder in headers.iterdir():
            if folder.is_dir() and folder.name not in ZIG_HEADERS_KEEP:
                shutil.rmtree(folder, ignore_errors=True)
    if not WINDOWS:
        (zig / 'zig').chmod(0o755)
    emit('step', 2, 2, 'Unpacking capstone')
    site = kit / 'site'
    shutil.rmtree(site, ignore_errors=True)
    with zipfile.ZipFile(args.wheel) as z:
        z.extractall(site)
    if not WINDOWS:
        for lib in site.rglob('libcapstone.so*'):
            lib.chmod(0o755)
    emit('done', json.dumps({'zig': str(zig), 'site': str(site)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--dump')
    parser.add_argument('--user')
    parser.add_argument('--kit')
    parser.add_argument('--cxx')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    parser.add_argument('--extra', action='append', default=[])
    parser.add_argument('--port-version')
    parser.add_argument('--keep-work', action='store_true')
    parser.add_argument('--target', choices=['host', ANDROID], default='host')
    parser.add_argument('--setup-kit')
    parser.add_argument('--zig-archive')
    parser.add_argument('--wheel')
    parser.add_argument('--info', action='store_true', help='print the runtime and recipe hashes')
    args = parser.parse_args()
    if args.kit and (pathlib.Path(args.kit) / 'site').is_dir():
        sys.path.insert(1, str(pathlib.Path(args.kit) / 'site'))
    try:
        if args.info:
            print(json.dumps({'runtime_sha256': runtime_sha256(args.target), 'recipe_sha256': recipe_sha256(args.target)}))
        elif args.setup_kit:
            if not (args.zig_archive and args.wheel):
                parser.error('--setup-kit needs --zig-archive and --wheel')
            setup_kit(args)
        else:
            if not (args.dump and args.user):
                parser.error('--dump and --user are needed')
            build(args)
    except BuildError as error:
        emit('error', str(error).replace('\n', ' | '))
        sys.exit(1)
    except KeyboardInterrupt:
        emit('error', 'Cancelled')
        sys.exit(1)
    except ImportError as error:
        emit('error', 'The build tools are incomplete (%s): download them again.' % error)
        sys.exit(1)
    except Exception as error:  # noqa: BLE001 - reported to the launcher
        import traceback
        traceback.print_exc()
        emit('error', '%s: %s' % (type(error).__name__, error))
        sys.exit(1)


if __name__ == '__main__':
    main()
