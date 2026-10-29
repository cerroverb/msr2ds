# msr2ds

*Metroid: Samus Returns* as a single-screen PC game, built on the Azahar 3DS
emulator. You need your own decrypted dump of the game (USA): none is included,
and nothing from the game is in this repository. Windows and Linux downloads are
on the [releases page](https://github.com/gminustwo/msr2ds/releases).

One 16:9 screen in the style of *Metroid Dread*. Energy and missiles sit at the
upper left with the Aeion gauge and the beam slots (the usual beam, Ice and
Grapple) under them, and the map is at the upper right with the DNA and Metroid
counts below it. Start brings up the game's own bottom screen (map, inventory,
options); the right trigger switches between the usual, Ice and Grapple Beams.
It renders in widescreen up to 4K internal resolution, at 30 (the original), 60,
120 or 144 frames per second, and loads texture packs. A launcher picks the game
dump and holds the settings, which are also in an in-game overlay (F1 or Select).

It follows the approach of **DekUI**, a *Majora's Mask 3D* port: a fork of the
Azahar 3DS emulator, turned into a single-game player by a game module in
`src/msr/` and a few guarded hooks. Azahar is pinned at **2126.2**; its own
README is now `README-AZAHAR.md`. Released builds run the game on Azahar's JIT
CPU. Built from your own dump, the game's ARM code can instead be recompiled
ahead of time into C++, for your own use (see [Recompiled build](#recompiled-build)).
Either way, Azahar provides the 3DS system services, PICA GPU emulation on
Vulkan and audio.

## Play

1. Download the Windows zip or the Linux AppImage from the
   [releases page](https://github.com/gminustwo/msr2ds/releases).
2. Windows: unzip it anywhere and run `SamusReturns.exe`. Linux: make the
   AppImage executable (`chmod +x`) and run it.
3. The launcher asks for your decrypted dump (`.3ds` or `.cci`) the first time
   and remembers it. Press Play.

Afterwards `SamusReturns-Direct.exe` (Linux: `--play`) skips the launcher. It
needs a GPU with Vulkan. The first start uses a 4K internal resolution: lower it
in the launcher's Display tab on a weaker GPU. No texture pack is included;
Citra and Azahar packs work (see [Texture packs](#texture-packs)).

## How the single screen works

Samus Returns draws its whole HUD on the bottom screen. The top screen shows
only the world. The Vulkan presenter (`renderer_vulkan.cpp`, "Samus Returns
single-screen compositor") rebuilds the layout every frame:

- **Game view:** the top screen fills a 16:9 view.
- **State detection:** a 320×240 copy of the bottom screen is read back
  asynchronously, and `src/msr/bottom_classifier.h` decides what is showing,
  using thresholds measured on the USA release:
  - **Gameplay HUD** (both cyan framing rules, found by contrast, at full
    brightness): its pieces are composited onto
    the view. A brightness key makes the dark backdrop transparent, and soft
    shades sit behind the digits. Energy, tanks, ammo, the Aeion gauge (rows
    45-50) and the ability selector on their left (down to row 58) go upper
    left (at 0.8×), with no soft edge along the screen's border, where the
    selector's left icon (Lightning Armor) sits. Below them, the beam slots in a row: the Aeion ability (the
    usual beam), then the Ice and Grapple Beams once Samus has them, each as
    just its icon (a circular mask drops the slot's octagonal frame, a key its
    striped fill: a higher one for the lit slot, whose fill is brighter). The
    lit one glows. The classifier reads which slot is lit (a quarter of it
    brighter than 200) and which hold an icon (brightest minus darkest fifth of
    the middle above 40; the empty Grapple slot is a plain striped fill). In
    Morph Ball the game greys the row out (the lit slot no longer reaches 240)
    and its unlit icons read as empty, so the slots Samus has are kept: one
    joins after 0.1 s holding an icon and leaves after 1 s empty with the row
    lit, or at the file select. Greyed out, the icons stay in place, keyed
    lower to follow the game's dimming. A
    framed minimap around Samus sits upper right, with the DNA and
    Metroid counts under it, kept clear of the game's messages along the bottom.
    Its window follows Samus's marker (`FindMapMarker`: the strongest 12 px
    square of red and orange pixels on the HUD's map, read only when fully
    lit), gliding to keep her in the middle: near an area's edges the game
    stops scrolling its map and moves the marker instead. Past the game's map
    (`MapRect`) the window shows the backdrop measured along that edge.
  - **Framed menu** (any long cyan rule: pause pages, map pins, file select):
    the whole bottom screen appears as a centered panel over the dimmed game,
    and the mouse touches it. The title menus (file select and the screens
    after it) lose their backdrop: the rasterizer leaves out its two draws
    (`SkipMsrBackdrop` in `vk_rasterizer.cpp`: the first two bottom-screen
    quads, from a 1024×512 I8 atlas), and the compositor treats the menu as
    drawn over black, its brightness as coverage, on a dark card (the HUD
    tab's "Title menu background", 0.75) that lets a little of the title's 3D
    scene through.
  - **Pages**, for touch navigation, the control reminders and the gallery
    layout: a mostly black screen is a title menu. Among those, the gallery
    viewer has a cyan edge down each side of its page counter, and the file
    select a box at y 33 (closed at y 85 in the list; an opened file's runs on).
    Otherwise the rule under the title tells the pause menu's map page, Samus
    page and map pins tool apart.
  - **Gallery viewer:** the art or track name is on the top screen and only the
    page counter on the bottom, so the view shrinks to 72% on the left and the
    panel stands beside it. The layout eases in and out, and touches follow the
    panel.
  - **Prompt** (title, prologue): the text floats in the lower part of the view,
    like subtitles. It shows once it has held for 0.3 s, since menus fading in
    or out can read as a prompt for a few frames. The title's "Press A to Start"
    fades almost to black every 1.5 s, and dim it reads as the idle emblem: a
    prompt on screen stays through gaps of up to 1.5 s, and text alone on black
    is shown at full brightness (scaled by the frame's peak), pulsing between
    70% and 100%. The game centres prompt text on row ~125 and the prologue
    types pages of up to nine lines (rows 40-211). One line (the title's
    prompt) shows at the menu panel's scale with row 120 at 80% of the view;
    from a second line on, the text eases to a scale that fits rows 36-216 in
    the lower half and keeps it while pages follow (5 s), so it never moves as
    it is typed. Behind it sits the title menus' dark card (the same "Title
    menu background" opacity), sized to the text (`BrightBox`, made even about
    x 160, so a line's card has its full width from its first letter): it grows
    as lines appear and takes a new page's size at once when the first line
    moves, or after smaller text has held for 0.5 s. The prompt is drawn from
    the live bottom screen, so it goes at once, without fading, when another
    screen comes (a menu, the HUD, or anything reaching the screen's edges,
    which a prompt's text never does), and none shows for 0.3 s after: the
    file select after the title's A and the loading emblem after the prologue
    fade in from black, and drawn as the prompt, brightened as text on black
    is, they flashed.
  - **Idle emblem** (cutscenes, loading) and the **dimmed HUD** the game shows
    during cutscenes: hidden. A dimmed HUD is told by its framing rule at row
    40 and by having no title rule (rows 18-31: at most 23 px on the HUD,
    118-259 px under a pause page's title). The title rule matters on a
    well-explored pause map, whose outlines can cover most of row 40.
- **Pause transitions:** when Start reaches the game during gameplay (HID
  records it), the HUD fades at once and nothing else shows until the pause
  menu does; when that menu closes, no prompt shows until the HUD is back. The
  readback runs a frame or two behind, and the menu opens and closes over a
  black bottom screen, which otherwise flashed pieces of it in the wrong place.
  Fades follow emulated time, so they look the same at every frame rate. A new
  reading takes effect once it has held for 3 readbacks and 80 ms of emulated
  time: as the menu opens and closes, the HUD slides a few rows up for ~40 ms
  and reads as a menu, and above 60 FPS each of those frames is read back
  several times, which flashed the whole bottom screen.
- **Touch navigation** (`src/msr/touch_nav.cpp`): Tab, or L3 while a menu is
  on screen, shows the touch screen with a highlight. The D-pad or left stick moves it between the
  screen's buttons: the HUD, the pause map page toolbar, the Samus page grids,
  the map pins and the file select (the files; an opened file's buttons, copy
  and delete), all measured. On other screens it moves a free cursor. A is a
  finger: tap, or hold and move to drag (map pins). B, L3 or Esc leaves. Taps
  are injected into HID directly.
- **Beam slots** (`CycleBeam` in `input_bridge.cpp`): the game switches to the
  Ice or Grapple Beam only by touching its slot, and back by touching the Aeion
  ability's. The right trigger (the 3DS ZR, which the game leaves unused) taps
  the next slot Samus has: usual beam, Ice, Grapple, usual beam. Right after a
  tap it counts from the slot it tapped, since the bottom screen shows it a few
  frames later.
- **Morph Ball** (`ToggleMorphBall` in `input_bridge.cpp`): the left trigger
  (the 3DS ZL, also unused) morphs and unmorphs at once, as in Dread, by tapping
  the HUD's minimap, which the game treats as the Morph Ball button. It works
  only on the gameplay HUD.
- **Control reminders** (`DrawControlHints` in `overlay.cpp`): beside the pause
  menu's pages and the file select, a column lists their buttons and points to
  touch mode for what the game only offers by touch (scan, map pins, info and
  settings on the map page; copy and delete for an opened file). In touch mode
  it lists touch mode's own controls. Measured in the game: on the map page
  only R, B and touch do anything (the D-pad, X, Y, ZL, ZR and the C-stick do
  nothing); on the Samus page the circle pad picks an item and A pages its
  description. The HUD tab's **Menu help text** option turns the column off,
  touch mode's included.

**Widescreen** follows DekUI's renderer approach. Depth-tested draws in the top
viewport (the 3D world) are squeezed by 15/16 on the display's horizontal axis
before clipping (`clip_scale_y` in the generated vertex shaders). That reveals
more of the level, and presentation stretches it back to 16:9. 2D layers
(fades, the game's full-screen composites) keep their full width.

## Build (Windows, MSYS2 UCRT64)

Packages: `mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,python,qt6-base,qt6-multimedia,qt6-tools,SDL2,ccache}`.

```sh
git submodule update --init --recursive   # first time only
bash tools/msr/windows/build.sh build-jit   # the JIT build, as released
```

No dump is needed to build. The playable, self-contained folder is
`build-jit/bundle/`, with `SamusReturns.exe`, `SamusReturns-Direct.exe` and
every DLL they need.

The recompiled build needs your dump:

```sh
MSR_PYTHON=<python with capstone> bash tools/msr/windows/build_recomp.sh <dump.cci>
```

This recompiles the game's code from your dump, then configures, builds and
bundles into `build-windows/bundle/`. Later, `bash tools/msr/windows/build.sh`
rebuilds the same folder (still recompiled) after source changes. It embeds code
translated from the game: keep it for your own use. Only the JIT build can be
shared.

### Releases (1.0)

`src/msr/version.h` names the project (`msr2ds`) and its version, shown in the
launcher and logged at startup. The public release is the JIT build without
textures; a personal one can carry the recompiled build and the HD texture pack,
for your own machines only.

```sh
# Public: JIT, no textures -> build-release/public/
python tools/msr/package_release.py public       # packages build-jit/bundle
wsl -d Ubuntu -- env PUBLIC=1 bash tools/msr/linux/build_in_wsl.sh   # or on Linux:
bash tools/msr/linux/build_appimage.sh - - build-release/public build-linux-jit

# Personal: recompiled, HD texture pack built in -> build-release/
python tools/msr/package_release.py textures <folder with 00040000001BB200>
python tools/msr/package_release.py windows      # -> build-release/<name>-<version>-windows-x64.zip
bash tools/msr/linux/build_appimage.sh <generated> build-release/textures build-release   # on Linux
```

The packages leave out `user/` (saves, settings, the remembered dump), any game
dump and the emulator's scripting sample, and add a `README.txt` and the
license. `public` refuses a recompiled build. `textures` stages a Citra-style
pack as `build-release/textures/`: "HD Texture Pack", "HD Texture Pack
Addons/<add-on>" and the pack's readme (credits). The game lists packs found in
a `textures` folder beside the executable (or
`usr/share/samus-returns-recomp/textures` in the AppImage) as included with
the game; a pack of the same name in `user/mods/textures/` takes its place.
`windows` packages `build-windows/bundle` with those textures. The Linux script
(`-` for no generated code, `-` for no textures) downloads Qt 6.10.3 with
aqtinstall (Azahar only bundles Qt on Windows and macOS), uses Azahar's bundled
SDL2 and LibreSSL, reuses any generated code (it is plain C++), fetches Qt's
missing xcb libraries with `apt-get download`, and packs an AppImage with
linuxdeploy: nothing is installed on the build system. Built on Ubuntu 24.04 (in
WSL), it needs glibc 2.38 or newer: Ubuntu 24.04, Fedora 39, current
SteamOS and Arch. It runs on X11 (XWayland on Wayland desktops). The AppImage
keeps its user data in `~/.local/share/samus-returns-recomp`, or in a `user`
folder beside the AppImage file if there is one. Under WSL, drop the `/mnt/`
entries from `PATH` first, or CMake finds MSYS2's Windows Qt.

- `CITRA_WARNINGS_AS_ERRORS=OFF`, because MSYS2's GCC 16 is newer than
  Azahar's CI compiler.
- `ENABLE_WEB_SERVICE=OFF` exposed an upstream link bug: `citra_core` never
  linked `crypt32`. Fixed in `src/core/CMakeLists.txt`.
- The exe icon is generated by `tools/msr/make_icon.py` into `dist/msr/`.

## Running

`SamusReturns.exe` opens the launcher (`src/citra_qt/msr_launcher.cpp`) in the
game window. It shows the game dump and the same Game, Display, HUD, Controls
and Mods settings as the in-game overlay, saved to the same files. Play boots the
game in that window; if the game stops with an error, the launcher comes back.
The mouse, the keyboard (arrows, Space) and a gamepad all work: D-pad or stick
up and down move, left and right change a value, A activates, LB/RB switch
tabs, and Start plays.

Menus use the game's own button layout, Nintendo's: A (choose) is the right
face button, Xbox B, and B (back) the bottom one, Xbox A. That holds for the
launcher, the overlay and touch navigation as it does in the game.

**Skipping the launcher:** `SamusReturns-Direct.exe`, beside `SamusReturns.exe`,
boots straight into the game with the saved settings (`src/msr/direct/`). It
runs `SamusReturns.exe --play`, passing on its own arguments, and waits for the
game: Steam and other front ends see it running as long as the game does, and
ending it ends the game. The launcher still opens when no dump is found, or if
the game does not boot. Settings stay reachable in game (F1 or Select). On
Linux, or in a Steam shortcut's launch options, `--play` does the same.

The launcher looks for the dump:

1. a `.3ds`, `.cci` or `.cxi` Samus Returns dump beside the exe,
2. the same inside `game/`,
3. the path remembered in `user/msr/game-path.txt`.

If none is found, Browse (or dropping the file on the window) picks it, and the
location is remembered. Only decrypted Samus Returns dumps are accepted
(product code `CTR-P-A9A*`); encrypted dumps are refused with an explanation,
since the emulator cannot boot them. Tested with the USA release, title
`00040000001BB200`.

Everything lives in `user/` beside the exe:

- saves: `user/sdmc`
- emulator config: `user/config/qt-config.ini`
- player settings: `user/msr/settings.ini`
- texture packs: `user/mods/textures/`
- shader cache: `user/shaders`
- log: `user/log/samus_returns_log.txt`

Keep the folder path short: Windows' 260-character path limit applies to the
save files deep under `user/sdmc`.

## Controls

**Gamepad:** mapped automatically on hotplug, by position (Xbox X/A/B/Y act as
3DS Y/B/A/X), so Shoot is the left face button and Jump the bottom one. A
profile you edit is never overwritten. Gamepad bindings read every connected
pad (Azahar's "all controllers" map type), as single-player PC games do: under
Steam Input each controller reaches the game as one of Steam's virtual pads,
with a different GUID and slot (a Steam Controller has no other way in), and
bindings tied to one pad's GUID went dead. Profiles saved before are converted
once at startup; the log lists each pad SDL opens and the pads Steam hides.

**Keyboard:** circle pad = arrows, A/B/X/Y = `A`/`S`/`Z`/`X`, L/R = `Q`/`W`,
D-pad = `T`/`F`/`G`/`H`, Start = `M`, Select = `N`.

| Input | Action |
| --- | --- |
| **F1** / **Select** | Settings overlay (the game keeps running; its input is held). The game treats Select as Start, so it never receives Select |
| Start | Game's pause menu and map, shown as the bottom-screen panel |
| Right trigger (ZR) / `2` | Next beam: usual, Ice, Grapple (once Samus has them) |
| Left trigger (ZL) / `1` | Morph Ball in and out, as in Dread (once Samus has it) |
| Tab / L3 in menus | Touch navigation: D-pad or stick moves, A taps (hold to drag), B leaves. The pause map's tools (scan, map pins, info, settings) and file copy and delete are touch-only in the game. In play, L3 and R3 do nothing, so a stray stick click opens nothing |
| F11 / Esc | Fullscreen / leave fullscreen |
| Mouse | Touch, on the bottom-screen panel |

In the overlay: D-pad or stick to move, A to choose, B or Esc to go back, LB/RB
to change tabs, Select or F1 to close. Tabs (the launcher has the same ones):

- **Game:** volume, the amiibo extras (below), quick save and load, restart,
  quit. The launcher's Game tab also imports saves (below).
- **Display:** fullscreen, widescreen, internal resolution (9× is 2160p, 4K),
  frame rate, V-Sync, FPS counter, menu size.
- **HUD:** size, opacity, map size and opacity, counters, Aeion abilities, menu
  panel size, title menu background (the card behind the file select and the
  other title menus), menu help text (the control reminders).
- **Controls:** every 3DS control and its binding. Select one and press the new
  button, key or stick (Esc cancels). Gamepad layout maps everything to the
  connected pad by position; Keyboard layout restores the defaults.
- **Mods:** texture packs (below).
- **Cheats:** infinite energy, unlimited missiles, unlimited Aeion (below).

Default sizes: HUD 0.60, map 1.15, menu panel 1.00, menu 1.34.

## Saves

The game keeps its three files (A, B, C) in one save archive:
`profile0`-`profile2` folders, each with `samus.bmssv`, `common.bmssv` and
`pkprfl.bmssv`. Here it lives in `user/sdmc/Nintendo 3DS/<zeros>/<zeros>/title/
00040000/001bb200/data/00000001/`, beside Azahar's `00000001.metadata`.

**Import saves...** in the launcher's Game tab brings saves from a 3DS or
another emulator (`src/msr/saves.cpp`). Pick any folder that holds them: a
Checkpoint or JKSM backup (or the game's folder of backups), or the folder of
Citra, Lime3DS or another Azahar build (its user folder, `sdmc`, or anything
down to `data/00000001`). It searches up to 12 folders deep for one holding
`profileN` folders with `.bmssv` files, preferring this game's title folder,
then the newest; a folder inside a save (`profile0`, highlighted when the
dialog closed) finds the save around it. After a confirmation that shows what it found, the import
replaces all three files: the current saves are copied to
`user/msr/save-backups/<date-time>/` first, and the new ones are copied beside
them before taking their place, so a failed import changes nothing. The
emulator's format file comes along when there is one; otherwise the one here
stays, or the game's own is written. **Open saves folder** shows where they
are. Importing is only offered before the game starts, since a running game
would write its own saves over the import. Raw cartridge save dumps (`.sav`
from GodMode9) are encrypted images, not these files; use Checkpoint or JKSM to
back saves up from a 3DS.

## amiibo extras

The extras the game gives through amiibo, without them. Each is a Game-tab
switch, applied immediately by rewriting a few instructions in memory
(`src/msr/cheats.h`); nothing is written to the saves.

- **Unlock amiibo galleries** (on by default): Options > Gallery on any save
  lists Metroid II Art, Concept Art and Music (the sound test) beside Chozo
  Memories.
- **Unlock Fusion Mode** (on by default): a save's New Game asks for a difficulty
  even before the game has been beaten: Normal, Hard and Fusion. The game shows
  this list only once the save has beaten the game, so Hard comes with it. An
  empty file slot's Play still starts on Normal, as in the original.
- **Fusion suit, any difficulty** (off by default): Samus wears the Fusion suit
  (models and resource packages) at Normal and Hard, which keep their damage. It
  switches at once, also in the middle of play or in the pause menu.

The game itself picks Samus's models (`actors/characters/samus` or
`samusfusion`) and suit packages (`packs/players/*.pkg` or `*_fusion.pkg`) only
while an area loads, and keeps one family loaded; even its developers' toggle
(`ToggleForceFusionMode` in its Lua) waits for a reload. So the switch in play
(`src/msr/suit_swap.h`) does what a load would, on the game thread: a small ARM
stub (`suit_swap_stub.s`, assembled into `suit_swap_stub.h` by
`tools/msr/suit_swap/assemble.py`) is written into the zeros after the game's
code, and the frame limiter's first instruction branches to it while it has a
command. One command per frame: load the other family's packages (as a save
does, 0x143EB8), then empty each of Samus's model components (Samus and the
morph ball, found on the heap by their vtable) as their destructor does,
register the models again and replace the model on screen, then release the
first family. It takes about half a second. The emulator reads which family
the models really are from their file names, so a save state made in the other
suit switches back to the setting too.

The aim laser, the muzzle flash, the charge glow and other effects hang from
Samus's bones by index, resolved once when she is created, and the Fusion
skeleton orders its bones differently (the cannon's are 19 and 22 in one, 23
and 26 in the other). So before a swap the emulator notes each model hanging
from her bones and the bone's name, and afterwards hangs it from the bone of
the same name in the new skeleton. Without that the laser started at her feet
and the flash at her hip.

## Cheats

The Cheats tab (overlay and launcher), all off by default, applied each frame
while on (`ApplyMsrRefills` in `src/video_core/gpu.cpp`, addresses in
`src/msr/cheats.h`); nothing is written to the saves beyond what the game
itself saves.

- **Infinite energy:** Samus's life component (`CPlayerLifeComponent`) is
  filled to its maximum and locked with the game's own "current life locked"
  flag, so hits take nothing.
- **Unlimited missiles:** missiles, Super Missiles and Power Bombs are set to
  their maximums in Samus's inventory, where the HUD reads them.
- **Unlimited Aeion:** the same as energy, on `CSpecialEnergyComponent` and its
  "special energy locked" flag: abilities work and the gauge stays full.

Energy and Aeion are found on the heap by their vtables (once per area, about
6 ms). The game keeps a copy of each in the inventory for the HUD and updates
it only through the component's setter, so the value is filled by calling that
setter on the game thread, through the suit swap stub's call command, and only
then locked. Turning a cheat off releases the lock it set. The amiibo reserve
tanks were not used, since they refill only once.

## Frame rate

Samus Returns draws 30 frames per second on the 3DS. Its main loop sleeps until
33 ms have passed since the previous frame, then waits for the screen's refresh;
everything in the game advances by the time measured between frames. So 60, 120
and 144 FPS only change three things, and the game keeps its speed:

- the loop's 33 ms limit (two instructions at `0x156484`), rewritten in memory
  to 16, 8 or 6 ms;
- the emulated screen refreshes at the frame rate above 60;
- the emulated CPU clock rises with the frame rate (250% at 60, 500% at 120,
  600% at 144), so each frame finishes in time.

Parts of the interface, the HUD on the bottom screen among them, do not follow
measured time: they advance a constant 1/30 s per frame, which at 144 FPS made
their animations (the Aeion ability icon's pulse, for one) 4.8 times too fast.
Above 30 FPS three instructions are rewritten so these updates take the measured
frame time instead (`StepPatches`); at 30 FPS that is the same 1/30 s. Measured on
the bottom screen: the Aeion icon pulses once a second at 30 and at 144 FPS.

`src/msr/frame_rate.h` has the details. 60 FPS is the default. The setting
applies immediately, in game too. The FPS counter shows the game's own frames.
Above your monitor's refresh rate, frames are replaced instead of queued (Vulkan
mailbox), so the game never slows down to wait for the display. Tested at all
four rates: walking for a set time covers the same distance at each, and
recompiled code stays at 100%.

## Texture packs

Each folder in `user/mods/textures/` is a pack: textures named the way Azahar
dumps them (`tex1_WxH_HASH_FMT.png`, `.dds` or `.ktx`, in any subfolders), with
an optional `pack.json`, so existing Azahar and Citra packs work as they are. The
Mods tab (launcher and overlay) lists the packs: enable or disable them, and move
them up or down. Where two packs replace the same texture, the one listed first
wins. A folder that holds only folders, like a pack's "Optional Addons", lists each
add-on as its own pack. New packs join the top of the list, smallest first, so
add-ons override the pack they extend. Packs load when the game starts; after
changes in game, the overlay offers Restart now. Packs in the `textures` folder
that ships with a release (the HD texture pack) are listed the same way, marked
as included with the game.

To make a pack, turn on **Dump game textures** and play. Textures go to
`user/dump/textures/00040000001BB200/` with a `pack.json`. Copy the ones you edit,
and that `pack.json`, into a new folder under `user/mods/textures/`. Edited
textures can be larger than the originals. **Preload textures** loads every pack
texture at boot: no stutter when new ones appear, at the cost of memory and a
longer start.

## What changed from upstream Azahar

Left out, as this port never builds them: the Android app (`src/android`), the
libretro core (`src/citra_libretro` and the `libretro-common` submodule),
`libadrenotools` (Android), the installer script, Apple bundle files, and
Azahar's project tooling (`.github`, `.ci`, `docker`, `hooks`, `Doxyfile`,
`AI-POLICY.md`). They stay in the git history and in upstream Azahar.

| Area | Change |
| --- | --- |
| `tools/msr/` | Build and packaging scripts; icon generator. |
| `src/msr/` | Game module: ROM discovery and validation, defaults, controller auto-config (from DekUI), presentation state, bottom-screen classifier, ImGui settings overlay, input bridge, frame rate, texture packs (`mods.cpp`), save import (`saves.cpp`), developer draw probe; `direct/`, the launcher-skipping `SamusReturns-Direct.exe` (Windows). |
| `externals/msr-imgui/` | Dear ImGui 1.91.9b, unmodified (as in DekUI). |
| `src/video_core/renderer_vulkan/` | Single-screen compositor, bottom-screen readback, overlay hook, widescreen squeeze of top-screen 3D draws. |
| `src/video_core/shader/generator/`, `host_shaders/vulkan_present_msr.frag` | `clip_scale_y` vertex uniform; compositor shader (luma key, feathering). |
| `src/core/frontend/` | `MsrSingleScreenLayout`; touches only reach the game while the panel is visible, mapped to where the renderer drew it. |
| `src/core/hle/service/hid/hid.cpp` | Game input held while the overlay is open; Select held back (it opens the overlay); injected Start and touches. |
| `src/video_core/gpu.cpp`, `src/core/core.cpp`, `renderer_vulkan/vk_swapchain.cpp` | Frame rate: the game's frame limit, the emulated refresh rate, the CPU clock, mailbox presentation above the monitor's rate. The amiibo extras' instruction patches (`msr/cheats.h`) are applied at the same point, the Fusion suit is switched in play from there (`msr/suit_swap.h`), and the cheats keep energy, missiles and Aeion full. |
| `src/video_core/custom_textures/` | Texture packs from several folders in priority order, `pack.json` options per pack; the dump's `pack.json` is written on the first dump too; misses logged at debug level. |
| `src/video_core/rasterizer_cache/rasterizer_cache.h` | Fixed a crash with preloaded textures: a custom texture upload could replace and move the surface `ValidateSurface` was holding. |
| `src/core/arm/msr_aot/` | The recompiled CPU backend. |
| `src/citra_qt/`, `src/citra_meta/` | Game-only window: launcher (`msr_launcher.cpp`) in place of the game list, no menus or emulator dialogs; shared control bindings (`msr_controls.cpp`); game icon and title; own exe icon and version info; dialogs renamed. |
| `src/common/` | Log file named `samus_returns_log.txt` in this build. |
| `src/input_common/` | `GetConnectedGameControllers()`. |

Without `-DENABLE_MSR_GAME_FOLDER=ON` the tree builds plain Azahar. Everything
above is inert unless the frontend enables it.

### Developer tools

Run with `MSR_DEV=1`:

- Creating `user/msr/drawlog.trigger` logs the next frames' draws (target,
  viewport, shader hashes, textures, uniforms) to `user/msr/drawlog-N.txt`.
- Creating `user/msr/bottom.trigger` dumps the next bottom-screen readback as
  `bottom-N-s<state>.ppm`. Use these to recalibrate the classifier or the HUD
  crop rectangles.
- Writing a number into `user/msr/bottomseq.trigger` saves that many consecutive
  bottom-screen frames as `seq-*-t<emulated ms>.ppm`, to compare animation
  speeds between frame rates.
- Creating `user/msr/coverage.trigger` writes `user/msr/coverage.bin`: every
  instruction word the CPU JIT has translated, i.e. the code the game really
  ran. `tools/msr/recomp/` uses it.

`tools/msr/dev/` holds the tools used to test and reverse-engineer the port:

- `ui_check.ps1` boots the test copy (`build-windows/test`, with its own
  `user/` and the keyboard profile) on the freshly built exe and walks key
  presses, waits, screenshots and bottom-screen dumps; `drive.ps1` has the
  helpers (it never touches the playable bundle, and keeps the test copy muted).
- `bottom_classify.py` runs the bottom-screen classifier over dumped frames.
- `disasm.py` disassembles the game's code, lists callers of a function and
  instructions that form an address, from the recompiler's extracted `code.bin`.
- `grid.py` makes contact sheets of screenshots.

### Recompiled build

Built from your dump, the game can run from its ARM code recompiled ahead of time
into C++, instead of on the emulated CPU. Everything else stays Azahar's: the 3DS
system services, the GPU and audio. `tools/msr/windows/build_recomp.sh <dump>`
builds it into `build-windows/bundle/`. It boots and plays, with 100% of executed
instructions recompiled in testing, the emulator's exact timing, and the same
speed as the JIT. `MSR_AOT=0` in the environment runs the JIT instead.

`tools/msr/recomp/README.md` covers how it works, the verification against an
ARM11 reference, and next steps. `FEASIBILITY.md` beside it is the study that
preceded it. The recompiled build embeds code translated from your dump: keep it
for your own use.

## Known limits and next steps

- The HUD pieces are crops of the game's own bottom screen at fixed rectangles,
  checked at the start of the game and on a completed save (all tanks, ammo,
  Aeion abilities and beams). Adjust the `piece(...)` rectangles in
  `DrawMsrScreens` if anything is cut off.
- Bottom-screen dialogs that keep the HUD framing (if any) will not pop up on
  their own; Tab shows them.
- The single-screen presentation is Vulkan only (the build forces Vulkan).
- Frame rates above 30 were tested in the first area: walking, jumping, shooting,
  menus, a room transition and save states. Anything in the game that counts
  frames instead of time would run faster at higher rates; nothing like that has
  shown up yet. If something looks too fast, compare it at 30 FPS.
- Widescreen reveals world at the sides that the game may not have meant to
  show. Watch for pop-in at the screen edges.
- The gallery layout was checked on Metroid II Art and Music. Concept Art and
  the Chozo Memories grid have the same bottom screen and are recognized from
  their dumps; the screens a memory opens were not checked.
- Fusion suit: the pause menu's Samus page still shows the Power Suit picture.
  The switch in play was tested standing, walking, crouching, in the pause menu,
  back and forth five times, at 60 and 144 FPS, and across quick saves and
  loads; the morph ball's model is replaced the same way, but the test input did
  not get Samus into the ball to look at it. Switching during an elevator or
  teleporter ride waits until play resumes; that was not tested either.
- A full relocation of the UI in the game's own code (Lua GUI scripts in
  `gui/scripts/*.lc`, via LayeredFS) remains possible, but it's a deeper
  reverse-engineering project.
