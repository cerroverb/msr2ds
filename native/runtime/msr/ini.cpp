// settings.ini as QSettings reads and writes it (ini.h). The escaping follows Qt 6's qsettings.cpp
// (iniEscapedString, iniUnescapedStringList, variantToString): UTF-8 text, quotes around a value with ',' ';' '='
// or spaces at either end, backslash escapes, a leading '@' doubled.
#include "ini.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" int rt_replace_file(const char* tmp, const char* dst);   // runtime.c

namespace MsrIni {
namespace {

/// Held while a file is read or replaced: the Mods and Textures tabs' scanner reads settings.ini on its own thread, and
/// on Windows a file open in one thread cannot be replaced from another (MoveFileEx fails).
std::mutex& FileLock() {
    static std::mutex lock;
    return lock;
}

std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

bool IsSection(const std::string& line, std::string* name) {
    const std::string t = Trim(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return false;
    if (name) *name = t.substr(1, t.size() - 2);
    return true;
}

void AppendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | code >> 6);
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | code >> 12);
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// A value's leading '@' marks a type (@Invalid(), @ByteArray(...)); a string starting with '@' has it doubled.
std::string FromVariant(const std::string& s) {
    if (s.size() >= 2 && s[0] == '@' && s[1] == '@') return s.substr(1);
    return s;
}

std::string ToVariant(const std::string& s) {
    return !s.empty() && s[0] == '@' ? "@" + s : s;
}

} // namespace

std::string Escape(const std::string& value) {
    std::string out;
    bool quotes = false;
    for (unsigned char c : value) {
        if (c == ';' || c == ',' || c == '=') quotes = true;
        switch (c) {
        case '\a': out += "\\a"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\v': out += "\\v"; break;
        case '"':
        case '\\':
            out += '\\';
            out += static_cast<char>(c);
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char hex[8];
                std::snprintf(hex, sizeof hex, "\\x%x", c);
                out += hex;
            } else {
                out += static_cast<char>(c);   // UTF-8 bytes as they are
            }
        }
    }
    if (quotes || (!out.empty() && (out.front() == ' ' || out.back() == ' '))) out = '"' + out + '"';
    return out;
}

std::vector<std::string> Unescape(const std::string& raw) {
    std::vector<std::string> list;
    std::string current;
    bool in_quotes = false, quoted = false, is_list = false;
    std::size_t i = 0, chop = 0;   // chop: trailing spaces before this point are kept (escaped or quoted)
    const auto chop_spaces = [&] {
        while (current.size() > chop && (current.back() == ' ' || current.back() == '\t')) current.pop_back();
    };
    const auto skip_spaces = [&] {
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
    };
    skip_spaces();
    chop = 0;
    while (i < raw.size()) {
        const char c = raw[i];
        if (c == '\\') {
            if (++i >= raw.size()) break;
            const char e = raw[i++];
            const char* from = "abfnrtv\"?'\\";
            const char* to = "\a\b\f\n\r\t\v\"?'\\";
            if (const char* p = std::strchr(from, e); p && e) {
                current += to[p - from];
            } else if (e == 'x') {
                unsigned code = 0;
                while (i < raw.size() && HexDigit(raw[i]) >= 0) code = code * 16 + static_cast<unsigned>(HexDigit(raw[i++]));
                AppendUtf8(current, code);
            } else if (e >= '0' && e <= '7') {
                unsigned code = static_cast<unsigned>(e - '0');
                while (i < raw.size() && raw[i] >= '0' && raw[i] <= '7') code = code * 8 + static_cast<unsigned>(raw[i++] - '0');
                AppendUtf8(current, code);
            }
            chop = current.size();
        } else if (c == '"') {
            ++i;
            quoted = true;
            in_quotes = !in_quotes;
            if (!in_quotes) {
                chop = current.size();
                skip_spaces();
            }
        } else if (c == ',' && !in_quotes) {
            if (!quoted) chop_spaces();
            is_list = true;
            list.push_back(current);
            current.clear();
            quoted = false;
            ++i;
            skip_spaces();
            chop = 0;
        } else {
            current += c;
            ++i;
        }
    }
    if (!quoted) chop_spaces();
    if (is_list) {
        list.push_back(current);
        return list;
    }
    return {current};
}

