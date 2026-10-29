// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <vector>
#include "common/archives.h"
#include "common/hacks/hack_manager.h"
#include "common/microprofile.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/loader/loader.h"
#include "msr/cheats.h"
#include "msr/frame_rate.h"
#include "msr/presentation.h"
#include "msr/suit_swap.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "video_core/gpu_debugger.h"
#include "video_core/gpu_impl.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_software/sw_blitter.h"
#include "video_core/right_eye_disabler.h"
#include "video_core/video_core.h"

namespace VideoCore {

constexpr VAddr VADDR_LCD = 0x1ED02000;
constexpr VAddr VADDR_GPU = 0x1EF00000;

class DelayGenerator {
private:
    DelayGenerator() = default;

    // Average transfer speed based on measurements taken from real
    // hardware. 4 different modes have been taken into consideration:
    // RAM -> RAM, RAM -> VRAM, VRAM -> RAM and VRAM -> VRAM.
    // Furthermore, measurements are split into DMA transfers and tex
    // copies. For simplicity, we will assume fills are as fast as
    // texture copies.

    static constexpr double mibps_to_ns_per_byte(double mib_per_sec) {
        return 1'000'000'000.0 / (mib_per_sec * 1024.0 * 1024.0);
    }

    static constexpr std::array<std::array<double, 4>, 2> speed_mibps = {
        {{
             190.0, // DMA RAMTORAM
             310.0, // DMA RAMTOVRAM
             380.0, // DMA VRAMTORAM
             380.0, // DMA VRAMTOVRAM
         },
         {
             450.0,  // TEX RAMTORAM
             3100.0, // TEX RAMTOVRAM
             5400.0, // TEX VRAMTORAM
             5400.0, // TEX VRAMTOVRAM
         }}};

public:
    enum class CopyMode {
        RAMTORAM,
        RAMTOVRAM,
        VRAMTORAM,
        VRAMTOVRAM,
    };

    static CopyMode GetCopyMode(bool input_vram, bool output_vram) {
        if (!input_vram && !output_vram) {
            return CopyMode::RAMTORAM;
        } else if (!input_vram && output_vram) {
            return CopyMode::RAMTOVRAM;
        } else if (input_vram && !output_vram) {
            return CopyMode::VRAMTORAM;
        } else {
            return CopyMode::VRAMTOVRAM;
        }
    }

    static u64 CalculateDelayNanoseconds(CopyMode mode, bool is_textre, size_t size) {
        double base_ns_per_byte =
            mibps_to_ns_per_byte(speed_mibps[is_textre][static_cast<u32>(mode)]);

        return static_cast<u64>(size * base_ns_per_byte);
    }
};

MICROPROFILE_DEFINE(GPU_DisplayTransfer, "GPU", "DisplayTransfer", MP_RGB(100, 100, 255));
MICROPROFILE_DEFINE(GPU_CmdlistProcessing, "GPU", "Cmdlist Processing", MP_RGB(100, 255, 100));

GPU::GPU(Core::System& system, Frontend::EmuWindow& emu_window,
         Frontend::EmuWindow* secondary_window)
    : right_eye_disabler{std::make_unique<RightEyeDisabler>(*this)},
      impl{std::make_unique<Impl>(system, emu_window, secondary_window)} {
    impl->vblank_event = impl->timing.RegisterEvent(
        "GPU::VBlankCallback",
        [this](uintptr_t user_data, s64 cycles_late) { VBlankCallback(user_data, cycles_late); });
    impl->timing.ScheduleEvent(FRAME_TICKS, impl->vblank_event);

