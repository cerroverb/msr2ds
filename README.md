# msr-native

Metroid: Samus Returns on PC without an emulator, for private testing. The game's ARM code is recompiled from your
own dump into a library (`msr_aot.dll`), and a small runtime stands in for the 3DS: kernel, system services, the
PICA200 GPU (Vulkan), the DSP, input and the window. The runtime comes from
[Woohoo! Flight School](https://github.com/thefixinhixon/woohoo-flight-school). Samus Returns' own features are on
top: the 16:9 or ultrawide (to 32:9) single screen with the HUD, 30-144 FPS, up to 10x internal resolution, HDR,
texture packs, game mods, the randomizer, save states, cheats, mouse aiming and per-device button icons.
`native/README.md` has the details.

**No game code or data is in this repository.** The dump, the library made from it, saves and texture packs live in
`run/` and `test/`, which git ignores.

## The launcher

Started on its own (a double-click, no options), msr-native is its own launcher: the settings menu, with Play, the
game file and the native code on its Game tab (`native/runtime/msr/launcher.cpp`, drawn by `src/msr/overlay.cpp`).

- **Game file**: the player's dump, typed in or chosen with the system's file dialog (`user/msr/game-path.txt`).
- **Native code**: Build runs the recompiler shipped beside the program (`recomp/`) on the dump. On the first build it
  downloads the build tools (Python on Windows, Zig, capstone) with curl, pinned by SHA-256, into
  `user/msr/recomp-kit/`. Progress shows in the menu; the log is `user/msr/recomp-build.log`.
- **Play**: the program again with `--play` and the display settings (`settings.ini [native]`). The launcher hides
  until the game ends and starts it again when the game's Restart asks.
- **The direct start**: once the native code is built, the launcher writes `msr-native-direct.exe` beside the program
  (Linux: `msr-native-direct.sh` beside the AppImage). It starts the game without the launcher, for a shortcut, Steam
  or a frontend: `msr-native --direct`, Play and Restart without the launcher's window, or the launcher itself while
  the game cannot start (no game file, the native code missing or built by another version). The Windows one is
  `native/runtime/win/direct.c`, a small program that msr-native.exe carries inside it (resource 200) and writes again
  when its own differs, after an update.

The user folder is `user/` beside the program (Windows; Linux when one is there, also beside the AppImage), else
`%LOCALAPPDATA%\msr-native` or `~/.local/share/msr-native`. `MSR_NATIVE_HOME` overrides it. The launcher logs to
`user/launcher.log`, the game to `user/msr-native.log`.

## Android

The Android app (`native/android`, `io.github.gminustwo.msrnative`) is the same program as `libmain.so` in SDL's
activity, with the same launcher. A phone cannot run the recompiler, so the PC's launcher builds the Android native
code too: **Build for Android** on its Game tab runs `build_module.py --target android-arm64`, which cross-compiles
the same generated code for AArch64 with the kit's Zig into `user/msr/recomp-android/` (`libmsr_aot.so`, no imports;
`src/core/arm/msr_aot/a64/` stands in for the SSE intrinsics, on FPCR, and is part of the Android runtime's hash).
The player copies that folder and the game file into the app's user folder, `Android/data/io.github.gminustwo.msrnative/files/user`
(a PC reaches it over USB); the launcher finds the game file there and copies the native code to the app's private
folder before loading it (`native/runtime/android.c`). Play, Restart and Quit start the app again in a new process
(`MsrActivity.relaunch`), as the PC's launcher and game are separate processes. Vulkan 1.3 with dynamic rendering;
push descriptors where the device has them, else descriptor sets in their place (`native/runtime/vk_push.c`). A
controller (or keyboard) is needed to play: there are no touch controls. Back opens the game's menu.

## Layout

| Folder | What |
| --- | --- |
| `native/` | msr-native: the runtime (`runtime/`), Samus Returns' features (`runtime/msr/`), shaders, CMake, the Android app (`android/`) |
| `src/msr/` | Samus Returns code shared with the emulator-based build: settings menu, touch navigation, HUD layout, button icons, cheats, frame-rate patches |
| `src/common/`, `src/core/arm/` | the few helpers those need, the recompiled module's interface (`msr_aot/aot_runtime.h`) and the ARM11 timing table |
| `externals/msr-imgui/` | Dear ImGui (MIT), unmodified |
| `tools/msr/recomp/` | the recompiler: your dump in, `msr_aot.dll` out |
| `tools/dev/shot.ps1` | test driver: starts the game (or, with `-GameArgs ''`, the launcher) from `test/` in the background, sends keys and mouse input, takes screenshots |
| `tools/package/` | the release packages: `windows.py` (zip), `appimage.sh` (Linux), the players' `README.txt`, the icon |
| `run/` | your play folder (not in git) |
| `test/` | the test folder (not in git) |

## Build (Windows)

You need MSYS2 UCRT64 with these packages: `mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,SDL2,libpng,zlib,zstd,shaderc,vulkan-headers,vulkan-loader}`.
The Vulkan SDK also works for the Vulkan headers and glslc.

```sh
# in an MSYS2 UCRT64 shell, or with C:\msys64\ucrt64\bin first in PATH
cmake -S native -B build -G Ninja
cmake --build build -j 8
```

Keep builds at `-j 8` or less on this PC: the i9-14900K resets under all-core loads until its BIOS is updated.

## Releases

```sh
python tools/package/windows.py                  # dist/msr-native-<version>-windows-x64.zip
# Linux, from PowerShell (WSL Ubuntu; SDL2 and zstd built from the emulator tree's source, as the system has neither's
# headers):
wsl -d Ubuntu --cd /mnt/d/Games/pcports/msr-native -- env MSR_JOBS=8 bash tools/package/appimage.sh \
    /home/lou/msr-native-build /home/lou/samus-build/src/externals/sdl2/SDL /home/lou/samus-build/src/externals/zstd
# Android, from PowerShell (Android SDK with NDK 27.3.13750724 and CMake 3.31.6, JDK 17+; the sources: SDL2 2.32.10,
# zstd 1.5.7 and libpng 1.6.47 release tarballs, unpacked; MSR_ANDROID_KEYSTORE/_PASS/_ALIAS: the release key):
cd native/android
./gradlew.bat assembleRelease --max-workers=8 -Pmsr.sdl2=<SDL2-2.32.10> -Pmsr.zstd=<zstd-1.5.7> -Pmsr.png=<libpng-1.6.47>
# -> native/android/app/build/outputs/apk/release/msr-native-<version>-android-arm64.apk
```

The version is `MSR_NATIVE_VERSION` in `native/CMakeLists.txt`. Each package holds the program, the recompiler
(`recomp/`), `tools/package/README.txt` and the licences. The APK holds the program and SDL2 only (the native code
comes from the PC's Build for Android); its native build is capped at `msr.jobs` (8) compiles by CMake job pools.

Updates: the launcher checks the releases of `MSR_UPDATE_REPO` (`native/CMakeLists.txt`, `gminustwo/msr-native`) on
GitHub each time it starts, and offers the newest one above its own version that has the player's package (Update now,
What's new). Update downloads it, checks it against the SHA-256 digest GitHub gives each release file, and puts its
files where the installation has them (Windows: the old ones renamed aside and deleted by the new version; Linux: the
AppImage replaced at the same path), then starts the new version. The user folder is not touched, so the native code
built from the player's dump keeps working; only a version whose `aot_runtime.h` changed asks for a rebuild. To publish
an update:

- raise `MSR_NATIVE_VERSION`, build and package as above;
- make a GitHub release tagged `v<version>` with the two packages as they are named
  (`msr-native-<version>-windows-x64.zip`, `msr-native-<version>-x86_64.AppImage`). Pre-releases are offered too.

The releases must be public: a private repository's are invisible to the players' launchers. `MSR_UPDATE_URL=<url>`
reads another release list (GitHub's format; `file://` works, for tests), `MSR_UPDATE_URL=off` skips the check, and
`user/msr/update.log` records what it did.

## The game's code

`tools/msr/recomp/build_module.py` checks the dump, recompiles its code and writes the library. Supported dump:
Samus Returns (USA), decrypted. The `--kit` folder holds the compiler tools (Zig, Python with capstone), as the
emulator-based build's launcher downloads them. `run/user/msr/recomp-kit` is a copy of yours. Use
`--cxx g++` to build with MSYS2's compiler instead.

```sh
python tools/msr/recomp/build_module.py --dump "<dump.cci>" --user run/user --kit run/user/msr/recomp-kit --jobs 8
```

The library lands in `run/user/msr/recomp/`. Rebuild it whenever `src/core/arm/msr_aot/aot_runtime.h` or the
recompiler changes. msr-native refuses a library built against another `aot_runtime.h`.

## Play

```powershell
.\run.ps1                       # 9x internal resolution, the settings' frame rate
.\run.ps1 -Resolution 4 -Fps 60 -Fullscreen
```

`run.ps1` starts `build\msr-native.exe` in `run\` and restarts it when its menu asks to. The log is
`run\msr-native.log`. The play folder holds:

- `user\msr\game-path.txt`: the dump's path
- `user\msr\recomp\`: the library
- `user\msr\settings.ini`: settings, controls, texture-pack order
- `user\native-save\`: saves
- `user\mods\textures\`, `user\mods\game\`: texture packs, game mods
- `pwr_shaders.bin`, `pwr_pipelines.bin`: the shader cache

## Controls

Keyboard and mouse (rebind in F1 → Controls):

| Key | Does |
| --- | --- |
| WASD | Move |
| Space | Jump |
| Mouse left / V | Shoot |
| Mouse right (held) | Free Aim at the pointer |
| Q / E | Free Aim / ready missiles (L / R) |
| Left Shift | Morph Ball |
| C, mouse wheel | Change beam |
| F, Enter | Aeion, confirm |
| R | Melee counter |
| 1 2 3 4 | Aeion abilities |
| Tab | Pause menu |
| T | Touch navigation |
| F1, Backspace | Settings |
| F5 / F8 | Quick save / load |
| F11 | Fullscreen |

Gamepads are read by button position. The game's button icons and the hints beside the menus follow the device you
used last: Xbox, PlayStation, Nintendo or the keyboard's keys.

## Testing

`tools\dev\shot.ps1` runs the game from `test\` without taking the foreground. It posts keys (VK codes in hex) and
mouse input and saves screenshots to `test\shots\`:

```powershell
.\tools\dev\shot.ps1 -Seconds 14 -Shot title -Keys '46:200,wait6000,shot'
```

`test\` is set up like `run\`, with test saves; profile B is a 100% file. The script stops only the process it
started, so a game running from `run\` is safe. `native/README.md` lists the debugging switches (`PWR_*`).

## Licences

- `native/`: GPL-3.0-or-later (`native/LICENSE`), as Woohoo! Flight School.
- `src/` and `tools/`: GPL-2.0-or-later (`license.txt`), from the emulator-based Samus Returns port.
- Dear ImGui and AMD FidelityFX FSR 1: MIT.
- The UI font: DejaVu Sans Mono.

The program as built is GPL-3.0-or-later.
