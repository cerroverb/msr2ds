# Static recompilation feasibility (Samus Returns, USA)

> Superseded by the working recompiled build: see [README.md](README.md). This is the study
> that led to it.

Question: can Samus Returns' ARM code be recompiled ahead of time into native C++
(in the style of TriAevum, `../TriAevum`), replacing the emulated CPU while keeping
this fork's single-screen frontend?

**Verdict: feasible.** Static analysis finds 99.995% of the code the game ran during
play, and TriAevum's translator handles it. Translated functions matched an ARM11
reference in 15,987 of 15,996 randomized runs; the 9 misses trace to one fidelity bug
in TriAevum's float compare. The remaining risk is integration with Azahar's kernel
(threads, system calls, timing), which this check does not cover.

## The code

From the decrypted dump (`ctr.py`): internal name MATADORA, title `00040000001BB200`.

| Segment | Address | Size |
| --- | --- | --- |
| text | `0x00100000` | 6,047,816 bytes |
| rodata | `0x006C5000` | 238,056 bytes (59 pages) |
| data | `0x00700000` | 459,128 bytes, plus 384,528 bss |

All code is in `code.bin`. The RomFS has no CRO/CRS modules, and no code ran outside
the text segment.

## Static analysis (`analyze.py`)

The analyzer does recursive descent with Capstone from these seeds:

- the entry point and direct calls;
- code pointers in rodata, data and literal pools, which cover the C++ vtables;
- ARMCC's init array of 1,204 static constructors, stored as entry-relative offsets at
  `0x6FDF18`;
- ARM switch tables (`cmp` + `ldrls/ldrlo pc, [pc, rN, lsl #2]` or `addls pc, pc, ...`);
- ARM↔Thumb veneers and mode switches (`add ip, pc, #1; bx ip`, `ldr ip, =f; bx ip`).

Each candidate is validated as ARM11 (ARMv6K) code: no ARMv7 or Thumb-2 encodings, real
3DS SVC numbers only, coprocessor access only to p15.

| Measure | Result |
| --- | --- |
| Instructions found | 1,373,641 (21,670 Thumb, 1.6%) |
| Functions | 22,688 |
| Text explained | 5.45 MB code + 0.26 MB literal pools and tables, of 6.05 MB; 0.33 MB unexplained, with 178 prologues inside |
| Switch tables | 141 resolved (1,655 cases); 1 unresolved |
| Direct calls / indirect calls / indirect jumps | 115,620 / 11,326 / 577 |
| Vtable-like runs | 1,874 runs, 22,843 slots |
| System calls | 164 sites, 29 distinct SVCs (all standard Horizon calls) |
| VFP / exclusive / CP15 | 217,358 / 114 / 72 instructions |

The indirect calls are C++ virtual calls. A recompiled build resolves them through a
table of the 22,688 entry points.

## Runtime check

`src/msr/code_coverage.h` (`MSR_DEV=1`, trigger `user/msr/coverage.trigger`) records
every instruction word the JIT translates. Coverage came from one scripted session:
boot, title, file select, the first area (movement, beam, missiles, melee, Aeion, crouch
and morph attempts), the pause menu pages and the map pins tool, and the settings
overlay.

- Executed: 535,646 instruction words, about 39% of the statically found code.
- Found statically: **99.995%** of the executed words.
- Missed: 28 words in 4 small Thumb fragments in library code. Nothing calls them
  directly; they are entered through runtime-computed pointers. A recompiled build needs
  a fallback for addresses it doesn't know: an interpreter, or log and rebuild.

Later areas will run more code. Repeat the session at a few points in the game before
trusting the function list completely.

## Translation sample (`sample_translate.py`)

**Translator coverage, whole program.** TriAevum's lifting rules accept 99.999% of the
1,351,971 ARM instructions; 15 are left (rare `mrs`, `msr`, `qsub8`, and a few likely
mis-decodes). Its narrower direct ("true AOT") C++ emitter handles 83.5%, plus 109,523
direct calls that need its whole-program mode. The rest of what it lacks is VFP
arithmetic, `vpush`/`vpop`/`vldm`/`vstm`, `ldrd`/`strd`, register-controlled shifts,
`blx rN` and PC operands. Neither emitter handles Thumb.

**Differential test.** The emitter translates 5,167 of the 5,986 leaf functions (no
calls, no system calls) whole. I sampled 1,000 of them (3,856 instructions; 127 use VFP),
compiled the C++ with TriAevum's runtime, and ran each 16 times on random registers, VFP
registers, flags and memory. Unicorn emulating the ARM11 MPCore was the reference, with
the 3DS's FPSCR (flush-to-zero, default NaN).

| Outcome | Runs |
| --- | --- |
| Identical return: registers, flags, VFP, memory writes | 11,973 |
| Identical memory fault: same instruction, same address | 4,014 |
| Mismatch | 9, in 4 functions |
| Reference did not finish (random inputs looped) | 4, skipped |

Of the matching runs, 8,950 changed registers, 977 wrote memory, 730 changed VFP
registers and 667 changed flags. All 9 mismatches are `vcmpe.f32` +
`vmrs APSR_nzcv` on denormal inputs. In flush-to-zero mode the ARM11 compares them as
zero; TriAevum's `VfpBinary32Compare` does not flush them. The fix is small and belongs
in the shared runtime.

## What this means for the estimate

| Phase | Before | Now | Why |
| --- | --- | --- | --- |
| Find the code | 1–2 weeks | days | Done here, apart from the Thumb fragments, one switch and more coverage sessions |
| Translator | 2–4 weeks | 1–2 weeks | Reuse TriAevum's whole-program generator with this function list; add a Thumb front end; fix FZ compares |
| Hook into Azahar | 2–4 weeks | 2–4 weeks | Not tested here: entry dispatch, the 29 SVCs through Azahar's HLE kernel, guest threads as host fibers, ticks, the exclusive monitor, CP15 TLS |
| Make it hold | 3–6 weeks | 3–6 weeks | Long tail |

Overall: about 1.5–3 months instead of 2–4.

## Reproduce

```sh
# venv with capstone, unicorn, numpy; MSYS2 g++ on PATH for the sample
python tools/msr/recomp/ctr.py "<dump>.cci" build-windows/msr-recomp
python tools/msr/recomp/analyze.py build-windows/msr-recomp [coverage.bin]
python tools/msr/recomp/diagnose.py build-windows/msr-recomp coverage.bin
python tools/msr/recomp/sample_translate.py build-windows/msr-recomp --functions 1000 --cases 16
```

Run the game with `MSR_DEV=1` and create `user/msr/coverage.trigger` to write
`user/msr/coverage.bin`. Output in `build-windows/` contains game code: keep it out of Git
and out of packages.