    // Bind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);
}

GPU::~GPU() = default;

PAddr GPU::VirtualToPhysicalAddress(VAddr addr) {
    if (addr == 0) {
        return 0;
    }

    if (addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END) {
        return addr - Memory::VRAM_VADDR + Memory::VRAM_PADDR;
    }
    if (addr >= Memory::LINEAR_HEAP_VADDR && addr <= Memory::LINEAR_HEAP_VADDR_END) {
        return addr - Memory::LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    if (addr >= Memory::NEW_LINEAR_HEAP_VADDR && addr <= Memory::NEW_LINEAR_HEAP_VADDR_END) {
        return addr - Memory::NEW_LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    PAddr plg_fb_addr;
    if (addr >= Memory::PLUGIN_3GX_FB_VADDR && addr <= Memory::PLUGIN_3GX_FB_VADDR_END &&
        (plg_fb_addr = impl->system.Memory().Plugin3GXFramebufferAddress())) {
        return addr - Memory::PLUGIN_3GX_FB_VADDR + plg_fb_addr;
    }

    LOG_ERROR(HW_Memory, "Unknown virtual address @ 0x{:08X}", addr);
    return addr;
}

void GPU::SetInterruptHandler(Service::GSP::InterruptHandler handler) {
    impl->signal_interrupt = handler;
    impl->pica.SetInterruptHandler(handler);
}

void GPU::FlushRegion(PAddr addr, u32 size) {
    impl->rasterizer->FlushRegion(addr, size);
}

void GPU::InvalidateRegion(PAddr addr, u32 size) {
    impl->rasterizer->InvalidateRegion(addr, size);
}

void GPU::ClearAll(bool flush) {
    impl->rasterizer->ClearAll(flush);
}

void GPU::Execute(const Service::GSP::Command& command) {
    using Service::GSP::CommandId;
    auto& regs = impl->pica.regs;

    switch (command.id) {
    case CommandId::RequestDma: {
        impl->system.Memory().RasterizerFlushVirtualRegion(
            command.dma_request.source_address, command.dma_request.size, Memory::FlushMode::Flush);
        impl->system.Memory().RasterizerFlushVirtualRegion(command.dma_request.dest_address,
                                                           command.dma_request.size,
                                                           Memory::FlushMode::Invalidate);

        // TODO(Subv): These memory accesses should not go through the application's memory mapping.
        // They should go through the GSP module's memory mapping.
        const auto process = impl->system.Kernel().GetCurrentProcess();
        impl->memory.CopyBlock(*process, command.dma_request.dest_address,
                               command.dma_request.source_address, command.dma_request.size);

        auto is_vram = [&](u32 addr) {
            return addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END;
        };

        u64 delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(is_vram(command.dma_request.source_address),
                                        is_vram(command.dma_request.dest_address)),
            false, command.dma_request.size);

        impl->signal_interrupt(Service::GSP::InterruptId::DMA, delay);
        break;
    }
    case CommandId::SubmitCmdList: {
        auto& params = command.submit_gpu_cmdlist;
        auto& cmdbuffer = regs.internal.pipeline.command_buffer;

        // Write to the command buffer GPU registers
        cmdbuffer.addr[0].Assign(VirtualToPhysicalAddress(params.address) >> 3);
        cmdbuffer.size[0].Assign(params.size >> 3);
        cmdbuffer.trigger[0] = 1;

        // Trigger processing of the command list
        SubmitCmdList(0);
        break;
    }
    case CommandId::MemoryFill: {
        auto& params = command.memory_fill;
        auto& memfill = regs.memory_fill_config;

        // Write to the memory fill GPU registers.
        // If both buffers are set GSP dispatches PSC0 only.
        const bool has_both_bufs = params.start1 != 0 && params.start2 != 0;
        if (params.start1 != 0) {
            memfill[0].address_start = VirtualToPhysicalAddress(params.start1) >> 3;
            memfill[0].address_end = VirtualToPhysicalAddress(params.end1) >> 3;
            memfill[0].value_32bit = params.value1;
            memfill[0].control = params.control1;
            MemoryFill(0, has_both_bufs ? std::numeric_limits<u32>::max() : 0);
        }
        if (params.start2 != 0) {
            memfill[1].address_start = VirtualToPhysicalAddress(params.start2) >> 3;
            memfill[1].address_end = VirtualToPhysicalAddress(params.end2) >> 3;
            memfill[1].value_32bit = params.value2;
            memfill[1].control = params.control2;
            MemoryFill(1, has_both_bufs ? 0 : 1);
        }
        break;
    }
    case CommandId::DisplayTransfer: {
        auto& params = command.display_transfer;
        auto& display_transfer = regs.display_transfer_config;

        // Write to the transfer engine GPU registers.
        display_transfer.input_address = VirtualToPhysicalAddress(params.in_buffer_address) >> 3;
        display_transfer.output_address = VirtualToPhysicalAddress(params.out_buffer_address) >> 3;
        display_transfer.input_size = params.in_buffer_size;
        display_transfer.output_size = params.out_buffer_size;
        display_transfer.flags = params.flags;
        display_transfer.trigger.Assign(1);

        // Trigger the display transfer.
        MemoryTransfer();
        break;
    }
    case CommandId::TextureCopy: {
        auto& params = command.texture_copy;
        auto& texture_copy = regs.display_transfer_config;

        // Write to the transfer engine GPU registers.
        texture_copy.input_address = VirtualToPhysicalAddress(params.in_buffer_address) >> 3;
        texture_copy.output_address = VirtualToPhysicalAddress(params.out_buffer_address) >> 3;
        texture_copy.texture_copy.size = params.size;
        texture_copy.texture_copy.input_size = params.in_width_gap;
        texture_copy.texture_copy.output_size = params.out_width_gap;
        texture_copy.flags = params.flags;
        texture_copy.trigger.Assign(1);

        // Trigger the texture copy.
        MemoryTransfer();
        break;
    }
    case CommandId::CacheFlush: {
        // Rasterizer flushing handled elsewhere in CPU read/write and other GPU handlers
        // Use command.cache_flush.regions to implement this handler
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Unknown command {:#08X}", command.id.Value());
    }

    // Notify debugger that a GSP command was processed.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::GSPCommandProcessed, &command);
    }
}

void GPU::SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info) {
    const PAddr phys_address_left = VirtualToPhysicalAddress(info.address_left);
    const PAddr phys_address_right = VirtualToPhysicalAddress(info.address_right);

    // Update framebuffer properties.
    auto& framebuffer = impl->pica.regs.framebuffer_config[screen_id];
    if (info.active_fb == 0) {
        framebuffer.address_left1 = phys_address_left;
        framebuffer.address_right1 = phys_address_right;
    } else {
        framebuffer.address_left2 = phys_address_left;
        framebuffer.address_right2 = phys_address_right;
    }

    framebuffer.stride = info.stride;
    framebuffer.format = info.format;
    framebuffer.active_fb = info.shown_fb;

    // Notify debugger about the buffer swap.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::BufferSwapped, nullptr);
    }

    if (screen_id == 0) {
        MicroProfileFlip();
        MSR::FrameRate::game_frames.fetch_add(1, std::memory_order_relaxed);
        impl->system.perf_stats->EndGameFrame();
        right_eye_disabler->ReportEndFrame();
    }
}

void GPU::SetColorFill(const Pica::ColorFill& fill) {
    impl->pica.regs_lcd.color_fill_top = fill;
    impl->pica.regs_lcd.color_fill_bottom = fill;
}

u32 GPU::ReadReg(VAddr addr) {
    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        return impl->pica.regs_lcd[index];
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        return impl->pica.regs.reg_array[index];
    }
    default:
        UNREACHABLE_MSG("Read from unknown GPU address {:#08X}", addr);
    }
}

