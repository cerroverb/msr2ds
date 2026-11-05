// Runs Samus Returns' recompiled code natively: loads the module built from the player's dump
// (msr_aot.dll / libmsr_aot.so, the same library the Azahar-based build loads; see
// src/core/arm/msr_aot/aot_generated.h), looks up the function for each guest PC, and runs guest
// threads function to function. There is no emulator behind it: system calls go to the HLE kernel
// (kernel.c), and code the module lacks is reported (there is no JIT to fall back on).

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "aot_generated.h"
#include "msr/msr.h"
#include "cpu.h"

#ifndef MSR_AOT_RUNTIME_SHA256
#error "MSR_AOT_RUNTIME_SHA256 must be defined (the SHA-256 of aot_runtime.h)"
#endif

// cpu.h mirrors MsrAot::Ctx for the C runtime.
static_assert(sizeof(CPU) == sizeof(MsrAot::Ctx));
#define SAME(a, b) static_assert(offsetof(CPU, a) == offsetof(MsrAot::Ctx, b))
SAME(r, r);
SAME(n, n);
SAME(cf, c);
SAME(v, v);
SAME(q, q);
SAME(ge, ge);
SAME(cpsr_rest, cpsr_rest);
SAME(thumb, thumb);
SAME(s, s);
SAME(fpscr, fpscr);
SAME(tls, tls_ro);
SAME(tls_rw, tls_rw);
SAME(ticks, ticks);
SAME(budget, budget);
SAME(exit, exit);
SAME(svc, svc);
SAME(excl_valid, excl_valid);
SAME(excl_addr, excl_addr);
SAME(excl_value, excl_value);
SAME(pages, pages);
SAME(mem, mem);
#undef SAME

