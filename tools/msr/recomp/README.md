# Recompiled build

The game's ARM code is translated ahead of time into C++ and compiled into the player. A new
CPU backend runs that native code instead of emulating the CPU. Everything else is still
Azahar's: the 3DS operating system services (HLE), the PICA GPU on Vulkan, audio, and this
fork's single-screen frontend.

The feasibility study that led here is in [FEASIBILITY.md](FEASIBILITY.md).

## Status (2026-10-27)

- **Boots and plays.** These all work:
  - title, file select and the first area, over runs of up to five and a half minutes;
  - the pause menu pages and the settings overlay;
  - quick save and load (save states are shared with the JIT build);
  - restart and quit.
- **Coverage:** 100.000% of executed instructions are recompiled code, with nothing on the
  JIT. Over two minutes of play that was 3.5 billion instructions on the main core and 277
  million on the second.
- **Timing:** each straight-line run of code charges the emulator's own ARM11 cycle costs
  (`ticks.cpp` runs `arm_tick_counts.cpp`), so emulated time advances exactly as on the JIT.
- **Speed:** level with the JIT, uncapped:

  | Resolution | Recompiled | JIT |
  | --- | --- | --- |
  | Native | ~971 FPS | ~984 FPS |
  | 4K | ~548 FPS | ~554 FPS |

  The game's CPU work is not what limits these numbers.
- **Translation:** 1.37 million instructions in 22,697 functions. 23 are left to the
  interpreter: privileged or ARMv7-only encodings, most of them data decoded as code.

## How it works

```
code.bin (from your dump)
  -> ctr.py         decrypted dump -> code.bin, ExHeader layout
  -> analyze.py     functions: entry point, calls, vtables and pointers, the C++ init array,
                    switch tables, ARM/Thumb veneers, extra_entries.txt
  -> generate.py    one C++ function per region (code reachable from an entry without
                    crossing calls), via emit_arm.py (ARM, VFPv2, CP15) and emit_thumb.py
  -> aot_shard_*.cpp + aot_registry.cpp   (MSR_RECOMP_DIR, compiled at -O1;
                    registers, flags and ticks in locals; helpers always inlined)
  -> src/core/arm/msr_aot/   ARM_AOT backend + aot_runtime.h
```

**Regions.** Calls, returns, indirect branches and system calls leave a region and return to
the dispatcher. The guest call stack therefore stays in guest memory, and no host frames sit
between guest functions, so a system call can switch guest threads at any time. A region
resumes at:
- its entry;
- the instruction after each call or system call;
- loop heads, where it yields when the time slice ends;
- switch targets.

**Backend** (`ARM_AOT`):
- It wraps Azahar's dynarmic JIT, which keeps the canonical register state. Kernel calls,
  context switches and save states therefore work unchanged, and save states are
  interchangeable with the JIT build.
- `Run()` copies the state into the runtime context. It then dispatches from function to
  function, with an O(1) table lookup per PC, until a system call, a reschedule or the end of
  the time slice.
- Code without a recompiled entry point runs on the JIT one instruction at a time until it
  reaches one.
- Instructions listed in `patchable.txt` are rewritten in memory while the game runs (the
  frame limit, `src/msr/frame_rate.h`). Their recompiled code first compares the word in
  memory with the original, and when it differs hands that one instruction to the JIT, which
  runs the new word. These hand-offs are not reported as missing code.
- Memory goes through Azahar's page table, with the memory system as the slow path.
- Floating point runs on SSE, with MXCSR set from the guest FPSCR (the 3DS runs games in
  flush-to-zero, default-NaN mode). The NaN canonicalization is applied after each operation.
  On an AArch64 host (macOS on Apple Silicon) the guest's RMode, FZ and DN bits go into FPCR
  unchanged.
- The recompiled code is used only when the loaded program's text matches the SHA-256 it was
  generated from. Any other program runs on the JIT.

## Verification

`verify.py` compiles a sample of generated functions into a harness. It runs each one from
random states (registers, flags, VFP registers, memory full of pointers, small integers and
floats) and checks it against Unicorn emulating the 3DS's ARM11 MPCore. Everything must
match: exit address, registers, flags, VFP state, memory changes and faults.

| Run | Functions | Cases | Instructions executed | Match |
| --- | --- | --- | --- | --- |
| seed 2 | 1,500 | 11,971 | 0.66 M | 11,970 |
| seed 3 | 6,000 | 35,902 | 2.58 M | 35,893 |
| every function, seed 11 | 22,696 | 67,880 | 5.57 M | 67,858 (99.97%) |
| every function, final generator (register locals, cycle costs), seed 12 | 22,697 | 67,878 | 4.74 M | 67,854 (99.96%) |

Every difference traces to the verifier or to undefined behaviour, not to the translation:
- In lock loops (`ldrex`/`strex`), the test memory's exclusive monitor behaves differently
  from Unicorn's. The real backend uses Azahar's exclusive store.
- A Thumb switch helper reads its case table at LR. The reference maps the page of the fake
  return address used by the test, and the harness does not.
- Unicorn's ARM11 model rejects `ldrexd`.
- Unicorn executes a Thumb BL pair as two halves.
- One "function" is a literal pool decoded as code that uses PC in a register-shifted
  operation, which is undefined on the hardware.

A planted bug in the overflow flag was caught in about 5% of cases, so the comparison has
teeth.

## Building

```sh
python -m pip install capstone          # once, for the analysis
MSR_PYTHON=python bash tools/msr/windows/build_recomp.sh "<your dump>.cci"
```

The result is `build-windows/bundle/SamusReturns.exe`, the main build (a second argument picks
another build folder). The script folds in any
`build-windows/bundle/user/msr/recomp-jit-entries-core*.txt` files. The recompiled build writes
these when code is reached only through pointers computed at run time, so rebuilding after
playing further brings that code in too.

**Run-time switches:**
- `MSR_AOT=0`: run on the JIT.
- `MSR_TEST=1`: automated tests. Keyboard layout, no controller auto-config, Play pressed
  automatically.

The log reports the recompiled share every 30 emulated seconds:
`Core 0: 100.000% of instructions recompiled (...)`.

The generated code and the recompiled build folder contain code translated from the game. Keep them, and
any package made from them, for your own use. The source tree has no game code; the generator
reads your dump at build time.

## Next steps

- Play further into the game. Add entry points from `recomp-jit-entries-*.txt` (automatic on
  rebuild), and run `verify.py` on any new regions.
- Performance:
  - direct host calls for direct guest calls;
  - keep guest registers in host locals within a region;
  - a faster Thumb lookup;
  - `-O2` for hot shards.
- Native replacements for hot or interesting functions: the HUD, the camera.
  The dispatcher could take hand-written C++ registered for an address ahead of the generated
  function. (Frame pacing turned out to need only two patched instructions: `patchable.txt`.)