void GPU::WriteReg(VAddr addr, u32 data) {
    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        impl->pica.regs_lcd[index] = data;
        break;
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);

        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        impl->pica.regs.reg_array[index] = data;

        // Handle registers that trigger GPU actions
        switch (index) {
        case GPU_REG_INDEX(memory_fill_config[0].trigger):
            MemoryFill(0, 0);
            break;
        case GPU_REG_INDEX(memory_fill_config[1].trigger):
            MemoryFill(1, 1);
            break;
        case GPU_REG_INDEX(display_transfer_config.trigger):
            MemoryTransfer();
            break;
        case GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[0]):
            SubmitCmdList(0);
            break;
        case GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[1]):
            SubmitCmdList(1);
            break;
        default:
            break;
        }
        break;
    }
    default:
        UNREACHABLE_MSG("Write to unknown GPU address {:#08X}", addr);
    }
}

VideoCore::RendererBase& GPU::Renderer() {
    return *impl->renderer;
}

Pica::PicaCore& GPU::PicaCore() {
    return impl->pica;
}

const Pica::PicaCore& GPU::PicaCore() const {
    return impl->pica;
}

GraphicsDebugger& GPU::Debugger() {
    return impl->gpu_debugger;
}

void GPU::ApplyPerProgramSettings(u64 program_ID) {
    auto hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::ACCURATE_MULTIPLICATION, program_ID);
    bool use_accurate_mul = Settings::values.shaders_accurate_mul.GetValue();
    if (hack) {
        switch (hack->mode) {
        case Common::Hacks::HackAllowMode::DISALLOW:
            use_accurate_mul = false;
            break;
        case Common::Hacks::HackAllowMode::FORCE:
            use_accurate_mul = true;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    impl->rasterizer->SetAccurateMul(use_accurate_mul);
}

void GPU::SubmitCmdList(u32 index) {
    // Check if a command list was triggered.
    auto& config = impl->pica.regs.internal.pipeline.command_buffer;
    if (!config.trigger[index]) {
        return;
    }

    MICROPROFILE_SCOPE(GPU_CmdlistProcessing);

    // Forward command list processing to the PICA core.
    const PAddr addr = config.GetPhysicalAddress(index);
    const u32 size = config.GetSize(index);
    impl->pica.ProcessCmdList(addr, size,
                              !right_eye_disabler->ShouldAllowCmdQueueTrigger(addr, size));
    config.trigger[index] = 0;
}

void GPU::MemoryFill(u32 index, u32 intr_index) {
    // Check if a memory fill was triggered.
    auto& config = impl->pica.regs.memory_fill_config[index];
    if (!config.trigger) {
        return;
    }

    // Perform memory fill.
    if (!impl->rasterizer->AccelerateFill(config)) {
        impl->sw_blitter->MemoryFill(config);
    }

    // Treat fill as texture transfer from VRAM
    u64 delay = DelayGenerator::CalculateDelayNanoseconds(
        DelayGenerator::GetCopyMode(true, config.IsVRAM()), true,
        config.GetEndAddress() - config.GetStartAddress());

    // It seems that it won't signal interrupt if "address_start" is zero.
    // TODO: hwtest this
    if (config.GetStartAddress() != 0) {
        if (intr_index == 0) {
            impl->signal_interrupt(Service::GSP::InterruptId::PSC0, delay);
        } else if (intr_index == 1) {
            impl->signal_interrupt(Service::GSP::InterruptId::PSC1, delay);
        }
    }

    // Reset "trigger" flag and set the "finish" flag
    // This was confirmed to happen on hardware even if "address_start" is zero.
    config.trigger.Assign(0);
    config.finished.Assign(1);
}

void GPU::MemoryTransfer() {
    // Check if a transfer was triggered.
    auto& config = impl->pica.regs.display_transfer_config;
    if (!config.trigger.Value()) {
        return;
    }

    MICROPROFILE_SCOPE(GPU_DisplayTransfer);

    // Notify debugger about the display transfer.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::IncomingDisplayTransfer, nullptr);
    }

    u64 delay{};
    // Perform memory transfer
    if (config.is_texture_copy) {
        if (!impl->rasterizer->AccelerateTextureCopy(config)) {
            impl->sw_blitter->TextureCopy(config);
        }
        delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
            config.texture_copy.size);
    } else {
        if (right_eye_disabler->ShouldAllowDisplayTransfer(config.GetPhysicalInputAddress(),
                                                           config.input_height)) {
            if (!impl->rasterizer->AccelerateDisplayTransfer(config)) {
                impl->sw_blitter->DisplayTransfer(config);
            }
        }
        delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
            config.input_width * config.input_height * BytesPerPixel(config.input_format));
    }

    // Complete transfer.
    config.trigger.Assign(0);
    impl->signal_interrupt(Service::GSP::InterruptId::PPF, delay);
}