namespace {

const MsrAot::Module* module;
bool code_modded;   // a game mod changed the code (rt_code_changed)
const MsrAot::RegionSpan* spans;   // each function's addresses (msr_aot_region_spans; libraries built before it lack it)
MsrAot::u32 span_count;
std::vector<MsrAot::Handler> arm_handlers; // by (pc - text base) / 4
std::unordered_map<uint32_t, MsrAot::Handler> thumb_handlers;
std::vector<uint8_t*> pages; // every guest page, straight into g_mem

/// A time slice in ARM11 cycles: long-running code returns to the dispatcher this often (at loop
/// heads), so other guest threads get a turn (kernel.c k_slice_end).
constexpr uint64_t kSlice = 268'111'856ull / 1000; // about 1 ms of the 3DS's CPU

MsrAot::Handler Lookup(uint32_t pc, uint32_t thumb) {
    const uint32_t offset = pc - module->text_base;
    if (offset >= module->text_size) {
        return nullptr;
    }
    if (thumb) {
        const auto it = thumb_handlers.find(pc);
        return it != thumb_handlers.end() ? it->second : nullptr;
    }
    return (pc & 3) ? nullptr : arm_handlers[offset / 4];
}

// Slow paths: accesses across a page boundary (all memory is one block here).
uint8_t Read8(void*, uint32_t a) {
    return rd8(a);
}
uint16_t Read16(void*, uint32_t a) {
    return static_cast<uint16_t>(rd16(a));
}
uint32_t Read32(void*, uint32_t a) {
    return rd32(a);
}
void Write8(void*, uint32_t a, uint8_t v) {
    wr8(a, v);
}
void Write16(void*, uint32_t a, uint16_t v) {
    wr16(a, v);
}
void Write32(void*, uint32_t a, uint32_t v) {
    wr32(a, v);
}
bool WriteExclusive32(void*, uint32_t a, uint32_t v, uint32_t expected) {
    return __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(g_mem + a), &expected, v, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
bool WriteExclusive64(void*, uint32_t a, uint64_t v, uint64_t expected) {
    return __atomic_compare_exchange_n(reinterpret_cast<uint64_t*>(g_mem + a), &expected, v, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

[[noreturn]] void Missing(CPU* c, const char* why) {
    std::fprintf(stderr,
                 "native: %s at %08x (%s)\n"
                 "    r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x\n"
                 "    r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x sp=%08x lr=%08x\n",
                 why, c->r[15], c->thumb ? "Thumb" : "ARM", c->r[0], c->r[1], c->r[2], c->r[3], c->r[4],
                 c->r[5], c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13],
                 c->r[14]);
    if (const char* out = std::getenv("MSR_NATIVE_ENTRIES_OUT")) {
        // Like the emulator's recomp-jit-entries file: build_module.py --extra takes it.
        if (FILE* f = std::fopen(out, "a")) {
            std::fprintf(f, "0x%08X 1\n", c->r[15] | c->thumb);
            std::fclose(f);
        }
    }
    std::fflush(stderr);
    std::abort();
}

// Code the module lacks: logged once per address, to stderr and to the entries file (main.c: user/msr/
// recomp-jit-entries-core-native.txt, which the launcher's next build takes with --extra like the emulator's).
std::mutex missing_lock;
std::unordered_set<uint32_t> missing_seen;   // reported in this run
std::unordered_set<uint32_t> listed;         // already in the entries file (from earlier runs too)
std::string entries_file;
uint32_t quiet_lo, quiet_hi;
std::vector<std::pair<uint32_t, uint32_t>> quiet_spans;   // functions turned off on purpose (rt_code_patchable), at boot
// A host function called on entry to one guest function (ARM), before its code runs (rt_set_call_hook): every call goes
// through Lookup here, so this needs no change to the recompiled code.
uint32_t hook_pc = 0xFFFFFFFFu;
void (*hook_fn)(CPU*) = nullptr;   // code put in memory on purpose (Samus Returns' suit swap stub): interpreted silently
// more hooks (rt_add_call_hook): a few entries, checked only when the pc's low bits match one of them
constexpr int kMaxHooks = 32;
uint32_t hook_pcs[kMaxHooks];
void (*hook_fns[kMaxHooks])(CPU*);
int hook_count;
uint64_t hook_bits;   // bit (pc >> 2) & 63 of each hooked pc

void NoteMissing(const CPU* c) {
    const uint32_t at = c->r[15] | c->thumb;
    if (c->r[15] >= quiet_lo && c->r[15] < quiet_hi) {
        return;
    }
    for (const auto& [lo, hi] : quiet_spans) {
        if (c->r[15] >= lo && c->r[15] < hi) return;
    }
    // With a game mod's code the interpreter runs the functions it changed and the code it added: neither belongs in
    // the list for the next build of the game's own code.
    if (code_modded) {
        return;
    }
    std::lock_guard<std::mutex> lock(missing_lock);
    if (!missing_seen.insert(at).second) {
        return;
    }
    std::fprintf(stderr, "native: interpreting %08x (%s): the native code lacks it (lr=%08x)\n", c->r[15],
                 c->thumb ? "Thumb" : "ARM", c->r[14]);
    const char* out = std::getenv("MSR_NATIVE_ENTRIES_OUT");
    const std::string path = out ? out : entries_file;
    if (path.empty() || !listed.insert(at).second) {
        return;
    }
    const bool fresh = [&] { FILE* r = std::fopen(path.c_str(), "rb"); if (!r) return true; std::fclose(r); return false; }();
    if (FILE* f = std::fopen(path.c_str(), "a")) {
        if (fresh) {
            std::fprintf(f, "# msr-native: code the native code lacked (odd = Thumb), for the next build\n");
        }
        std::fprintf(f, "0x%08X 1\n", at);
        std::fclose(f);
    }
}

// Interprets (interp_full.c) until the program counter reaches code the module has: 0 there, 1 the slice ended,
// 2 a system call, -1 an instruction the interpreter does not know.
int InterpretUntilKnown(CPU* c, MsrAot::Ctx& ctx) {
    for (;;) {
        const int r = rt_interpret_step(c);
        if (r < 0) {
            return -1;
        }
        if (r == 1) {
            return 2;
        }
        if (ctx.r[15] == EXIT_MAGIC || Lookup(ctx.r[15], ctx.thumb) != nullptr) {
            return 0;
        }
        if (++ctx.ticks >= ctx.budget) {
            ctx.exit = MsrAot::Exit::Branch;
            return 1;
        }
    }
}

} // namespace

extern "C" void rt_set_call_hook(uint32_t pc, void (*fn)(CPU*)) {
    hook_fn = fn;
    hook_pc = fn ? pc : 0xFFFFFFFFu;
}
// another hook (on an ARM function's entry); 0 when the table is full. Set before the game runs the function.
extern "C" int rt_add_call_hook(uint32_t pc, void (*fn)(CPU*)) {
    if (hook_count == kMaxHooks || !fn) return 0;
    hook_pcs[hook_count] = pc; hook_fns[hook_count] = fn; hook_count++;
    hook_bits |= 1ull << ((pc >> 2) & 63);
    return 1;
}

extern "C" void rt_quiet_range(uint32_t lo, uint32_t hi) {
    quiet_lo = lo;
    quiet_hi = hi;
}

extern "C" void rt_set_entries_file(const char* path) {
    std::lock_guard<std::mutex> lock(missing_lock);
    entries_file = path ? path : "";
    if (FILE* f = path ? std::fopen(path, "rb") : nullptr) {   // the addresses listed by earlier runs are not added again
        char line[128];
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] != '#') listed.insert(static_cast<uint32_t>(std::strtoul(line, nullptr, 0)));
        }
        std::fclose(f);
    }
}

