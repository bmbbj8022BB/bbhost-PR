// The tests check the English text, so they link this instead of host/i18n.cpp.
#include "host/i18n.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace i18n {
void set_language(const std::string&) {}
const std::string& language() { static const std::string en = "en"; return en; }
bool active() { return false; }
const char* tr(const char* en) { return en; }
const char* trl(const char* en) { return en; }
std::string trs(const std::string& en) { return en; }
std::string tf(const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, fmt, ap2);
    va_end(ap2);
    return out;
}
std::string find_font(const std::string&) { return ""; }
}  // namespace i18n