void GPU::VBlankCallback(std::uintptr_t user_data, s64 cycles_late) {
    // Signal to GSP that GPU interrupt has occurred
    impl->signal_interrupt(Service::GSP::InterruptId::PDC0, 0);
    impl->signal_interrupt(Service::GSP::InterruptId::PDC1, 0);

    // Present renderered frame.
    impl->renderer->SwapBuffers();

    // Samus Returns at the player's frame rate (msr/frame_rate.h): the game's frame limit
    // follows it, and the display refreshes at least as often as the game draws.
    u64 frame_ticks = FRAME_TICKS;
    if (const int fps = MSR::FrameRate::target.load(std::memory_order_relaxed); fps != 0) {
        ApplyMsrFrameLimit(fps);
        if (fps > 60) {
            frame_ticks = BASE_CLOCK_RATE_ARM11 / static_cast<u64>(fps);
        }
        ApplyMsrCheats();
        ApplyMsrRefills();
        ApplyMsrSuitSwap();
    }

    // Reschedule recurrent event
    impl->timing.ScheduleEvent(frame_ticks - cycles_late, impl->vblank_event);
}

void GPU::ApplyMsrFrameLimit(int fps) {
    namespace FrameRate = MSR::FrameRate;
    const auto process = impl->system.Kernel().GetCurrentProcess();
    auto& memory = impl->memory;
    if (!process || !memory.IsValidVirtualAddress(*process, FrameRate::LimitCompare)) {
        return;
    }
    // Rewritten only where the limit's instructions are found: the save states of other rates
    // bring their own limit back, other programs and versions are left alone.
    const u32 compare = memory.Read32(FrameRate::LimitCompare);
    const u32 subtract = memory.Read32(FrameRate::LimitSubtract);
    if ((compare & ~0xFFu) != FrameRate::CompareBase ||
        (subtract & ~0xFFu) != FrameRate::SubtractBase ||
        memory.Read32(FrameRate::LimitBranch) != FrameRate::BranchWord) {
        return;
    }
    const u32 limit = FrameRate::LimitMs(fps);
    if ((compare & 0xFF) != limit || (subtract & 0xFF) != limit) {
        memory.Write32(FrameRate::LimitCompare, FrameRate::CompareBase | limit);
        memory.Write32(FrameRate::LimitSubtract, FrameRate::SubtractBase | limit);
        impl->system.InvalidateCacheRange(FrameRate::LimitCompare,
                                          FrameRate::LimitSubtract + 4 - FrameRate::LimitCompare);
    }

    // The interface's fixed 1/30 s steps follow the measured frame time above 30 FPS.
    for (const auto& patch : FrameRate::StepPatches) {
        const u32 word = memory.Read32(patch.address);
        const u32 wanted = fps > 30 ? patch.replacement : patch.original;
        if (word != wanted && (word == patch.original || word == patch.replacement)) {
            memory.Write32(patch.address, wanted);
            impl->system.InvalidateCacheRange(patch.address, 4);
        }
    }
}

void GPU::ApplyMsrCheats() {
    namespace Cheats = MSR::Cheats;
    const auto process = impl->system.Kernel().GetCurrentProcess();
    auto& memory = impl->memory;
    const u32 enabled = Cheats::enabled.load(std::memory_order_relaxed);
    for (const auto& patch : Cheats::Patches) {
        if (!process || !memory.IsValidVirtualAddress(*process, patch.address)) {
            return;
        }
        // Only where the game's instruction or its replacement is found, as for the frame limit.
        const u32 word = memory.Read32(patch.address);
        const u32 wanted = (enabled & patch.flag) != 0 ? patch.replacement : patch.original;
        if (word != wanted && (word == patch.original || word == patch.replacement)) {
            memory.Write32(patch.address, wanted);
            impl->system.InvalidateCacheRange(patch.address, 4);
        }
    }
}