extern "C" int rt_module_load(const char* path) {
#ifdef _WIN32
    const int wide = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wide);
    HMODULE lib = LoadLibraryExW(wpath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib) {
        std::fprintf(stderr, "native: cannot load %s (error %lu)\n", path, GetLastError());
        return -1;
    }
    auto get = reinterpret_cast<const MsrAot::Module* (*)()>(GetProcAddress(lib, "msr_aot_module"));
    auto get_spans = reinterpret_cast<const MsrAot::RegionSpan* (*)(MsrAot::u32*)>(GetProcAddress(lib, "msr_aot_region_spans"));
#else
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "native: cannot load %s (%s)\n", path, dlerror());
        return -1;
    }
    auto get = reinterpret_cast<const MsrAot::Module* (*)()>(dlsym(lib, "msr_aot_module"));
    auto get_spans = reinterpret_cast<const MsrAot::RegionSpan* (*)(MsrAot::u32*)>(dlsym(lib, "msr_aot_region_spans"));
#endif
    if (get_spans) spans = get_spans(&span_count);
    module = get ? get() : nullptr;
    if (!module || module->version != MsrAot::kModuleVersion ||
        module->ctx_size != sizeof(MsrAot::Ctx) ||
        std::strncmp(module->runtime_sha256, MSR_AOT_RUNTIME_SHA256, 64) != 0) {
        std::fprintf(stderr, "native: %s was built for another version of the recompiler\n", path);
        module = nullptr;
        return -1;
    }
    arm_handlers.assign(module->text_size / 4 + 1, nullptr);
    size_t entries = 0;
    for (uint32_t s = 0; s < module->shard_count; ++s) {
        const auto& shard = module->shards[s];
        for (uint32_t i = 0; i < *shard.count; ++i) {
            const auto& e = shard.entries[i];
            if (!e.handler || e.pc - module->text_base >= module->text_size) {
                continue;
            }
            if (e.thumb) {
                thumb_handlers.emplace(e.pc, e.handler);
            } else {
                arm_handlers[(e.pc - module->text_base) / 4] = e.handler;
            }
            ++entries;
        }
    }
    pages.resize(1u << 20);
    for (uint32_t i = 0; i < (1u << 20); ++i) {
        pages[i] = g_mem + (static_cast<size_t>(i) << 12);
    }
    std::fprintf(stderr, "native: %zu entry points in %u shards from %s\n", entries,
                 module->shard_count, path);
    if (const char* list = std::getenv("PWR_CALL_TRACE")) {   // <hex>[,<hex>...]: each call of those ARM functions
        for (const char* p = list; *p;) {
            char* end;
            const unsigned long pc = std::strtoul(p, &end, 16);
            if (end == p) break;
            rt_add_call_hook(static_cast<uint32_t>(pc), [](CPU* c) {
                std::fprintf(stderr, "call %08x r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x\n", c->r[15], c->r[0], c->r[1],
                             c->r[2], c->r[3], c->r[14]);
            });
            p = *end == ',' ? end + 1 : end;
        }
    }
    return 0;
}

