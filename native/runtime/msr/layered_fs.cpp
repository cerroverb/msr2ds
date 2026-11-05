// The RomFS the game reads on msr-native, with the game mods' files: a port of the emulator's LayeredFS
// (src/core/file_sys/layered_fs.cpp, from Citra). The game parses the RomFS image itself (its directory and file tables,
// then file data by offset), so with mods the image is rebuilt: the dump's tables are read, each mod's romfs/ replaces
// or adds files and its romfs_ext/ removes (.stub) or patches (.ips, .bps) them, applied from the lowest priority up,
// then new tables are written and each file's data comes from the dump, a mod's file or the patched copy.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "dump.h"
#include "msr.h"
#include "mods.h"
#include "patch.h"

namespace fs = std::filesystem;

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Descriptor {
    u32 offset, length;
};
struct RomFSHeader {
    u32 header_length;
    Descriptor directory_hash_table, directory_metadata_table, file_hash_table, file_metadata_table;
    u32 file_data_offset;
};
static_assert(sizeof(RomFSHeader) == 0x28);
struct DirectoryMetadata {
    u32 parent_directory_offset, next_sibling_offset, first_child_directory_offset, first_file_offset, hash_bucket_next,
        name_length;
};
static_assert(sizeof(DirectoryMetadata) == 0x18);
#pragma pack(push, 4)
struct FileMetadata {
    u32 parent_directory_offset, next_sibling_offset;
    u64 file_data_offset, file_data_length;
    u32 hash_bucket_next, name_length;
};
#pragma pack(pop)
static_assert(sizeof(FileMetadata) == 0x20);

struct Directory;
struct File {
    std::string name, path;
    int type = 0;                 // 0 the dump's, 1 replaced or added, 2 patched, 3 removed
    u64 original_offset = 0;      // type 0, from the image's start
    std::string replace_path;     // type 1
    std::vector<u8> patched;      // type 2
    u64 size = 0;
    Directory* parent = nullptr;
};
struct Directory {
    std::string name, path;   // path with a trailing '/'
    std::vector<std::unique_ptr<File>> files;
    std::vector<std::unique_ptr<Directory>> directories;
    Directory* parent = nullptr;
};

fs::path P(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}
std::string S(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::u16string Utf16(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned code, extra;
        if (c < 0x80) { code = c; extra = 0; }
        else if ((c >> 5) == 6) { code = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 14) { code = c & 0x0F; extra = 2; }
        else { code = c & 0x07; extra = 3; }
        ++i;
        for (unsigned k = 0; k < extra && i < s.size(); ++k, ++i) code = code << 6 | (static_cast<unsigned char>(s[i]) & 0x3F);
        if (code >= 0x10000) {
            code -= 0x10000;
            out += static_cast<char16_t>(0xD800 | code >> 10);
            out += static_cast<char16_t>(0xDC00 | (code & 0x3FF));
        } else {
            out += static_cast<char16_t>(code);
        }
    }
    return out;
}

