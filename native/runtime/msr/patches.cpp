// The amiibo extras on msr-native: src/msr/cheats.h's instruction patches, written at boot for the extras the player
// has on; then cheats.cpp keeps them following the settings each VBlank, as the emulator-based build does
// (video_core/gpu.cpp). With a randomizer the Fusion suit stays off (it has its own option).
#include <algorithm>
#include <array>
#include <cstdio>
#include "msr.h"
#include "msr/cheats.h"

#include "cpu.h"
extern "C" {
#include "kernel.h"
}

extern "C" void msr_apply_patches(void) {
    namespace C = MSR::Cheats;
    if (!C::randomizer.load() && (rd32(C::RandomizerHook) & 0xFF000000u) == 0xEB000000u) {   // its bl
        C::randomizer = true;
        std::fprintf(stderr, "msr: randomizer code found: the Fusion suit setting stays off\n");
    }
    const u32 enabled = C::enabled.load() & (C::randomizer.load() ? ~static_cast<u32>(C::FusionSuit) : ~0u);
    u32 bad = 0;
    for (const auto& p : C::Patches) {
        if ((enabled & p.flag) && rd32(p.address) != p.original) {
            std::fprintf(stderr, "msr: patch at %08x does not fit this program (%08x)\n", p.address, rd32(p.address));
            bad |= p.flag;
        }
    }
    u32 written = 0;
    for (const auto& p : C::Patches) {
        if ((enabled & p.flag) && !(bad & p.flag)) {
            wr32(p.address, p.replacement);
            written++;
        }
    }
    std::fprintf(stderr, "msr: extras %s%s%s%s: %u instructions rewritten\n",
                 (enabled & ~bad & C::Galleries) ? "[galleries]" : "",
                 (enabled & ~bad & C::FusionMode) ? "[Fusion Mode]" : "",
                 (enabled & ~bad & C::HardMode) ? "[Hard Mode]" : "",
                 (enabled & ~bad & C::FusionSuit) ? "[Fusion suit]" : "", written);
}

namespace {

// Hard Mode from the start, the rest of it: an empty slot's way through the difficulty list, which the game only has for
// a used slot's New Game (cheats.h's patch at 0x4E30C8 sends a new file's Play to it). Three call hooks on the file
// select (`this`, the menu), each acting only for an empty slot (its summary's +0x54, "has a game", is 0):
// - The state change (0x4E40A0: this, the new state, _, at once): the state left plays its way out, then the new one
//   its way in. The new-file panel's (0xE) fades the cockpit and the whole menu out, as it leads to the game's start, so
//   the change to the list (0x2A) leaves from the state a used slot's New Game leaves to it from ("Start a new game?",
//   0x28). The list's Back goes to a used slot's Continue / New Game / Options (0x12): for an empty one, back to the
//   files (0xD; the new-file panel does not open again from the list).
// - The questions after a difficulty (Normal 0x29, Hard 0x2C, Fusion 0x2E) say "Current data will be erased. Start a new
//   game in ... mode?": the text they copy (0x1913C0, from their returns) becomes the used slot's "Start a new game?".
// - Yes then starts the game as a new file's Play does: the list's Yes handlers store the difficulty in the summary
//   (+0x7C) and clear +0xF9C, which Play sets, and call the title music's fade (0x4C1570) without Play's last argument.
constexpr u32 StateChange = 0x4E40A0, TextCopy = 0x1913C0, MusicFade = 0x4C1570;
constexpr u32 MenuState = 0x138, MenuSlot = 0xF50, SlotSummary = 0x154, SummaryHasGame = 0x54, MenuNewFile = 0xF9C;
constexpr u32 Files = 0xD, NewFilePanel = 0xE, SlotMenu = 0x12, NewGameQuestion = 0x28, DifficultyList = 0x2A;
constexpr u32 StartNewGameText = 0x707AD4;                                // "Start a new game?" (0x28's text global)
constexpr std::array<u32, 3> QuestionTextReturns{0x4DF380, 0x4DDDF0, 0x4DF2DC};   // Normal (from the list), Hard, Fusion
constexpr std::array<u32, 3> YesFadeReturns{0x4E36FC, 0x4E3554, 0x4E3898};        // the same three's Yes handlers

bool Mapped(u32 address) { return address >= 0x00100000u && k_region_state(address) != 0; }

// The menu's slot has no game (a new file).
bool EmptySlot(u32 menu) {
    if (!Mapped(menu + MenuSlot)) return false;
    const u32 slot = rd32(menu + MenuSlot);
    if (!Mapped(slot + SlotSummary)) return false;
    const u32 summary = rd32(slot + SlotSummary);
    return Mapped(summary + SummaryHasGame) && rd8(summary + SummaryHasGame) == 0;
}

bool Returns(const std::array<u32, 3>& returns, u32 lr) {
    return std::find(returns.begin(), returns.end(), lr) != returns.end();
}

void StateChangeHook(CPU* c) {
    const u32 menu = c->r[0];
    if (c->r[3] != 0 || !Mapped(menu + MenuState)) return;
    const u32 now = rd32(menu + MenuState);
    if (c->r[1] == DifficultyList && now == NewFilePanel) {
        wr32(menu + MenuState, NewGameQuestion);
    } else if (c->r[1] == SlotMenu && now == DifficultyList && EmptySlot(menu)) {
        c->r[1] = Files;
    }
}

void TextCopyHook(CPU* c) {
    if (Returns(QuestionTextReturns, c->r[14]) && EmptySlot(c->r[4])) c->r[1] = rd32(StartNewGameText);
}

void MusicFadeHook(CPU* c) {
    if (!Returns(YesFadeReturns, c->r[14]) || !EmptySlot(c->r[0])) return;
    wr8(c->r[0] + MenuNewFile, 1);
    c->r[3] = 1;
}

// A bl to `fn` just before each return address: this game's code.
bool CallsFrom(u32 fn, const u32* returns, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const u32 bl = rd32(returns[i] - 4);
        const u32 target = returns[i] + 4 + (static_cast<u32>(static_cast<int32_t>(bl << 8) >> 6));
        if ((bl & 0xFF000000u) != 0xEB000000u || target != fn) return false;
    }
    return true;
}

void HookNewFileDifficulty() {
    static constexpr u32 StateChangeCalls[] = {0x4E3470, 0x4DD4D4};   // the Yes handler's to the list, the list's Back
    if (!CallsFrom(StateChange, StateChangeCalls, 2) || !CallsFrom(TextCopy, QuestionTextReturns.data(), 3) ||
        !CallsFrom(MusicFade, YesFadeReturns.data(), 3)) {
        std::fprintf(stderr, "msr: Hard Mode from the start: not this game's code, a new file starts on Normal\n");
        return;
    }
    rt_add_call_hook(StateChange, StateChangeHook);
    rt_add_call_hook(TextCopy, TextCopyHook);
    rt_add_call_hook(MusicFade, MusicFadeHook);
}

} // namespace

// The words patchable.txt lists that native code built before them does not check: the recompiled functions holding
// them are turned off (menu code only), so the interpreter runs them as the patches leave them, whichever native code
// the player has. The words added since a release; a rebuilt module checks them itself, which changes nothing here.
extern "C" void msr_patches_module_loaded(void) {
    static constexpr u32 Unchecked[] = {
        0x4E30C8,   // Hard Mode from the start: a new file's Play (2026-11-03)
    };
    rt_code_patchable(Unchecked, static_cast<u32>(sizeof Unchecked / sizeof Unchecked[0]));
    HookNewFileDifficulty();
}
