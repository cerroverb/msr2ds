// The cheats on msr-native (src/msr/cheats.h): energy, missiles and Aeion kept full, as the emulator-based build's
// GPU::ApplyMsrRefills (video_core/gpu.cpp) does at each VBlank. Missiles are written straight into Samus's
// inventory; energy and Aeion go through their components' setters, which must run on the game's thread: the
// emulator branches the frame limiter into an ARM stub for that (msr/suit_swap.h), here the dispatcher stops at the
// frame limiter's first instruction (a patchable word, rewritten while calls wait) and calls the setters through
// the recompiled code itself (msr_game_hook). Also, each VBlank, the extras' instruction patches follow the
// settings (GPU::ApplyMsrCheats).
#include <array>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "msr.h"
#include "msr/cheats.h"
#include "msr/presentation.h"
#include "msr/suit_swap.h"
#include "cpu.h"
extern "C" {
#include "kernel.h"
}

namespace {

namespace C = MSR::Cheats;
namespace P = MSR::Presentation;

constexpr u32 Hook = 0x156450;           // the frame limiter's first instruction (patchable.txt)
constexpr u32 HookOriginal = 0xE92D4070; // push {r4-r6, lr}
constexpr u32 HookTrap = 0xE7F000F0;     // a permanently undefined instruction: the dispatcher's cue

struct Call { u32 function, r0, r1, s0; };
std::mutex lock;
std::vector<Call> calls;   // waiting for the game thread

struct Refills {
    std::array<u32, 2> components{};
    std::array<bool, 2> locked{};
    double next_scan = 0;
} refills;

// A guest range is readable: inside a region the game was given (kernel.c's map), not past its end.
bool Valid(u32 address) { return address >= 0x00100000 && k_region_state(address) != 0; }

bool InGameplay() {
    switch (static_cast<P::Bottom>(P::bottom.load(std::memory_order_relaxed))) {
    case P::Bottom::Hud: return true;
    case P::Bottom::Menu: {
        const auto page = static_cast<P::Page>(P::page.load(std::memory_order_relaxed));
        return page == P::Page::PauseMap || page == P::Page::PauseSamus || page == P::Page::MapPins;
    }
    default: return false;
    }
}

// FindMsrObjects: objects of one class on the game's heaps (the heap and the linear heap), by their vtable.
template <typename Fits>
std::vector<u32> FindObjects(u32 vtable, u32 size, Fits fits, std::size_t limit) {
    std::vector<u32> found;
    uint32_t base[64], length[64], state[64];
    const int n = k_region_list(base, length, state, 64);
    for (int r = 0; r < n; ++r) {
        if ((state[r] != 5 && state[r] != 7) || base[r] < 0x08000000 || length[r] < size) continue;
        for (u32 offset = 0; offset + size <= length[r]; offset += 4) {
            const u32 at = base[r] + offset;
            if (rd32(at) != vtable) continue;
            if (fits([at](u32 field) { return static_cast<u32>(rd32(at + field)); })) {
                found.push_back(at);
                if (found.size() == limit) return found;
            }
        }
    }
    return found;
}
template <typename Fits>
u32 FindObject(u32 vtable, u32 size, Fits fits) {
    const auto found = FindObjects(vtable, size, fits, 1);
    return found.empty() ? 0 : found.front();
}

void Queue(const Call& call) {
    std::lock_guard<std::mutex> guard(lock);
    calls.push_back(call);
    if (rd32(Hook) == HookOriginal) wr32(Hook, HookTrap);
}

bool CallsWaiting() {
    std::lock_guard<std::mutex> guard(lock);
    return !calls.empty();
}

void CancelCalls(u32 function) {   // still waiting (the game is loading): dropped
    std::lock_guard<std::mutex> guard(lock);
    calls.erase(std::remove_if(calls.begin(), calls.end(), [function](const Call& c) { return c.function == function; }), calls.end());
    if (calls.empty() && rd32(Hook) == HookTrap) wr32(Hook, HookOriginal);
}

// ---- the Fusion suit switched in play (msr/suit_swap.h; the emulator build's GPU::ApplyMsrSuitSwap) ----
// The same ARM stub as the emulator build, at the same place: it loads a suit family's packages, rebuilds Samus's model
// components and releases a family, one command per frame on the game thread. Here it is called like a function from
// the frame limiter's trap (msr_game_hook), so its last instruction, which replays the limiter's push and jumps back
// into it, becomes a return; the fallback interpreter runs it (it is not in the recompiled code) and the game
// functions it calls run as recompiled code.
namespace S = MSR::SuitSwap;

std::vector<u32> stub;   // the stub as installed here
int stub_room = -1;      // -1 not checked, 0 something else is there, 1 free or ours

const std::vector<u32>& Stub() {
    if (stub.empty()) {
        stub.assign(S::Stub.begin(), S::Stub.end());
        for (std::size_t i = 0; i + 1 < stub.size(); ++i) {
            if (stub[i] == S::HookOriginal && (stub[i + 1] & 0xFFFFF000u) == 0xE59FF000u) {   // push; ldr pc, =RESUME
                stub[i] = 0xE12FFF1Eu;   // bx lr
                break;
            }
        }
    }
    return stub;
}

std::vector<u32> FindModelComponents() {
    return FindObjects(S::ComponentVtable, S::ComponentSize, [](auto word) {
        const u32 count = word(S::ComponentCountOffset);
        return count != 0 && count <= 8 && word(S::ComponentCapacityOffset) >= count;
    }, S::MaxComponents);
}

bool ComponentsCurrent(const std::vector<u32>& components) {
    for (const u32 c : components) {
        if (!Valid(c) || !Valid(c + S::ComponentSize - 4) || rd32(c) != S::ComponentVtable) return false;
        const u32 count = rd32(c + S::ComponentCountOffset);
        if (count == 0 || count > 8) return false;
    }
    return !components.empty();
}

// 1 when the component's models are the Fusion ones (actors/characters/samusfusion/...), 0 if not, -1 unreadable
int ModelsFusion(u32 component) {
    const u32 entries = rd32(component + S::ComponentEntriesOffset);
    if (!Valid(entries + S::EntryPathOffset)) return -1;
    const u32 path = rd32(entries + S::EntryPathOffset);
    if (!Valid(path)) return -1;
    const u32 text = rd32(path);
    if (!Valid(text)) return -1;
    std::string file;
    for (u32 i = 0; i < 96 && Valid(text + i); ++i) {
        const char ch = static_cast<char>(rd8(text + i));
        if (!ch) break;
        file += ch;
    }
    if (file.rfind("actors/characters/", 0) != 0) return -1;
    return file.find("fusion") != std::string::npos ? 1 : 0;
}

// MsrBoneHashes / MsrBoneAttachments / MsrReattach: effects hang from Samus's bones by index, and the Fusion skeleton
// orders its bones differently, so they are moved to the bone of the same name after a swap.
std::vector<u32> BoneHashes(u32 instance) {
    std::vector<u32> hashes;
    const u32 skeleton = rd32(instance + S::InstanceSkeletonOffset);
    if (!Valid(skeleton + 0x44)) return hashes;
    const u32 data = rd32(skeleton + 0x44);
    if (!Valid(data + 0x3C)) return hashes;
    const u32 table = rd32(data + 0x3C);
    if (!Valid(table + 4)) return hashes;
    const u32 count = rd32(table), bones = rd32(table + 4);
    if (count > 512 || !Valid(bones) || !Valid(bones + count * S::BoneSize - 4)) return hashes;
    for (u32 i = 0; i < count; ++i) {
        const u32 name = rd32(bones + i * S::BoneSize + S::BoneNameOffset);
        hashes.push_back(Valid(name + 0x10) ? rd32(name + 0x10) : 0);
    }
    return hashes;
}

std::vector<std::pair<u32, u32>> BoneAttachments(const std::vector<u32>& components) {
    std::vector<std::pair<u32, u32>> found;
    for (const u32 c : components) {
        const u32 instance = rd32(c + S::ComponentInstanceOffset);
        if (!instance || !Valid(instance)) continue;
        const auto bones = BoneHashes(instance);
        u32 child = rd32(instance + S::InstanceNodeOffset + S::NodeFirstChildOffset);
        for (int guard = 0; child && guard < 256 && Valid(child + S::NodeBoneOffset); ++guard) {
            const s32 bone = static_cast<s32>(rd32(child + S::NodeBoneOffset));
            if (bone >= 0 && static_cast<std::size_t>(bone) < bones.size()) found.emplace_back(child, bones[bone]);
            child = rd32(child + S::NodeNextOffset);
        }
    }
    return found;
}

void Reattach(const std::vector<u32>& components, const std::vector<std::pair<u32, u32>>& attachments) {
    int moved = 0, missing = 0;
    for (const u32 c : components) {
        const u32 instance = rd32(c + S::ComponentInstanceOffset);
        if (!instance || !Valid(instance)) continue;
        const auto bones = BoneHashes(instance);
        u32 child = rd32(instance + S::InstanceNodeOffset + S::NodeFirstChildOffset);
        for (int guard = 0; child && guard < 256 && Valid(child + S::NodeBoneOffset); ++guard) {
            const auto recorded = std::find_if(attachments.begin(), attachments.end(), [child](const auto& a) { return a.first == child; });
            if (recorded != attachments.end()) {
                const auto bone = std::find(bones.begin(), bones.end(), recorded->second);
                if (bone != bones.end()) { wr32(child + S::NodeBoneOffset, static_cast<u32>(bone - bones.begin())); ++moved; }
                else ++missing;
            }
            child = rd32(child + S::NodeNextOffset);
        }
    }
    std::fprintf(stderr, "msr: Fusion suit: %d attached models moved to their bones, %d without one\n", moved, missing);
}

enum class Step { Idle, Loading, Settling, Swapping, Releasing, Aborting };
struct Swap {
    Step step = Step::Idle;
    bool target = false;
    int given_up = -1;
    double since = 0, next_scan = 0;
    u32 runs = 0;
    std::vector<u32> components;
    std::vector<std::pair<u32, u32>> attachments;
} swap;

// A command for the stub, run on the game's next frame.
void Send(u32 command, bool family, double now) {
    const auto& words = Stub();
    rt_quiet_range(S::StubAddress, S::StubAddress + static_cast<u32>(words.size()) * 4);   // not game code for the next build
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (rd32(S::StubAddress + static_cast<u32>(i) * 4) != words[i]) wr32(S::StubAddress + static_cast<u32>(i) * 4, words[i]);
    }
    const u32 count = command == S::SwapModels ? std::min<u32>(static_cast<u32>(swap.components.size()), S::MaxComponents) : 0;
    wr32(S::Mailbox + S::CommandField, command);
    wr32(S::Mailbox + S::Family, family ? 1 : 0);
    wr32(S::Mailbox + S::Count, count);
    for (u32 i = 0; i < count; ++i) wr32(S::Mailbox + S::Components + i * 4, swap.components[i]);
    wr32(S::Mailbox + S::CallCount, 0);
    swap.runs = rd32(S::Mailbox + S::Runs);
    wr32(S::Mailbox + S::Armed, 1);
    swap.since = now;
    Queue({S::StubAddress, 0, 0, 0});
}