namespace {

/// Objects of one class on the game's heaps, by their vtable (~6 ms for all of them): those of
/// `size` bytes whose words `fits` accepts (`word(offset)` reads the object), up to `limit`.
template <typename Fits>
std::vector<u32> FindMsrObjects(const Kernel::Process& process, u32 vtable, u32 size, Fits fits,
                                std::size_t limit) {
    std::vector<u32> found;
    for (const auto& [base, vma] : process.vm_manager.vma_map) {
        if (vma.type != Kernel::VMAType::BackingMemory || base < 0x08000000 || vma.size < size ||
            !vma.backing_memory) {
            continue;
        }
        const u8* data = vma.backing_memory.GetPtr();
        for (u32 offset = 0; offset + size <= vma.size; offset += 4) {
            const auto word = [data, offset](u32 field) {
                u32 value;
                std::memcpy(&value, data + offset + field, sizeof(value));
                return value;
            };
            if (word(0) != vtable || !fits(word)) {
                continue;
            }
            found.push_back(base + offset);
            if (found.size() == limit) {
                return found;
            }
        }
    }
    return found;
}

/// Samus's model components (Samus and the morph ball), by their vtable.
std::vector<u32> FindMsrModelComponents(const Kernel::Process& process) {
    namespace Swap = MSR::SuitSwap;
    return FindMsrObjects(
        process, Swap::ComponentVtable, Swap::ComponentSize,
        [](const auto& word) {
            const u32 count = word(Swap::ComponentCountOffset);
            return count != 0 && count <= 8 && word(Swap::ComponentCapacityOffset) >= count;
        },
        Swap::MaxComponents);
}

/// The components found earlier are still there (an area load replaces them).
bool MsrComponentsCurrent(Memory::MemorySystem& memory, const Kernel::Process& process,
                          const std::vector<u32>& components) {
    namespace Swap = MSR::SuitSwap;
    for (const u32 component : components) {
        if (!memory.IsValidVirtualAddress(process, component) ||
            !memory.IsValidVirtualAddress(process, component + Swap::ComponentSize - 4) ||
            memory.Read32(component) != Swap::ComponentVtable) {
            return false;
        }
        const u32 count = memory.Read32(component + Swap::ComponentCountOffset);
        if (count == 0 || count > 8) {
            return false;
        }
    }
    return !components.empty();
}

/// Whether a component's models are the Fusion ones, from its first model's file
/// (actors/characters/samusfusion/...): 1 or 0, -1 when it cannot be read.
int MsrModelsFusion(Memory::MemorySystem& memory, const Kernel::Process& process, u32 component) {
    namespace Swap = MSR::SuitSwap;
    const auto valid = [&](u32 address) { return memory.IsValidVirtualAddress(process, address); };
    const u32 entries = memory.Read32(component + Swap::ComponentEntriesOffset);
    if (!valid(entries + Swap::EntryPathOffset)) {
        return -1;
    }
    const u32 path = memory.Read32(entries + Swap::EntryPathOffset);
    if (!valid(path)) {
        return -1;
    }
    const u32 text = memory.Read32(path);
    if (!valid(text)) {
        return -1;
    }
    const std::string file = memory.ReadCString(text, 96);
    if (!file.starts_with("actors/characters/")) {
        return -1;
    }
    return file.find("fusion") != std::string::npos ? 1 : 0;
}

/// The name hashes of a model instance's bones, in index order (empty when unreadable).
std::vector<u32> MsrBoneHashes(Memory::MemorySystem& memory, const Kernel::Process& process,
                               u32 instance) {
    namespace Swap = MSR::SuitSwap;
    const auto valid = [&](u32 address) { return memory.IsValidVirtualAddress(process, address); };
    std::vector<u32> hashes;
    const u32 skeleton = memory.Read32(instance + Swap::InstanceSkeletonOffset);
    if (!valid(skeleton + 0x44)) {
        return hashes;
    }
    const u32 data = memory.Read32(skeleton + 0x44);
    if (!valid(data + 0x3C)) {
        return hashes;
    }
    const u32 table = memory.Read32(data + 0x3C);
    if (!valid(table + 4)) {
        return hashes;
    }
    const u32 count = memory.Read32(table);
    const u32 bones = memory.Read32(table + 4);
    if (count > 512 || !valid(bones) || !valid(bones + count * Swap::BoneSize - 4)) {
        return hashes;
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 name = memory.Read32(bones + i * Swap::BoneSize + Swap::BoneNameOffset);
        hashes.push_back(valid(name + 0x10) ? memory.Read32(name + 0x10) : 0);
    }
    return hashes;
}

/// The models hanging from the components' bones: their scene nodes and the bones' name hashes.
std::vector<std::pair<u32, u32>> MsrBoneAttachments(Memory::MemorySystem& memory,
                                                    const Kernel::Process& process,
                                                    const std::vector<u32>& components) {
    namespace Swap = MSR::SuitSwap;
    std::vector<std::pair<u32, u32>> found;
    for (const u32 component : components) {
        const u32 instance = memory.Read32(component + Swap::ComponentInstanceOffset);
        if (instance == 0 || !memory.IsValidVirtualAddress(process, instance)) {
            continue;
        }
        const auto bones = MsrBoneHashes(memory, process, instance);
        u32 child = memory.Read32(instance + Swap::InstanceNodeOffset + Swap::NodeFirstChildOffset);
        for (int guard = 0; child != 0 && guard < 256; ++guard) {
            if (!memory.IsValidVirtualAddress(process, child + Swap::NodeBoneOffset)) {
                break;
            }
            const s32 bone = static_cast<s32>(memory.Read32(child + Swap::NodeBoneOffset));
            if (bone >= 0 && static_cast<std::size_t>(bone) < bones.size()) {
                found.emplace_back(child, bones[bone]);
            }
            child = memory.Read32(child + Swap::NodeNextOffset);
        }
    }
    return found;
}

/// Hangs the recorded models from the bones of the same names in the components' new skeletons.
void MsrReattach(Memory::MemorySystem& memory, const Kernel::Process& process,
                 const std::vector<u32>& components,
                 const std::vector<std::pair<u32, u32>>& attachments) {
    namespace Swap = MSR::SuitSwap;
    int moved = 0;
    int missing = 0;
    for (const u32 component : components) {
        const u32 instance = memory.Read32(component + Swap::ComponentInstanceOffset);
        if (instance == 0 || !memory.IsValidVirtualAddress(process, instance)) {
            continue;
        }
        const auto bones = MsrBoneHashes(memory, process, instance);
        u32 child = memory.Read32(instance + Swap::InstanceNodeOffset + Swap::NodeFirstChildOffset);
        for (int guard = 0; child != 0 && guard < 256; ++guard) {
            if (!memory.IsValidVirtualAddress(process, child + Swap::NodeBoneOffset)) {
                break;
            }
            const auto recorded = std::find_if(attachments.begin(), attachments.end(),
                                               [child](const auto& a) { return a.first == child; });
            if (recorded != attachments.end()) {
                const auto bone = std::find(bones.begin(), bones.end(), recorded->second);
                if (bone != bones.end()) {
                    memory.Write32(child + Swap::NodeBoneOffset,
                                   static_cast<u32>(bone - bones.begin()));
                    ++moved;
                } else {
                    ++missing;
                }
            }
            child = memory.Read32(child + Swap::NodeNextOffset);
        }
    }
    LOG_INFO(HW_GPU, "Fusion suit: {} attached models moved to their bones, {} without one",
             moved, missing);
}

/// Samus is in play (or in the pause menu), not in a load or a cutscene.
bool MsrInGameplay() {
    namespace P = MSR::Presentation;
    switch (static_cast<P::Bottom>(P::bottom.load(std::memory_order_relaxed))) {
    case P::Bottom::Hud:
        return true;
    case P::Bottom::Menu: {
        const auto page = static_cast<P::Page>(P::page.load(std::memory_order_relaxed));
        return page == P::Page::PauseMap || page == P::Page::PauseSamus ||
               page == P::Page::MapPins;
    }
    default:
        return false;
    }
}

} // namespace

