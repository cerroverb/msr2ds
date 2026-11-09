# msr-native: Samus Returns without an emulator

`msr-native` runs Metroid: Samus Returns the way
[Woohoo! Flight School](https://github.com/thefixinhixon/woohoo-flight-school) runs Pilotwings Resort: the game's
ARM code is native code recompiled from the player's own dump, and everything around it (the 3DS kernel, the system
services, the PICA200 GPU, the DSP) is a small hand-written runtime instead of an emulator. Azahar is not part of it.

```
your dump ──> tools/msr/recomp/build_module.py ──> user/msr/recomp/msr_aot.dll   (the same library the launcher's
                                                                                  "Native code" Build makes)
msr-native.exe ── loads the dump's .code, ExHeader and RomFS straight from the dump file (a compressed .zcci too)
               ── loads msr_aot.dll and checks it (module version, sizeof(Ctx), SHA-256 of aot_runtime.h)
               ── runtime/dispatch.cpp: one host thread per guest thread, function to function through the module
               ── runtime/kernel.c, services.c, fs.c, gsp.c, dsp.c, pica*.c, video.c, input.c (from Woohoo! Flight School)
```

## Status (2026-11-02)

**Works:** boot, the game's own start-up (kernel objects, threads, timers, srv/APT/fs/cfg/hid/gsp/dsp/ir/nfc),
loading from the RomFS inside the dump, the title screen, the file select with the player's saves, and gameplay
(walking, jumping, room transitions, the pause map) at 30, 60, 90, 120 or 144 FPS (`--fps`, else the settings'). Audio runs (no underruns in
tests). Tested on Windows with an RTX 5090 at 1x-10x internal resolution (4K = 9x), and on a 100% save.

**Samus Returns' own features** (`runtime/msr/`, C++ sharing `src/msr`'s headers with the emulator-based build):
- The player's settings: `user/msr/settings.ini`, as the launcher writes it (`--settings`; by default
  `user/msr/settings.ini`, or `../msr/settings.ini` from `user/native`). Its frame rate applies unless `--fps` says.
- The single 16:9 screen (`--layout=single`, F9 cycles to it): `compositor.cpp` ports the emulator build's compositor
  (renderer_vulkan.cpp DrawMsrScreens): the bottom screen is classified each frame (`bottom_classifier.h`) and its HUD
  pieces (energy, beam slots, the minimap following Samus's marker: by its deep red, so a red map pin is not taken
  for it, with the map's thin orange lines, the elevator shafts, left out (#40, #44), and taken far from its last place
  only once it holds there 0.75 s; the counters), menus (a panel, the title menus without
  their backdrop), prompts and the gallery layout are drawn over the view by `msr.vert`/`msr.frag`. `pica_vk.c`
  squeezes the top screen's world draws to 16:9 and applies the camera zoom in clip space, and leaves out the title
  menus' backdrop quads (out of play only: the map pins tool draws the same ones once a pin is placed, #43). The
  mouse touches the panel while it shows. While R is held (missiles, Power Bombs) the HUD
  keeps the beams in its beam slots (2026-11-03): the game draws the missiles in them then (the touch screen's choice
  of missile), so `video.c` draws the slots from its copy of the bottom screen's slot column, made each frame R is not
  held, from a press while the HUD shows until 350 ms after R comes up (the beams flash back in), and their
  classification waits; the missile counter lights as the game lights it. In Morph Ball the game greys the beam row,
  but the beam can still be changed: the lit slot is then the one with by far the most of its pixels over 120
  (`bottom_classifier.h` ReadBeamSlots; before, the HUD kept its highlight on the old slot, over a dimmed icon, and
  RT tapped the same slot again). A picture on the bottom screen with a cyan edge, such as the ship's cockpit after a
  file's Continue, is not taken for a menu (it had shown as the panel, its outline over the view for about a second):
  the pause menu's pages stand over its bottom bar, a rule in the last rows, which the picture lacks.