void SuitFrame(double now) {
    if (stub_room < 0) {   // the stub's place holds zeros or the stub; anything else is a code mod's
        bool free = true;
        for (std::size_t i = 0; i < S::Stub.size(); ++i) {
            const u32 word = rd32(S::StubAddress + static_cast<u32>(i) * 4);
            free &= word == 0 || word == S::Stub[i] || word == Stub()[i];
        }
        stub_room = free ? 1 : 0;
        if (!free) std::fprintf(stderr, "msr: code found at %08x, where the suit swap stub goes: no live Fusion suit switch\n", S::StubAddress);
    }
    if (!stub_room) return;
    const u32 game = rd32(S::GamePointer);
    if (!game || !Valid(game + S::ForceFusionOffset)) return;
    // with a randomizer only its own option (ForceFusion) or Fusion Mode: the setting stays off
    const bool fusion = ((C::enabled.load() & C::FusionSuit) != 0 && !C::randomizer.load()) ||
                        rd8(game + S::FusionModeOffset) != 0 || rd8(game + S::ForceFusionOffset) != 0;
    switch (swap.step) {
    case Step::Idle: {
        if (swap.given_up >= 0 && swap.given_up != static_cast<int>(fusion)) swap.given_up = -1;
        if (!InGameplay()) return;
        if (!ComponentsCurrent(swap.components)) {   // found again after a load replaced them
            if (now < swap.next_scan) return;
            swap.next_scan = now + 1.0;
            swap.components = FindModelComponents();
            if (swap.components.empty()) return;
        }
        const int models = ModelsFusion(swap.components.front());
        if (models < 0 || models == static_cast<int>(fusion) || swap.given_up == static_cast<int>(fusion)) return;
        swap.target = fusion;
        std::fprintf(stderr, "msr: Fusion suit %s: loading the packages\n", fusion ? "on" : "off");
        Send(S::LoadFamily, fusion, now);
        swap.step = Step::Loading;
        return;
    }
    case Step::Settling:
        if (now - swap.since < 0.3) return;   // the packages' resources settle before the models use them
        if (fusion != swap.target) {          // switched back meanwhile
            Send(S::ReleaseFamily, swap.target, now);
            swap.step = Step::Aborting;
            return;
        }
        if (!InGameplay()) return;
        swap.components = FindModelComponents();
        if (swap.components.empty()) {
            Send(S::ReleaseFamily, !swap.target, now);
            swap.step = Step::Releasing;
            return;
        }
        std::fprintf(stderr, "msr: Fusion suit: replacing the models of %zu components\n", swap.components.size());
        swap.attachments = BoneAttachments(swap.components);
        Send(S::SwapModels, swap.target, now);
        swap.step = Step::Swapping;
        return;
    default:
        break;
    }
    // A command is out: has the stub run it?
    if (rd32(S::Mailbox + S::Runs) == swap.runs) {
        // the frame limiter has not run (a load): a swap is taken back and prepared again
        if (swap.step == Step::Swapping && now - swap.since > 0.5) {
            CancelCalls(S::StubAddress);
            wr32(S::Mailbox + S::Armed, 0);
            swap.step = Step::Settling;
        }
        return;
    }
    switch (swap.step) {
    case Step::Loading:
        swap.step = Step::Settling;
        swap.since = now;
        break;
    case Step::Swapping:
        Reattach(swap.components, swap.attachments);
        Send(S::ReleaseFamily, !swap.target, now);
        swap.step = Step::Releasing;
        break;
    case Step::Releasing:
        if (ComponentsCurrent(swap.components) && ModelsFusion(swap.components.front()) != static_cast<int>(swap.target)) {
            std::fprintf(stderr, "msr: Fusion suit: the models did not change\n");
            swap.given_up = swap.target;
        } else {
            std::fprintf(stderr, "msr: Fusion suit: done\n");
        }
        swap.step = Step::Idle;
        break;
    default:
        swap.step = Step::Idle;
        break;
    }
}

