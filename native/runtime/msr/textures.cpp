// Texture packs on msr-native: the packs the player enabled in the launcher (Textures tab), in the format the
// emulator-based build reads (Azahar's custom textures: tex1_<w>x<h>_<hash>_<format>.png, optionally a pack.json).
// A texture is replaced when its hash names a file: by default the "legacy" hash, CityHash64 of the texture decoded
// exactly as Azahar's rasterizer cache decodes it (texture_codec.h, MortonCopy without conversion: tiles into a
// bottom-up linear buffer, the colour formats as their own bytes, the others as RGBA8); with a pack.json that sets
// use_new_hash, CityHash64 of the guest bytes. Like Azahar, the first pack's options hold for all, the first pack to
// name a hash wins, PNGs load on worker threads (the game's own texture shows until then) and are flipped as
// flip_png_files says (default on), and mipmaps are generated (pica_vk.c). Without libpng (MSR_HAVE_PNG) this is
// compiled out.
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "msr.h"
#include "common/cityhash.h"
#ifdef MSR_HAVE_PNG
#include <png.h>
#endif
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- Azahar's decode for the legacy hash (rasterizer_cache/texture_codec.h, video_core/texture/etc1.cpp) ----
constexpr u32 FormatBits[14] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8};
constexpr u32 LinearBytes[14] = {4, 3, 2, 2, 2, 4, 4, 4, 4, 4, 4, 4, 4, 4};

u32 MortonInterleave(u32 x, u32 y) {
    static constexpr u32 xlut[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static constexpr u32 ylut[] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    return xlut[x % 8] + ylut[y % 8];
}
u8 Convert4To8(u8 v) { return static_cast<u8>((v << 4) | v); }
u8 Convert5To8(u8 v) { return static_cast<u8>((v << 3) | (v >> 2)); }

// SampleETC1Subtile: one texel of a 4x4 ETC1 block (the 3DS stores it as a little-endian 64-bit word)
void Etc1(u64 raw, u32 x, u32 y, u8* rgb) {
    static constexpr u8 table[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    const u32 texel = 4 * x + y;
    if ((raw >> 32) & 1) std::swap(x, y);   // flip
    int r, g, b;
    if ((raw >> 33) & 1) {   // differential
        const auto sx3 = [](u64 v) { return static_cast<int>(static_cast<std::int64_t>(v << 61) >> 61); };
        r = static_cast<int>((raw >> 59) & 31);
        g = static_cast<int>((raw >> 51) & 31);
        b = static_cast<int>((raw >> 43) & 31);
        if (x >= 2) {
            r += sx3(raw >> 56);
            g += sx3(raw >> 48);
            b += sx3(raw >> 40);
        }
        r = Convert5To8(static_cast<u8>(r));
        g = Convert5To8(static_cast<u8>(g));
        b = Convert5To8(static_cast<u8>(b));
    } else if (x < 2) {
        r = Convert4To8((raw >> 60) & 15);
        g = Convert4To8((raw >> 52) & 15);
        b = Convert4To8((raw >> 44) & 15);
    } else {
        r = Convert4To8((raw >> 56) & 15);
        g = Convert4To8((raw >> 48) & 15);
        b = Convert4To8((raw >> 40) & 15);
    }
    const u32 index = x < 2 ? (raw >> 37) & 7 : (raw >> 34) & 7;
    int modifier = table[index][(raw >> texel) & 1];
    if ((raw >> (16 + texel)) & 1) modifier = -modifier;
    rgb[0] = static_cast<u8>(std::clamp(r + modifier, 0, 255));
    rgb[1] = static_cast<u8>(std::clamp(g + modifier, 0, 255));
    rgb[2] = static_cast<u8>(std::clamp(b + modifier, 0, 255));
}

// DecodePixel<format, false> / DecodePixel4 / DecodePixelETC1 for one texel of an 8x8 tile
void DecodeTexel(int fmt, const u8* tile, u32 x, u32 y, u8* out) {
    if (fmt == 12 || fmt == 13) {
        const bool alpha4 = fmt == 13;
        const u8* sub = tile + ((x / 4) + 2 * (y / 4)) * (alpha4 ? 16 : 8);
        x %= 4;
        y %= 4;
        u8 alpha = 255;
        if (alpha4) {
            u64 packed;
            std::memcpy(&packed, sub, 8);
            sub += 8;
            alpha = Convert4To8((packed >> (4 * (x * 4 + y))) & 0xF);
        }
        u64 raw;
        std::memcpy(&raw, sub, 8);
        Etc1(raw, x, y, out);
        out[3] = alpha;
        return;
    }
    const u32 morton = MortonInterleave(x, y);
    if (fmt == 10 || fmt == 11) {   // I4, A4
        const u8 v = tile[morton >> 1];
        const u8 p = Convert4To8((morton % 2) ? (v >> 4) : (v & 0xF));
        if (fmt == 10) { out[0] = out[1] = out[2] = p; out[3] = 255; }
        else { out[0] = out[1] = out[2] = 0; out[3] = p; }
        return;
    }
    const u8* p = tile + morton * (FormatBits[fmt] / 8);
    switch (fmt) {
    case 5: out[0] = out[1] = out[2] = p[1]; out[3] = p[0]; return;                   // IA8
    case 6: out[0] = p[1]; out[1] = p[0]; out[2] = 0; out[3] = 255; return;           // RG8
    case 7: out[0] = out[1] = out[2] = p[0]; out[3] = 255; return;                    // I8
    case 8: out[0] = out[1] = out[2] = 0; out[3] = p[0]; return;                      // A8
    case 9: out[0] = out[1] = out[2] = Convert4To8(p[0] >> 4); out[3] = Convert4To8(p[0] & 0xF); return;   // IA4
    default: std::memcpy(out, p, FormatBits[fmt] / 8); return;                        // the colour formats as stored
    }
}

u64 LegacyHash(const u8* data, u32 w, u32 h, int fmt) {
    const u32 lb = LinearBytes[fmt], tile_bytes = FormatBits[fmt] * 64 / 8;
    std::vector<u8> linear(static_cast<std::size_t>(w) * h * lb);
    u8 texel[4];
    for (u32 ty = 0; ty < h / 8; ++ty) {
        for (u32 tx = 0; tx < w / 8; ++tx) {
            const u8* tile = data + (static_cast<std::size_t>(ty) * (w / 8) + tx) * tile_bytes;
            for (u32 y = 0; y < 8; ++y) {
                for (u32 x = 0; x < 8; ++x) {
                    DecodeTexel(fmt, tile, x, y, texel);
                    const std::size_t row = h - 1 - (ty * 8 + y), col = tx * 8 + x;
                    std::memcpy(linear.data() + (row * w + col) * lb, texel, lb);
                }
            }
        }
    }
    return Common::CityHash64(reinterpret_cast<const char*>(linear.data()), linear.size());
}

// paths as UTF-8 (C++20's u8string is std::u8string)
std::string Utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path FromUtf8(const char* s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s))); }