- Fill 16:10 screens (2026-11-06, issue 38; on by default, `settings.ini` `fill_16_10`, never on without Widescreen,
  as Ultrawide): in play the view also takes a window's aspect when it is taller than 16:9, down to 16:10 (the Steam
  Deck, most laptops, a Mac in fullscreen below its notch), keeping 16:9's width and showing more above and below:
  `presentation.h` ViewScaleV squeezes the world up the view (0.9 at 16:10, `msr_world_scale`'s clip x), and the
  camera's gain takes the larger of the two squeezes (1/0.9). The mouse's aim reads the same scale. Off, 16:9 with bars
  above and below. Where a room's camera limits hold the 3DS's frame at its edge, the extra rows (5.5% at the top and
  the bottom) may show past the room.
- Ultrawide (2026-11-03; the Display tab's Ultrawide, on by default, `settings.ini` `ultrawide`): in play the view
  takes the window's aspect when it is wider than 16:9, up to 32:9, everything still on the one screen
  (`compositor.cpp` ViewAspectWanted; wider still, bars at the sides). It widens Widescreen (16:9), so it is never on
  without it (2026-11-04): turning it on turns Widescreen on, Widescreen off turns it off, and a settings file with
  Ultrawide but not Widescreen reads as neither (`settings.cpp`). The world's squeeze becomes 5:3 over the view's
  aspect (`P::view_aspect`, `presentation.h` ViewScale: 15/16 at 16:9, 15/32 at 32:9), and the camera's field of view
  widens to match (`camera_fov.cpp`: a gain of about 2.1 at 32:9, 3.05 with the smallest zoom), so the game shows and
  culls for the whole width. The HUD keeps to the view's corners, or to its middle 16:9's (the HUD tab's "HUD within
  16:9", `hud_within_16_9`); menus stay centred. The title menus, the gallery and the prologue stay 16:9, as the game
  draws their scenes for its own view and their art (the title's logo) as camera-facing quads without depth test,
  which would stretch; the view eases between the two (~0.4 s). The picture is rendered 400 x 240 times the internal
  resolution and stretched across, so a wide view is softer across than up: a higher internal resolution, or the FSR
  filter, sharpens it. `--window_aspect=21:9` (or `32:9`, `2.37`) opens the single layout's window at that shape.
- Input: RT taps the next beam slot, LT the minimap (Morph Ball), as injected touches while the HUD shows (C and
  Left Shift too, and the mouse wheel: down the next beam, up the one before); the pad's Back/Select opens the settings
  panel (`select_layout`: `settings`, or true for the next layout, false for 3DS Select). Every pad SDL can map is
  open; the one used last is read.
- The buttons as the player's device names them (`msr/glyphs.cpp`, `src/msr/button_glyphs.h`), from the last input
  (a pad's button or a stick pushed past half way, a key, a mouse button) and SDL's controller type: an Xbox pad (and
  Steam's virtual pads, and pads SDL cannot name) shows the game's letter icons as Xbox labels those buttons, a
  PlayStation pad its symbols (drawn into the icons as the game cuts its letters), a Nintendo pad the game's own, the
  keyboard the letter or digit of the key bound to each (the space bar's sign for Space, the game's icon where the
  key's name is longer). The icons are redrawn in the game's three icon textures as `pica_vk.c` uploads them (the font
  sheet, the interface atlas and the top screen's atlas with the dialogs' A and B; a texture pack's too; uploaded again
  when the device changes; `PWR_MSR_TRACE=1` lists the hashes of textures of a sheet's size that are not it). The control reminders beside the menus, the Controls tab's
  reference and the settings' footer name the same buttons (the keys by name, the circle pad's and touch navigation's
  from the bindings). `MSR_BUTTON_ICONS=xbox|playstation|nintendo|keyboard` forces one.
- Choosing and going back (2026-11-03): an Xbox or PlayStation pad chooses with its bottom face button (A, Cross:
  3DS A) and goes back with its right one (B, Circle: 3DS B) in the game's menus, its dialogs over play ("Save your
  progress?"), touch navigation and the settings, while in play the bottom button stays B (jump) and the right one A,
  as bound (`input.c` input_pad_bound, the menus' layout). The layout applies unless the HUD shows: from the
  classifier's Menu at once, from anything else once held 300 ms (a dialog over play shows the emblem below, as
  cutscenes and loading do; `compositor.cpp` msr_buttons_state). A button keeps the 3DS button it pressed until it
  comes up, so the button that chose Continue does not jump. The reminders follow, and the icons: the bottom screen's
  and text's from a Menu or a Prompt held 300 ms until the HUD shows (the menus' check mark shows A or Cross there; a
  dialog over play keeps play's, as its text names play's buttons), the dialogs' "A check mark" and "B cross" always.
  A Nintendo pad keeps the game's layout, which its labels already match; SDL is asked for every pad's buttons by
  position (SDL 2 gave a Nintendo pad's by label, so its A came as the bottom button). `PWR_MSR_TRACE=1` logs the
  layouts changing. The Controls tab's "Nintendo menu layout" (off by default; `settings.ini` `[native_controls]`
  `menu_confirm_right`; msr2ds issue 1) turns the menus' layout off for every pad: menus, dialogs, touch navigation and
  the settings then choose with the button bound to the game's A and go back with the one bound to its B (by default
  the right and bottom ones), and the icons and reminders name those (`glyphs.cpp` ChooseButton, BackButton).
- The Aeion abilities on the right stick (2026-11-03; the Controls tab's checkbox, in the launcher and the game's menu,
  `settings.ini` `[native_controls]` `right_stick_dpad`): the right stick pushed past half way is the 3DS D-pad in the
  one direction it is pushed furthest (let go under a third), and the pad's D-pad moves the circle pad as the left
  stick does, whatever its buttons are bound to (`input.c` input_pad_rstick; a raw joystick's hat too). Touch
  navigation still steps with the D-pad. The Controls rows and reference name the right stick and the D-pad then.
- Mouse aiming (`video.c`, `input.c`): in play the left button shoots (3DS Y; on the bottom screen it still touches)
  and the right one held is Free Aim (3DS L) with the circle pad all the way toward the pointer from Samus: her
  entity's position (Samus's handle at the game +0xFC, then +0xFC..+0x104, about her arm cannon's height) seen by the
  game's camera (`msr_samus_view`), then the zoom and the 16:9 squeeze, then the top screen's place in the window. The
  pointer is a crosshair over the top screen in play and hides while a pad is in use until the mouse moves.
- Touch navigation (`src/msr/touch_nav.cpp`, compiled in as it is): T, or the pad's L3 while a menu shows, puts
  a highlight on the touch screen's buttons (a free cursor where none are known); the D-pad, the circle pad's keys,
  arrows or left stick move it, the pad's choosing button (above), A's key or Return touches (hold to drag), its back
  button, B's key or Escape leaves.
  The panel shows meanwhile and the game gets no other input. The classifier's page is published only once it has
  held for 3 frames and 80 ms, like its state: menus sliding in read as no page for a few frames.
- The settings menu (F1 or Select) is the emulator build's own: `src/msr/overlay.cpp` (Dear ImGui) compiled in as it is,
  drawn last in the presentation pass with dynamic rendering (`msr/overlay_host.cpp`), so its tabs and look are the same.
  The player's options are its `PlayerSettings` in the same `settings.ini`; display and sound changes apply at once
  and go to `settings.ini` `[native]`, which the launcher takes over when msr-native exits. The resolution goes up to
  4x. Restart exits with code 75 and the launcher starts it again. msr-native's own rows: Bloom (the game's glow,
  0-100%, default 75%: `pica_vk.c bloom_scale` scales the combiner constant of the pass that adds the blurred,
  downscaled scene) and HDR (below). The Controls tab rebinds a key and a pad button per 3DS button (`input.c`, `settings.ini`
  `[native_controls]`); Select's binding opens the menu, ZL and ZR are the triggers' taps.
- Save states (`state.c`, `state.h`): the menu's Quick save and Quick load, F5 and F8. The guest's memory (the
  reservation's committed pages, without the game's code) and every part that keeps state, in sections, zlib-compressed
  on a worker thread into `<user>/states/native-quick.state` (~13 MiB); a state of other game files or game mods is
  refused. Saving stops the game's threads where their registers are their whole state (`kernel.c k_freeze`: at a
  scheduling point, or blocked in a wait or sleep; never inside a nested call) and lets the GX worker finish. Loading
  reads the file first, then every guest thread leaves what it was doing (`k_abandon_all`) and parks in `thread_body`;
  memory, objects, threads, services, GSP, PICA, DSP and open files are put back, a wait is issued again, threads the
  state lacks end and missing ones start. Render targets are made again from guest memory.
- HDR (`video.c`, `shaders/hdr.frag`, `win/hdr_win.c`): with the player's HDR on and the display in HDR mode (DXGI's
  colour space; its peak and Windows' SDR white from DXGI and the display configuration), an HDR10 (or scRGB)
  swapchain. The game is drawn as before; hdr.frag expands its highlights towards the peak (default at most 1000 nits)
  and lays the interface (HUD pieces, menus, the settings: their own layer) over it at paper white. Under the
  interface's cards and dims (from a quarter of coverage) the picture is not expanded, so they darken it as in SDR
  (2026-11-03: behind the file select the title scene's highlights, expanded first, stayed brighter than the menu).
  Behind the title menus (the file select) the view darkens by 25-60% as the Title menu background setting says
  (20% before), the pause menu's by 45%.
- Hard Mode from the start (2026-11-03, the Game tab, on by default): the game offers Hard only in a beaten save's New
  Game. With it on, every New Game opens the difficulty list (Normal, Hard, Fusion) and so does a new file's Play
  (`cheats.h`: its handler branches to the New Game's), which the game otherwise starts on Normal. The list was made
  for a used slot, so `msr/patches.cpp` adapts it to an empty one with three call hooks on the file select (the
  dispatcher's `rt_add_call_hook`): the change to the list leaves from the used slot's "Start a new game?" state (the
  new-file panel's way out fades everything out for the game's start), the list's Back goes to the files rather than
  a used slot's Continue / New Game / Options, the questions say "Start a new game?" rather than "Current data will be
  erased", and Yes starts the game as a new file's Play does. The difficulty is the slot summary's +0x7C, as the
  game's own Yes handlers write it. The patched word is newer than the players' native code, which does not check it:
  `dispatch.cpp` `rt_code_patchable` turns off the two recompiled functions holding it, so the interpreter runs them
  (quietly: not listed as code the native code lacks), and no rebuild is needed. `PWR_CALL_TRACE=<hex>[,<hex>...]`
  logs each call of those ARM functions (r0-r3, lr), as used to follow the file select's states (0x4E40A0).
- The amiibo extras (galleries, Fusion Mode, the Fusion suit) and the cheats (energy, missiles, Aeion kept full):
  `cheats.h`'s patches follow the settings each VBlank; `cheats.cpp` refills as the emulator build's
  ApplyMsrRefills, calling the components' setters on the game thread from a trap at the frame limiter.
- The Fusion suit switched in play (`cheats.cpp`, the emulator build's ApplyMsrSuitSwap): the same ARM stub
  (`src/msr/suit_swap.h`) at the same place loads the other suit family, rebuilds Samus's model components and
  releases the first family, one command per frame on the game thread, called from the frame limiter's trap with
  its last instruction turned into a return; the fallback interpreter runs it (silently: `rt_quiet_range`), and
  the effects hanging from her bones are moved to the bones of the same names.
- Texture packs: `textures.cpp` reads the packs the launcher passes (`--texture-pack <folder>`, the Textures tab's
  enabled ones, highest priority first) in Azahar's format and matches textures as the emulator build does: by default
  the legacy hash, CityHash64 (`src/common/cityhash.cpp`) of the texture decoded exactly as Azahar's rasterizer cache
  decodes it (bottom-up rows, colour formats as stored, the others as RGBA8, Azahar's ETC1); `use_new_hash` in a
  pack.json hashes the guest bytes. PNGs (libpng; without it packs are left out) load on worker threads, flipped as
  `flip_png_files` says, and `pica_vk.c` gives each replacement mipmaps down past the game's own levels by its scale.
  The workers also work out the legacy hash (from a copy of the texture; the game's texture shows meanwhile) and
  prepare each replacement's upload: its staging buffer and image (`pica_vk.c` stage_make), so the GX thread only
  records the copy and the mipmaps. `PWR_TEXPACK_SYNC=1` does both on the GX thread, as before (A/B).
- Game mods (`msr/mods.cpp`, `msr/layered_fs.cpp`): the launcher's Mods list (`<user>/mods/game`, `settings.ini`
  `[game_mods]`, read and written as QSettings does by `msr/ini.cpp`) and the Textures list. The game parses its
  RomFS image itself, so with mods the image is rebuilt as the emulator's LayeredFS does: each mod's `romfs/` replaces
  or adds files, `romfs_ext/` removes (`.stub`) or patches (`.ips`, `.bps`) them, the first mod listed winning. A
  mod's `exheader.bin` and `code.bin` replace the dump's, and its code patches apply (BPS, then IPS from the lowest
  priority up). The recompiled functions covering a word a mod changed are turned off and run on the fallback
  interpreter (`dispatch.cpp` `rt_code_changed`, from the function ranges `msr_aot_region_spans` the module exports
  since 2026-11-02, one entry per run of a function's code: an older module asks for a rebuild; `PWR_CODE_MOD_TRACE=1`
  lists the functions each changed word turns off); code a mod adds runs there too, its threads included. A Randovania
  seed is such a mod (41 changed words, 14 functions interpreted, 60 FPS); its loader's OpenProcess and
  ControlProcessMemory succeed; with one the Fusion suit setting stays off, as in the emulator build.
- The Mods and Textures tabs follow their folders (2026-11-04, `overlay_host.cpp` ModsScanner): while one of them
  shows, in the launcher or the game, a thread of its own scans the folders again about once a second, so a pack or
  mod copied in with the menu open appears with what it holds (a pack's texture count grows while it is copied), and
  in the game the restart that loads it is offered. The lists used to be read only when the menu started, when the
  game ended and after a change in the menu: after a first install a pack copied in stayed unlisted, though the game
  loaded it. A scan that began before a change in the menu is dropped, and `ini.cpp` reads or replaces a file under
  one lock (on Windows a file open in one thread cannot be replaced from another).
- Save import (2026-11-04, the launcher's Game tab, `launcher.cpp` the saves section; the emulator-based build's
  `src/msr/saves.cpp`, which went with it): Import saves... asks for a folder (the shell's folder picker; zenity or
  kdialog on Linux) and looks under it, 12 folders deep and 20000 folders at most on a worker thread, for one holding
  `profileN` folders with `.bmssv` files: a Checkpoint or JKSM backup, an emulator's `.../001bb200/data/00000001`, an
  installation's `native-save/savedata`. The game's title folder comes first, then saves in use before a launcher's
  `save-backups`, then the newest; the saves here are never offered. Once confirmed, the folder is copied beside the
  saves, the current ones are copied to `user/msr/save-backups/<date-time>/savedata`, and the copy takes their place
  (a failure leaves them as they were). Azahar's `00000001.metadata` is not needed. Open saves folder shows
  `user/native-save/savedata`.
- Code the module lacks (reached only through pointers computed at run time): `interp_full.c` interprets it (ARMv6K
  ARM and Thumb, VFPv2) until the module's code is reached, where the emulator build uses its JIT. Each such address
  is logged once and added to `user/msr/recomp-jit-entries-core-native.txt`, which the launcher's next build of the
  native code takes.
- Command lists are copied as the game queues them: the runtime's hook (from Woohoo! Flight School) on Samus Returns'
  GX enqueue (0x1AE170), called by the dispatcher on entry. Run late from guest memory at a higher internal resolution,
  the little set-up lists the game builds in a scratch buffer had been rewritten: the bottom screen drew into the wrong
  target or not at all, and the file select flashed black or shifted in about one frame of four.
- Shaders preloaded (`pica_vk.c`, the emulator build's shader seed): `pwr_shaders.bin` keeps every generated vertex
  shader (key and SPIR-V) and pipeline key the game has used, and at start-up they are all built again on four threads
  (with `pwr_pipelines.bin`, the driver's cache, about 15 ms), so an effect seen in an earlier session no longer waits
  for its shaders. A file from another build of the shader generators is dropped and written again;
  `--preload-shaders=off` (the launcher's "Preload shaders") leaves it alone.

**The internal resolution** (1x-10x; 9x = 2160p/4K) reaches the screen: the game renders its scene into a 240x400
target and TextureCopies it into a 256x512 texture its final composite samples. `pica_vk_texture_copy` makes that copy
from the target's image at the internal resolution (an alias), which `tex_get` uses while the texture's bytes are
still that copy's. Until 2026-11-02 the whole picture came out at 1x whatever `--resolution` said.
The bottom screen too (2026-11-02, the compositor's minimap, icons, counters and pause map; they were blurry and
texture packs looked squeezed): its DisplayTransfer copies the first 320 of 400 lines, and the scan-out copy now takes
those from the 240x320 target the game drew its bottom screen into. Its guest bytes are still made for the compositor's
page classification.

**The renderer never waits for the GPU in a normal frame** (2026-11-02): batches are submitted without waiting, six
batch sets (command buffers, rings, fence) taking turns (a frame is about 10 batches). A target's download reaches
guest memory only when something reads those bytes (a GX command over them, a texture there, a save state), when its
set comes round again, or at the VBlank tick if its batch is done. The TextureCopy above and the bloom/light-shaft
chain's 2x2 downscales of it (`pica_vk_transfer_gpu`) run between aliases on the GPU; their guest bytes are made from
the image (`alias_writeback`) only if something reads them, which nothing does in a usual frame. The scan-out
DisplayTransfer copies the target's image into the image video.c shows, skipping the CPU copy. All of it at every
internal resolution, 1x included. `PWR_VK_NO_GPUCOPY=1` goes back to the CPU copies (A/B), `PWR_VK_SYNC=1` waits for
every batch.

**OpenGL** (2026-11-08, msr2ds issue 34; Windows, and Linux untried: CMake's `MSR_OPENGL`): for graphics without Vulkan
(Intel's before the 6th generation on Windows, issue 24) or without its 1.3. `--renderer=auto|vulkan|opengl` (the
Display tab's Graphics API, `settings.ini` `[native]` `renderer`, which the launcher passes on and uses for its own
window): auto takes Vulkan where its device has 1.3, else OpenGL, else Vulkan without 1.3 (its CPU presenter).
- The presenter (`video_gl.c`, `gl_load.c`): an OpenGL 3.3 core context (the newest core profile the driver has) on the
  same window, laid out and drawn as video.c's Vulkan path, with the same shaders: `present`, `msr`, `fsr_easu` and
  `fsr_rcas` keep their Vulkan-only lines under `#ifdef VULKAN` (glslc defines it; their SPIR-V is unchanged, byte for
  byte), and CMake embeds their text (`cmake/glsl2inc.cmake`, which puts FidelityFX's headers in place of the includes).
  FSR needs OpenGL 4.0, else sharp bilinear. The settings menu is Dear ImGui's OpenGL 3 backend (`overlay.cpp`
  InitOpenGL, DrawOpenGL). No HDR. The game's frames are presented without waiting for the display, as main.c paces
  them to its vblanks: a swap that waited held up the renderer's context (on NVIDIA every write-back waited for the next
  vblank: 7 FPS on the title screen instead of 27); the launcher waits (V-Sync).
- The PICA renderer (`pica_gl.c`): pica_vk.c's design on OpenGL 3.3, its public functions forwarding there. Its own
  context, sharing the presenter's, current on the GX worker (gsp.c runs no GX commands on the game's threads with
  OpenGL). `pica.vert`, `pica_vs.vert` and `pica.frag` are the Vulkan renderer's (as above; the LUTs, the vertex shader
  program and the attributes are texture buffers, storage buffers being OpenGL 4.3; the depth range is Vulkan's through
  glClipControl, else `CLIP_Z`). Draws run as they come.
  - Programs: a vertex shader (pica_vsgen.c's translation of the game's, which it also writes as GLSL 3.30,
    `pica_vsgen_glsl`: the raw vertex bytes through `RAW()`, a texture buffer from word `d.perm.w` on; or pica.vert for
    CPU-shaded vertices) with pica.frag specialised for the draw (pica_vk.c's 30 specialisation constants, written as
    GLSL constants; `PWR_VK_NOSPEC=1` keeps to the generic one, which reads them from the uniforms). They are compiled
    and linked on a thread of their own with a third shared context, a fence saying when one is ready; until then a draw
    takes its translation with the generic fragment shader, else the interpreter's or pica.vert's program (made at the
    start). On NVIDIA a link takes 300-900 ms (the driver compiles there), so an effect seen for the first time shows
    the generic program for a moment. (The generator's source changed, so Vulkan players' saved translations,
    `pwr_shaders.bin`, are built again once, in the background.)
  - `pwr_gl_programs.bin` (beside `pwr_shaders.bin`): the translations and every program made, with its binary where
    the driver gives one (glGetProgramBinary: OpenGL 4.1 or ARB_get_program_binary), saved by the compile thread when
    idle (at most every 5 s) and at exit. At the start (the launcher's Preload shaders; not with `PWR_NO_PRELOAD=1`)
    every program in it is made again from its binary before it is wanted: 4 ms each on NVIDIA, 74-82 programs in
    well under a second on the second run, against 300-900 ms each the first time. Stamped with the generators' and
    shaders' sources and the driver (vendor, renderer, version): after an update of either they are made from their
    shaders again. 11 MB after a few minutes of play on NVIDIA.
  - A draw's data (its uniforms, attributes or CPU-shaded vertices, indices) goes into one buffer: mapped for good with
    OpenGL 4.4 (ARB_buffer_storage), a ring of four 16 MB parts with a fence each, else mapped per draw without the
    driver's synchronisation and replaced when full (`PWR_GL_NO_PERSISTENT=1` takes that way). Textures, samplers,
    vertex arrays and framebuffers (kept per texture pair, for copies and read-backs too) are bound only when they
    change.
  - Each command list's GPU time is a timer query, for main.c's load line ("GPU drawing"). OpenGL's elapsed time
    counts the gaps where the GPU waited for the driver, so it reads higher than Vulkan's timestamps.
  - The GX copies on the GPU (the aliases: the scene's TextureCopy, the bloom chain's halvings) and the scan-out of each
    screen at the internal resolution, which the presenter shows after a fence (and the renderer's next copy waits for
    the presenter's fence after its use, all under `scan_lock`); the bottom screen is also read back at 1x for the
    compositor.
  - Write-backs without waiting: each colour target drawn in a list, and each alias, is read back at 1x into a pixel
    buffer of its own with a fence after it, and lands in guest memory only when something reads its bytes. Landing is a
    fence wait and a buffer map, so the main thread does it too, in the presenter's context, where the VBlank and the
    save states ask (the GX worker idle then); a loaded state drops every read-back pending.
  On an RTX 5090 in play at 3x, the machine otherwise idle (2026-11-08): 59-60 of 60 FPS, GX commands 4.1-4.5 ms a
  frame (Vulkan's 4.1, OpenGL's 5 before the programs were specialised and the draw data put in one buffer), at 1x 57-60;
  quick save and load work; the pictures match Vulkan's. With another program keeping the GPU busy OpenGL falls behind
  where Vulkan stays at 60: 40-50 FPS with it at 60%, 18-20 at 85%. There each read-back's glReadPixels into its pixel
  buffer waits (0.2 ms a call idle, 1-2 ms shared), whatever the read format (BGRA too), the timer queries or the way
  the draw data goes up; the version before, built again for comparison under the same load, made 30 FPS where this
  one made 45-50. The first version (CPU vertex shading, waiting write-backs, CPU copies) made 23 FPS, the screens at 1x.
  Every translation of a session (62 structured, and 32 in the dispatcher form with `PWR_VSGEN_FLAT=1`), the
  specialised fragment shader and every other shader check as GLSL 3.30 core with glslangValidator. `PWR_GL_CPU_VS=1`
  shades on the CPU, `PWR_VK_NO_GPUCOPY=1` copies on the CPU (25 and 54 FPS with the version before). Not yet: a Linux
  build, older hardware.
  Comparing screenshots with Vulkan's: with HDR on (the test profile's `hdr=true`) a capture of Vulkan's HDR10 window
  looks washed out and glowing; OpenGL has no HDR, so compare with `hdr=false`.
- `PWR_NO_VULKAN=1` acts as if there were no Vulkan, `=13` as if its device had no 1.3 (the loader ignores
  `VK_DRIVER_FILES` in an elevated process); `PWR_GL_DEBUG=1` a debug context with the driver's messages in the log (it
  slows the game); `PWR_GL_PROF=1` where the renderer's time goes, every 2 s; `PWR_GL_DUMPVS=1` each translation to
  `vs_<key>.vert`; `PWR_VSGEN_TRACE=1` each program as it is made or loaded, with its time.

**Known issues:**
- 30 FPS paced by the display (2026-11-06): the game's 33 ms limit runs a little short of two 60 Hz VBlanks, so with a
  VBlank on every tick it ran at 30.3 FPS and caught up with a 50 ms frame every 50 frames (1.65 s), a stutter. While it
  keeps up, main() now gives the game a VBlank on every other tick (30 Hz; presentation, the menu and the HUD stay at
  60): 30.0 FPS, no frame over 34 ms in play. More than one late frame in half a second goes back to every tick, which
  let an overloaded game run faster (19.8 FPS against 17.7 at 30 Hz on one processor thread), and a second at 30 returns
  to 30 Hz (one 50 ms frame then). `PWR_VBLANK60=1`: every tick always.
- 120 and 144 FPS: the frame limit becomes 8 or 6 ms and main() runs VBlank at that rate, presenting through a
  mailbox swapchain; a VBlank held back while the GX worker lags is delivered as soon as it catches up. The game sends
  ~30 GX commands a frame one after the other, so a frame is their work plus the hand-offs between its threads and the
  worker (2026-11-02: a blocked guest thread is woken alone, a higher-priority one preempts at the next loop head,
  small commands run on the calling thread, sleeps are exact to the microsecond). Measured on an RTX 5090 in gameplay
  with the GPU never waited for (above): 144 holds 142.5-143 at 4x and at 9x (4K) once a room is loaded, 60 holds
  59.5 at 9x, 1x about 131 (it waited for the GPU before: about 94). A busy PC (a video playing) costs 10-15 FPS at
  144. `PWR_GX_STATS=1` prints the worker's time per command type, the hand-off latencies, token waits, where a
  list's time goes, the GPU's busy time per frame (timestamps) and its waits by cause; `PWR_EVTRACE=<s>` records
  256 Ki events (svcs, token, GX) to evtrace.txt.
- No stutter (2026-11-02, the user: "SERIOUSLY bad jitter and stutter" when firing): vertex shaders compile on two
  background threads, unoptimized (11-22 ms instead of 250-360 ms on the GX thread), draws using the interpreter shader
  until theirs and its pipeline are ready; `pwr_shaders.bin` is stamped with a hash of `pica_vsgen.c` (CMake), so a
  rebuild no longer throws it away; the log (stderr) is buffered and written with the 2 s stats (unbuffered, those
  writes cost ~19 ms on the VBlank thread every 2 s); the shader/pipeline cache saves and the infinite energy/Aeion heap
  scan run on threads of their own. The 2 s line shows `frame-max` and the frames over 10/20/50 ms; `PWR_HITCH=<ms>`
  logs each longer frame with the GX worker's and the main loop's share. At 9x/144 while firing: 144.0 FPS, longest
  frame 7.1 ms; from an empty cache no frame over 20 ms. `PWR_LOG_UNBUFFERED=1`, `PWR_VSGEN_SYNC=1`,
  `PWR_VSGEN_OPT=<0-2>` go back to the old behaviour for comparison.
- 90, 120 and 144 FPS hold steady (2026-11-02): the GX worker no longer waits for the GPU in a frame. The bottom
  screen's pixels for the compositor come from a read-back of its scan-out copy (no CPU copy waiting for the target),
  twelve batch sets, and VBlank follows the display's own vblanks (DWM) when the frame rate is its refresh. At 9x
  while firing: 144.0 FPS, longest frame 7.1 ms; 120.0, 8.5 ms.
- Camera zoom without objects popping in at the edges (msr/camera_fov.cpp): the scene's render camera is given a field
  of view wide enough for the zoomed (and 16:9) view (a call hook on its set-up, 0x173A70 called by the camera's
  manager, widens the field of view it is handed), and the renderer scales the world draws back, so the picture is the
  same and the game culls for all of it. The game's camera (vtable 0x6D1FD4) keeps its own field of view, and for its
  update (0x33715C, which keeps the view inside the room's camera limits from the render camera's last field of view)
  the render camera gets the game's values back: the camera stays where it is at zoom 1 (2026-11-03, the user: when
  it moved between a large room's areas at a zoom under 1 the focus drifted; the first version widened the game
  camera's own field of view, and the limits moved it).
- The GX queue is never full for the game (gsp.c's enqueue hook waits for the worker): with two threads queueing, a
  failed enqueue let the other thread's command in first and the file select's bottom screen jittered at 144 FPS.
- Not here: texture dumping, the randomizer's remote connection (Randovania's tracker: no `soc:U`).
- Linux: the public AppImage ships it (`tools/package/appimage.sh`, when Vulkan's headers and glslc or
  glslangValidator are there). Tested on a Steam Deck OLED (RADV, 2026-11-03), at the landing site with the 4x texture
  pack, `PWR_GX_STATS=1` and `--fps=144` for the most it does:
  - The GPU's vertex work had been most of its frame (`PWR_VK_CPU_VS=1` took it off: 11 ms of GPU a frame at 2x became
    3.8). The dispatcher form of the translated shaders compiled to 20-46k instructions with all 256 VGPRs, 100 of them
    spilled: 55 FPS at 4x, 74 at 2x, and 20-38 in play at 4x. The structured form (`pica_vsgen.c`
    generate_structured: 40 VGPRs, ~2.5k instructions, no spills) gives 77-80 at 4x, 98-104 at 3x, 95-111 at 2x, and 60
    held at 4x in play; the picture is the dispatcher's (0.01-0.04% of pixels differ, by the animation's timing, one
    second after a quick load). `PWR_VSGEN_FLAT=1` keeps the dispatcher (A/B). The interpreter shader (`pica_vs.vert`,
    a new program's draws until its translation is built) is as heavy, for those moments only.
  - The game never exited: once its window closed (or Restart, or SIGTERM) `exit()`'s static destructors waited in
    glibc's `pthread_cond_destroy` for the texture packs' PNG workers, which never leave their condition variable. On
    Linux main() now ends with `_exit` after the shutdown.
  - "Match window" (`--resolution=0`, which the launcher had turned into 2x as the option refused 0) follows the top
    screen as shown, the nearest 240-line multiple: 3x on the Deck's 1280x800 (it was 4x by the window's height).
  - Hitches as new textures came in (walking into new rooms with the 4x pack): the GX thread decoded each new texture
    per texel through `pica_texel` (39 ms for a 1024x1024 ETC1 texture and its mipmaps), worked out its legacy hash for
    the pack (20 ms) and copied the replacement into a new staging buffer and image (up to 17 ms + 1-5 ms). Now
    `pica_decode_level` decodes by tiles and ETC1 blocks, shared with the worker pool (ETC1 1024x1024: ~3 ms; the same
    texels as `pica_texel`, checked on every format), and the pack's work is the workers' (texture packs, above). A
    walk through new rooms at 3x: 68 frames over 20 ms before, 2 now (60 FPS but for those).
  - The settings' Display tab on a Steam Deck (its firmware's names, `overlay_host.cpp` SteamDeck) starts with a preset:
    3x, 60 FPS and V-Sync, which held 60 there with the 4x pack (~100 FPS without a limit); it suggests the Deck's
    refresh rate at 60 Hz, as 60 FPS on the OLED's 90 Hz shows frames for one refresh or two in turn.

**From the launcher:** once the native code is built, the Game tab's "Without the emulator (experimental)" makes Play
start `msr-native` (beside the exe) with `--layout=single --scale=3 --resolution=<the launcher's> --filter=fsr`, plus
`--fullscreen=borderless`, `--fps=60`, `--preload-shaders=off`, `--vsync=off` and `--volume=<0-100>` as set there; the
game mods and texture packs it finds itself (`--texture-pack <folder>` instead, for tests). It runs in
`user/native/` (log: `msr-native.log`) with saves in `user/native-save/`, a copy of the emulator's on the first start.

- The Android app (2026-11-04, README.md Android): the program as `libmain.so` in SDL's activity. AArch64 builds take aot_runtime.h's SSE intrinsics from `src/core/arm/msr_aot/a64/` (MXCSR on FPCR: rounding, flush-to-zero; fsqrt, frinti/fcvtzs), which is part of their runtime hash; the native code is built on a PC (`build_module.py --target android-arm64`, Zig, no imports, 16 KB pages) and copied to the app's private folder before `dlopen` (`android.c`). Vulkan 1.3 functions come from `vkGetDeviceProcAddr` there (`vk13_load.c`, as on Windows, where `vulkan-1.dll` may be older than 1.3), push descriptors from descriptor sets where the device has none (`vk_push.c`, the emulator's gfxstream), the swapchain is identity-transformed at the window's size (SUBOPTIMAL is expected), and the window's surface is made again after the background (`recreate_surface`). Back is F1 in play, Escape in the launcher.

## Build and run

Windows (MSYS2 UCRT64: gcc, cmake, ninja, SDL2, zstd, Vulkan headers and loader, shaderc):

```sh
cmake -S native -B build -G Ninja && cmake --build build -j 8
cd <a folder with user/msr/recomp/msr_aot.dll and user/msr/game-path.txt>   # run/ (../run.ps1 does this), test/
PATH=/c/msys64/ucrt64/bin:$PATH <repo>/build/msr-native.exe [--dump <dump.cci>] [--mute]
```

Keyboard, mouse and gamepad (`runtime/input.c`, rebindable in the menu's Controls tab; a PC layout for the mouse since
2026-11-03): WASD = circle pad (arrows too), Space = B (jump), F = A (Aeion, confirm; Enter too), R = X (melee
counter), V = Y (shoot), Q/E = L/R, Left Shift = ZL (Morph Ball), C = ZR (the next beam), Tab = Start, Backspace =
Select (the menu), 1/2/3/4 = D-pad up/left/right/down (the Aeion abilities), T = touch navigation; in play the mouse's
left button shoots, the right one held aims at the pointer and the wheel changes beams; the pad by position in play
(bottom = B, jump; right = A), and in menus and dialogs an Xbox or PlayStation pad's bottom button chooses and its
right one goes back. F1 opens the menu, F5 quick saves, F8 quick loads, F9 switches the screen layout, F11 fullscreen.

**What a slow computer is short of** (2026-11-06), in every `msr-native.log`, so the logs players send answer it: at boot
`video: Vulkan device ...` gives the graphics' kind (discrete, integrated), memory and driver, and `native: running at
...` the settings that decide the load (internal resolution, frame rate, V-Sync, upscaling, anisotropic filtering,
power-saving waits, the wider view) and the processor, its threads and the memory. Every 2 s, `native: load: ...` gives,
per game frame: the game's thread at work (its frame less its time waiting or asleep, kernel.c `g_k_frame_idle_ns`; the
thread that runs the frame limiter), the GX commands' time (worker and inline, gsp.c `g_gx_busy_ns`), the GPU's drawing
(the 3D batches' timestamps, now always on; not the upscaling and HUD after them), and how many processor threads the
program kept busy. Under 95% of the frame rate asked for, it names the likeliest shortage: the graphics (GPU drawing at
80% of the frame or more), one processor thread (GX commands at 80%), the game's own work (80%), or every thread busy.
Measured overloaded (one processor thread, `PWR_VK_CPU_VS=1`): 21 of 30 FPS, GX commands 45-49 ms a frame, "one processor
thread (GX commands)".

Debugging switches (environment, as in Woohoo! Flight School): `PWR_SKIP=<first>-<last>` (drop those draws of every
frame, counted from each top-screen swap: bisects an artifact to its draws; with `PWR_SKIP_DUMP=<frame>` it prints their
state in that frame instead), `PWR_GX_COPYLOG=1` (each distinct TextureCopy once), `PWR_GXDUMP=<frame>` (VRAM after
every GX command of that game frame, `gxdump_<n>_<cmd>.bin`: decode the tiled surfaces to see where an artifact appears), `PWR_TRACE=1` (kernel calls and IPC),
`PWR_HANG_AT=<s>` (every thread and what it waits on), `PWR_RENDERER=sw` (the software rasterizer),
`PWR_DRAWDBG=<target>,<shader>,<after s>,<count>` (draw state), `PWR_PEEK=<addr>,<words>` (guest memory), `PWR_CALL_TRACE=<hex>[,<hex>...]` (each call of
those ARM functions: r0-r3, lr).
`MSR_NATIVE_ENTRIES_OUT=<file>` collects code the module lacks, for `build_module.py --extra`. `PWR_TEX_TRACE=1` lists
each texture sampled into each target once, with the targets it lies in (how render-to-texture flows);
`PWR_TEXPACK_TRACE=1` lists each texture a pack replaces, each one of 128 px or more the packs are asked about (the file
name that would replace it, found or not, and the alias it was found as) and each replacement dropped because the game
rewrote its texture; `PWR_AIM_TRACE=1` logs why mouse aiming cannot place Samus, each time the reason changes. `PWR_SEQ=<presented frame>,<count>[,<every>]` writes both
screens of those frames (or of every `<every>`th) as PPM files (seq_*): one-frame glitches such as the file select's
flashes show up there.

## Adapting Woohoo! Flight School's runtime

- `cpu.h` mirrors `MsrAot::Ctx` field for field (`dispatch.cpp` checks every offset at compile time), so the C runtime
  reads guest registers directly. Memory is Woohoo's 4 GiB reservation; every page of `Ctx::pages` points into it.
- Guest threads run `rt_run` (the dispatcher) instead of a recompiled C function: system calls come back as
  `Exit::Svc`, time slices end at loop heads (`kSlice`, about 1 ms) and become scheduling points (`k_slice_end`).
- Samus Returns needed beyond Pilotwings: `svcQueryMemory` (a region map), timers (one host thread), thread priority,
  `CreateMemoryBlock`, `GetSystemInfo`, `GetProcessInfo` (type 20 is the linear heap's physical offset: without it the
  game hands the GPU virtual addresses), `OutputDebugString`; services `APT:A`, `ir:USER`, `nfc:u` and aliases.
- The GPU's vertex and index buffers are offsets from the attribute base in physical address space. Samus Returns sets
  that base to VRAM and keeps its buffers in FCRAM, so the sum is converted to a virtual address, not the base
  (`pica_sw.c` `vtx_va`).
- The RomFS is read in place from the dump (`fs.c`), not extracted.
- Compressed dumps (2026-11-03, msr2ds issue 22): Azahar's Z3DS (`.zcci`, `.zcxi`: a 0x20-byte header, metadata, then
  zstd's seekable format, 256 KiB frames each compressed on its own and their sizes in a seek table at the end) are read
  in place too: `dump.c` serves the loader (`main.c`), the RomFS (`fs.c`, through the one handle `g_dump`) and the game
  mods' rebuilt RomFS (`layered_fs.cpp`), decompressing the frames a read covers (whole ones straight into the game's
  buffer, partial ones through an 8 MiB cache) with zstd's decoder: the system's libzstd, or built from
  `MSR_ZSTD_SOURCE` (the emulator-based build's `externals/zstd`; the Linux release). The recompiler, which reads the
  dump itself in Python, is given the image's start decompressed (`launcher.cpp` WriteImageStart: up to the ExeFS's
  end, a few MB). A 1 GiB dump is 527 MB as a `.zcci`; reading it all takes about 0.8 s of CPU.
- The TEV combiner buffer lags one stage, as the hardware (Citra's model): Samus Returns' post-processing composite
  adds the scene (stage 0) and the bloom (stage 1) through it.
- DisplayTransfer bit 5 (no swizzle conversion, tiled to tiled) for the bloom and light-shaft downscales, and with
  scaling the output size register is the size before it (halved for the image written, as Citra reads it): taken as
  the real size, the downscaled textures had twice their tile-row stride and the landing site showed bright bars.
- Depth targets that a GX copy reads are written back to guest memory after each list (D16/D24/D24S8).
- `interp.c` runs the single instruction the module hands back (patched words: the frame limit, the interface's
  time steps): data processing, B/BL, VMOV.F32. `--fps=60` writes the same patches as the main build.

## Licence

The files in this folder that come from Woohoo! Flight School keep its licence, GPL-3.0-or-later (`LICENSE`); the
rest of `native/` is offered under the same terms. The files under `src/` and `tools/` come from the Azahar-based
Samus Returns port and stay GPL-2.0-or-later (`../license.txt`); Dear ImGui (`externals/msr-imgui`) is MIT. Third-party parts: AMD FidelityFX FSR 1 (MIT, `runtime/shaders/fsr/`), the overlay font from DejaVu
Sans Mono (Bitstream Vera licence, `runtime/font_ui.h`). No game code or data is in this folder.