// GPU::ApplyMsrCheats: each patch follows its flag, where the game's word or its replacement is found.
void SyncPatches(u32 enabled) {
    for (const auto& patch : C::Patches) {
        const u32 word = rd32(patch.address);
        const u32 wanted = (enabled & patch.flag) ? patch.replacement : patch.original;
        if (word != wanted && (word == patch.original || word == patch.replacement)) wr32(patch.address, wanted);
    }
}

} // namespace

// The energy/Aeion heap scan in the background: 0 idle, 1 running, 2 done (found[] holds what it found).
struct HeapScan {
    std::atomic<int> state{0};
    std::array<std::atomic<u32>, 8> found{};
};
static HeapScan heap_scan;
static_assert(C::LockedValues.size() <= 8, "HeapScan::found");

extern "C" void msr_cheats_frame(double now) {
    u32 enabled = C::enabled.load(std::memory_order_relaxed);
    if (rd32(Hook) != HookOriginal && rd32(Hook) != HookTrap) return;   // not this game's code
    if (C::randomizer.load(std::memory_order_relaxed)) enabled &= ~static_cast<u32>(C::FusionSuit);
    SyncPatches(enabled);
    if (!Valid(C::GamePointer)) return;
    SuitFrame(now);

    // Energy and Aeion: their components, found again once an area load has replaced them.
    bool missing = false;
    for (std::size_t i = 0; i < C::LockedValues.size(); ++i) {
        const auto& value = C::LockedValues[i];
        u32& component = refills.components[i];
        if (component != 0 && (!Valid(component + value.size - 4) || rd32(component) != value.vtable)) {
            component = 0;
            refills.locked[i] = false;
        }
        if ((enabled & value.flag) == 0) {
            if (refills.locked[i]) {
                wr8(component + value.lock, 0);
                refills.locked[i] = false;
            }
            continue;
        }
        if (component == 0) {
            missing = true;
            continue;
        }
        // Full: locked. Otherwise unlocked and filled through the setter, on the game thread.
        const u32 maximum = rd32(component + value.maximum);
        if (rd32(component + value.current) == maximum) {
            wr8(component + value.lock, 1);
            refills.locked[i] = true;
        } else if (!CallsWaiting()) {
            wr8(component + value.lock, 0);
            Queue({value.setter, component, 0, maximum});
        }
    }
    // The heap scan runs on a thread of its own (on this one, the VBlank thread, it took about 27 ms: a hitch as play
    // resumed after each door). Its results are checked like any component above before they are used.
    if (missing && InGameplay()) {
        if (heap_scan.state.load() == 2) {
            for (std::size_t i = 0; i < C::LockedValues.size(); ++i) {
                const u32 found = heap_scan.found[i].exchange(0);
                if (found == 0 || refills.components[i] != 0 || (enabled & C::LockedValues[i].flag) == 0) continue;
                refills.components[i] = found;
                std::fprintf(stderr, "msr: cheats: %s kept full (%08x)\n",
                             C::LockedValues[i].flag == C::InfiniteEnergy ? "energy" : "Aeion", found);
            }
            heap_scan.state.store(0);
        } else if (heap_scan.state.load() == 0 && now >= refills.next_scan) {
            refills.next_scan = now + 1.0;
            u32 wanted = 0;
            for (std::size_t i = 0; i < C::LockedValues.size(); ++i)
                if ((enabled & C::LockedValues[i].flag) != 0 && refills.components[i] == 0) wanted |= 1u << i;
            heap_scan.state.store(1);
            std::thread([wanted] {
                for (std::size_t i = 0; i < C::LockedValues.size(); ++i) {
                    if ((wanted & (1u << i)) == 0) continue;
                    const auto& value = C::LockedValues[i];
                    heap_scan.found[i].store(FindObject(value.vtable, value.size, [&value](auto word) {
                        float maximum;
                        const u32 bits = word(value.maximum);
                        std::memcpy(&maximum, &bits, sizeof maximum);
                        return maximum > 0.f && maximum < 100000.f;
                    }));
                }
                heap_scan.state.store(2);
            }).detach();
        }
    }

    // Missiles: Samus's inventory, as the game finds it.
    if ((enabled & C::InfiniteMissiles) == 0) return;
    const u32 game = rd32(C::GamePointer);
    u32 inventory = 0;
    for (const u32 offset : C::PlayerHandles) {
        if (!Valid(game + offset)) return;
        const u32 handle = rd32(game + offset);
        const u32 entity = Valid(handle + 8) ? rd32(handle + 8) : 0;
        inventory = Valid(entity + C::EntityInventoryOffset) ? rd32(entity + C::EntityInventoryOffset) : 0;
        if (inventory != 0) break;
    }
    if (!Valid(inventory + 0x48)) return;
    const auto item = [&](u32 key) -> u32 {
        const u32 name = Valid(key) ? rd32(key) : 0;
        if (!Valid(name + 0x10) || rd32(inventory + 0x44) == 0) return 0;
        const u32 hash = rd32(name + 0x10), buckets = rd32(inventory + 0x40), count = rd32(inventory + 0x48);
        if (count == 0 || !Valid(buckets + (hash % count) * 4)) return 0;
        u32 node = rd32(buckets + (hash % count) * 4);
        for (int guard = 0; node != 0 && guard < 64 && Valid(node + 0x10); ++guard) {
            if (rd32(node) == hash) {
                const u32 v = rd32(node + 4);
                return Valid(v) ? v : 0;
            }
            node = rd32(node + 0x10);
        }
        return 0;
    };
    for (const auto& refill : C::Refills) {
        const u32 current = item(refill.current), maximum = item(refill.maximum);
        if (current != 0 && maximum != 0 && rd32(current) != rd32(maximum)) wr32(current, rd32(maximum));
    }
}