// Turns off every recompiled function covering one of `words` (sorted), so the fallback interpreter runs that code as
// memory has it; calls between the functions go through Lookup, so the others stay recompiled. `quiet`: their code is
// not listed as code the module lacks. The functions' number and their entry points'.
static std::pair<size_t, size_t> TurnOff(const std::vector<uint32_t>& words, bool quiet, const char* trace) {
    std::unordered_set<MsrAot::Handler> off;
    for (MsrAot::u32 i = 0; i < span_count; ++i) {
        const auto& s = spans[i];
        const auto it = std::lower_bound(words.begin(), words.end(), s.lo & ~3u);
        if (it == words.end() || *it >= s.hi) continue;
        if (trace && std::getenv(trace)) {
            std::fprintf(stderr, "native: %08x in function %08x [%08x, %08x) runs on the interpreter\n", *it, s.entry, s.lo,
                         s.hi);
        }
        if (quiet) quiet_spans.emplace_back(s.lo & ~3u, s.hi);
        if (const MsrAot::Handler h = Lookup(s.entry, s.thumb)) off.insert(h);
    }
    size_t entries = 0;
    for (auto& h : arm_handlers) {
        if (h && off.count(h)) {
            h = nullptr;
            ++entries;
        }
    }
    for (auto it = thumb_handlers.begin(); it != thumb_handlers.end();) {
        if (off.count(it->second)) {
            it = thumb_handlers.erase(it);
            ++entries;
        } else {
            ++it;
        }
    }
    return {off.size(), entries};
}

// Instructions the extras rewrite while the game runs that this native code does not check (msr/patches.cpp: added after
// players built theirs): the functions covering them run on the interpreter, which reads them as memory has them.
extern "C" void rt_code_patchable(const uint32_t* words, uint32_t count) {
    if (!module || !count) return;
    if (!spans) {
        std::fprintf(stderr, "native: this native code was built before 2026-11-02: %u of the extras' instructions do not "
                             "change the game until it is rebuilt (launcher, Native code)\n", count);
        return;
    }
    std::vector<uint32_t> sorted(words, words + count);
    std::sort(sorted.begin(), sorted.end());
    const auto [functions, entries] = TurnOff(sorted, true, "PWR_CODE_MOD_TRACE");
    std::fprintf(stderr, "native: %u of the extras' instructions: %zu functions (%zu entry points) run on the interpreter\n",
                 count, functions, entries);
}

// A game mod changed the code (main.c): every recompiled function covering a changed word is turned off, so the
// fallback interpreter runs the code as the mod left it there. Code a mod adds outside them has no recompiled code in
// any case.
extern "C" void rt_code_changed(uint32_t base, const uint8_t* original, uint32_t original_size, const uint8_t* modded,
                                uint32_t modded_size) {
    if (!module) return;
    code_modded = true;
    std::vector<uint32_t> changed;
    for (uint32_t a = module->text_base; a + 4 <= module->text_base + module->text_size; a += 4) {
        const uint32_t at = a - base;
        uint32_t was = 0, now = 0;
        if (at + 4 <= original_size) std::memcpy(&was, original + at, 4);
        if (at + 4 <= modded_size) std::memcpy(&now, modded + at, 4);
        if (was != now) changed.push_back(a);
    }
    if (changed.empty()) {
        std::fprintf(stderr, "native: the game mods add code but change none of the recompiled code\n");
        return;
    }
    if (!spans) {
        std::fprintf(stderr, "native: a game mod changes %zu words of the game's code, but this native code was built "
                             "before game mods could change it: rebuild it in the launcher (Native code). Until then the "
                             "recompiled code runs the game's own code there.\n", changed.size());
        return;
    }
    const auto [functions, entries] = TurnOff(changed, false, "PWR_CODE_MOD_TRACE");
    std::fprintf(stderr, "native: a game mod changes %zu words of the game's code: %zu functions (%zu entry points) run "
                         "on the interpreter\n", changed.size(), functions, entries);
}

extern "C" int rt_has_code(uint32_t pc) {
    return module && Lookup(pc & ~1u, pc & 1) != nullptr;
}