std::string Utf8(const std::u16string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned code = s[i];
        if (code >= 0xD800 && code < 0xDC00 && i + 1 < s.size()) code = 0x10000 + ((code - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) { out += static_cast<char>(0xC0 | code >> 6); out += static_cast<char>(0x80 | (code & 0x3F)); }
        else if (code < 0x10000) { out += static_cast<char>(0xE0 | code >> 12); out += static_cast<char>(0x80 | (code >> 6 & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        else { out += static_cast<char>(0xF0 | code >> 18); out += static_cast<char>(0x80 | (code >> 12 & 0x3F)); out += static_cast<char>(0x80 | (code >> 6 & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
    }
    return out;
}

u64 AlignUp(u64 v, u64 a) {
    return (v + a - 1) / a * a;
}

std::FILE* OpenRead(const std::string& path) {
#ifdef _WIN32
    return _wfopen(P(path).c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

bool ReadWhole(const std::string& path, std::vector<u8>& out) {
    std::FILE* f = OpenRead(path);
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
    const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

class LayeredFS {
public:
    bool Build(const std::string& dump, u64 offset, u64 size, const std::vector<std::string>& mods);
    u64 Size() const { return metadata.size() + data_size; }
    std::size_t Read(u64 offset, std::size_t length, u8* out);

private:
    bool ReadBase(u64 offset, std::size_t length, u8* out);
    std::string ReadName(u64 offset, u32 length);
    u32 LoadDirectory(Directory& current, u32 offset);
    u32 LoadFile(Directory& parent, u32 offset);
    void LoadRelocations(const std::string& romfs);
    void LoadExtRelocations(const std::string& romfs_ext);
    std::vector<u8> ReadCurrent(const File& file);
    void PrepareDirectory(Directory& current);
    void PrepareFile(File& current);
    void Prepare(Directory& current);
    void BuildDirectories();
    void BuildFiles();
    void RebuildMetadata();

    DumpFile* base = nullptr;   // the game file, plain or compressed (dump.c)
    u64 base_offset = 0, base_size = 0;
    RomFSHeader header{};
    Directory root;
    std::unordered_map<std::string, File*> file_path_map;
    std::unordered_map<std::string, Directory*> directory_path_map;

    std::map<u64, File*> data_offset_map;
    std::vector<u8> metadata;
    std::vector<u32> directory_hash_table, file_hash_table;
    std::unordered_map<Directory*, u32> directory_offsets;
    std::vector<Directory*> directory_list;
    u64 directory_offset = 0;
    std::vector<u8> directory_table;
    std::unordered_map<File*, u32> file_offsets;
    std::vector<File*> file_list;
    u64 file_offset = 0;
    std::vector<u8> file_table;
    u64 data_size = 0;

    std::mutex lock;
    std::string open_path;   // the replacement file read last, kept open
    std::FILE* open_file = nullptr;
};

bool LayeredFS::ReadBase(u64 offset, std::size_t length, u8* out) {
    if (offset > base_size || length > base_size - offset) return false;
    return dump_read(base, base_offset + offset, out, length) == 0;
}

std::string LayeredFS::ReadName(u64 offset, u32 length) {
    std::vector<u8> raw(length);
    ReadBase(offset, length, raw.data());
    std::u16string name(length / 2, 0);
    for (std::size_t i = 0; i < name.size(); ++i) name[i] = static_cast<char16_t>(raw[2 * i] | raw[2 * i + 1] << 8);
    return Utf8(name);
}

u32 LayeredFS::LoadDirectory(Directory& current, u32 offset) {
    DirectoryMetadata m{};
    ReadBase(header.directory_metadata_table.offset + offset, sizeof m, reinterpret_cast<u8*>(&m));
    current.name = ReadName(header.directory_metadata_table.offset + offset + sizeof m, m.name_length);
    current.path = current.parent->path + current.name + "/";
    directory_path_map.emplace(current.path, &current);
    for (u32 f = m.first_file_offset; f != 0xFFFFFFFF;) f = LoadFile(current, f);
    for (u32 d = m.first_child_directory_offset; d != 0xFFFFFFFF;) {
        auto child = std::make_unique<Directory>();
        Directory& directory = *child;
        directory.parent = &current;
        current.directories.push_back(std::move(child));
        d = LoadDirectory(directory, d);
    }
    return m.next_sibling_offset;
}

u32 LayeredFS::LoadFile(Directory& parent, u32 offset) {
    FileMetadata m{};
    ReadBase(header.file_metadata_table.offset + offset, sizeof m, reinterpret_cast<u8*>(&m));
    auto file = std::make_unique<File>();
    file->name = ReadName(header.file_metadata_table.offset + offset + sizeof m, m.name_length);
    file->path = parent.path + file->name;
    file->original_offset = header.file_data_offset + m.file_data_offset;
    file->size = m.file_data_length;
    file->parent = &parent;
    file_path_map.emplace(file->path, file.get());
    parent.files.push_back(std::move(file));
    return m.next_sibling_offset;
}

void LayeredFS::LoadRelocations(const std::string& romfs) {
    const fs::path top = P(romfs);
    std::error_code e;
    if (!fs::is_directory(top, e)) return;
    for (fs::recursive_directory_iterator it(top, fs::directory_options::skip_permission_denied, e), end; !e && it != end;
         it.increment(e)) {
        const std::string rel = S(it->path().lexically_relative(top));
        const std::string name = S(it->path().filename());
        const auto slash = rel.rfind('/');
        const std::string parent_path = slash == std::string::npos ? "/" : "/" + rel.substr(0, slash) + "/";
        const auto parent_it = directory_path_map.find(parent_path);
        if (parent_it == directory_path_map.end()) continue;
        Directory* parent = parent_it->second;
        std::error_code e2;
        if (it->is_directory(e2)) {
            const std::string path = "/" + rel + "/";
            if (!directory_path_map.count(path)) {
                auto child = std::make_unique<Directory>();
                child->name = name;
                child->path = path;
                child->parent = parent;
                directory_path_map.emplace(path, child.get());
                parent->directories.push_back(std::move(child));
            }
            continue;
        }
        const std::string path = "/" + rel;
        if (!file_path_map.count(path)) {
            auto file = std::make_unique<File>();
            file->name = name;
            file->path = path;
            file->parent = parent;
            file_path_map.emplace(path, file.get());
            parent->files.push_back(std::move(file));
        }
        File* file = file_path_map.at(path);
        file->type = 1;
        file->replace_path = S(it->path());
        file->patched.clear();
        file->size = it->file_size(e2);
    }
}

void LayeredFS::LoadExtRelocations(const std::string& romfs_ext) {
    const fs::path top = P(romfs_ext);
    std::error_code e;
    if (!fs::is_directory(top, e)) return;
    for (fs::recursive_directory_iterator it(top, fs::directory_options::skip_permission_denied, e), end; !e && it != end;
         it.increment(e)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string path = "/" + S(it->path().lexically_relative(top));
        if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".stub") == 0) {
            const std::string target = path.substr(0, path.size() - 5);
            if (const auto f = file_path_map.find(target); f != file_path_map.end()) {
                f->second->type = 3;
                f->second->size = 0;
                f->second->patched.clear();
                file_path_map.erase(f);
            } else {
                std::fprintf(stderr, "msr: romfs_ext: no file %s to remove\n", target.c_str());
            }
            continue;
        }
        if (path.size() < 4) continue;
        const std::string extension = path.substr(path.size() - 4), target = path.substr(0, path.size() - 4);
        if (extension != ".ips" && extension != ".bps") {
            std::fprintf(stderr, "msr: romfs_ext: unknown file %s\n", path.c_str());
            continue;
        }
        const auto f = file_path_map.find(target);
        if (f == file_path_map.end()) {
            std::fprintf(stderr, "msr: romfs_ext: no file %s to patch\n", target.c_str());
            continue;
        }
        std::vector<u8> patch;
        if (!ReadWhole(S(it->path()), patch)) continue;
        std::vector<u8> buffer = ReadCurrent(*f->second);
        if (extension == ".ips" ? MsrPatch::ApplyIps(patch, buffer) : MsrPatch::ApplyBps(patch, buffer)) {
            f->second->type = 2;
            f->second->size = buffer.size();
            f->second->patched = std::move(buffer);
        } else {
            std::fprintf(stderr, "msr: romfs_ext: %s does not fit %s\n", path.c_str(), target.c_str());
        }
    }
}

std::vector<u8> LayeredFS::ReadCurrent(const File& file) {
    if (file.type == 2) return file.patched;
    std::vector<u8> buffer(file.size);
    if (file.type == 1) {
        std::vector<u8> whole;
        if (ReadWhole(file.replace_path, whole)) buffer = std::move(whole);
    } else {
        ReadBase(file.original_offset, buffer.size(), buffer.data());
    }
    return buffer;
}

std::size_t NameSize(const std::string& name) {
    return AlignUp(Utf16(name).size() * 2, 4);
}

void LayeredFS::PrepareDirectory(Directory& current) {
    directory_offsets.emplace(&current, static_cast<u32>(directory_offset));
    directory_list.push_back(&current);
    directory_offset += sizeof(DirectoryMetadata) + NameSize(current.name);
}

void LayeredFS::PrepareFile(File& current) {
    if (current.type == 3) return;   // removed
    file_offsets.emplace(&current, static_cast<u32>(file_offset));
    file_list.push_back(&current);
    file_offset += sizeof(FileMetadata) + NameSize(current.name);
}

void LayeredFS::Prepare(Directory& current) {
    for (const auto& child : current.files) PrepareFile(*child);
    for (const auto& child : current.directories) PrepareDirectory(*child);
    for (const auto& child : current.directories) Prepare(*child);
}

u32 CalcHash(const std::string& name, u32 parent_offset) {   // 3dbrew
    u32 hash = parent_offset ^ 123456789;
    for (char16_t c : Utf16(name)) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= static_cast<std::uint16_t>(c);
    }
    return hash;
}

std::size_t WriteName(u8* dest, const std::u16string& name) {
    const std::size_t size = AlignUp(name.size() * 2, 4);
    std::memset(dest, 0, size);
    for (std::size_t i = 0; i < name.size(); ++i) {
        dest[2 * i] = static_cast<u8>(name[i]);
        dest[2 * i + 1] = static_cast<u8>(name[i] >> 8);
    }
    return size;
}

void LayeredFS::BuildDirectories() {
    directory_table.assign(directory_offset, 0xFF);
    std::size_t written = 0;
    for (Directory* directory : directory_list) {
        DirectoryMetadata m;
        std::memset(&m, 0xFF, sizeof m);
        m.parent_directory_offset = directory_offsets.at(directory->parent);
        if (directory->parent != directory) {
            bool found = false;
            for (const auto& sibling : directory->parent->directories) {
                if (found) {
                    m.next_sibling_offset = directory_offsets.at(sibling.get());
                    break;
                }
                found = sibling.get() == directory;
            }
        }
        if (!directory->directories.empty()) m.first_child_directory_offset = directory_offsets.at(directory->directories.front().get());
        // The first file not removed
        for (const auto& file : directory->files) {
            if (file->type != 3) {
                m.first_file_offset = file_offsets.at(file.get());
                break;
            }
        }
        const u32 bucket = CalcHash(directory->name, m.parent_directory_offset) % directory_hash_table.size();
        m.hash_bucket_next = directory_hash_table[bucket];
        directory_hash_table[bucket] = directory_offsets.at(directory);
        const std::u16string name = Utf16(directory->name);
        m.name_length = static_cast<u32>(name.size() * 2);
        std::memcpy(directory_table.data() + written, &m, sizeof m);
        written += sizeof m;
        written += WriteName(directory_table.data() + written, name);
    }
}

void LayeredFS::BuildFiles() {
    file_table.assign(file_offset, 0xFF);
    std::size_t written = 0;
    for (File* file : file_list) {
        FileMetadata m;
        std::memset(&m, 0xFF, sizeof m);
        m.parent_directory_offset = directory_offsets.at(file->parent);
        bool found = false;
        for (const auto& sibling : file->parent->files) {
            if (sibling->type == 3) continue;
            if (found) {
                m.next_sibling_offset = file_offsets.at(sibling.get());
                break;
            }
            found = sibling.get() == file;
        }
        m.file_data_offset = data_size;
        m.file_data_length = file->size;
        data_size += AlignUp(file->size, 16);
        if (file->size != 0) data_offset_map.emplace(m.file_data_offset, file);
        const u32 bucket = CalcHash(file->name, m.parent_directory_offset) % file_hash_table.size();
        m.hash_bucket_next = file_hash_table[bucket];
        file_hash_table[bucket] = file_offsets.at(file);
        const std::u16string name = Utf16(file->name);
        m.name_length = static_cast<u32>(name.size() * 2);
        std::memcpy(file_table.data() + written, &m, sizeof m);
        written += sizeof m;
        written += WriteName(file_table.data() + written, name);
    }
}

std::size_t HashTableSize(std::size_t entries) {   // 3dbrew
    if (entries < 3) return 3;
    if (entries < 19) return entries | 1;
    std::size_t count = entries;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 || count % 13 == 0 ||
           count % 17 == 0)
        ++count;
    return count;
}

void LayeredFS::RebuildMetadata() {
    PrepareDirectory(root);
    Prepare(root);
    directory_hash_table.assign(HashTableSize(directory_list.size()), 0xFFFFFFFF);
    file_hash_table.assign(HashTableSize(file_list.size()), 0xFFFFFFFF);
    BuildDirectories();
    BuildFiles();
    RomFSHeader h{};
    h.header_length = sizeof h;
    h.directory_hash_table = {sizeof h, static_cast<u32>(directory_hash_table.size() * 4)};
    h.directory_metadata_table = {h.directory_hash_table.offset + h.directory_hash_table.length, static_cast<u32>(directory_table.size())};
    h.file_hash_table = {h.directory_metadata_table.offset + h.directory_metadata_table.length, static_cast<u32>(file_hash_table.size() * 4)};
    h.file_metadata_table = {h.file_hash_table.offset + h.file_hash_table.length, static_cast<u32>(file_table.size())};
    h.file_data_offset = static_cast<u32>(AlignUp(h.file_metadata_table.offset + h.file_metadata_table.length, 16));
    metadata.assign(h.file_data_offset, 0);
    std::memcpy(metadata.data(), &h, sizeof h);
    std::memcpy(metadata.data() + h.directory_hash_table.offset, directory_hash_table.data(), h.directory_hash_table.length);
    std::memcpy(metadata.data() + h.directory_metadata_table.offset, directory_table.data(), h.directory_metadata_table.length);
    std::memcpy(metadata.data() + h.file_hash_table.offset, file_hash_table.data(), h.file_hash_table.length);
    std::memcpy(metadata.data() + h.file_metadata_table.offset, file_table.data(), h.file_metadata_table.length);
}

bool LayeredFS::Build(const std::string& dump, u64 offset, u64 size, const std::vector<std::string>& mods) {
    base = dump_open(dump.c_str());
    base_offset = offset;
    base_size = size;
    if (!base || !ReadBase(0, sizeof header, reinterpret_cast<u8*>(&header)) || header.header_length != sizeof header) {
        std::fprintf(stderr, "msr: cannot read the RomFS for the game mods\n");
        return false;
    }
    root.parent = &root;
    LoadDirectory(root, 0);
    for (auto mod = mods.rbegin(); mod != mods.rend(); ++mod) {   // the lowest priority first
        LoadRelocations(*mod + "romfs/");
        LoadExtRelocations(*mod + "romfs_ext/");
    }
    RebuildMetadata();
    return true;
}

std::size_t LayeredFS::Read(u64 offset, std::size_t length, u8* out) {
    std::scoped_lock guard{lock};
    if (offset >= Size()) return 0;
    length = static_cast<std::size_t>(std::min<u64>(length, Size() - offset));
    std::size_t done = 0;
    if (offset < metadata.size()) {
        const std::size_t n = static_cast<std::size_t>(std::min<u64>(metadata.size() - offset, length));
        std::memcpy(out, metadata.data() + offset, n);
        done = n;
        offset = 0;
    } else {
        offset -= metadata.size();
    }
    if (done == length || data_offset_map.empty()) {
        std::memset(out + done, 0, length - done);
        return length;
    }
    auto current = data_offset_map.upper_bound(offset);
    if (current != data_offset_map.begin()) --current;
    while (done < length) {
        if (current == data_offset_map.end()) {   // past the last file: padding
            std::memset(out + done, 0, length - done);
            break;
        }
        File& file = *current->second;
        if (offset < current->first) {   // before this file (a gap of empty files)
            const std::size_t gap = static_cast<std::size_t>(std::min<u64>(current->first - offset, length - done));
            std::memset(out + done, 0, gap);
            done += gap;
            offset += gap;
            continue;
        }
        const u64 relative = offset - current->first;
        const std::size_t n = file.size > relative ? static_cast<std::size_t>(std::min<u64>(file.size - relative, length - done)) : 0;
        const std::size_t pad = static_cast<std::size_t>(std::min<u64>(AlignUp(file.size, 16) - relative, length - done)) - n;
        if (file.type == 0) {
            ReadBase(file.original_offset + relative, n, out + done);
        } else if (file.type == 1) {
            if (open_path != file.replace_path) {
                if (open_file) std::fclose(open_file);
                open_file = OpenRead(file.replace_path);
                open_path = file.replace_path;
            }
            if (!open_file || fseeko(open_file, static_cast<off_t>(relative), SEEK_SET) != 0 ||
                std::fread(out + done, 1, n, open_file) != n) {
                std::fprintf(stderr, "msr: cannot read %s\n", file.replace_path.c_str());
                std::memset(out + done, 0, n);
            }
        } else {
            std::memcpy(out + done, file.patched.data() + relative, n);
        }
        std::memset(out + done + n, 0, pad);
        done += n + pad;
        offset += n + pad;
        ++current;
    }
    return length;
}

std::unique_ptr<LayeredFS> layered;

} // namespace

extern "C" int msr_romfs_open(const char* dump, uint64_t offset, uint64_t size) {
    const auto& mods = NativeMods::BootedMods();
    bool any = false;
    std::error_code e;
    for (const std::string& mod : mods) {
        any |= fs::is_directory(P(mod + "romfs"), e) || fs::is_directory(P(mod + "romfs_ext"), e);
    }
    if (!any) return 0;
    auto built = std::make_unique<LayeredFS>();
    if (!built->Build(dump, offset, size, mods)) return -1;
    std::fprintf(stderr, "msr: RomFS with the game mods: %llu bytes\n", static_cast<unsigned long long>(built->Size()));
    layered = std::move(built);
    return 0;
}

extern "C" int msr_romfs_layered(void) {
    return layered != nullptr;
}

extern "C" uint64_t msr_romfs_size(void) {
    return layered ? layered->Size() : 0;
}

extern "C" size_t msr_romfs_read(uint64_t offset, size_t length, uint8_t* out) {
    return layered ? layered->Read(offset, length, out) : 0;
}