void GPU::ApplyMsrRefills() {
    namespace Cheats = MSR::Cheats;
    auto& refills = impl->msr_refills;
    const u32 enabled = Cheats::enabled.load(std::memory_order_relaxed);
    const auto process = impl->system.Kernel().GetCurrentProcess();
    auto& memory = impl->memory;
    const auto valid = [&](u32 address) {
        return process && memory.IsValidVirtualAddress(*process, address);
    };
    if (!valid(Cheats::GamePointer)) {
        return;
    }

    // Energy and Aeion: their components, found again once an area load has replaced them.
    bool missing = false;
    for (std::size_t i = 0; i < Cheats::LockedValues.size(); ++i) {
        const auto& value = Cheats::LockedValues[i];
        u32& component = refills.components[i];
        if (component != 0 &&
            (!valid(component + value.size - 4) || memory.Read32(component) != value.vtable)) {
            component = 0;
            refills.locked[i] = false;
        }
        if ((enabled & value.flag) == 0) {
            if (refills.locked[i]) {
                memory.Write8(component + value.lock, 0);
                refills.locked[i] = false;
            }
            continue;
        }
        if (component == 0) {
            missing = true;
            continue;
        }
        // Full: locked. Otherwise unlocked and filled through the setter, once the stub is free.
        const u32 maximum = memory.Read32(component + value.maximum);
        if (memory.Read32(component + value.current) == maximum) {
            memory.Write8(component + value.lock, 1);
            refills.locked[i] = true;
        } else if (impl->msr_suit_swap.calls.empty()) {
            memory.Write8(component + value.lock, 0);
            impl->msr_suit_swap.calls.push_back({value.setter, component, 0, maximum});
        }
    }
    const s64 now_us = impl->timing.GetGlobalTimeUs().count();
    if (missing && MsrInGameplay() && now_us >= refills.next_scan_us) {
        refills.next_scan_us = now_us + 1'000'000;
        for (std::size_t i = 0; i < Cheats::LockedValues.size(); ++i) {
            const auto& value = Cheats::LockedValues[i];
            if ((enabled & value.flag) == 0 || refills.components[i] != 0) {
                continue;
            }
            // A plausible maximum, up to which the current value goes.
            const auto found = FindMsrObjects(
                *process, value.vtable, value.size,
                [&value](const auto& word) {
                    float maximum;
                    const u32 bits = word(value.maximum);
                    std::memcpy(&maximum, &bits, sizeof(maximum));
                    return maximum > 0.f && maximum < 100000.f;
                },
                1);
            if (!found.empty()) {
                refills.components[i] = found.front();
                LOG_INFO(HW_GPU, "Cheats: {} kept full",
                         value.flag == Cheats::InfiniteEnergy ? "energy" : "Aeion");
            }
        }
    }

    // Missiles: Samus's inventory, as the game finds it.
    if ((enabled & Cheats::InfiniteMissiles) == 0) {
        return;
    }
    const u32 game = memory.Read32(Cheats::GamePointer);
    u32 inventory = 0;
    for (const u32 offset : Cheats::PlayerHandles) {
        if (!valid(game + offset)) {
            return;
        }
        const u32 handle = memory.Read32(game + offset);
        const u32 entity = valid(handle + 8) ? memory.Read32(handle + 8) : 0;
        inventory = valid(entity + Cheats::EntityInventoryOffset)
                        ? memory.Read32(entity + Cheats::EntityInventoryOffset)
                        : 0;
        if (inventory != 0) {
            break;
        }
    }
    if (!valid(inventory + 0x48)) {
        return;
    }
    // Where an item's value is (0 when it is not in the inventory).
    const auto item = [&](u32 key) -> u32 {
        const u32 name = valid(key) ? memory.Read32(key) : 0;
        if (!valid(name + 0x10) || memory.Read32(inventory + 0x44) == 0) {
            return 0;
        }
        const u32 hash = memory.Read32(name + 0x10);
        const u32 buckets = memory.Read32(inventory + 0x40);
        const u32 count = memory.Read32(inventory + 0x48);
        if (count == 0 || !valid(buckets + (hash % count) * 4)) {
            return 0;
        }
        u32 node = memory.Read32(buckets + (hash % count) * 4);
        for (int guard = 0; node != 0 && guard < 64 && valid(node + 0x10); ++guard) {
            if (memory.Read32(node) == hash) {
                const u32 value = memory.Read32(node + 4);
                return valid(value) ? value : 0;
            }
            node = memory.Read32(node + 0x10);
        }
        return 0;
    };
    for (const auto& refill : Cheats::Refills) {
        const u32 current = item(refill.current);
        const u32 maximum = item(refill.maximum);
        if (current != 0 && maximum != 0 && memory.Read32(current) != memory.Read32(maximum)) {
            memory.Write32(current, memory.Read32(maximum));
        }
    }
}

