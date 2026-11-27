// UTF-8 text <-> std::filesystem::path, the same in C++17 and C++20 (u8path is
// deprecated in 20, and u8string() changes its type there).
#pragma once

#include <filesystem>
#include <string>

namespace mods {

inline std::filesystem::path path_from_utf8(const std::string& s) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
    return std::filesystem::u8path(s);
#endif
}

inline std::string path_to_utf8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

inline std::string path_to_generic_utf8(const std::filesystem::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace mods