// ---- the packs ----
std::vector<fs::path> packs;
std::unordered_map<u64, fs::path> files;   // hash -> PNG, the first pack to name it
std::unordered_map<u64, std::size_t> file_pack;   // hash -> its pack's place in the list (0: the first, which wins)

// The prologue's nine pictures (512x256 RGB8, #20): the game writes 400 of their 512 columns and leaves the rest as
// memory held, so their hash depends on what was there before. The emulator the packs were dumped with saw each
// picture under two hashes (the HD Texture Pack and its Higher Res Intro add-on name both); here the rest is white and
// the hash another one again. Each is looked up under the packs' two names as well (the pairs matched by picture).
constexpr std::array<std::array<u64, 3>, 9> Aliases{{
    {0x0FF65C8E661011ECull, 0x56FE01C8009B7DD6ull, 0xFF6F671C72BFFFBDull},
    {0x1FD6F94901729614ull, 0x6D4B30277557A3CBull, 0x1B8721A987DB59D7ull},
    {0x22696E785E259786ull, 0x0A535304AFA8FF72ull, 0x4AD910FA88999906ull},
    {0x6E0391D509FEC4DEull, 0x47A85B5F586311D5ull, 0x5CBC7999B9277E5Bull},
    {0x9AEE430EC9AF4DA7ull, 0x98FECB9CBB1B53ABull, 0xF719232EB8B36E7Eull},
    {0xCAFAFD8ABE93EC4Bull, 0xD69D91D5805083CFull, 0x8CBB99E9708B35AEull},
    {0xD04F0586B12CBAB5ull, 0xE964A30BE89A69E6ull, 0xFCDCC2C7F4228F05ull},
    {0xD18E646E9775E0CCull, 0x159B71CD9086BEDCull, 0xF6FDF25679B2B316ull},
    {0xD6A7A5871C01ED1Full, 0x899A8FC266947786ull, 0x0E0A2E641CA605A1ull},
}};

