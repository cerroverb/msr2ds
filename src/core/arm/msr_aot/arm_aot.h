// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include "core/arm/arm_interface.h"
#include "core/arm/msr_aot/aot_runtime.h"

namespace Kernel {
class SVCContext;
}

namespace Core {

class ARM_Dynarmic;
class ExclusiveMonitor;
class System;

/// CPU backend that runs Samus Returns' ahead-of-time recompiled code (tools/msr/recomp).
///
/// It wraps the dynarmic JIT, which keeps the canonical guest state: every accessor, context
/// switch and save state goes through it unchanged. Run() copies the state into the recompiled
/// code's context and dispatches from function to function until a system call, an
/// instruction left to the interpreter, a reschedule or the end of the time slice; code without
/// a recompiled entry point runs on the JIT one instruction at a time until it reaches one.
/// The recompiled code is used only when the loaded program's text matches the hash it was
/// generated from; anything else runs entirely on the JIT.
class ARM_AOT final : public ARM_Interface {
public:
    ARM_AOT(Core::System& system, Memory::MemorySystem& memory, u32 core_id,
            std::shared_ptr<Core::Timing::Timer> timer, Core::ExclusiveMonitor& exclusive_monitor);
    ~ARM_AOT() override;

    /// Whether this build carries recompiled code and MSR_AOT is not set to 0.
    static bool Available();

    void Run() override;
    void Step() override;
    void ClearInstructionCache() override;
    void InvalidateCacheRange(u32 start_address, std::size_t length) override;
    void ClearExclusiveState() override;
    void SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) override;
    void SetPC(u32 addr) override;
    u32 GetPC() const override;
    u32 GetReg(int index) const override;
    void SetReg(int index, u32 value) override;
    u32 GetVFPReg(int index) const override;
    void SetVFPReg(int index, u32 value) override;
    u32 GetVFPSystemReg(VFPSystemRegister reg) const override;
    void SetVFPSystemReg(VFPSystemRegister reg, u32 value) override;
    u32 GetCPSR() const override;
    void SetCPSR(u32 cpsr) override;
    u32 GetCP15Register(CP15Register reg) const override;
    void SetCP15Register(CP15Register reg, u32 value) override;
    void SaveContext(ThreadContext& ctx) override;
    void LoadContext(const ThreadContext& ctx) override;
    void PrepareReschedule() override;
    bool HasSingleInstructionBreakAccuracy() override {
        return false;
    }

protected:
    std::shared_ptr<Memory::PageTable> GetPageTable() const override;

private:
    MsrAot::Handler Lookup(u32 pc, u32 thumb) const;
    bool CheckProgram();
    void LoadFromJit();
    void StoreToJit();
    /// Runs the JIT up to the next recompiled PC. `missing`: code the build lacks, which the
    /// report lists for the next build (a patched instruction is not).
    void Interpret(bool missing = true);
    void ReportStats();

    Core::System& system;
    Memory::MemorySystem& memory;
    std::unique_ptr<ARM_Dynarmic> jit;
    std::unique_ptr<Kernel::SVCContext> svc;
    std::shared_ptr<Memory::PageTable> page_table;

    MsrAot::Ctx ctx;
    bool reschedule = false;
    int program_checked = 0; ///< 0 not yet, 1 matches, -1 another program
    std::vector<MsrAot::Handler> arm_handlers; ///< By (pc - text base) / 4
    std::unordered_map<u32, MsrAot::Handler> thumb_handlers;

    // Statistics (logged periodically): instructions run recompiled and on the JIT.
    u64 aot_instructions = 0;
    u64 jit_steps = 0;
    u64 svc_count = 0;
    u64 next_report = 0;
    std::unordered_map<u32, u64> jit_hot; ///< PCs that fell back to the JIT, for the report
};

} // namespace Core
