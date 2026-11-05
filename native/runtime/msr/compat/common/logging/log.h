// msr-native: the logging macros src/msr's code uses (common/logging/log.h in the emulator), printed to stderr.
// Only the "{}" placeholder of the emulator's fmt-style messages is supported.
#pragma once

#include <cstdio>
#include <sstream>
#include <string_view>

namespace MsrCompatLog {

inline void Format(std::ostringstream& out, std::string_view text) {
    out << text;
}

template <typename T, typename... Rest>
void Format(std::ostringstream& out, std::string_view text, const T& value, const Rest&... rest) {
    const auto at = text.find("{}");
    if (at == std::string_view::npos) {
        out << text;
        return;
    }
    out << text.substr(0, at) << value;
    Format(out, text.substr(at + 2), rest...);
}

template <typename... Args>
void Print(const char* level, const char* category, std::string_view text, const Args&... args) {
    std::ostringstream out;
    Format(out, text, args...);
    std::fprintf(stderr, "msr: %s <%s> %s\n", category, level, out.str().c_str());
}

} // namespace MsrCompatLog

#define LOG_CRITICAL(category, ...) ::MsrCompatLog::Print("Critical", #category, __VA_ARGS__)
#define LOG_ERROR(category, ...) ::MsrCompatLog::Print("Error", #category, __VA_ARGS__)
#define LOG_WARNING(category, ...) ::MsrCompatLog::Print("Warning", #category, __VA_ARGS__)
#define LOG_INFO(category, ...) ::MsrCompatLog::Print("Info", #category, __VA_ARGS__)
#define LOG_DEBUG(category, ...) ((void)0)
#define LOG_TRACE(category, ...) ((void)0)