// The file for a texture's hash: its own name's, or an alias's, from the pack highest in the list (an add-on above
// the main pack wins whichever of the names each uses); 0 when no pack names it. Called with `lock` held.
u64 Resolve(u64 hash) {
    u64 best = files.count(hash) ? hash : 0;
    for (const auto& names : Aliases) {
        if (names[0] != hash) continue;
        for (std::size_t i = 1; i < names.size(); ++i)
            if (files.count(names[i]) && (!best || file_pack[names[i]] < file_pack[best])) best = names[i];
    }
    return best;
}
bool use_new_hash = false, flip_png = true;
std::atomic<bool> active{false};

bool JsonBool(const std::string& text, const char* key, bool fallback) {
    const auto at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos) return fallback;
    const auto colon = text.find(':', at);
    if (colon == std::string::npos) return fallback;
    const auto value = text.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string::npos) return fallback;
    return text.compare(value, 4, "true") == 0 ? true : text.compare(value, 5, "false") == 0 ? false : fallback;
}

// ---- loading: PNGs decoded on worker threads, kept within a budget once uploaded ----
enum class State { Idle, Queued, Ready, Failed };
struct Image {
    State state = State::Idle;
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
    bool uploaded = false;
    void* staged = nullptr;      // its upload prepared by a worker (pica_vk.c's stager) until the GX thread takes it
    bool stage_queued = false;   // a worker is to prepare it again from the kept pixels
    bool stage_failed = false;   // the stager could not: the GX thread uploads it itself
    bool busy = false;           // a worker reads its pixels unlocked: not let go meanwhile
};
struct Found { u64 key; bool pending; };   // a texture content's pack hash (0: none), or its legacy hash under way
struct HashJob { u64 seen; u32 w, h; int fmt; std::vector<u8> level0; };
std::mutex lock;
std::condition_variable wake;
std::unordered_map<u64, Image> images;     // by pack hash
std::unordered_map<u64, Found> memo;       // texture content -> pack hash
std::deque<u64> queue;                     // PNGs to decode, or kept pixels to prepare for the GPU again
std::deque<HashJob> hash_jobs;             // legacy hashes to work out (before the PNGs: they are small)
// The upload of a replacement, prepared on a worker (2026-11-03): on a Steam Deck the GX thread spent up to 17 ms
// copying a 4096x4096 PNG into a new staging buffer and 1-5 ms creating its image, a missed frame each time one came in.
// pica_vk.c registers make (pixels -> a staging buffer and an image, any thread) and drop (one never taken).
void* (*stage_make)(const u8* rgba, u32 w, u32 h) = nullptr;
void (*stage_drop)(void* staged) = nullptr;
std::size_t staged_bytes = 0;              // prepared and not yet taken
constexpr std::size_t StageBudget = 256u << 20;
bool sync_find = false;                    // PWR_TEXPACK_SYNC=1: legacy hashes on the GX thread, no stager (as before)
std::list<u64> kept;                       // uploaded images still in memory, oldest first
std::size_t kept_bytes = 0;
constexpr std::size_t KeptBudget = 512u << 20;
std::vector<std::thread> workers;
std::atomic<unsigned> loaded{0}, failed{0};

bool DecodePng(const fs::path& path, Image& out) {
#ifdef MSR_HAVE_PNG
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, bytes.data(), bytes.size())) return false;
    png.format = PNG_FORMAT_RGBA;
    std::vector<u8> rgba(PNG_IMAGE_SIZE(png));
    // A negative row stride writes the rows bottom-up: the flip Azahar applies (flip_png_files).
    const png_int_32 stride = static_cast<png_int_32>(PNG_IMAGE_ROW_STRIDE(png));
    if (!png_image_finish_read(&png, nullptr, rgba.data(), flip_png ? -stride : stride, nullptr)) {
        png_image_free(&png);
        return false;
    }
    out.w = png.width;
    out.h = png.height;
    out.rgba = std::move(rgba);
    return true;
#else
    (void)path; (void)out;
    return false;
#endif
}

// Queues the PNG of a pack hash (lock held)
void Request(u64 key) {
    Image& image = images[key];
    if (image.state == State::Idle) { image.state = State::Queued; queue.push_back(key); wake.notify_one(); }
}

// PWR_TEXPACK_TRACE: each texture of 128 px or more the packs are asked about, by the file name that replaces it
// (and the alias's, when one of those names it)
void Trace(u32 w, u32 h, int fmt, u64 hash, u64 key) {
    static const bool on = std::getenv("PWR_TEXPACK_TRACE") != nullptr;
    if (!on || (w < 128 && h < 128)) return;
    std::fprintf(stderr, "texpack: %s tex1_%ux%u_%016llX_%d.png", key ? "found  " : "no file", w, h,
                 static_cast<unsigned long long>(hash), fmt);
    if (key && key != hash) std::fprintf(stderr, " as %016llX", static_cast<unsigned long long>(key));
    std::fputc('\n', stderr);
}

