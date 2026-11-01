// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cryptopp/sha.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/msr_aot/aot_generated.h"
#include "core/arm/msr_aot/arm_aot.h"
#include "core/core.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

namespace Core {
namespace {

Memory::MemorySystem& Mem(void* user) {
    return *static_cast<Memory::MemorySystem*>(user);
}
u8 SlowRead8(void* m, u32 a) {
    return Mem(m).Read8(a);
}
u16 SlowRead16(void* m, u32 a) {
    return Mem(m).Read16(a);
}
u32 SlowRead32(void* m, u32 a) {
    return Mem(m).Read32(a);
}
void SlowWrite8(void* m, u32 a, u8 v) {
    Mem(m).Write8(a, v);
}
void SlowWrite16(void* m, u32 a, u16 v) {
    Mem(m).Write16(a, v);
}
void SlowWrite32(void* m, u32 a, u32 v) {
    Mem(m).Write32(a, v);
}
bool WriteExclusive32(void* m, u32 a, u32 v, u32 expected) {
    return Mem(m).WriteExclusive32(a, v, expected);
}
bool WriteExclusive64(void* m, u32 a, u64 v, u64 expected) {
    return Mem(m).WriteExclusive64(a, v, expected);
}

/// Restores the host's floating-point control (MXCSR, or FPCR on AArch64) on scope exit; the
/// recompiled code runs with the guest's.
class GuestFpControl {
public:
    explicit GuestFpControl(u32 fpscr) : host(MsrAot::HostFpControl()) {
        Enter(fpscr);
    }
    ~GuestFpControl() {
        Leave();
    }
    void Enter(u32 fpscr) {
        MsrAot::SetHostFpControl(MsrAot::GuestFpControl(fpscr, host));
    }
    void Leave() {
        MsrAot::SetHostFpControl(host);
    }

private:
    MsrAot::FpControl host;
};

constexpr u64 ReportInterval = 268'111'856ull * 30; // about 30 emulated seconds

} // namespace

bool ARM_AOT::Available() {
    const char* setting = std::getenv("MSR_AOT");
    return MsrAot::kShardCount > 0 && !(setting && setting[0] == '0');
}

ARM_AOT::ARM_AOT(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id,
                 std::shared_ptr<Core::Timing::Timer> timer,
                 Core::ExclusiveMonitor& exclusive_monitor)
    : ARM_Interface(core_id, timer), system(system_), memory(memory_),
      jit(std::make_unique<ARM_Dynarmic>(system_, memory_, core_id, timer, exclusive_monitor)),
      svc(std::make_unique<Kernel::SVCContext>(system_)),
      page_table(memory_.GetCurrentPageTable()) {
    ctx.mem = {&memory,     SlowRead8,   SlowRead16,       SlowRead32,      SlowWrite8,
               SlowWrite16, SlowWrite32, WriteExclusive32, WriteExclusive64};
    ctx.pages = page_table ? page_table->GetPointerArray().data() : nullptr;

    arm_handlers.assign(MsrAot::kTextSize / 4 + 1, nullptr);
    std::size_t entries = 0;
    for (u32 s = 0; s < MsrAot::kShardCount; ++s) {
        const auto& shard = MsrAot::kShards[s];
        for (u32 i = 0; i < *shard.count; ++i) {
            const auto& entry = shard.entries[i];
            if (!entry.handler || entry.pc < MsrAot::kTextBase ||
                entry.pc - MsrAot::kTextBase >= MsrAot::kTextSize) {
                continue;
            }
            if (entry.thumb) {
                thumb_handlers.emplace(entry.pc, entry.handler);
            } else {
                arm_handlers[(entry.pc - MsrAot::kTextBase) / 4] = entry.handler;
            }
            ++entries;
        }
    }
    if (core_id == 0) {
        LOG_INFO(Core_ARM11, "Recompiled code: {} entry points in {} shards", entries,
                 MsrAot::kShardCount);
    }
}

ARM_AOT::~ARM_AOT() = default;

MsrAot::Handler ARM_AOT::Lookup(u32 pc, u32 thumb) const {
    const u32 offset = pc - MsrAot::kTextBase;
    if (offset >= MsrAot::kTextSize) {
        return nullptr;
    }
    if (thumb) {
        const auto it = thumb_handlers.find(pc);
        return it != thumb_handlers.end() ? it->second : nullptr;
    }
    return (pc & 3) ? nullptr : arm_handlers[offset / 4];
}

bool ARM_AOT::CheckProgram() {
    // Hashed as generated: instructions the player may have rewritten (the frame limit, kept in
    // save states too) count with their original encodings.
    CryptoPP::SHA256 sha;
    std::array<u8, Memory::CITRA_PAGE_SIZE> copy;
    for (u32 offset = 0; offset < MsrAot::kTextSize; offset += Memory::CITRA_PAGE_SIZE) {
        const u32 address = MsrAot::kTextBase + offset;
        const u8* page = memory.GetPointer(address);
        if (!page) {
            LOG_INFO(Core_ARM11, "Recompiled code unused: no program text at 0x{:08X}", address);
            return false;
        }
        const u32 length = std::min<u32>(Memory::CITRA_PAGE_SIZE, MsrAot::kTextSize - offset);
        for (u32 i = 0; i < MsrAot::kPatchableCount; ++i) {
            const auto& patch = MsrAot::kPatchable[i];
            if (patch.address >= address && patch.address + patch.size <= address + length) {
                if (page != copy.data()) {
                    std::memcpy(copy.data(), page, length);
                    page = copy.data();
                }
                std::memcpy(copy.data() + (patch.address - address), &patch.original, patch.size);
            }
        }
        sha.Update(page, length);
    }
    u8 digest[CryptoPP::SHA256::DIGESTSIZE];
    sha.Final(digest);
    const bool match = std::memcmp(digest, MsrAot::kCodeSha256, sizeof(digest)) == 0;
    if (GetID() == 0) {
        LOG_INFO(Core_ARM11, "Recompiled code {}", match ? "matches the program: running it"
                                                       : "is for another program: using the JIT");
    }
    return match;
}

void ARM_AOT::LoadFromJit() {
    for (int i = 0; i < 16; ++i) {
        ctx.r[i] = jit->GetReg(i);
    }
    const u32 cpsr = jit->GetCPSR();
    MsrAot::SetCpsrFlags(ctx, cpsr, false);
    ctx.thumb = (cpsr >> 5) & 1;
    ctx.cpsr_rest = cpsr & ~0xF80F0020u;
    for (int i = 0; i < 32; ++i) {
        ctx.s[i] = jit->GetVFPReg(i);
    }
    ctx.fpscr = jit->GetVFPSystemReg(VFP_FPSCR);
    ctx.tls_ro = jit->GetCP15Register(CP15_THREAD_URO);
    ctx.tls_rw = jit->GetCP15Register(CP15_THREAD_UPRW);
    ctx.excl_valid = 0;
}

void ARM_AOT::StoreToJit() {
    for (int i = 0; i < 16; ++i) {
        jit->SetReg(i, ctx.r[i]);
    }
    jit->SetCPSR(MsrAot::Cpsr(ctx));
    for (int i = 0; i < 32; ++i) {
        jit->SetVFPReg(i, ctx.s[i]);
    }
    jit->SetVFPSystemReg(VFP_FPSCR, ctx.fpscr);
    jit->SetCP15Register(CP15_THREAD_UPRW, ctx.tls_rw);
}

void ARM_AOT::Run() {
    if (break_flag) [[unlikely]] {
        return;
    }
    if (program_checked == 0) {
        program_checked = CheckProgram() ? 1 : -1;
    }
    if (program_checked < 0 || !ctx.pages) {
        jit->Run();
        return;
    }
    auto& timer = GetTimer();
    reschedule = false;
    LoadFromJit();
    GuestFpControl fp_control(ctx.fpscr);
    // Ticks accumulate across functions and settle with the timer only when leaving the loop
    // (system calls and the JIT read and advance it), keeping each dispatch to a table lookup.
    const auto settle = [&] {
        timer.AddTicks(std::max<u64>(ctx.ticks, 1));
        aot_instructions += ctx.ticks;
        ctx.ticks = 0;
    };
    ctx.ticks = 0;
    while (!reschedule) {
        const s64 downcount = timer.GetDowncount();
        if (downcount <= 0) {
            break;
        }
        ctx.budget = static_cast<u64>(downcount);
        ctx.exit = MsrAot::Exit::Branch;
        MsrAot::Handler handler;
        while ((handler = Lookup(ctx.r[15], ctx.thumb)) != nullptr) {
            handler(ctx);
            if (ctx.exit != MsrAot::Exit::Branch || ctx.ticks >= ctx.budget) {
                break;
            }
        }
        settle();
        if (!handler) {
            fp_control.Leave();
            Interpret();
            fp_control.Enter(ctx.fpscr);
        } else if (ctx.exit == MsrAot::Exit::Svc) {
            ++svc_count;
            fp_control.Leave();
            StoreToJit();
            svc->CallSVC(ctx.svc);
            LoadFromJit();
            fp_control.Enter(ctx.fpscr);
        } else if (ctx.exit == MsrAot::Exit::Interpret || ctx.exit == MsrAot::Exit::Patched) {
            fp_control.Leave();
            Interpret(ctx.exit == MsrAot::Exit::Interpret);
            fp_control.Enter(ctx.fpscr);
        }
    }
    fp_control.Leave();
    StoreToJit();
    ReportStats();
}

void ARM_AOT::Interpret(bool missing) {
    StoreToJit();
    if (missing) {
        ++jit_hot[ctx.r[15] | ctx.thumb];
    }
    for (int i = 0; i < 4096; ++i) {
        jit->Step();
        ++jit_steps;
        if (reschedule || GetTimer().GetDowncount() <= 0) {
            break;
        }
        if (Lookup(jit->GetPC(), (jit->GetCPSR() >> 5) & 1)) {
            break;
        }
    }
    LoadFromJit();
}

void ARM_AOT::ReportStats() {
    const u64 now = GetTimer().GetTicks();
    if (now < next_report) {
        return;
    }
    if (next_report != 0) {
        const u64 total = aot_instructions + jit_steps;
        std::vector<std::pair<u64, u32>> hot;
        for (const auto& [pc, count] : jit_hot) {
            hot.emplace_back(count, pc);
        }
        std::sort(hot.rbegin(), hot.rend());
        std::string top;
        for (std::size_t i = 0; i < std::min<std::size_t>(hot.size(), 8); ++i) {
            top += fmt::format(" {:08X}x{}", hot[i].second, hot[i].first);
        }
        if (!hot.empty()) {
            // Entry points to add to the next build (generate.py --extra this file).
            const std::string path = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) +
                                     fmt::format("msr/recomp-jit-entries-core{}.txt", GetID());
            if (FileUtil::IOFile file(path, "w"); file.IsOpen()) {
                std::string text = "# Recompiled build: addresses run on the JIT (odd = Thumb), "
                                   "times entered\n";
                for (const auto& [count, pc] : hot) {
                    text += fmt::format("0x{:08X} {}\n", pc, count);
                }
                file.WriteString(text);
            }
        }
        LOG_INFO(Core_ARM11,
                 "Core {}: {:.3f}% of instructions recompiled ({} recompiled, {} on the JIT, {} "
                 "system calls); JIT entries:{}",
                 GetID(), total ? 100.0 * aot_instructions / total : 0.0, aot_instructions,
                 jit_steps, svc_count, top);
    }
    next_report = now + ReportInterval;
}