// A save state was loaded: the objects found on the game's heaps are other ones now (or none), the calls queued meant
// for the old ones.
extern "C" void msr_cheats_reset(void) {
    std::lock_guard<std::mutex> guard(lock);
    calls.clear();
    refills = Refills{};
    swap = Swap{};
    stub_room = -1;
}

// The dispatcher found the trap at the frame limiter (on the game's main thread): the waiting calls run as
// ordinary calls into the game's code, the CPU state put back after each, then the limiter's own instruction.
// Returns 0 when the trap was ours.
extern "C" int msr_game_hook(CPU* c) {
    if (c->r[15] != Hook || c->thumb || rd32(Hook) != HookTrap) return -1;
    std::vector<Call> todo;
    {
        std::lock_guard<std::mutex> guard(lock);
        todo.swap(calls);
        wr32(Hook, HookOriginal);
    }
    for (const auto& call : todo) {
        CPU saved = *c;
        c->r[0] = call.r0;
        c->r[1] = call.r1;
        c->s[0] = call.s0;
        c->r[13] = (c->r[13] - 0x40) & ~7u;   // below the caller's frame, 8-byte aligned
        c->r[14] = EXIT_MAGIC;
        c->r[15] = call.function & ~1u;
        c->thumb = call.function & 1;
        rt_run(c);
        *c = saved;
    }
    return 0;
}