void Worker() {
#ifdef __linux__
    // below the game's threads: a PNG decode is never in a hurry (the game's own texture shows meanwhile)
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 5);
#endif
    std::unique_lock<std::mutex> guard(lock);
    for (;;) {
        wake.wait(guard, [] { return !queue.empty() || !hash_jobs.empty(); });
        if (!hash_jobs.empty()) {   // a texture's legacy hash (msr_texture_find): its pack hash, then its PNG
            HashJob job = std::move(hash_jobs.front());
            hash_jobs.pop_front();
            guard.unlock();
            const u64 hash = LegacyHash(job.level0.data(), job.w, job.h, job.fmt);
            guard.lock();
            const u64 key = Resolve(hash);
            Trace(job.w, job.h, job.fmt, hash, key);
            memo[job.seen] = {key, false};
            if (key) Request(key);
            continue;
        }
        const u64 key = queue.front();
        queue.pop_front();
        Image& image = images[key];   // (elements of an unordered_map stay where they are)
        if (image.state == State::Ready) {   // the pixels kept: their upload prepared again (msr_texture_take)
            // stage_queued stays set until it is done (msr_texture_take waits for it); cleared, this entry is stale
            if (!image.stage_queued || image.staged || image.rgba.empty() || !stage_make) { image.stage_queued = false; continue; }
            image.busy = true;
            const u8* px = image.rgba.data();
            const u32 w = image.w, h = image.h;
            guard.unlock();
            void* staged = stage_make(px, w, h);
            guard.lock();
            image.busy = false;
            image.stage_queued = false;
            if (staged) { image.staged = staged; staged_bytes += image.rgba.size(); }
            else image.stage_failed = true;
            continue;
        }
        if (image.state != State::Queued) continue;
        const fs::path path = files[key];
        const bool stage = stage_make && staged_bytes < StageBudget;
        guard.unlock();
        Image decoded;
        const bool ok = DecodePng(path, decoded);
        void* staged = ok && stage ? stage_make(decoded.rgba.data(), decoded.w, decoded.h) : nullptr;
        guard.lock();
        if (ok) {
            image.w = decoded.w;
            image.h = decoded.h;
            image.rgba = std::move(decoded.rgba);
            image.state = State::Ready;
            if (staged) { image.staged = staged; staged_bytes += image.rgba.size(); }
            loaded++;
        } else {
            image.state = State::Failed;
            if (failed++ < 8) std::fprintf(stderr, "msr: texture %s could not be read\n", Utf8(path).c_str());
        }
    }
}

} // namespace

extern "C" int msr_textures_add_pack(const char* dir) {
    fs::path root = FromUtf8(dir);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        std::fprintf(stderr, "msr: texture pack %s not found\n", dir);
        return -1;
    }
    // pack.json nearest the pack's top: its options, from the first pack only (as the emulator's build)
    fs::path config;
    std::size_t depth = SIZE_MAX, before = files.size();
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) continue;
        const std::string name = Utf8(it->path().filename());
        if (name == "pack.json" && static_cast<std::size_t>(it.depth()) < depth) {
            config = it->path();
            depth = static_cast<std::size_t>(it.depth());
            continue;
        }
        unsigned w, h, format;
        unsigned long long hash;
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0 ||
            std::sscanf(name.c_str(), "tex1_%ux%u_%llX_%u", &w, &h, &hash, &format) != 4) {
            continue;
        }
        const auto mip = name.find("_mip");
        if (mip != std::string::npos && name.compare(mip, 6, "_mip0.") != 0) continue;   // only level 0
        if (files.emplace(hash, it->path()).second) file_pack[hash] = packs.size();
    }
    if (packs.empty()) {
        use_new_hash = false;
        flip_png = true;
        if (!config.empty()) {
            std::ifstream file(config, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            use_new_hash = JsonBool(text, "use_new_hash", false);
            flip_png = JsonBool(text, "flip_png_files", true);
        }
    }
    packs.push_back(root);
    std::fprintf(stderr, "msr: texture pack %s: %zu textures%s\n", Utf8(root).c_str(), files.size() - before,
                 packs.size() == 1 ? (use_new_hash ? " (new hash)" : " (legacy hash)") : "");
    return 0;
}

extern "C" void msr_textures_start(void) {
#ifndef MSR_HAVE_PNG
    if (!files.empty()) std::fprintf(stderr, "msr: texture packs need libpng, which this build lacks\n");
    return;
#endif
    if (files.empty() || active) return;
    sync_find = std::getenv("PWR_TEXPACK_SYNC") != nullptr;
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2 - 1, 2u, 4u);
    for (unsigned i = 0; i < n; ++i) workers.emplace_back(Worker).detach();
    active = true;
}

