// The English text, for tests that compile host sources but do not link host/i18n.cpp.
#include "host/i18n.h"

#include <cstdarg>
#include <cstdio>

namespace i18n {
const char* tr(const char* en) { return en; }
std::string trs(const std::string& en) { return en; }
std::string tf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}
}  // namespace i18n