void GPU::ApplyMsrSuitSwap() {
    namespace Swap = MSR::SuitSwap;
    using Step = Impl::MsrSuitSwap::Step;
    auto& swap = impl->msr_suit_swap;
    const auto process = impl->system.Kernel().GetCurrentProcess();
    auto& memory = impl->memory;
    if (!process || !memory.IsValidVirtualAddress(*process, Swap::Mailbox) ||
        !memory.IsValidVirtualAddress(*process, Swap::Hook)) {
        return;
    }
    // Only in the game this was written for: the hook holds the frame limiter's instruction or
    // the branch to the stub.
    const u32 hook = memory.Read32(Swap::Hook);
    if (hook != Swap::HookOriginal && hook != Swap::HookBranch) {
        return;
    }
    const u32 game = memory.Read32(Swap::GamePointer);
    if (game == 0 || !memory.IsValidVirtualAddress(*process, game + Swap::ForceFusionOffset)) {
        return;
    }
    const bool fusion =
        (MSR::Cheats::enabled.load(std::memory_order_relaxed) & MSR::Cheats::FusionSuit) != 0 ||
        memory.Read8(game + Swap::FusionModeOffset) != 0 ||
        memory.Read8(game + Swap::ForceFusionOffset) != 0;
    const s64 now_us = impl->timing.GetGlobalTimeUs().count();

    const auto set_hook = [&](bool on) {
        const u32 word = on ? Swap::HookBranch : Swap::HookOriginal;
        if (memory.Read32(Swap::Hook) != word) {
            memory.Write32(Swap::Hook, word);
            impl->system.InvalidateCacheRange(Swap::Hook, 4);
        }
    };
    // A command for the stub, which runs it on the game's next frame.
    const auto send = [&](u32 command, bool family) {
        bool stub_current = true;
        for (std::size_t i = 0; i < Swap::Stub.size(); ++i) {
            stub_current &= memory.Read32(Swap::StubAddress + static_cast<u32>(i) * 4) == Swap::Stub[i];
        }
        if (!stub_current) {
            for (std::size_t i = 0; i < Swap::Stub.size(); ++i) {
                memory.Write32(Swap::StubAddress + static_cast<u32>(i) * 4, Swap::Stub[i]);
            }
            impl->system.InvalidateCacheRange(Swap::StubAddress,
                                              static_cast<u32>(Swap::Stub.size()) * 4);
        }
        const u32 count = command == Swap::SwapModels
                              ? std::min<u32>(static_cast<u32>(swap.components.size()),
                                              Swap::MaxComponents)
                              : 0;
        memory.Write32(Swap::Mailbox + Swap::CommandField, command);
        memory.Write32(Swap::Mailbox + Swap::Family, family ? 1 : 0);
        memory.Write32(Swap::Mailbox + Swap::Count, count);
        for (u32 i = 0; i < count; ++i) {
            memory.Write32(Swap::Mailbox + Swap::Components + i * 4, swap.components[i]);
        }
        const u32 calls = command == Swap::CallList
                              ? std::min<u32>(static_cast<u32>(swap.calls.size()), Swap::MaxCalls)
                              : 0;
        memory.Write32(Swap::Mailbox + Swap::CallCount, calls);
        for (u32 i = 0; i < calls; ++i) {
            for (u32 field = 0; field < 4; ++field) {
                memory.Write32(Swap::Mailbox + Swap::Calls + i * 16 + field * 4,
                               swap.calls[i][field]);
            }
        }
        swap.runs = memory.Read32(Swap::Mailbox + Swap::Runs);
        memory.Write32(Swap::Mailbox + Swap::Armed, 1);
        swap.since_us = now_us;
        set_hook(true);
    };

    switch (swap.step) {
    case Step::Idle: {
        // A save state may have kept a command or the branch.
        set_hook(false);
        if (memory.Read32(Swap::Mailbox + Swap::Armed) != 0) {
            memory.Write32(Swap::Mailbox + Swap::Armed, 0);
        }
        if (swap.given_up >= 0 && swap.given_up != static_cast<int>(fusion)) {
            swap.given_up = -1;
        }
        if (!MsrInGameplay()) {
            return;
        }
        // The refills' setters go first: the stub runs one command at a time.
        if (!swap.calls.empty()) {
            send(Swap::CallList, false);
            swap.step = Step::Calling;
            return;
        }
        // Samus's models as they are: the components found before, or found again after a load
        // (or a save state) replaced them.
        if (!MsrComponentsCurrent(memory, *process, swap.components)) {
            if (now_us < swap.next_scan_us) {
                return;
            }
            swap.next_scan_us = now_us + 1'000'000;
            swap.components = FindMsrModelComponents(*process); // ~6 ms, once per area
            if (swap.components.empty()) {
                return;
            }
        }
        const int models = MsrModelsFusion(memory, *process, swap.components.front());
        if (models < 0 || models == static_cast<int>(fusion) ||
            swap.given_up == static_cast<int>(fusion)) {
            return;
        }
        swap.target = fusion;
        LOG_INFO(HW_GPU, "Fusion suit {}: loading the packages", fusion ? "on" : "off");
        send(Swap::LoadFamily, fusion);
        swap.step = Step::Loading;
        return;
    }
    case Step::Settling:
        // The packages are in; their resources settle for a moment before the models use them.
        if (now_us - swap.since_us < 300'000) {
            return;
        }
        if (fusion != swap.target) {
            send(Swap::ReleaseFamily, swap.target); // switched back meanwhile
            swap.step = Step::Aborting;
            return;
        }
        if (!MsrInGameplay()) {
            return;
        }
        swap.components = FindMsrModelComponents(*process);
        if (swap.components.empty()) {
            send(Swap::ReleaseFamily, !swap.target);
            swap.step = Step::Releasing;
            return;
        }
        LOG_INFO(HW_GPU, "Fusion suit: replacing the models of {} components",
                 swap.components.size());
        swap.attachments = MsrBoneAttachments(memory, *process, swap.components);
        send(Swap::SwapModels, swap.target);
        swap.step = Step::Swapping;
        return;
    default:
        break;
    }

    // A command is out.
    if (memory.Read32(Swap::Mailbox + Swap::Runs) == swap.runs) {
        // The frame limiter has not run: the game is loading. The components it was given may
        // be gone when it comes back, so a swap is taken back and prepared again, and calls are
        // dropped (the refills ask again once they have found their components).
        if ((swap.step == Step::Swapping || swap.step == Step::Calling) &&
            now_us - swap.since_us > 500'000) {
            memory.Write32(Swap::Mailbox + Swap::Armed, 0);
            set_hook(false);
            if (swap.step == Step::Calling) {
                swap.calls.clear();
                swap.step = Step::Idle;
            } else {
                swap.step = Step::Settling;
            }
        }
        return;
    }
    set_hook(false);
    switch (swap.step) {
    case Step::Calling:
        swap.calls.clear();
        swap.step = Step::Idle;
        break;
    case Step::Loading:
        swap.step = Step::Settling;
        swap.since_us = now_us;
        break;
    case Step::Swapping:
        MsrReattach(memory, *process, swap.components, swap.attachments);
        send(Swap::ReleaseFamily, !swap.target);
        swap.step = Step::Releasing;
        break;
    case Step::Releasing:
        // Not tried again for this setting if the models did not change.
        if (MsrComponentsCurrent(memory, *process, swap.components) &&
            MsrModelsFusion(memory, *process, swap.components.front()) !=
                static_cast<int>(swap.target)) {
            LOG_WARNING(HW_GPU, "Fusion suit: the models did not change");
            swap.given_up = swap.target;
        } else {
            LOG_INFO(HW_GPU, "Fusion suit: done");
        }
        swap.step = Step::Idle;
        break;
    default:
        swap.step = Step::Idle;
        break;
    }
}