extern "C" int msr_textures_active(void) { return active.load(std::memory_order_relaxed); }

extern "C" uint64_t msr_texture_find(const uint8_t* level0, uint32_t w, uint32_t h, int fmt, uint64_t content, int* pending) {
    *pending = 0;
    if (!active.load(std::memory_order_relaxed) || fmt < 0 || fmt > 13 || !w || !h || (w % 8) || (h % 8)) return 0;
    const u64 seen = content ^ (static_cast<u64>(w) << 40) ^ (static_cast<u64>(h) << 20) ^ static_cast<u64>(fmt);
    const std::size_t bytes = std::size_t{w} * h * FormatBits[fmt] / 8;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (const auto it = memo.find(seen); it != memo.end()) {
            if (it->second.pending) { *pending = 1; return 0; }
            if (it->second.key) Request(it->second.key);
            return it->second.key;
        }
        // The legacy hash decodes the texture as Azahar does: 20 ms for a 1024x1024 ETC1 texture on a Steam Deck, on the
        // GX thread the first time it was drawn. A worker works it out from a copy (the game's texture shows meanwhile).
        if (!use_new_hash && !sync_find) {
            memo[seen] = {0, true};
            hash_jobs.push_back({seen, w, h, fmt, std::vector<u8>(level0, level0 + bytes)});
            wake.notify_one();
            *pending = 1;
            return 0;
        }
    }
    const u64 hash = use_new_hash ? Common::CityHash64(reinterpret_cast<const char*>(level0), bytes)
                                  : LegacyHash(level0, w, h, fmt);
    std::lock_guard<std::mutex> guard(lock);
    const u64 key = Resolve(hash);
    Trace(w, h, fmt, hash, key);
    memo[seen] = {key, false};
    if (key) Request(key);
    return key;
}

extern "C" void msr_textures_set_stager(void* (*make)(const uint8_t*, uint32_t, uint32_t), void (*drop)(void*)) {
    std::lock_guard<std::mutex> guard(lock);
    if (sync_find) return;
    stage_make = make;
    stage_drop = drop;
}

extern "C" int msr_texture_take(uint64_t key, int want_staged, void** staged, const uint8_t** rgba, uint32_t* w, uint32_t* h) {
    std::lock_guard<std::mutex> guard(lock);
    *staged = nullptr;
    const auto it = images.find(key);
    if (it == images.end()) return -1;
    Image& image = it->second;
    if (image.state == State::Failed) return -1;
    if (image.state == State::Idle) Request(key);
    if (image.state != State::Ready) return 0;
    *rgba = image.rgba.data();
    *w = image.w;
    *h = image.h;
    if (image.staged && !want_staged) { stage_drop(image.staged); image.staged = nullptr; staged_bytes -= image.rgba.size(); }
    if (!want_staged) return 1;
    if (image.staged) {
        *staged = image.staged;
        image.staged = nullptr;
        staged_bytes -= image.rgba.size();
        return 2;
    }
    if (image.stage_queued) return 0;
    if (stage_make && !image.stage_failed && staged_bytes < StageBudget) {   // an upload again (the last one was taken)
        image.stage_queued = true;
        queue.push_back(key);
        wake.notify_one();
        return 0;
    }
    return 1;
}

extern "C" void msr_texture_done(uint64_t key) {
    std::lock_guard<std::mutex> guard(lock);
    const auto it = images.find(key);
    if (it == images.end() || it->second.state != State::Ready || it->second.uploaded) return;
    it->second.uploaded = true;
    kept.push_back(key);
    kept_bytes += it->second.rgba.size();
    while (kept_bytes > KeptBudget && kept.size() > 1) {   // the oldest uploads go; a later use loads them again
        Image& old = images[kept.front()];
        if (old.busy) break;   // a worker prepares its upload: later
        if (old.staged) { stage_drop(old.staged); old.staged = nullptr; staged_bytes -= old.rgba.size(); }
        old.stage_queued = false;
        kept_bytes -= old.rgba.size();
        old.rgba.clear();
        old.rgba.shrink_to_fit();
        old.state = State::Idle;
        old.uploaded = false;
        kept.pop_front();
    }
}

extern "C" void msr_textures_stats(unsigned* textures, unsigned* loaded_out, unsigned* failed_out) {
    *textures = static_cast<unsigned>(files.size());
    *loaded_out = loaded.load();
    *failed_out = failed.load();
}
