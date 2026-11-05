// IPS and BPS patches: a port of the emulator's (src/core/file_sys/patch.cpp, from Citra), with its own CRC-32.
#include "patch.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>

namespace MsrPatch {

bool ApplyIps(const std::vector<std::uint8_t>& ips, std::vector<std::uint8_t>& buffer) {
    if (ips.size() < 8 || std::memcmp(ips.data(), "PATCH", 5) != 0) {
        std::fprintf(stderr, "msr: not an IPS patch\n");
        return false;
    }
    std::size_t cursor = 5;
    const std::size_t end = ips.size() - 3;
    while (cursor < end) {
        if (std::memcmp(&ips[cursor], "EOF", 3) == 0) break;
        if (cursor + 5 > ips.size()) return false;
        const std::size_t offset = static_cast<std::size_t>(ips[cursor]) << 16 | ips[cursor + 1] << 8 | ips[cursor + 2];
        std::size_t length = static_cast<std::size_t>(ips[cursor + 3]) << 8 | ips[cursor + 4];
        if (length == 0) {   // a run of one byte
            if (cursor + 8 > ips.size()) return false;
            length = static_cast<std::size_t>(ips[cursor + 5]) << 8 | ips[cursor + 6];
            if (buffer.size() < offset + length) return false;
            std::memset(&buffer[offset], ips[cursor + 7], length);
            cursor += 8;
            continue;
        }
        if (buffer.size() < offset + length || cursor + 5 + length > ips.size()) return false;
        std::memcpy(&buffer[offset], &ips[cursor + 5], length);
        cursor += length + 5;
    }
    return true;
}

namespace {

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

using Number = std::uint32_t;
constexpr std::size_t MagicSize = 4;
constexpr std::size_t FooterSize = 12;

template <typename T>
class Stream {
public:
    Stream(T* ptr, std::size_t size) : ptr{ptr}, length{size} {}
    bool Read(void* out, std::size_t n) {
        if (offset + n > length) return false;
        std::memcpy(out, ptr + offset, n);
        offset += n;
        return true;
    }
    template <typename Other>
    bool CopyFrom(Stream<Other>& other, std::size_t n) {
        if (offset + n > length || !other.Read(ptr + offset, n)) return false;
        offset += n;
        return true;
    }
    template <typename V>
    std::optional<V> Read() {
        static_assert(std::is_trivial_v<V>);
        V v{};
        if (!Read(&v, sizeof v)) return std::nullopt;
        return v;
    }
    Number ReadNumber() {
        Number data = 0, shift = 1;
        std::optional<std::uint8_t> x;
        while ((x = Read<std::uint8_t>())) {
            data += (*x & 0x7f) * shift;
            if (*x & 0x80) break;
            shift <<= 7;
            data += shift;
        }
        return data;
    }
    T* data() const { return ptr; }
    std::size_t size() const { return length; }
    std::size_t Tell() const { return offset; }
    bool Seek(std::size_t to) {
        if (to > length) return false;
        offset = to;
        return true;
    }

private:
    T* ptr;
    std::size_t length;
    std::size_t offset = 0;
};

class Applier {
public:
    Applier(Stream<const std::uint8_t> source, Stream<std::uint8_t> target, Stream<const std::uint8_t> patch)
        : source{source}, target{target}, patch{patch} {}

    bool Apply() {
        const auto magic = patch.Read<std::array<char, MagicSize>>();
        if (!magic || std::string_view(magic->data(), magic->size()) != "BPS1") {
            std::fprintf(stderr, "msr: not a BPS patch\n");
            return false;
        }
        const Number source_size = patch.ReadNumber();
        const Number target_size = patch.ReadNumber();
        const Number metadata_size = patch.ReadNumber();
        if (source_size > source.size() || target_size > target.size() || metadata_size != 0 ||
            patch.size() < FooterSize) {
            std::fprintf(stderr, "msr: BPS patch: sizes do not fit\n");
            return false;
        }
        const std::size_t start = patch.Tell(), end = patch.size() - FooterSize;
        patch.Seek(end);
        const std::uint32_t source_crc = patch.Read<std::uint32_t>().value_or(0);
        const std::uint32_t target_crc = patch.Read<std::uint32_t>().value_or(0);
        patch.Seek(start);
        if (Crc32(source.data(), source_size) != source_crc) {
            std::fprintf(stderr, "msr: BPS patch: made for another file\n");
            return false;
        }
        std::memset(target.data(), 0, target.size());
        while (patch.Tell() < end) {
            if (!Command()) return false;
        }
        if (Crc32(target.data(), target_size) != target_crc) {
            std::fprintf(stderr, "msr: BPS patch: the result does not match\n");
            return false;
        }
        return true;
    }

private:
    bool Command() {
        const Number data = patch.ReadNumber();
        const Number length = (data >> 2) + 1;
        switch (data & 3) {
        case 0:   // source read
            return source.Seek(target.Tell()) && target.CopyFrom(source, length);
        case 1:   // target read
            return target.CopyFrom(patch, length);
        case 2: {   // source copy
            const Number d = patch.ReadNumber();
            source_relative += (d & 1 ? -1 : +1) * static_cast<int>(d >> 1);
            if (!source.Seek(source_relative) || !target.CopyFrom(source, length)) return false;
            source_relative += length;
            return true;
        }
        default: {   // target copy, byte by byte
            const Number d = patch.ReadNumber();
            target_relative += (d & 1 ? -1 : +1) * static_cast<int>(d >> 1);
            if (target.Tell() + length > target.size() || target_relative + length > target.size()) return false;
            for (std::size_t i = 0; i < length; ++i) target.data()[target.Tell() + i] = target.data()[target_relative++];
            target.Seek(target.Tell() + length);
            return true;
        }
        }
    }

    std::size_t source_relative = 0, target_relative = 0;
    Stream<const std::uint8_t> source;
    Stream<std::uint8_t> target;
    Stream<const std::uint8_t> patch;
};

} // namespace

bool ApplyBps(const std::vector<std::uint8_t>& bps, std::vector<std::uint8_t>& buffer) {
    Stream<const std::uint8_t> header{bps.data(), bps.size()};
    header.Seek(MagicSize);
    const Number source_size = header.ReadNumber();
    const Number target_size = header.ReadNumber();
    if (target_size > source_size) buffer.resize(target_size);
    const std::vector<std::uint8_t> original = buffer;
    Applier applier{{original.data(), original.size()}, {buffer.data(), buffer.size()}, {bps.data(), bps.size()}};
    return applier.Apply();
}

} // namespace MsrPatch