File::File(std::string path_) : path(std::move(path_)) {
    std::string text;
    {
        std::scoped_lock lock{FileLock()};
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            char buf[4096];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
    }
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        start = end + 1;
    }
}

std::optional<std::string> File::Raw(const std::string& group, const std::string& key) const {
    std::string section = "General", name;
    for (const std::string& line : lines) {
        if (IsSection(line, &name)) {
            section = name;
            continue;
        }
        if (section != group) continue;
        const auto eq = line.find('=');
        if (eq != std::string::npos && Trim(line.substr(0, eq)) == key) return Trim(line.substr(eq + 1));
    }
    return std::nullopt;
}

void File::SetRaw(const std::string& group, const std::string& key, const std::string& raw) {
    const std::string entry = key + "=" + raw;
    // The sections, in order: lines before the first header are [General], as QSettings reads them.
    struct Section {
        std::string name;
        std::size_t begin, end;   // the lines after the header
    };
    std::vector<Section> sections{{"General", 0, lines.size()}};
    std::string name;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!IsSection(lines[i], &name)) continue;
        sections.back().end = i;
        sections.push_back({name, i + 1, lines.size()});
    }
    const Section* last = nullptr;
    for (const Section& section : sections) {
        if (section.name != group) continue;
        for (std::size_t i = section.begin; i < section.end; ++i) {
            const auto eq = lines[i].find('=');
            if (eq != std::string::npos && Trim(lines[i].substr(0, eq)) == key) {
                lines[i] = entry;
                return;
            }
        }
        if (section.end > section.begin || &section != &sections.front()) last = &section;
    }
    if (last) {   // before the blank lines ending the section
        std::size_t at = last->end;
        while (at > last->begin && lines[at - 1].empty()) --at;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), entry);
    } else if (group == "General") {
        lines.insert(lines.begin(), {"[General]", entry, ""});
    } else {
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back("[" + group + "]");
        lines.push_back(entry);
    }
}

std::string File::String(const std::string& group, const std::string& key, const std::string& fallback) const {
    const auto raw = Raw(group, key);
    if (!raw) return fallback;
    const auto parts = Unescape(*raw);
    return parts.size() == 1 ? FromVariant(parts[0]) : fallback;
}

bool File::Bool(const std::string& group, const std::string& key, bool fallback) const {
    const std::string v = String(group, key, "");
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return fallback;
}

double File::Number(const std::string& group, const std::string& key, double fallback) const {
    const std::string v = String(group, key, "");
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return end != v.c_str() ? d : fallback;
}

std::vector<std::string> File::List(const std::string& group, const std::string& key) const {
    const auto raw = Raw(group, key);
    if (!raw || *raw == "@Invalid()" || raw->empty()) return {};
    auto parts = Unescape(*raw);
    for (auto& part : parts) part = FromVariant(part);
    return parts;
}

void File::SetString(const std::string& group, const std::string& key, const std::string& value) {
    SetRaw(group, key, Escape(ToVariant(value)));
}

void File::SetBool(const std::string& group, const std::string& key, bool value) {
    SetRaw(group, key, value ? "true" : "false");
}

void File::SetNumber(const std::string& group, const std::string& key, double value) {
    // whole numbers exactly (SDL's key codes reach 0x400000E1, Left Shift: "%.9g" made it 1.07374205e+09, read back as
    // 1073742050, Left Alt), the others to float precision as Qt writes them
    char buf[40];
    if (std::floor(value) == value && std::fabs(value) < 9.0e15)
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(value));
    else
        std::snprintf(buf, sizeof buf, "%.9g", value);
    SetRaw(group, key, buf);
}

void File::SetList(const std::string& group, const std::string& key, const std::vector<std::string>& values) {
    if (values.empty()) {
        SetRaw(group, key, "@Invalid()");
        return;
    }
    std::string raw;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) raw += ", ";
        raw += Escape(ToVariant(values[i]));
    }
    SetRaw(group, key, raw);
}

bool File::Save() const {
    std::scoped_lock lock{FileLock()};
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    for (const std::string& line : lines) std::fprintf(f, "%s\n", line.c_str());
    const bool ok = std::fclose(f) == 0;
    if (!ok || rt_replace_file(tmp.c_str(), path.c_str())) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace MsrIni