void GPU::RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window) {
    // Reset the renderer (this will destroy OpenGL resources)
    impl->renderer.reset();

    // Create a new renderer
    impl->renderer =
        VideoCore::CreateRenderer(emu_window, secondary_window, impl->pica, impl->system);
    impl->rasterizer = impl->renderer->Rasterizer();

    // Rebind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);

    // Update the sw_blitter with the new rasterizer
    impl->sw_blitter = std::make_unique<SwRenderer::SwBlitter>(impl->memory, impl->rasterizer);

    // Re-apply per-game configuration and reload disk shader cache
    u64 program_id{};
    impl->system.GetAppLoader().ReadProgramId(program_id);
    ApplyPerProgramSettings(program_id);
    if (Settings::values.use_disk_shader_cache) {
        impl->renderer->Rasterizer()->LoadDefaultDiskResources(false, nullptr);
    }

    // Mark ALL GPU registers as dirty so current state gets uploaded to new renderer
    impl->pica.dirty_regs.SetAllDirty();

    // Also mark shader setups as dirty so uniforms get re-uploaded and
    // stale pointers to the old rasterizer's JIT cache are cleared.
    impl->pica.vs_setup.uniforms_dirty = true;
    impl->pica.vs_setup.cached_shader = nullptr;
    impl->pica.gs_setup.uniforms_dirty = true;
    impl->pica.gs_setup.cached_shader = nullptr;

    // Mark all cached LUT/table state in pica as dirty
    impl->pica.lighting.lut_dirty = impl->pica.lighting.LutAllDirty;
    impl->pica.fog.lut_dirty = true;
    impl->pica.proctex.table_dirty = impl->pica.proctex.TableAllDirty;
}

void GPU::ReleaseRenderer() {
    // Just reset the renderer to release OpenGL resources
    // Don't null out rasterizer pointer as it will become dangling
    impl->renderer.reset();
    impl->sw_blitter.reset();
    LOG_INFO(HW_GPU, "Renderer released for context destroy");
}

template <class Archive>
void GPU::serialize(Archive& ar, const u32 file_version) {
    ar & impl->pica;
    if (Archive::is_loading::value) {
        // The loaded state brings its own models, components and stub.
        impl->msr_suit_swap = {};
        impl->msr_refills = {};
    }
}

SERIALIZE_IMPL(GPU)

} // namespace VideoCore
