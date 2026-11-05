// settings.ini as the launcher's QSettings (IniFormat) writes it, read and changed by msr-native without Qt: sections,
// key=value lines, strings escaped and quoted as QSettings does, string lists joined by ", " (an empty list is
// "@Invalid()"). Every line msr-native does not change is kept, so the launcher's keys and sections survive.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace MsrIni {

class File {
public:
    /// Reads `path`; a missing file is an empty one.
    explicit File(std::string path);

    /// The raw (still escaped) value of `key` in `[group]` ("General" for the top level).
    std::optional<std::string> Raw(const std::string& group, const std::string& key) const;
    /// Replaces or adds `key` in `[group]`, adding the section when missing.
    void SetRaw(const std::string& group, const std::string& key, const std::string& raw);

    std::string String(const std::string& group, const std::string& key, const std::string& fallback) const;
    bool Bool(const std::string& group, const std::string& key, bool fallback) const;
    double Number(const std::string& group, const std::string& key, double fallback) const;
    std::vector<std::string> List(const std::string& group, const std::string& key) const;

    void SetString(const std::string& group, const std::string& key, const std::string& value);
    void SetBool(const std::string& group, const std::string& key, bool value);
    void SetNumber(const std::string& group, const std::string& key, double value);
    void SetList(const std::string& group, const std::string& key, const std::vector<std::string>& values);

    /// Written beside the file, then moved over it. Returns false when it could not be written.
    bool Save() const;

private:
    std::string path;
    std::vector<std::string> lines;
};

/// QSettings' escaping of one string (iniEscapedString), and the parsing of a value into strings
/// (iniUnescapedStringList): one string for a plain value, several for a list.
std::string Escape(const std::string& value);
std::vector<std::string> Unescape(const std::string& raw);

} // namespace MsrIni