void ARM_AOT::Step() {
    jit->Step();
}
void ARM_AOT::ClearInstructionCache() {
    jit->ClearInstructionCache();
}
void ARM_AOT::InvalidateCacheRange(u32 start_address, std::size_t length) {
    jit->InvalidateCacheRange(start_address, length);
}
void ARM_AOT::ClearExclusiveState() {
    ctx.excl_valid = 0;
    jit->ClearExclusiveState();
}
void ARM_AOT::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table_) {
    // Loading a save state replaces the timer just before this: the JIT follows it.
    jit->SetTimer(timer);
    page_table = page_table_;
    ctx.pages = page_table ? page_table->GetPointerArray().data() : nullptr;
    jit->SetPageTable(page_table_);
}
std::shared_ptr<Memory::PageTable> ARM_AOT::GetPageTable() const {
    return page_table;
}
void ARM_AOT::SetPC(u32 addr) {
    jit->SetPC(addr);
}
u32 ARM_AOT::GetPC() const {
    return jit->GetPC();
}
u32 ARM_AOT::GetReg(int index) const {
    return jit->GetReg(index);
}
void ARM_AOT::SetReg(int index, u32 value) {
    jit->SetReg(index, value);
}
u32 ARM_AOT::GetVFPReg(int index) const {
    return jit->GetVFPReg(index);
}
void ARM_AOT::SetVFPReg(int index, u32 value) {
    jit->SetVFPReg(index, value);
}
u32 ARM_AOT::GetVFPSystemReg(VFPSystemRegister reg) const {
    return jit->GetVFPSystemReg(reg);
}
void ARM_AOT::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    jit->SetVFPSystemReg(reg, value);
}
u32 ARM_AOT::GetCPSR() const {
    return jit->GetCPSR();
}
void ARM_AOT::SetCPSR(u32 cpsr) {
    jit->SetCPSR(cpsr);
}
u32 ARM_AOT::GetCP15Register(CP15Register reg) const {
    return jit->GetCP15Register(reg);
}
void ARM_AOT::SetCP15Register(CP15Register reg, u32 value) {
    jit->SetCP15Register(reg, value);
}
void ARM_AOT::SaveContext(ThreadContext& context) {
    jit->SaveContext(context);
}
void ARM_AOT::LoadContext(const ThreadContext& context) {
    jit->LoadContext(context);
}
void ARM_AOT::PrepareReschedule() {
    reschedule = true;
    jit->PrepareReschedule();
}

} // namespace Core