extern "C" void rt_cpu_init(CPU* c, uint32_t entry) {
    std::memset(c, 0, sizeof(*c));
    c->cpsr_rest = 0x10;     // user mode
    c->fpscr = 0x03C00000;   // RunFast: default NaN, flush to zero, round to nearest
    c->thumb = entry & 1;
    c->r[15] = entry & ~1u;
    c->pages = pages.data();
    c->mem = {nullptr, Read8, Read16, Read32, Write8, Write16, Write32, WriteExclusive32,
              WriteExclusive64};
}

/// The guest's MXCSR while its code runs; the host's around system calls.
class GuestMxcsr {
public:
    explicit GuestMxcsr(uint32_t fpscr) : host(_mm_getcsr()) {
        Enter(fpscr);
    }
    ~GuestMxcsr() {
        Leave();
    }
    void Enter(uint32_t fpscr) {
        _mm_setcsr(MsrAot::MxcsrFor(fpscr, host));
    }
    void Leave() {
        _mm_setcsr(host);
    }

private:
    uint32_t host;
};

extern "C" void rt_run(CPU* c) {
    auto& ctx = *reinterpret_cast<MsrAot::Ctx*>(c);
    GuestMxcsr mxcsr(ctx.fpscr);
    struct Depth {   // save states never stop a thread inside a nested call (kernel.c k_freeze)
        Depth() { k_nest(1); }
        ~Depth() { k_nest(-1); }
    } depth;
    for (;;) {
        if (ctx.r[15] == EXIT_MAGIC || k_abandoned()) {
            return;
        }
        ctx.ticks = 0;
        ctx.budget = k_preempt_take() ? 0 : kSlice;
        ctx.exit = MsrAot::Exit::Branch;
        MsrAot::Handler handler;
        while ((handler = Lookup(ctx.r[15], ctx.thumb)) != nullptr) {
            if (ctx.r[15] == hook_pc && !ctx.thumb) {
                hook_fn(c);
                if (k_abandoned()) {   // the hook waited (gsp.c) while a save state was loaded
                    return;
                }
            }
            if ((hook_bits >> ((ctx.r[15] >> 2) & 63)) & 1 && !ctx.thumb) {
                for (int i = 0; i < hook_count; i++) if (hook_pcs[i] == ctx.r[15]) hook_fns[i](c);
            }
            handler(ctx);
            if (ctx.exit != MsrAot::Exit::Branch || ctx.ticks >= ctx.budget) {
                break;
            }
        }
        if (!handler) {
            if (ctx.r[15] == EXIT_MAGIC) {
                return;
            }
            // Code the module lacks (reached only through a pointer computed at run time): the emulator-based
            // build runs it on its JIT; here the fallback interpreter runs it until it reaches the module's code.
            NoteMissing(c);
            const int r = InterpretUntilKnown(c, ctx);
            if (r < 0) {
                mxcsr.Leave();
                Missing(c, "no recompiled code, and an instruction the fallback interpreter cannot run");
            }
            if (r == 0) {
                continue;
            }
        }
        switch (ctx.exit) {
        case MsrAot::Exit::Branch:
            // The slice ran out: a scheduling point, like a system call.
            mxcsr.Leave();
            k_slice_end();
            mxcsr.Enter(ctx.fpscr);
            break;
        case MsrAot::Exit::Svc:
            mxcsr.Leave();
            rt_svc(c, ctx.svc, ctx.r[15] - (ctx.thumb ? 2 : 4));
            mxcsr.Enter(ctx.fpscr);
            break;
        case MsrAot::Exit::Interpret:
        case MsrAot::Exit::Patched:
            // Samus Returns' cheats: the frame limiter's trap (msr/cheats.cpp) runs the setter calls waiting for the
            // game thread; then the limiter's own instruction, put back, runs here.
            if (ctx.exit == MsrAot::Exit::Patched && msr_game_hook(c) == 0) {
                if (rt_interpret_step(c) != 0) {
                    mxcsr.Leave();
                    Missing(c, "the frame limiter's first instruction");
                }
                break;
            }
            // One instruction (interp.c); the next one is always a resume point of the module.
            if (rt_interpret_one(c) != 0) {
                mxcsr.Leave();
                Missing(c, ctx.exit == MsrAot::Exit::Patched ? "a patched instruction interp.c cannot run"
                                                             : "an instruction interp.c cannot run");
            }
            break;
        }
    }
}
