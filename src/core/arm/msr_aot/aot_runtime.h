// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Runtime for the ahead-of-time recompiled Samus Returns code (tools/msr/recomp/generate.py).
//
// Each generated function holds the code reachable from one guest entry point, with calls,
// returns, indirect branches and system calls leaving it: the guest call stack stays in guest
// memory and the dispatcher (arm_aot.cpp) is the only host frame between guest functions, so a
// system call can switch guest threads at any time. A function starts at the PC in r[15]: its
// entry, a return site after a call, or a loop head (where it yields when the time slice ends).
//
// Memory goes through the emulator's page table, falling back to the memory system for
// anything that is not plain RAM. Floating point uses the host's SSE with MXCSR set from the
// guest FPSCR (flush-to-zero as denormals-are-zero plus flush-to-zero, and the rounding mode);
// default-NaN mode is applied after each operation.

#pragma once

#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <xmmintrin.h>

// The generated code is compiled at -O1 to keep its build time down; the small helpers it calls
// on every instruction must still inline.
#define MSR_AOT_INLINE inline __attribute__((always_inline))

namespace MsrAot {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

enum class Exit : u32 {
    Branch,    ///< Continue at r[15] (and Thumb state `thumb`)
    Svc,       ///< System call `svc`; r[15] is the next instruction
    Interpret, ///< r[15] is an instruction the generator left to the interpreter
    Patched,   ///< r[15] was rewritten in memory (patchable.txt): the interpreter runs it
};

/// Slow paths: the memory system for pages without a direct pointer.
struct MemoryCallbacks {
    void* user = nullptr;
    u8 (*read8)(void*, u32) = nullptr;
    u16 (*read16)(void*, u32) = nullptr;
    u32 (*read32)(void*, u32) = nullptr;
    void (*write8)(void*, u32, u8) = nullptr;
    void (*write16)(void*, u32, u16) = nullptr;
    void (*write32)(void*, u32, u32) = nullptr;
    bool (*write_exclusive32)(void*, u32, u32 value, u32 expected) = nullptr;
    bool (*write_exclusive64)(void*, u32, u64 value, u64 expected) = nullptr;
};

struct Ctx {
    u32 r[16]{};
    u32 n = 0, z = 0, c = 0, v = 0; ///< Condition flags, 0 or 1
    u32 q = 0;                      ///< Sticky saturation flag
    u32 ge = 0;                     ///< GE[3:0]
    u32 cpsr_rest = 0x10;           ///< CPSR bits other than NZCVQ, GE and T (mode, masks)
    u32 thumb = 0;
    u32 s[32]{}; ///< VFP S0-S31; D0-D15 alias consecutive pairs
    u32 fpscr = 0;
    u32 tls_ro = 0, tls_rw = 0; ///< CP15 c13 thread registers
    u64 ticks = 0;              ///< Instructions executed in this run
    u64 budget = 0;             ///< Yield at loop heads once ticks reach this
    Exit exit = Exit::Branch;
    u32 svc = 0;
    u32 excl_valid = 0, excl_addr = 0;
    u64 excl_value = 0;
    u8* const* pages = nullptr; ///< 4 KiB page -> host pointer, or null
    MemoryCallbacks mem;
};

using Handler = void (*)(Ctx&);

struct Entry {
    u32 pc;
    u32 thumb;
    Handler handler;
};

// -- memory ---------------------------------------------------------------------------------

template <typename T>
MSR_AOT_INLINE T* Direct(const Ctx& c, u32 a) {
    u8* page = c.pages[a >> 12];
    return (page && (a & 0xFFF) <= 0x1000 - sizeof(T)) ? reinterpret_cast<T*>(page + (a & 0xFFF))
                                                        : nullptr;
}

MSR_AOT_INLINE u8 R8(Ctx& c, u32 a) {
    if (u8* p = Direct<u8>(c, a)) return *p;
    return c.mem.read8(c.mem.user, a);
}
MSR_AOT_INLINE u16 R16(Ctx& c, u32 a) {
    if (u16* p = Direct<u16>(c, a)) { u16 v; std::memcpy(&v, p, 2); return v; }
    return c.mem.read16(c.mem.user, a);
}
MSR_AOT_INLINE u32 R32(Ctx& c, u32 a) {
    if (u32* p = Direct<u32>(c, a)) { u32 v; std::memcpy(&v, p, 4); return v; }
    return c.mem.read32(c.mem.user, a);
}
MSR_AOT_INLINE void W8(Ctx& c, u32 a, u32 v) {
    if (u8* p = Direct<u8>(c, a)) { *p = static_cast<u8>(v); return; }
    c.mem.write8(c.mem.user, a, static_cast<u8>(v));
}
MSR_AOT_INLINE void W16(Ctx& c, u32 a, u32 v) {
    if (u16* p = Direct<u16>(c, a)) { const u16 h = static_cast<u16>(v); std::memcpy(p, &h, 2); return; }
    c.mem.write16(c.mem.user, a, static_cast<u16>(v));
}
MSR_AOT_INLINE void W32(Ctx& c, u32 a, u32 v) {
    if (u32* p = Direct<u32>(c, a)) { std::memcpy(p, &v, 4); return; }
    c.mem.write32(c.mem.user, a, v);
}

// Exclusive access: one guest core runs the application, so a value check suffices.
inline u32 LoadExclusive(Ctx& c, u32 a, u32 value) {
    c.excl_valid = 1;
    c.excl_addr = a;
    c.excl_value = value;
    return value;
}
template <int Size>
inline u32 StoreExclusive(Ctx& c, u32 a, u32 value) {
    if (!c.excl_valid || c.excl_addr != a) {
        c.excl_valid = 0;
        return 1;
    }
    c.excl_valid = 0;
    if constexpr (Size == 4) {
        return c.mem.write_exclusive32(c.mem.user, a, value, static_cast<u32>(c.excl_value)) ? 0 : 1;
    } else {
        // Byte and halfword: compare and write through the plain path.
        const u32 current = Size == 1 ? R8(c, a) : R16(c, a);
        const u32 mask = Size == 1 ? 0xFFu : 0xFFFFu;
        if (current != (c.excl_value & mask)) return 1;
        if (Size == 1) W8(c, a, value); else W16(c, a, value);
        return 0;
    }
}

inline void LoadExclusive64(Ctx& c, u32 a, u32 lo, u32 hi) {
    c.excl_valid = 1;
    c.excl_addr = a;
    c.excl_value = (static_cast<u64>(hi) << 32) | lo;
}
inline u32 StoreExclusive64(Ctx& c, u32 a, u32 lo, u32 hi) {
    if (!c.excl_valid || c.excl_addr != a) {
        c.excl_valid = 0;
        return 1;
    }
    c.excl_valid = 0;
    return c.mem.write_exclusive64(c.mem.user, a, (static_cast<u64>(hi) << 32) | lo, c.excl_value) ? 0 : 1;
}

// -- integer helpers ------------------------------------------------------------------------

MSR_AOT_INLINE u32 Ror(u32 v, u32 n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

/// Shift by a register (ARM): value and carry out. Type 0 LSL, 1 LSR, 2 ASR, 3 ROR.
MSR_AOT_INLINE u32 ShiftReg(u32 type, u32 v, u32 amount, u32 carry_in, u32* carry) {
    amount &= 0xFF;
    if (amount == 0) { *carry = carry_in; return v; }
    switch (type) {
    case 0:
        if (amount < 32) { *carry = (v >> (32 - amount)) & 1; return v << amount; }
        *carry = amount == 32 ? (v & 1) : 0;
        return 0;
    case 1:
        if (amount < 32) { *carry = (v >> (amount - 1)) & 1; return v >> amount; }
        *carry = amount == 32 ? (v >> 31) : 0;
        return 0;
    case 2:
        if (amount < 32) { *carry = (v >> (amount - 1)) & 1; return static_cast<u32>(static_cast<s32>(v) >> amount); }
        *carry = v >> 31;
        return (v >> 31) ? 0xFFFFFFFFu : 0u;
    default:
        if ((amount & 31) == 0) { *carry = v >> 31; return v; }
        *carry = (v >> ((amount & 31) - 1)) & 1;
        return Ror(v, amount);
    }
}

MSR_AOT_INLINE u32 AddFlags(Ctx& c, u32 a, u32 b, u32 carry) {
    const u64 wide = static_cast<u64>(a) + b + carry;
    const u32 x = static_cast<u32>(wide);
    c.n = x >> 31;
    c.z = x == 0;
    c.c = static_cast<u32>(wide >> 32);
    c.v = ((a ^ x) & (b ^ x)) >> 31;
    return x;
}
MSR_AOT_INLINE u32 Logical(Ctx& c, u32 x, u32 carry) {
    c.n = x >> 31;
    c.z = x == 0;
    c.c = carry;
    return x;
}
MSR_AOT_INLINE u32 NZ(Ctx& c, u32 x) {
    c.n = x >> 31;
    c.z = x == 0;
    return x;
}

// The same on flags held in the generated function's locals.
MSR_AOT_INLINE u32 AddFlagsL(u32& n, u32& z, u32& c, u32& v, u32 a, u32 b, u32 carry) {
    const u64 wide = static_cast<u64>(a) + b + carry;
    const u32 x = static_cast<u32>(wide);
    n = x >> 31;
    z = x == 0;
    c = static_cast<u32>(wide >> 32);
    v = ((a ^ x) & (b ^ x)) >> 31;
    return x;
}
MSR_AOT_INLINE u32 LogicalL(u32& n, u32& z, u32& c, u32 x, u32 carry) {
    n = x >> 31;
    z = x == 0;
    c = carry;
    return x;
}
MSR_AOT_INLINE u32 NZL(u32& n, u32& z, u32 x) {
    n = x >> 31;
    z = x == 0;
    return x;
}

inline u32 SignedSat(Ctx& c, s64 value, u32 bits) {
    const s64 max = (s64{1} << (bits - 1)) - 1, min = -(s64{1} << (bits - 1));
    if (value > max) { c.q = 1; return static_cast<u32>(max); }
    if (value < min) { c.q = 1; return static_cast<u32>(min); }
    return static_cast<u32>(value);
}
inline u32 UnsignedSat(Ctx& c, s64 value, u32 bits) {
    const s64 max = (s64{1} << bits) - 1;
    if (value > max) { c.q = 1; return static_cast<u32>(max); }
    if (value < 0) { c.q = 1; return 0; }
    return static_cast<u32>(value);
}
/// Signed add that sets Q on 32-bit overflow but keeps the wrapped result (SMLAxy, SMLAD...).
inline u32 AddQ(Ctx& c, s64 a, s64 b) {
    const s64 x = a + b;
    if (x != static_cast<s32>(x)) c.q = 1;
    return static_cast<u32>(x);
}
inline u32 Clz(u32 v) {
    return v ? static_cast<u32>(__builtin_clz(v)) : 32u;
}

/// Parallel add/subtract (ARMv6 media). Kind: 0 signed, 1 saturating, 2 halving (signed);
/// 4 unsigned, 5 unsigned saturating, 6 unsigned halving. Op: 0 ADD16, 1 ASX, 2 SAX, 3 SUB16,
/// 4 ADD8, 7 SUB8.
inline u32 Parallel(Ctx& c, u32 kind, u32 op, u32 a, u32 b) {
    const bool is_unsigned = kind >= 4;
    const u32 mode = kind & 3; // 0 plain (sets GE), 1 saturating, 2 halving
    const bool bytes = op >= 4;
    const u32 lanes = bytes ? 4 : 2, width = bytes ? 8 : 16, mask = bytes ? 0xFFu : 0xFFFFu;
    u32 result = 0, ge = 0;
    for (u32 i = 0; i < lanes; ++i) {
        auto lane = [&](u32 x, u32 index) -> s64 {
            const u32 raw = (x >> (index * width)) & mask;
            if (is_unsigned) return raw;
            return bytes ? static_cast<s8>(raw) : static_cast<s16>(raw);
        };
        s64 left = lane(a, i), right;
        bool subtract;
        if (op == 1) { right = lane(b, 1 - i); subtract = i == 0; }      // ASX
        else if (op == 2) { right = lane(b, 1 - i); subtract = i == 1; } // SAX
        else { right = lane(b, i); subtract = op == 3 || op == 7; }
        s64 x = subtract ? left - right : left + right;
        if (mode == 0) {
            const bool ok = is_unsigned ? (subtract ? x >= 0 : x >= static_cast<s64>(mask) + 1) : x >= 0;
            if (ok) ge |= (bytes ? 1u : 3u) << (i * (bytes ? 1 : 2));
        } else if (mode == 1) {
            const s64 hi = is_unsigned ? static_cast<s64>(mask) : static_cast<s64>(mask >> 1);
            const s64 lo = is_unsigned ? 0 : -static_cast<s64>(mask >> 1) - 1;
            x = x > hi ? hi : (x < lo ? lo : x);
        } else {
            x >>= 1;
        }
        result |= (static_cast<u32>(x) & mask) << (i * width);
    }
    if (mode == 0) c.ge = ge;
    return result;
}

inline u32 Cpsr(const Ctx& c) {
    return (c.n << 31) | (c.z << 30) | (c.c << 29) | (c.v << 28) | (c.q << 27) | (c.ge << 16) |
           (c.thumb << 5) | c.cpsr_rest;
}
inline void SetCpsrFlags(Ctx& c, u32 value, bool control) {
    c.n = value >> 31;
    c.z = (value >> 30) & 1;
    c.c = (value >> 29) & 1;
    c.v = (value >> 28) & 1;
    c.q = (value >> 27) & 1;
    c.ge = (value >> 16) & 0xF;
    if (control) c.cpsr_rest = (c.cpsr_rest & 0xFFFFFF00u) | (value & 0xDFu);
}

// -- branches -------------------------------------------------------------------------------

/// BX-style branch: bit 0 selects Thumb.
MSR_AOT_INLINE void BranchX(Ctx& c, u32 target) {
    c.thumb = target & 1;
    c.r[15] = target & (c.thumb ? ~1u : ~3u);
}

// -- floating point -------------------------------------------------------------------------

constexpr u32 FpscrDn = 1u << 25;
constexpr u32 FpscrFz = 1u << 24;

/// MXCSR for a guest FPSCR: flush-to-zero (inputs and outputs) and the rounding mode.
inline u32 MxcsrFor(u32 fpscr, u32 host) {
    static constexpr u32 rounding[4] = {0x0000, 0x4000, 0x2000, 0x6000}; // RN, RP, RM, RZ
    u32 m = host & ~(0x6000u | 0x8040u);
    m |= rounding[(fpscr >> 22) & 3];
    if (fpscr & FpscrFz) m |= 0x8040u; // FTZ | DAZ
    return m | 0x1F80u;                // all exceptions masked
}

inline void SetFpscr(Ctx& c, u32 value) {
    c.fpscr = value;
    _mm_setcsr(MxcsrFor(value, _mm_getcsr()));
}

MSR_AOT_INLINE float F(const Ctx& c, u32 i) {
    float f;
    std::memcpy(&f, &c.s[i], 4);
    return f;
}
MSR_AOT_INLINE void SetF(Ctx& c, u32 i, float f) {
    std::memcpy(&c.s[i], &f, 4);
}
MSR_AOT_INLINE double D(const Ctx& c, u32 i) {
    double d;
    std::memcpy(&d, &c.s[i * 2], 8);
    return d;
}
MSR_AOT_INLINE void SetD(Ctx& c, u32 i, double d) {
    std::memcpy(&c.s[i * 2], &d, 8);
}
MSR_AOT_INLINE float NanF(const Ctx& c, float x) {
    if (x != x && (c.fpscr & FpscrDn)) {
        const u32 bits = 0x7FC00000u;
        std::memcpy(&x, &bits, 4);
    }
    return x;
}
MSR_AOT_INLINE double NanD(const Ctx& c, double x) {
    if (x != x && (c.fpscr & FpscrDn)) {
        const u64 bits = 0x7FF8000000000000ull;
        std::memcpy(&x, &bits, 8);
    }
    return x;
}
MSR_AOT_INLINE float SqrtF(float x) {
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
}
MSR_AOT_INLINE double SqrtD(double x) {
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_set_sd(0.0), _mm_set_sd(x)));
}
MSR_AOT_INLINE u32 AbsBits(u32 v) {
    return v & 0x7FFFFFFFu;
}
MSR_AOT_INLINE u32 NegBits(u32 v) {
    return v ^ 0x80000000u;
}
/// FCMP: NZCV in FPSCR. Equal 0110, less 1000, greater 0010, unordered 0011.
template <typename T>
MSR_AOT_INLINE void Compare(Ctx& c, T a, T b) {
    u32 f;
    if (a != a || b != b) f = 0x3;
    else if (a == b) f = 0x6;
    else if (a < b) f = 0x8;
    else f = 0x2;
    c.fpscr = (c.fpscr & 0x0FFFFFFFu) | (f << 28);
}
/// Float to integer, saturating; NaN gives 0. Rounds toward zero or with the current mode.
inline u32 ToS32(double x, bool toward_zero) {
    if (x != x) return 0;
    if (x >= 2147483648.0) {
        const s64 i = toward_zero ? _mm_cvttsd_si64(_mm_set_sd(x)) : _mm_cvtsd_si64(_mm_set_sd(x));
        if (x >= 4.0e18 || i > 0x7FFFFFFF) return 0x7FFFFFFFu;
    }
    if (x <= -2147483649.0) return 0x80000000u;
    const s64 i = toward_zero ? _mm_cvttsd_si64(_mm_set_sd(x)) : _mm_cvtsd_si64(_mm_set_sd(x));
    if (i > 0x7FFFFFFF) return 0x7FFFFFFFu;
    if (i < -0x80000000LL) return 0x80000000u;
    return static_cast<u32>(static_cast<s32>(i));
}
inline u32 ToU32(double x, bool toward_zero) {
    if (x != x) return 0;
    if (x >= 4294967296.0) return 0xFFFFFFFFu;
    if (x <= -1.0) {
        const s64 i = toward_zero ? _mm_cvttsd_si64(_mm_set_sd(x)) : _mm_cvtsd_si64(_mm_set_sd(x));
        if (i < 0) return 0;
    }
    const s64 i = toward_zero ? _mm_cvttsd_si64(_mm_set_sd(x)) : _mm_cvtsd_si64(_mm_set_sd(x));
    if (i < 0) return 0;
    if (i > 0xFFFFFFFFLL) return 0xFFFFFFFFu;
    return static_cast<u32>(i);
}

} // namespace MsrAot
