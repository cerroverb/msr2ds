// IPS and BPS patches, as the emulator applies them to code.bin and RomFS files (src/core/file_sys/patch.cpp).
#pragma once

#include <cstdint>
#include <vector>

namespace MsrPatch {

/// Each returns false when the patch is not one or does not fit `buffer` (left as it was for IPS only partly).
bool ApplyIps(const std::vector<std::uint8_t>& ips, std::vector<std::uint8_t>& buffer);
bool ApplyBps(const std::vector<std::uint8_t>& bps, std::vector<std::uint8_t>& buffer);

} // namespace MsrPatch
