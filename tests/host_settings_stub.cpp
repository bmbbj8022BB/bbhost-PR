// The settings at their defaults, for tests that compile host sources but do not link host/options.cpp.
#include "host/settings.h"

const HostSettings& host_settings_ref() {
    static const HostSettings s{};
    return s;
}
